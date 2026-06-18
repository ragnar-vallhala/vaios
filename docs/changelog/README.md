# VaiOS Changelog

All significant bug fixes and improvements are documented here.
Each file covers one discrete issue — root cause, fix, and lesson learned.
Files are prefixed with their date (`YYYY-MM-DD-`) so the directory sorts
chronologically.

## Index

| File | Summary | Severity | Date |
| ---- | ------- | -------- | ---- |
| [2026-05-22-improve-compile-kernel-at-o2.md](2026-05-22-improve-compile-kernel-at-o2.md) | Built vaios + NavHAL at `-O2` — flat 2–6× speedup; the prior "8–100× slower" result was an unfair `-O0`-vs-optimized comparison | Improvement | 2026-05-22 |
| [2026-05-22-improve-scheduler-hot-path.md](2026-05-22-improve-scheduler-hot-path.md) | SysTick-driven wakeup (killed ~983 µs loop jitter); call-free, scan-free context switch with O(1) ready lists and `clz` priority pick | Improvement | 2026-05-22 |
| [2026-05-22-improve-allocator-segregated-free-lists.md](2026-05-22-improve-allocator-segregated-free-lists.md) | O(1) `v_free` via a `prev` pointer, then segregated free lists for `v_malloc` — closed an ~11× small-malloc deficit | Improvement | 2026-05-22 |
| [2026-05-22-improve-inline-critical-sections.md](2026-05-22-improve-inline-critical-sections.md) | Inlined `v_enter/exit_critical` and allocator helpers — removed `bl` round-trips from every kernel primitive's hot path | Improvement | 2026-05-22 |
| [2026-05-22-fix-priority-inheritance.md](2026-05-22-fix-priority-inheritance.md) | Priority-ordered semaphore wait queues; transitive PI chain walk; release recomputes the priority floor from all held mutexes | **High** | 2026-05-22 |
| [2026-05-22-fix-semaphore-waitqueue-corruption.md](2026-05-22-fix-semaphore-waitqueue-corruption.md) | Wait queue and `blocked_list` shared `TCB->next` — corrupted under contention; recursive-mutex unlock leaked a critical section | **Critical** | 2026-05-22 |
| [2026-05-22-improve-modular-build-and-navhal-integration.md](2026-05-22-improve-modular-build-and-navhal-integration.md) | Opt-out CMake modules (~32% smaller flight `.text`); NavHAL Kconfig owned by the vaios repo so the submodule stays pristine | Improvement | 2026-05-22 |
| [2026-05-21-fix-basepri-critical-sections.md](2026-05-21-fix-basepri-critical-sections.md) | Critical sections used `cpsid i`, masking motor PWM / IMU IRQs — switched to BASEPRI so hard-real-time IRQs stay unmaskable | **Critical** | 2026-05-21 |
| [2026-05-21-improve-strip-hot-path-logging.md](2026-05-21-improve-strip-hot-path-logging.md) | `V_KLOG` compile-time gate removes `v_log()` from malloc/sem/ctx-switch hot paths — ~22% `.text` reclaimed | Improvement | 2026-05-21 |
| [2026-03-09-improve-timer-init-and-kernel-security.md](2026-03-09-improve-timer-init-and-kernel-security.md) | Added `timer_init_freq()` API and secured `task.c` primitives with critical sections to prevent 1000Hz interrupt races | **Critical** | 2026-03-09 |
| [2026-03-07-improve-portability-and-dma-logging.md](2026-03-07-improve-portability-and-dma-logging.md) | Refactored architecture-specific ASM into portable layer and integrated DMA-backed logging | Improvement | 2026-03-07 |
| [2026-03-07-improve-memory-protection-and-panic-system.md](2026-03-07-improve-memory-protection-and-panic-system.md) | Hardened memory safety via proactive stack monitoring (PSP), heap watermarks, and stack-safe panic reporter | Improvement | 2026-03-07 |
| [2026-03-05-fix-scheduler-get-next-task-ordering.md](2026-03-05-fix-scheduler-get-next-task-ordering.md) | Scheduler re-enqueued `current_task` after picking next task — single-task yield fell through to `idle_task` spuriously | **Medium** | 2026-03-05 |
| [2026-03-05-improve-hardfault-handler-diagnostics.md](2026-03-05-improve-hardfault-handler-diagnostics.md) | Added CFSR, HFSR, MMFAR, BFAR register dump to HardFault handler for faster root-cause identification | Improvement | 2026-03-05 |
| [2026-03-05-fix-idle-task-stack-size.md](2026-03-05-fix-idle-task-stack-size.md) | Idle task stack was 256 bytes — too small for `v_log` + GC call chain (needs ≥ 1 KB) | **High** | 2026-03-05 |
| [2026-03-05-fix-heap-block-alignment.md](2026-03-05-fix-heap-block-alignment.md) | `Heap_Mem_Block` was 12 bytes, causing unaligned heap allocations — violated 8-byte FPU stack requirement | **High** | 2026-03-05 |
| [2026-03-05-fix-ipc-semaphore-give-double-count.md](2026-03-05-fix-ipc-semaphore-give-double-count.md) | `semaphore_give_common()` incremented count AND woke a waiter — double-spending the token | **Medium** | 2026-03-05 |
| [2026-03-05-fix-ipc-semaphore-timeout.md](2026-03-05-fix-ipc-semaphore-timeout.md) | `semaphore_take_common()` stored raw duration in `delay_ticks` instead of absolute deadline — caused immediate timeouts | **High** | 2026-03-05 |
| [2026-03-05-fix-task-exit-stack-overflow.md](2026-03-05-fix-task-exit-stack-overflow.md) | `task_exit()` called `v_log()`, overflowing small-stack tasks and corrupting heap headers — causing HardFault | **Critical** | 2026-03-05 |

