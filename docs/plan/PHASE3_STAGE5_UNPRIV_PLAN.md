# Phase 3, Stage 5 — The Unprivileged Flip (design plan)

> Status: **design draft, not started.** This is the last item of Phase 3
> (kernel/user separation). It turns the *mechanism* built in Stages 1–4 and MPU
> Phases 1–2 into *enforced* isolation. Supersedes/expands §3 of
> `MPU_CACHE_INTEGRATION_PLAN.md`.

## 0. Where we start from (ground truth)

Already built, HW-validated on the Nucleo-F401RE (`build/vaios_autoconf.h`):

- **SVC gateway** (`VAIOS_SYSCALL_SVC=1`): `svc 1` → `SVCall_Handler`
  (`port.c:318`) → `v_syscall_dispatch(num, args)` (`syscall.c:22`). Number in
  stacked r12, args = stacked r0–r3, result written back to stacked r0.
- **fd-typed IPC** (`VAIOS_IPC_FD`) + **devfs fd table** (`VAIOS_DEVFS`): the
  handle-safe IPC/file surface (fd → kernel object via type-checked table).
- **Per-task heap** (`VAIOS_TASK_HEAP`): `malloc/free` out of the task's own
  block; kernel keeps `v_malloc/v_free`.
- **MPU Phase 1** stack guard (region 7, `AP_NONE` at block base) applied on
  switch-in via `v_port_apply_current_mpu` (`port.c:443`).
- **MPU Phase 2** static map: SRAM `AP_RW`+XN (region 0), peripherals
  `AP_PRIV_RW`+XN (region 1), NULL guard (region 3), PRIVDEFENA on.

**But tasks still run PRIVILEGED.** `init_task_stack` (`port.c:397`) builds a
thread/PSP frame (`EXC_RETURN=0xFFFFFFFD`) and `scheduler_start` sets `CONTROL=0`
(`port.c:222`) — `nPRIV=0`. Nothing sets `nPRIV=1`, and there is **no
syscall-boundary pointer validation** (`syscall.c:9` says so explicitly). So
today a buggy/malicious task can read/write all SRAM, hit peripherals, and hand
the kernel forged pointers.

**MPU region budget (F401RE = 8 regions).** In use: 0 (SRAM), 1 (periph),
3 (NULL), 7 (guard). **Free: 2, 4, 5, 6** — the comment at `port_hw.c:226`
already reserves "4..6 for the Phase 3 per-task set." Budget is not a blocker.

## 1. Goal & invariants

**Goal:** a task runs unprivileged and can only (a) execute its own code from
flash, (b) read/write its own block, (c) enter the kernel via SVC. A misbehaving
task cannot corrupt the kernel or another task; the kernel never dereferences a
user pointer it hasn't proven belongs to the caller.

**Invariants to hold after Stage 5:**

1. **I-priv** — thread mode runs `nPRIV=1` for user tasks; the kernel executes
   only in handler mode (SVC/PendSV/SysTick), which is privileged by architecture.
2. **I-mem** — every address an unprivileged task touches lies in an enabled MPU
   region granting it unprivileged access: its own block (RW) or flash (RX). All
   else (kernel SRAM, other blocks, peripherals, NULL) faults. *Consequence of
   PRIVDEFENA: the background map does NOT cover unprivileged code, so coverage
   must be explicit.*
3. **I-ptr** — the kernel validates any user pointer/length against the caller's
   block before dereference; a bad pointer yields `-EFAULT`, never a kernel fault.
4. **I-handle** — the kernel never dereferences a user-supplied *kernel* pointer.
   Raw-handle IPC (non-fd) becomes privileged-only; unprivileged tasks use the
   fd-typed API (the whole reason Stage 3 built it).

## 2. Sub-stages (each independently reviewable; ordered for a safe ladder)

The safe ordering lands all the *machinery* while tasks are still privileged (no
behavioral change, easy to prove no regression), routes faults, and flips
`nPRIV` **last** — the single commit where isolation goes live.

### 5a — Memory layout prep *(no behavior change; foundation for 5c/5e)*

