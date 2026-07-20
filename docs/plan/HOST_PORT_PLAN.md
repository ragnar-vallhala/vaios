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

MPU, SDIO, FPU, and the cycle counter have no host analogue and are no-ops (or a
`clock_gettime`).

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

- `init_task_stack` carves a `ucontext_t` from the base of the task's block,
  points its stack at the rest, and `makecontext`s a shared trampoline;
  `task->sp` (never a raw SP on host) carries the context pointer.
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
ucontext-sized stacks, watermark off, SVC off).

## Caveats (worth knowing before relying on it)

- **Stack sizes are large.** A ucontext stack must clear `MINSIGSTKSZ` plus the
  `ucontext_t`, so `VAIOS_ARCH_MIN_STACK` is 32 KB on host and the idle stack /
  heap are sized up in the defconfig. Real overflow detection is off (host stacks
  are ucontext-managed).
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