---

## 2026-05 Flight-Profile Performance Campaign

A single campaign drove vaios toward flight-controller parity with
FreeRTOS and Zephyr, tracked in
[`../benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md`](../benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md)
and the phased plan at
[`../benchmark/implementation_plan/`](../benchmark/implementation_plan/PHASED_IMPLEMENTATION_PLAN.md).
The nine 2026-05 entries above are its discrete changes. Every change was
verified on STM32F401RE hardware.

### The single biggest finding

The original "vaios is 8–100× slower than FreeRTOS/Zephyr" conclusion was an
**unfair-build artifact**: vaios was being benchmarked at `-O0` while FreeRTOS
ran at `-O2` and Zephyr at `-Os`. Rebuilding vaios (and NavHAL) at `-O2` gave a
flat 2–6× speedup with no algorithmic change and corrected the comparison. The
remaining real gaps were then genuinely algorithmic and were fixed one by one.

### Results

Measured on STM32F401RE @ 84 MHz. All three RTOSes built optimized
(vaios + FreeRTOS `-O2`, Zephyr `-Os`). Figures are CPU cycles, lower is
better, unless marked. `—` = exact rival figure not recorded (vaios was
fastest).

| Metric | vaios | FreeRTOS | Zephyr | Verdict |
| ------ | ----: | -------: | -----: | ------- |
| `malloc_8B` | 299 | 271 | 369 | parity |
| `malloc_64B` | 267 | 271 | 369 | **vaios fastest** |
| `malloc_512B` | 240 | — | — | **vaios fastest** |
| `malloc_4KB` | 227 | — | — | **vaios fastest** |
| `malloc_fragmented` | 268 | 1122 | 459 | **vaios fastest** |
| `malloc_pattern_random` (ops/s ↑) | 326 200 | 264 500 | 206 800 | **vaios fastest** |
| `free_8B` | 238 | 272 | 380 | **vaios fastest** |
| `sem_give_take` | 246 | 264 | 214 | beats FreeRTOS |
| `sem_pingpong_2task` | 2110 | 2829 | 4446 | **vaios fastest** |
| `mutex_lock_unlock` | 395 | 333 | 266 | close |
| `task_wake_latency` | 931 | 548 | 1196 | beats Zephyr |
| `ctx_switch_yield` | 667 → ↓ | 164 | 304 | behind (see note) |
| `control_loop_1khz_jitter` (µs) | ~0 | 0 | 0 | parity |
| `delay_accuracy_5ms` (µs) | 4999 | ~5000 | 6000 | beats Zephyr |
| `flash` (`.text`, KB) | 56.1 | 24.8 | 41.1 | — |

Notes:

- **Allocator** went from an ~11× small-malloc deficit to fastest-of-three on
  nearly every malloc/free metric (segregated free lists + O(1) coalescing
  free + helper inlining).
- **`ctx_switch_yield`** is shown at 667 — the snapshot after the allocator and
  inlining round. Subsequent scheduler work landed afterward (per-switch wake
  scan removed, O(1) ready lists, call-free `get_next_task`), lowering it
  further; the authoritative final figure is tracked with the benchmark
  harness in [`../benchmark/`](../benchmark/). The residual gap to FreeRTOS's
  164 is the scheduler *model* — FreeRTOS rotates an index within circular
  ready lists rather than removing/re-adding the running task.
- **`flash`** can be cut well below 40 KB for a flight build by disabling the
  optional modules (`terminal`, `vfs`, `semihosting`, `fifo`) via the new
  CMake module config — see
  [the modular-build entry](2026-05-22-improve-modular-build-and-navhal-integration.md).

---

## 2026-03 Benchmark Suite (historical)

The earlier March campaign used a pass/fail suite (tick-duration based, not
cross-RTOS cycle counts). Kept here for the record:

```
TOTAL: 14 PASS  0 FAIL  0 TIMEOUT  0 SKIP
*** ALL BENCHMARKS PASSED ***
```

| Benchmark | Result | Duration |
| --------- | ------ | -------- |
| FPU: sinf throughput | PASS | 1 tick, 100 000 ops/s |
| FPU: sqrtf throughput | PASS | 1 tick, 100 000 ops/s |
| FPU: multi-task context-save | PASS | 93 ticks, 1 075 ops/s |
| DMA: M2M 64-byte transfer | PASS | 0 ticks, 64 000 ops/s |
| DMA: M2M 4096-byte transfer | PASS | 1 tick, 4 000 ops/s |
| DMA: concurrent streams | PASS | 0 ticks |
| TASK: context switch rate | PASS | 221 ticks, 361 sw/s |
| TASK: priority preemption | PASS | 307 ticks |
| TASK: delay accuracy | PASS | 101 ticks |
| IPC: semaphore ping-pong | PASS | 154 ticks, 6 493 ops/s |
| IPC: mutex shared counter | PASS | 261 ticks |
| MEM: alloc/free throughput | PASS | 13 438 ticks, 44 ops/s |
| MEM: fragmentation resilience | PASS | 0 ticks |
| STRESS: all subsystems concurrent | PASS | 3 250 ticks |