Two things must change before the flip, because unprivileged code can't reach
where the heap machinery lives now:

- **Task block must be a single MPU region.** An MPU region needs a
  power-of-two size on a size-aligned base. `task_create` allocates with
  `v_memalign(VAIOS_MPU_GUARD_SIZE, size)` (`task.c:210`) — aligned to 32, not to
  `size`. Change to `v_memalign(size, size)` so `[mem_block, mem_block+size)` is
  one aligned region. (Size is already required power-of-two ≥128.) Guard at the
  base stays 32-aligned trivially. *This is coupled to 5b (only matters once the
  per-task region exists), so it can land with 5b rather than here.*
- **`malloc/free/calloc/realloc` become SVC syscalls** (trap-once, like the
  fd-IPC calls). *Rationale — the naive "relocate metadata into the block" fix
  does NOT work:* an unprivileged task can neither write the TCB heap fields
  (`heap_brk`/`heap_peak_brk`, `task.h:102-103`, kernel SRAM) **nor even read
  `current_task`** (a kernel global in privileged-only SRAM) to locate its own
  block. So the allocator can't run in userspace at all under `nPRIV` without a
  reserved base register (`-ffixed-r9` SB ABI — rejected as too heavy).
  Instead the task-facing `malloc` traps via `SYS_malloc`; the privileged handler
  reads `current_task`, runs the existing boundary-tag logic over the in-block
  free list, and returns the pointer. The heap *memory* stays in the task's block
  (unpriv-usable); only the bookkeeping traps.
  - New syscalls `SYS_malloc`/`SYS_free`/`SYS_calloc`/`SYS_realloc` (values
    20–23), gated on `VAIOS_TASK_HEAP && VAIOS_MPU_USER_SEPARATION`.
  - **Live-stack bound must read PSP, not MSP.** The current bound reads SP via
    `mov` (`memory.c` `task_live_sp`), fine in thread mode. Running in the SVC
    handler that reads MSP — wrong. Read the task's stack via `mrs <r>, psp`
    (the task's PSP still holds its stack, now below its pre-call SP by one
    stacked exception frame — a safe, slightly conservative bound).
  - None-blocking, so no deferred-result machinery: the pointer/`NULL` returns
    directly in stacked r0.

*Validate:* build `task_heap_test` with the flag on (malloc now traps, tasks
still privileged) → identical `used=…/0` and `perf` output on HW as the
direct-call path.

### 5b — Per-task MPU region set *(benign while privileged)*

Turn the static map from "all SRAM RW" into "kernel SRAM privileged-only + this
task's block granted." Program at `v_port_apply_current_mpu` (`port.c:443`) —
extend from `count=1` (guard) to the per-task set.

| Rgn | Region | AP | XN | When |
|--|--|--|--|--|
| 0 | SRAM 0x2000_0000 128K | **`AP_PRIV_RW`** (was `AP_RW`) | yes | static |
| 1 | Peripherals | `AP_PRIV_RW` | yes | static (unchanged) |
| 2 | Flash 0x0800_0000 512K | **`AP_RO` + executable** | no | static (**new: always on**, was FLASH_RO-only) |
| 3 | NULL guard | `AP_NONE` | yes | static (unchanged) |
| 4 | **This task's block** `[mem_block, +size)` | `AP_RW` (unpriv) | yes | **per-task, switch-in** |
| 5,6 | free | — | — | headroom (shared-RO, or split stack/heap) |
| 7 | Stack guard (block base) | `AP_NONE` | yes | per-task (unchanged; wins on overlap) |

Region 0 → `AP_PRIV_RW` is invisible to privileged tasks (they keep RW), so this
whole change is a no-op until `nPRIV` flips. Region 2 flash-RX must be always-on
so unprivileged code can fetch instructions and read `.rodata`. The per-task
region (4) + guard (7) are the count=2 set applied on switch-in (in `PendSV` and
the `svc 0` first-launch path, which currently skips the MPU hook — `port.c:333`).

