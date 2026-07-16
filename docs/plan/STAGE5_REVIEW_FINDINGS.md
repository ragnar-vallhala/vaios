# Stage 5 Review Findings — `a11fb44c80..caa406c`

Correctness/security review of the Phase 3 / Stage 5 work (MPU static protection,
SVC syscall dispatch, fd-typed IPC, per-task heap, the unprivileged flip) plus the
on-target gcov tooling. Findings are ranked by severity, each with the exact
site(s) and a concrete reproduction.

Legend: 🔴 critical · 🟠 high · 🟡 medium · 🟢 low. "Boundary" = defeats the Stage 5
privilege isolation (an unprivileged task attacking the kernel); "host" = bites
the 64-bit host test build, latent on the 32-bit target.

> **Gate:** issues **1–3** are confirmed paths for an unprivileged task to corrupt
> or read kernel/peer memory. They defeat the point of the unprivileged flip and
> should be fixed before the flip is trusted. **4–6** share one root cause: task
> teardown never releases fds / mutexes / wait-queue membership.

---

## Test coverage — host vs. hardware

Each finding is tagged with how it can be exercised:

- **HOST** — the buggy logic is in kernel C the host suite already compiles
  (`memory.c`, `task.c`, `ipc.c`); a host test can drive it directly.
- **HOST\*** — host-coverable, but needs a harness change first (see below).
- **HW** — requires the target / Renode: the ARM-only SVC/MPU/nPRIV machinery, or
  a real UART, is what surfaces it.

