# vaios — Host (POSIX) Port

A `portable/host/` port that **runs the scheduler natively on the host** (x86
Linux), not just unit-tests its data structures. Selected with
`-DVAIOS_PORT=host`; produces a normal executable.

> **Status: IMPLEMENTED.** `cmake -S . -B build_host -DVAIOS_PORT=host
> -DNAVHAL=OFF -DEXAMPLES=ON -DVAIOS_EXAMPLE=HOST_DEMO && cmake --build
> build_host` builds `build_host/examples/main`, which runs two preemptively
> time-sliced tasks to completion. No regression: ARM firmware clean under
> `NAVHAL=OFF`/`ON`; host unit suite 246/246; portability tripwire green.

---

## Why this was tractable

The portability cleanup did the heavy lifting: the scheduler core is
architecture-neutral and reused **verbatim** — `get_next_task` / `set_next_task`
(`kernel/task.c`), `v_kernel_tick` (`kernel/utils.c`), `wake_up_delayed_tasks_isr`,
`scheduler_init`, the IPC and memory layers. The host port only supplies the four
things that are silicon on Cortex-M.

| Subsystem | Cortex-M | Host |
|---|---|---|
| Context switch | PendSV asm saves r4-r11/PSP; `init_task_stack` builds a fake exception frame; `svc 0` launches the first task | **ucontext**: `makecontext` per task, `swapcontext` *is* the switch |
| Timer tick | SysTick IRQ → `v_kernel_tick` | POSIX interval timer → `SIGALRM` → `v_kernel_tick` |
| Critical section | `msr basepri` masks SysTick | `sigprocmask(SIGALRM)` — masks the tick, the exact analogue |
| Console | UART / semihosting | stdio |
| MPU | ARMv7-M MPU regions + MemManage fault | **software MPU**: `mprotect` guard pages + a `SIGSEGV`→fault translator |

SDIO, FPU, and the cycle counter have no host analogue and are no-ops (or a
`clock_gettime`).

## Software MPU

The kernel touches the MPU only through `v_port_mpu_*`, so the host models the
*peripheral's behavior* behind that facade instead of emulating the CPU:
`mprotect()` is the region access control, and a violating access raises
`SIGSEGV`, which `portable/host/port_mpu.c` translates into the same `v_panic`
path as the ARM `MemManage_Handler`. `VAIOS_ARCH_HOST` therefore `select`s
`VAIOS_ARCH_HAS_MPU`.

Implemented so far — **the per-task stack-overflow guard**: `init_task_stack`
`mmap`s each task's stack page-aligned with a `PROT_NONE` guard page just below
its base, so an overflow faults cleanly (the `STACK_OVERFLOW` example panics with
the offending task + address instead of silently corrupting memory). The fault
handler runs on a `sigaltstack` so it works even when the task stack is the thing
that overflowed.

Next slices (not yet): per-task RW regions for user separation (per-switch
`mprotect` + a privilege toggle at the syscall boundary, since the host has no CPU
privilege level), and then authentic `v_access_ok` validation. Those need kernel
objects in mprotect-able page-aligned memory and reckon with the 32-bit syscall
pointer ABI — see the deferred word-size item in `PORTABILITY_CLEANUP_PLAN.md`.
Hardware-only concerns beyond region control still belong on Renode/hardware.

## Decisions

- **ucontext, not pthreads.** One execution context is live at a time and
  `swapcontext` *is* the context switch — directly analogous to PendSV, and
  tasks run on vaios's own stacks (so the model matches the target). ucontext is
  POSIX-deprecated but universal on Linux/glibc; fine for a dev/sim port.
- **Preemptive, not cooperative.** A `SIGALRM` interval timer at
  `TICK_PERIOD_US` drives round-robin + delayed-task wakeups, exactly like the
  target SysTick. A CPU-bound task is preempted; it need not yield.

## The switch engine (`portable/host/port.c`)

The kernel funnels every switch through two seams — `v_port_trigger_pendsv()`
(request) and `set_next_task()` (pick next) — so the whole engine hangs off
those.