*Validate (still privileged):* boots, all examples unchanged. Then a temporary
unit probe: set `nPRIV` for one task and confirm it RW's its own block and faults
(MemManage) on a kernel address / neighbor block.

### 5c — Syscall-boundary pointer validation *(benign while privileged)*

Add to the kernel:

- `bool v_access_ok(const void *p, size_t len, bool write)` — true iff
  `[p, p+len)` ⊆ `current_task`'s `[mem_block, mem_block+stack_size)`, with
  overflow check. (`write` is a hook for a future finer split; the whole block is
  RW today.)
- `long v_strnlen_user(const char *s, size_t max)` — bounded NUL scan that never
  reads past the caller's block; used for the `path`/`name` string args.

Wire into the **single choke point** `v_syscall_dispatch` (`syscall.c:22`),
keyed by each syscall's pointer arity (table from the syscall map):

| Syscall | Check |
|--|--|
| `open`(path) | `v_strnlen_user(path)` in block |
| `write`(buf,len) | `v_access_ok(buf,len,read)` |
| `read`(buf,len) | `v_access_ok(buf,len,write)` |
| `sem_open`/`mtx_open`(name) | `v_strnlen_user(name)` in block |
| `wait`(fds,nfds) | `v_access_ok(fds, nfds*sizeof(int), read)` |

Bad pointer → return `-EFAULT` (write to stacked r0), no dereference.

**Close the raw-handle hole (I-handle):** dispatch cases 3–6 (`sem_give/take`,
`mutex_lock/unlock`, non-fd) cast `args[0]` straight to a kernel `sema_t*/rmutex_t*`
(`syscall.c:38-52`). Under `nPRIV` these are unsafe (forged pointer → arbitrary
kernel deref) and can't be bounds-checked (the object lives in kernel memory).
**Gate them to privileged callers**; the fd-typed IPC (cases 11–17) is the
sanctioned unprivileged path. Concrete mechanism: exception entry does **not**
clear `CONTROL.nPRIV`, so it still holds the *caller's* value inside the handler.
`v_syscall_dispatch` reads it (`mrs <r>, control` via a small helper) and returns
`-EPERM` for cases 3–6 when `nPRIV=1`. No per-task flag needed for the gate. This
gate is a **no-op today** (all callers privileged) and activates at 5e — so it
lands as dormant, compile-checked prep, validated once nPRIV goes live.

*Validate (still privileged):* validation logic exercised by host unit tests
(craft in/out-of-block pointers against a synthetic TCB); privileged tasks pass
real pointers → unaffected.

### 5d — Fault routing *(prep for the flip)*

Before isolation goes live, make violations report cleanly:

- Enable **BusFault** + **UsageFault** in `SHCSR` and route both through the
  CFSR decode + `v_panic`, like `MemManage`/`HardFault` today (they currently
  spin — `port.c:174-181`). Unprivileged privileged-instruction attempts raise
  UsageFault (`INVSTATE`/`NOCP`); stray bus accesses raise BusFault.
- Keep the `MemManage` panic decode (`port.c:161`) — already reports task id +
  MMFAR + MMFSR.

*Scope decision — detect vs. recover.* Stage 5 as specified **detects and
reports** a violating task (panic with full decode). *Killing the faulting task
and rescheduling the rest* — the true "kernel survives a bad task" payoff — is
larger (unwind the task from the fault handler, free its block/fds, mark it dead,
return to the scheduler) and is proposed as **Stage 6 / a fast-follow**, not
folded in here. Flagged as an open question in §6.

### 5e — The flip *(the one commit where isolation activates)*

- Add a per-task `uint8_t privileged` to the TCB (default 0 = unprivileged).
- On switch-in — in `PendSV` (`port.c:252`) before `bx lr`, and in the `svc 0`
  first-launch path (`port.c:333`) — set `CONTROL.nPRIV` from
  `current_task->privileged` (`msr control` + `isb`). Handler mode is privileged
  regardless; the value takes effect on exception return to thread.
