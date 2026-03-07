# VAiOS Changelog

All significant bug fixes and improvements are documented here.
Each file covers one discrete issue — root cause, fix, and lesson learned.

## Index

| File                                                                                           | Summary                                                                                                                 | Severity     | Date       |
| ---------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | ------------ | ---------- |
| [fix-task-exit-stack-overflow.md](fix-task-exit-stack-overflow.md)                             | `task_exit()` called `v_log()`, overflowing small-stack tasks and corrupting heap headers — causing HardFault           | **Critical** | 2026-03-05 |
| [fix-ipc-semaphore-timeout.md](fix-ipc-semaphore-timeout.md)                                   | `semaphore_take_common()` stored raw duration in `delay_ticks` instead of absolute deadline — caused immediate timeouts | **High**     | 2026-03-05 |
| [fix-ipc-semaphore-give-double-count.md](fix-ipc-semaphore-give-double-count.md)               | `semaphore_give_common()` incremented count AND woke a waiter — double-spending the token                               | **Medium**   | 2026-03-05 |
| [fix-heap-block-alignment.md](fix-heap-block-alignment.md)                                     | `Heap_Mem_Block` was 12 bytes, causing unaligned heap allocations — violated 8-byte FPU stack requirement               | **High**     | 2026-03-05 |
| [fix-idle-task-stack-size.md](fix-idle-task-stack-size.md)                                     | Idle task stack was 256 bytes — too small for `v_log` + GC call chain (needs ≥ 1 KB)                                    | **High**     | 2026-03-05 |
| [improve-hardfault-handler-diagnostics.md](improve-hardfault-handler-diagnostics.md)           | Added CFSR, HFSR, MMFAR, BFAR register dump to HardFault handler for faster root-cause identification                   | Improvement  | 2026-03-05 |
| [fix-scheduler-get-next-task-ordering.md](fix-scheduler-get-next-task-ordering.md)             | Scheduler re-enqueued `current_task` after picking next task — single-task yield fell through to `idle_task` spuriously | **Medium**   | 2026-03-05 |
| [improve-memory-protection-and-panic-system.md](improve-memory-protection-and-panic-system.md) | Hardened memory safety via proactive stack monitoring (PSP), heap watermarks, and stack-safe panic reporter             | Improvement  | 2026-03-07 |

## Final Benchmark Results

After all fixes, the VAiOS benchmark suite achieves:

```
TOTAL: 14 PASS  0 FAIL  0 TIMEOUT  0 SKIP
*** ALL BENCHMARKS PASSED ***
```

| Benchmark                         | Result | Duration               |
| --------------------------------- | ------ | ---------------------- |
| FPU: sinf throughput              | PASS   | 1 tick, 100 000 ops/s  |
| FPU: sqrtf throughput             | PASS   | 1 tick, 100 000 ops/s  |
| FPU: multi-task context-save      | PASS   | 93 ticks, 1 075 ops/s  |
| DMA: M2M 64-byte transfer         | PASS   | 0 ticks, 64 000 ops/s  |
| DMA: M2M 4096-byte transfer       | PASS   | 1 tick, 4 000 ops/s    |
| DMA: concurrent streams           | PASS   | 0 ticks                |
| TASK: context switch rate         | PASS   | 221 ticks, 361 sw/s    |
| TASK: priority preemption         | PASS   | 307 ticks              |
| TASK: delay accuracy              | PASS   | 101 ticks              |
| IPC: semaphore ping-pong          | PASS   | 154 ticks, 6 493 ops/s |
| IPC: mutex shared counter         | PASS   | 261 ticks              |
| MEM: alloc/free throughput        | PASS   | 13 438 ticks, 44 ops/s |
| MEM: fragmentation resilience     | PASS   | 0 ticks                |
| STRESS: all subsystems concurrent | PASS   | 3 250 ticks            |