- `init_task_stack` allocates a `ucontext_t` + a large execution stack
  (separate from the task's small `mem_block`, so requested sizes stay
  on-target-normal) and `makecontext`s a shared trampoline; `task->sp` (never a
  raw SP on host) carries the allocation, and the GC frees it via
  `v_port_free_task_stack()`.
- `v_port_trigger_pendsv()` sets a pending flag — **deferred**, exactly like
  pending PendSV. The switch is performed at a safe point by `host_maybe_switch`:
  the outermost `EXIT_CRITICAL`, or the tick handler's exit. Never mid-critical,
  never mid-switch.
- `scheduler_start()` `swapcontext`s into the idle task; the first tick then
  preempts to the highest-ready task — the SysTick→PendSV chain.

### Signal-mask model (the subtle part)

`SIGALRM` is the interrupt. A running task keeps it unblocked (preemptible);
critical sections block it (== BASEPRI). A switch runs with it blocked and
`swapcontext` saves/restores each task's mask, so the block/unblock pair balances
across a switch. A freshly launched task explicitly unblocks it so it is
preemptible from its first instruction. `swapcontext` from inside the `SIGALRM`
handler is the standard, known-good ucontext preemption technique; the tick
period (1 ms) dwarfs the switch cost, so the small re-entrancy window on handler
exit is not reachable in practice.

## Build integration

`portable/host/arch.cmake` sets `PORT_IS_HOSTED TRUE`, a native (empty) toolchain
prefix, no arch flags, no startup source. The root `CMakeLists.txt` gates the
bare-metal-only bits on `NOT PORT_IS_HOSTED`: it leaves `CMAKE_SYSTEM_NAME` at
its native default (no cross), drops the FPU flags, and links a normal executable
against the system libc instead of the NavHAL board linker script + newlib specs.
`kernel/CMakeLists.txt` excludes `syscalls.c` on host (its newlib `_sbrk`/`_write`
stubs clash with glibc). The port seeds from `portable/host/defconfig` (host arch,
watermark off, SVC off).

## Caveats (worth knowing before relying on it)

- **Stacks: apps use normal on-target sizes.** A ucontext stack must clear
  `MINSIGSTKSZ` (kilobytes), but that is *decoupled* from the requested size:
  `init_task_stack` allocates the real ucontext stack separately (default 64 KB,
  or larger if a task asks for more), so `VAIOS_ARCH_MIN_STACK` is 128 B as on
  target and the same app code runs on both. The port allocation is released by
  the dead-task GC via `v_port_free_task_stack()` (a no-op on Cortex-M, which
  keeps its frame inside `mem_block`). With `VAIOS_MPU_STACK_GUARD` on, a
  `PROT_NONE` guard page below the stack gives real overflow detection (see
  Software MPU above); without it, an overflow is undetected as on a guard-less
  target.
- **Use `v_log`, not raw `printf`, from tasks.** Output should go through vaios's
  logger, which routes to the port console (`v_port_hw_console_*`) and serializes
  with a critical section — the `HOST_DEMO` example does this. Raw `printf`/stdio
  is *not* reentrant, and all tasks share one OS thread, so a tick landing mid-
  `printf` can let another task re-enter it (glibc's stdio lock is recursive-per-
  thread) and corrupt the output; if you must call it directly, wrap it in
  `ENTER_CRITICAL()` / `EXIT_CRITICAL()`.
- **Blocking host syscalls stall the scheduler.** `getchar()` and friends block
  the single execution thread. This port is for logic/dev, not host I/O
  concurrency.

## Not done (out of scope)

- Async stdin (`v_port_hw_console_rx_irq_init` is a no-op).
- `VAIOS_SYSCALL_SVC` on host — off in the defconfig; the public API calls
  straight through (no privilege boundary to trap). The host SVC *model* already
  exists in `tests/stubs/port_syscall.h` (`v_host_svc` → `v_syscall_dispatch`) if
  it is ever wanted.
- macOS/other POSIX — Linux/glibc only for now (ucontext + `setitimer`).