**Key distinction.** For every boundary bug (#1–#3, #11) the host suite can prove
the *memory-safety defect*, because a host test calling `v_wait_block_impl` /
`v_syscall_dispatch` / `realloc` directly with hostile arguments **is** the
syscall body the SVC path runs. What the host suite **cannot** prove is
*enforcement* — that an unprivileged task actually traps and the MPU/nPRIV
actually blocks the raw access. That is inherently target-only and is what the
Renode isolation regression + on-target gcov exist for.

### What the host build compiles today
- **In:** `memory.c` (global `v_malloc/v_free/v_memalign` always; per-task
  `malloc/free/realloc` **only if `VAIOS_TASK_HEAP`** — currently **off**),
  `task.c`, `ipc.c`, `devfs.c` (separate binary `vaios_devfs_tests`).
- **Out:** `portable/cortex-m4/syscall.c` (`v_syscall_dispatch`), `port.c`,
  `port_hw.c`, `startup.s` — all ARM-only.
- **No `-fsanitize`** anywhere; `-O0 -g`. Not defined: `VAIOS_MPU_USER_SEPARATION`,
  `VAIOS_SYSCALL_SVC`, `VAIOS_TASK_HEAP`.

### Coverage summary

| # | Tag | Where the logic lives / what's needed |
|---|-----|----------------------------------------|
| 1 | HOST | `ipc.c v_wait_block_impl`; observing the overrun write needs ASAN or a canary |
| 2 | HOST | validator is in un-linked `syscall.c`; fully covered if the clamp lands in the host-linked impl (recommended) |
| 3 | HOST | per-task `realloc` gated by `VAIOS_TASK_HEAP` (off); needs `VAIOS_TASK_HEAP=1` + synthetic `current_task` |
| 4 | HOST | `task.c` exit + `ipc.c` tables; fd-teardown wiring may need the devfs layer |
| 5 | HOST | `task.c` list ops; list truncation observable directly, UAF tail needs ASAN |
| 6 | HOST | `task.c` exit + `ipc.c` mutex owner model |
| 7 | HOST | pure `ipc.c`; trivial |
| 8 | HOST | **global `v_memalign`; host is the *only* place it manifests** (target interval empty) |
| 9 | HOST | per-task `malloc` gated by `VAIOS_TASK_HEAP` (off); needs `VAIOS_TASK_HEAP=1` |
| 10 | HOST decoder / HW loss | Python decoder unit-testable with a dropped-line stream; the real UART-loss event needs Renode/HW |
| 11 | HOST logic / HW fault | wrong `v_access_ok` return is host-assertable; the panic needs the live MPU |
| 12 | HOST | pure `ipc.c` |
| 13 | HW | needs >64 instrumented TUs / arena stress on target |
| 14 | HOST | `ipc.c`/`task.c`; drive the `v_get_ticks` stub near wrap |

### Harness changes that unlock coverage — DONE

All three landed under `tests/`; each new suite runs under ASan+UBSan and is
wired into `tools/run_tests.sh` + CTest. Result: 229 pre-existing asserts + 6
per-task-heap cases + 12 syscall-dispatch cases, all green under the sanitizers.

1. ✅ **ASan+UBSan on the host suites** (`VAIOS_TEST_SANITIZE`, default ON;
   `tests/CMakeLists.txt`). `-fno-sanitize-recover=all` makes UB/OOB a hard
   failure so a bug can't slip past CTest's "0 failed" gate. This is what makes
   #1, #5, #8 — already *reachable* — *observable* instead of silent.
2. ✅ **`vaios_syscall_tests`** links the real `v_syscall_dispatch`
   (`portable/cortex-m4/syscall.c`) + the real validators (`task.c`) with a
   synthetic caller whose privilege the test flips. The two ARM-asm seams
   (`CONTROL.nPRIV` read; the `svc` trampolines) resolve to host hooks under
   `VAIOS_HOST_TEST` (`syscall.c`, `include/syscall.h`); the caller block is a
   `MAP_32BIT` mapping so pointers survive the 32-bit syscall ABI. Unblocks
   **#2** and the dispatch side of #1/#3.
3. ✅ **`vaios_taskheap_tests`** builds `memory.c` with `VAIOS_TASK_HEAP=1`, SVC
   off, a synthetic `current_task`; the allocator symbols are renamed
   (`-Dmalloc=vtask_malloc …`) so they don't override libc on host, and the task
   block is a real ASan object so an allocator escaping it aborts. Unblocks
   **#3** and **#9**.

The **HOST\*** tags below are now plain **HOST** — the binaries exist and the
paths are reachable.

### Regression tests + fix status — ALL 14 FIXED

Every finding is fixed. Full host suite: **245/245** under ASan+UBSan (plus the
gcov decoder Python test), **0 warnings**. Each test below was verified to fail
before its fix and pass after.

| Finding | Test | Binary |
|---|---|---|
| #2 SYS_wait `nfds*sizeof` overflow | `test_bug2_wait_nfds_multiply_overflow` | `vaios_syscall_tests` |
| #3 realloc foreign-pointer disclosure | `test_bug3_realloc_foreign_ptr_discloses` | `vaios_taskheap_tests` |
| #4 named-object leak on exit | `test_bug4_named_sem_slot_freed_on_exit` | `vaios_ipcfd_tests` |
| #5 blocked-task exit corrupts `blocked_list` | `test_bug5_blocked_task_exit_corrupts_blocked_list` | `vaios_tests` |
| #6 mutex held on exit not released | `test_bug6_mutex_held_on_exit_not_released` | `vaios_tests` |
| #7 over-long name silently truncated | `test_bug7_overlong_name_rejected` | `vaios_ipcfd_tests` |
| #8 v_memalign under-reservation | `test_memalign_underalloc_regression` | `vaios_tests` |
| #9 per-task malloc no cap at block top | `test_bug9_malloc_past_block_top_returns_null` | `vaios_taskheap_tests` |
| #10 gcov no integrity check | `tools/test_gcov_uart_decode.py` | (python) |
| #11 `v_access_ok` includes guard region | `test_bug11_guard_region_rejected` | `vaios_uaccess_guard_tests` |
| #12 fd-mutex owner re-lock self-deadlock | `test_bug12_owner_relock_no_self_block` | `vaios_ipcfd_tests` |
| #14 tick-wrap finite wait → infinite | `test_bug14_finite_wait_not_infinite_on_tick_wrap` | `vaios_tests` |

**#1** (SYS_wait `wnodes` overrun) and **#13** (gcov `MAX_INFOS`/arena overflow)
are **fixed** without a dedicated host test: #1's overrun is an OOB write that
ASan-*aborts* rather than asserts (the clamp now lives in `v_wait_block_impl`),
and #13 is a target-side emitter marker (`__gcov_init` now counts dropped TUs and
the dump emits `@@VAIOS_GCDA_ERROR`).