- `task_exit` (the `LR` of every task frame, `port.c:412`) currently runs in
  thread mode — under `nPRIV` it must **trap** (make it `svc` a new `SYS_exit`)
  since it does kernel cleanup. Audit any other kernel routine reachable in thread
  mode (e.g. a task spawning a task ⇒ `task_create` must become a syscall too).

*Validate on HW (the payoff tests):* an unprivileged task (1) reads/writes its
own stack+heap fine; (2) faults on a read of a kernel/global address; (3) faults
on a write into a neighbor task's block; (4) faults on a direct peripheral poke;
(5) gets `-EFAULT` (not a crash) when it passes an out-of-block buffer to
`write()`; (6) still does fd-IPC ping-pong and `malloc/free` normally.

## 3. Kconfig

New gate so the whole flip is opt-in and mainline is byte-for-byte unchanged:

```
config VAIOS_MPU_USER_SEPARATION
    bool "Unprivileged tasks + per-task MPU isolation (Phase 3, Stage 5)"
    depends on VAIOS_SYSCALL_SVC && VAIOS_MPU_STATIC_PROTECT
    default n
```

(`VAIOS_SYSCALL_SVC` already implies `VAIOS_MPU_ENABLE`.) 5a–5d compile behind it
too, so nothing activates until a config turns it on. An
`examples/Kconfig` entry can then `select` it for a Stage-5 demo example.

## 4. ABI / behavioral consequences (call out in the PR)

- **Raw-handle IPC becomes privileged-only.** Examples using `v_semaphore_*` /
  `v_mutex_*` (non-fd) won't run as unprivileged tasks — they must use fd IPC.
  This is by design (Stage 3's purpose) but breaks the classic-IPC examples under
  the flag. A Stage-5 demo should use the fd API.
- **`task_exit` / `task_create` from a task become syscalls.**
- **`malloc/free/calloc/realloc` become syscalls** under the flag (5a) — API and
  return values unchanged; a trap per allocation replaces a direct call. Only
  active when `VAIOS_MPU_USER_SEPARATION` is set.
- **Task block alignment tightens** to its own size (5b) — slightly more
  alignment slack in `v_memalign`; quantify RAM cost on the F401.

## 5. Validation ladder (per sub-stage)

Renode first (deterministic, scriptable fault injection), then Nucleo-F401RE:

1. 5a/5b/5c/5d land privileged → **regression gate**: all HW examples from the
   Stage-4 sweep behave identically; host suite green.
2. 5c → **host unit tests** for `v_access_ok`/`v_strnlen_user` (synthetic TCB,
   in/out-of-block, overflow, NULL, unterminated string).
3. 5e → **isolation fault-injection example** (new, e.g. `mpu_user_probe`)
   exercising the six payoff tests above; each MPU/Bus/Usage fault must produce a
   clean decoded panic, not a lockup.
4. Confirm `perf`/`task_heap`/`fd_ipc`/`fd_wait` still pass **as unprivileged
   tasks** end-to-end.

## 6. Risks & open questions

- **Fault recovery scope** — detect-and-panic (this plan) vs. kill-and-reschedule
  (Stage 6?). Recommend shipping detect-first, then recovery; recovery is where
  isolation actually *pays off* (kernel survives a bad task), so it shouldn't lag
  long. **Decision needed.**
- **Region alignment RAM cost** — aligning each block to its own size wastes up
  to `size` bytes in the worst case. Measure on the F401 (limited SRAM) before
  committing; consider MPU subregions (SRD) if it bites.
- **Privileged system tasks** — audit idle / any daemon: do any need privilege?
  The per-task `privileged` flag covers exceptions, but the default must be safe.
- **FPU under nPRIV** — lazy stacking + `CONTROL.FPCA`; confirm an unprivileged
  task using the FPU (`fpu_usage`) stacks/unstacks correctly and CPACR grants
  unprivileged CP10/CP11.
- **Split stack/heap grant** — one region (4) covers the whole block RW today; a
  future refinement could make the stack and heap separate regions (regions 5/6)
  for W^X-ish intra-block hardening. Out of scope for 5, noted for later.