Fix commits: `1642a15` (#2/#3/#9), `ad211af` (#1/#5/#6), `8865690` (#4/#7/#8/#12),
`2c08ddf` (#10/#11/#13/#14).

### On-target (Renode) coverage

Beyond the host suite, three Renode jobs run in CI on real ARM under NAVHAL:

- **On-target unit suite** (`run_all_tests.sh sitl`) — 552 asserts incl. the
  memory (#8) and uaccess (#11-path) suites on 32-bit ARM.
- **Isolation regression** (`renode_isolation.sh`) — the unprivileged flip:
  nPRIV=1, the SVC + per-task-heap path (#3/#9), MPU trap + fault handlers.
- **Task-teardown regression** (`renode_lifecycle.sh`, `examples/34_stage5_lifecycle.c`)
  — **#5** (blocked-task force-termination) and **#6** (mutex held on exit) under
  the REAL running scheduler, not the host's frozen one. Verified red-on-revert.
- **fd-typed IPC regression** (`renode_fdipc.sh`, `examples/35_stage5_fdipc.c`)
  — **#4** (named-object slot reclaim on exit), **#7** (over-long name), **#12**
  (fd-mutex owner re-lock) under the real scheduler + SVC path (DEVFS + IPC_FD).
  Verified red-on-revert (FAIL #7, FAIL #4, and a self-deadlock hang at #12).

The host suite exercises the *logic* with a frozen scheduler; these prove the
*real* block/exit/wake/context-switch + SVC paths on-device. Every finding with
kernel-behaviour impact (#1–#9, #11, #12, #14) now has on-target validation;
#10/#13 are gcov host/target tooling.

**Genuinely target/Renode/HW-only (unchanged):** the enforcement proofs (unpriv
actually traps + MPU blocks), #11's fault, #13, and #10's real UART-loss event.

---

## 🔴 1. `SYS_wait` overruns `TCB.wnodes[VAIOS_MAX_FDS]` (boundary) — HOST

**Where:** write site `kernel/ipc.c:651`; unclamped entry `kernel/ipc.c:630`
(`v_wait_block_impl`); dispatch `portable/cortex-m4/syscall.c:150-153`.

**Cause:** the `nfds <= 0 || nfds > VAIOS_MAX_FDS` clamp exists **only** in the
userspace wrapper `v_wait` (`ipc.c:687`). The SVC path calls `v_wait_block_impl`
directly with the raw `args[1]`. Its arm loop writes `&cur->wnodes[k++]` once per
*valid* fd:

```c
// kernel/ipc.c:646-651
int k = 0;
for (int i = 0; i < nfds; i++) {
  named_sem_t *ns = (named_sem_t *)v_fd_obj(fds[i], &ipc_sem_ops);
  if (!ns) continue;
  v_wnode *node = &cur->wnodes[k++];   // no bound on k vs VAIOS_MAX_FDS
  ...
}
```

`wnodes[]` is a fixed 8-entry (`VAIOS_MAX_FDS`) array inside the TCB, which lives
on the kernel heap.

**Reproduce:**
1. Unprivileged task opens one sem → fd 3.
2. Build `int fds[64]`, every entry = 3 (256 bytes; passes `v_access_ok` on a
   ≥512-byte task block).
3. Issue the syscall raw, bypassing the wrapper: `svc` with `r12 = SYS_wait`,
   `r0 = fds`, `r1 = 64`, `r2 = 1`.
4. `v_fd_obj` returns valid for every entry → `k` runs to 64 → `wnodes[8..63]`
   are written past the array, into adjacent kernel-heap blocks / neighbouring
   TCBs. **Full kernel memory corruption from an unprivileged task.**

**Fix:** clamp `nfds` at the top of `v_wait_block_impl` (or in the dispatch),
returning `-1`/`V_EFAULT` for `nfds <= 0 || nfds > VAIOS_MAX_FDS` — don't rely on
the userspace wrapper for a security bound.

---

## 🟠 2. `SYS_wait` length check integer-overflows (boundary) — HOST

**Where:** `portable/cortex-m4/syscall.c:74-75`.

```c
case SYS_wait:
  if (!v_access_ok((const void *)(uintptr_t)args[0],
                   args[1] * (uint32_t)sizeof(int), 0))   // 32-bit multiply wraps
    return V_EFAULT;
```

**Cause:** `args[1] * 4` is a 32-bit multiply. `nfds = 0x40000000` → product `0`
→ `v_access_ok(ptr, 0)` returns true for any in-block pointer. The readiness loop
(`ipc.c:634`) then reads `fds[0 .. 0x3FFFFFFF]`.

**Reproduce:** raw `svc` with `SYS_wait`, `r1 = 0x40000000`, `r0` = any valid
in-block pointer. Validator passes; the kernel does ~1 GB of privileged reads off
the end of the buffer → BusFault → panic (system-wide DoS). Also compounds #1.

**Fix:** same as #1 — reject `nfds > VAIOS_MAX_FDS` **before** the multiply.

---

## 🟠 3. `realloc()` dereferences an unvalidated user pointer (boundary) — HOST

**Where:** `kernel/memory/memory.c:469-479`; dispatch `syscall.c:170-172`.

**Cause:** `free()` range-checks the block against `[heap_base, heap_brk)` before
touching it (`memory.c:415`). `realloc()` does **not** — it computes
`blk = ptr - THDR` and immediately reads it:

```c
// kernel/memory/memory.c:469-479
Heap_Mem_Block *blk = (Heap_Mem_Block *)((uint8_t *)ptr - THDR);
...
if (blk->magic_number == SANITY_MAGIC_NUMBER && blk->size >= need)  // read @ ptr-8
  return ptr;
void *np = malloc(size);
if (!np) return NULL;
uint32_t old = (blk->magic_number == SANITY_MAGIC_NUMBER) ? blk->size : 0;
v_memcpy(np, ptr, old < size ? old : size);   // copies FROM user-controlled ptr
```

`SYS_realloc` is not in `v_syscall_privileged_only` and has no validation case in
the dispatch switch, so an unprivileged task reaches it.

**Reproduce (two variants):**
- *DoS:* `realloc(target, N)` with `target-8` in unmapped/device memory →
  privileged read → BusFault → panic.
- *Cross-task disclosure:* point `target` into another live task's heap block
  (guessable `0x2000_xxxx`) where `target-8` legitimately holds the magic. With
  `N >` that block's `size`, `malloc(N)` succeeds in the caller's own heap and
  `v_memcpy(np, target, blk->size)` copies **the other task's heap contents**
  into the caller's readable buffer. (`free(ptr)` afterward is safe — it re-checks
  the range — but the read already happened.)

**Fix:** add the same `blk < heap_base || blk >= heap_brk` guard `free()` uses,
before the first dereference.

---

## 🟠 4. Named IPC objects leak their refcount on task exit — HOST

**Where:** `kernel/task.c:370-377` (`v_task_exit_impl`), `:432-451`
(`task_exit_request`) — neither walks `t->fds[]`. Refcount only drops via
`v_file_close` → `ipc_sem_close`/`ipc_mtx_close` (`ipc.c:388-395`, `:479-486`).

**Cause:** a task that `v_sem_open("s", V_IPC_CREATE)`s and then returns/exits
without an explicit `v_file_close` never decrements `named_sems[i].refcount`; the
slot's `used` never clears.

**Reproduce:** 8 short-lived worker tasks, each `v_sem_open("w", V_IPC_CREATE)`
then return. After the 8th, `MAX_NAMED_SEMS` is exhausted and every subsequent
`v_sem_open(..., V_IPC_CREATE)` returns `-1` **permanently**. Same for
`named_mtxs`. This is the exact leak the fd model exists to prevent.

**Fix:** in the exit path, iterate `t->fds[]` and call each live fd's
`ops->close`.

---

## 🟠 5. Terminating a `TASK_BLOCKED` task corrupts `blocked_list` + dangling wait nodes — HOST

**Where:** `kernel/task.c:432-446` (`task_exit_request`), interacting with
`enqueue_task` (`task.c:100-118`) and the sem `wait_q`/`observers` (`ipc.c`).

**Cause:** `task_exit_request` unlinks only `TASK_READY` / `TASK_DELAYED`. A task
in `TASK_BLOCKED` (parked in `v_sem_take` / `v_mtx_lock` / `v_wait`) is already on
`blocked_list` and linked in the sem's `wait_q` (via `wait_next`) and/or every
watched sem's `observers` (via `wnodes`). `task_exit_request` then calls
`enqueue_task(&blocked_list, task)`, whose first act is:

```c
// kernel/task.c:105-106
task->next = NULL;
task->prev = NULL;
```

on a node already inside `blocked_list` — severing every node after it and
re-appending `task` at the tail.

**Reproduce:**
1. Task B blocks in `v_sem_take(fd, V_WAIT_FOREVER)` (now on `blocked_list`, in
   the sem's `wait_q`).
2. Another task calls `task_exit_request(B_id)`.
3. `blocked_list` is truncated at B (nodes after B are lost). B stays linked in
   the sem's `wait_q`.
4. A later `v_sem_give`/`v_mutex_unlock` does `wait_q_dequeue` →
   `add_to_ready_list(B)` on a `TASK_TERMINATED` TCB that the idle GC may already
   have `v_free`'d → **use-after-free / ready-list corruption**. For `v_wait`,
   `mwait_wake_observers` later dereferences `n->owner` (freed TCB).

**Fix:** if `status == TASK_BLOCKED`, first `remove_from_blocked_list(task)` and
unlink it from every `wait_q` / observer list it's on, *then* mark terminated and
enqueue.

---

## 🟡 6. Task exits holding a mutex → never released — HOST

**Where:** `kernel/task.c:370-377` / `:432-451` don't walk `held_mutexes`; owner
model in `kernel/ipc.c`.

**Cause:** exit paths never release `held_mutexes`. `rm->owner` keeps pointing at
the soon-freed TCB and `rm->base.count` stays 0.

**Reproduce:** Task A holds named mutex "m" (refcount 2 — B also opened it), then
returns. B's `v_mtx_lock(fd, V_WAIT_FOREVER)` blocks forever (owner is dead, never
unlocks). If a third task later locks another mutex with B queued, the
priority-inheritance chain walk `mutex_lock_common` (`ipc.c:253-262`) dereferences
A's freed TCB via `m->owner`.

**Fix:** release all `held_mutexes` in the exit path (unlock + wake waiters).

---

## 🟡 7. Named sem/mutex names ≥16 chars silently truncate and can't be re-found — HOST

**Where:** `kernel/ipc.c:420-425` (`v_sem_open`), `:511-516` (`v_mtx_open`), with
`sname_eq` (`:374-379`) and `char name[16]`.

**Cause:** the copy loop `while (name[j] && j < 15)` stores ≤15 chars + NUL.
`named_sem_find` compares the *stored* (truncated) name against the caller's
*full* name; `sname_eq` stops at the stored NUL, so `stored[15]=='\0'` vs
`name[15]==<16th char>` mismatches — a ≥16-char name never matches its own entry.

**Reproduce:** Task A `v_sem_open("sensor_bus_north", CREATE)` (16 chars) creates
entry 0. Task B `v_sem_open("sensor_bus_north", CREATE)` → find fails → creates a
*second, distinct* object. A and B signal different semaphores and never
rendezvous; repeated opens exhaust the 8-slot table. No length cap is documented
in `ipc.h`.

**Fix:** reject names that don't fit (return error), or document the 15-char cap
and make `find`/`create` consistent about truncation.

---

## 🟡 8. `v_memalign` search reservation undersized → heap overwrite (host) — HOST (only)

**Where:** `kernel/memory/memory.c:161-162` (reservation), carve `:191-199`.

**Cause:** the find requests only `need + align + THDR`, but when the leading
remainder is too small the loop bumps the aligned address by a **full `align`**,
so the consumed lead can reach `d0 + align` (with `d0` up to
`VHEAP_MIN_PAYLOAD + THDR - 8`). When `d0 > THDR`, `aligned->size` comes out
`< need`.

- Bad-`d0` exists ⇔ a multiple of 8 lies in `(THDR, VHEAP_MIN_PAYLOAD + THDR)`:
  - **Target** (THDR=16, MIN=8): interval `(16,24)` → empty → **safe**.
  - **Host** (THDR=24, MIN=16): interval `(24,40)` → contains 32 → **affected**.

**Reproduce (host build):** `v_memalign(64, 64)` (`need=64`) requests a
`64+64+24 = 152`-byte block. If the free index returns a size-`[152,160)` block
whose header sits at addr ≡ 8 (mod 64) (so `payload0 ≡ 32 mod 64`, `d0=32`), the
loop bumps once, lead becomes 96, `aligned->size = 152 - 96 = 56 < 64`. The block
is marked ALLOC size 56; the caller's 64-byte payload runs 8 bytes past `orig_end`
into the next block's header → silent heap corruption. The regression test misses
it: it runs on a fresh whole-heap block and only uses align=32 (`d0<32`).

**Fix:** reserve `need + align + THDR + VHEAP_MIN_PAYLOAD`, or assert
`aligned->size >= need` after carving and bail/adjust.

---

## 🟡 9. Per-task `malloc` has no hard cap at the block top (host) — HOST

**Where:** `kernel/memory/memory.c:385-390`, `task_live_sp()` `:326-334`.

**Cause:** the only growth bound is `task_live_sp() - TASK_HEAP_STACK_MARGIN`. On
host `task_live_sp()` returns `UINTPTR_MAX`, so `new_brk > UINTPTR_MAX - 256` is
essentially never true — malloc never returns NULL on exhaustion and marches
`heap_brk` past `mem_block` into neighbours. On target the live-PSP bound keeps it
inside the block (stack lives at block top), but there is no independent
`mem_block + size` cap.

**Reproduce (host):** create a task with a small `mem_block`, `malloc` in a loop →
corruption of neighbouring memory instead of a NULL return.

**Fix:** add an explicit upper bound at `mem_block + block_size` independent of
the stack-collision check.

---

## 🟡 10. On-target gcov: no length/CRC → lossy capture looks valid — HOST decoder / HW loss

**Where:** `tools/gcov_uart_decode.py:120-124` and the emitter
`portable/cortex-m4/gcov_dump.c` (whole protocol).

**Cause:** no length field, CRC, or per-frame checksum. Every base64 line is a
multiple of 4 chars, so a dropped/garbled *whole line* is silently skipped
(`re.fullmatch(r"[A-Za-z0-9+/=]+", s)` fails → ignored), the remaining lines still
concatenate to a multiple of 4, `b64decode(validate=True)` succeeds, `raw[:4]`
still equals the magic (line 1), so `flush()` writes the file and the script exits
0.

**Reproduce:** drop one interior line of `port.c`'s frame under Renode/serial → a
`.gcda` ~45 bytes short in the interior that passes every check → gcov reports
wrong/garbage counts (or a merge error) with **no signal the capture was lossy**.

**Fix:** add a per-frame byte-length (and ideally CRC) that the decoder verifies
before writing. (Framing markers, endianness, magic `b"adcg"`, base64 alphabet,
and buffer sizes were all verified correct — this integrity check is the only
gap.)

---

## 🟢 Low

| # | Site | Coverage | Issue |
|---|------|----------|-------|
| 11 | `kernel/task.c:405-410` | HOST logic / HW fault | `v_access_ok` lower bound includes the `AP_NONE` stack-guard region; a buffer in `[mem_block, mem_block+GUARD)` passes validation but faults the kernel's own copy in handler mode → panic. Lower bound should be `heap_base`. |
| 12 | `kernel/ipc.c:532-541` | HOST | fd-typed `v_mtx_lock` has no `owner == current` recursion check; a holder re-locking self-deadlocks silently (forever under `V_WAIT_FOREVER`) instead of erroring. |
| 13 | `portable/cortex-m4/gcov_dump.c:73-76,184-193` | HW | `VAIOS_GCOV_MAX_INFOS=64` and the 8 KB arena silently drop TUs / HardFault on overflow. Fits today's ~25 instrumented TUs; latent. |
| 14 | `kernel/ipc.c:665`, `:183` | HOST | `v_get_ticks() + ticks == 0` (32-bit tick wrap) makes `delay_ticks==0`, which the scan treats as `V_WAIT_FOREVER` — a finite wait becomes infinite. Absolute-tick compares also mis-order after the ~49-day wrap. Shared with the base scheduler, not new to this diff. |

---

## Verified correct (checked, not defects)

- MPU region sizing/alignment/priority and RASR AP/XN bits; region covers
  `[base, base+size)` matching `v_access_ok`; guard(7) > user(4) > SRAM(0)
  priority ordering.
- The `CONTROL.nPRIV` flip and its restore on exception return — idle task is
  privileged by design, so no unprivileged-runs-privileged window; SPSEL-write in
  handler mode is architecturally a no-op (PSP preserved).
- The privileged-only raw-handle IPC gate (`v_syscall_privileged_only`) — blocks
  `SYS_sem_give/take`, `SYS_mutex_lock/unlock` for unprivileged callers; all
  pointer syscalls route through a validator except the three above.
- `v_access_ok` / `v_strnlen_user` bounds and wrap handling (one-past-end, `len`
  wrap, null task/block) — well covered by `tests/test_uaccess.c`.
- `v_free`: range-checks before dereferencing; foreign/double-free rejected.
  `malloc`/`calloc` multiply-overflow guards present. Free-list forward/backward
  coalescing and `heap_brk` lowering trace correctly.
- The prior `v_memalign` leading-remainder underflow fix (commit `ae256da`) is
  correct (the `#8` reservation gap is a *separate* issue).
- fd-table type/bounds/double-close; `v_fd_alloc` skips in-use + preopened 0/1/2
  (no aliasing).
- `v_wait` arm/disarm invariant (arm ⇒ negative return ⇒ caller disarms); reported
  index is the correct `fds[]` index; timeout path delivers `-1` (not a
  misreadable `0`).
- gcov emitter↔decoder framing: five markers match exactly, magic `b"adcg"` is
  correct little-endian `0x67636461`, base64 alphabet excludes `@`, line buffer
  can't overflow. `gcov_sections.ld` FLASH geometry matches the nucleo_f401re
  target; `KEEP()` prevents GC.

---

## Suggested fix order

1. **#1, #2, #3** together — small, localized boundary fixes; unblock the flip.
2. **#4, #5, #6** together — one root cause: task teardown must close fds, unlink
   from wait/observer lists, and release held mutexes.
3. **#8, #9** — allocator hardening (host-latent, but the host suite is the test
   oracle).
4. **#7, #10–#14** — correctness/robustness cleanups.
