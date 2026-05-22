# Improvement: Timer Initialization and Kernel Security Hardening

**Date:** 2026-03-09  
**Severity:** Critical / Safety Enhancement  
**Files Modified:** `extern/NavHAL/src/core/cortex-m4/timer/timer.c`, `extern/NavHAL/include/core/cortex-m4/timer.h`, `kernel/task.c`, `examples/block_wake_task.c`

## Problem

1.  **Manual Timer Calculation**: Previously, initializing a timer required manual calculation of Prescaler (PSC) and Auto-Reload (ARR) values. This was complex and error-prone, especially when trying to account for 16-bit vs 32-bit timer limits.
2.  **Race Conditions in Kernel**: Base task management primitives (`enqueue_task`, `remove_task`) and logical operations (`task_unblock`, `task_delay`) lacked critical section protection. High-frequency interrupts (e.g., a 1000Hz timer) could interrupt a task during list manipulation, leading to linked-list corruption and system hangs.
3.  **UART Saturation**: Attempting to log at 1000Hz on a 115200 baud serial port (~10KB/s) caused the system to stall in UART polling loops, as the log generation exceeded the physical bandwidth.

## Solution

Implemented a robust timer initialization API and hardened the kernel's core:

1.  **Automated Frequency Calculation**:
    - Added `timer_init_freq(timer, freq)`.
    - Implemented a searching algorithm for 16-bit timers to find the optimal `PSC`/`ARR` pair that minimizes frequency error.
    - The function returns the "rest" (clock cycles lost to integer division) to allow for precision compensation in user code.

2.  **Kernel Layer Security**:
    - Wrapped all base list primitives in `task.c` with `ENTER_CRITICAL()` and `EXIT_CRITICAL()`.
    - Secured logical operations (`task_unblock`, `task_block`, `wake_up_delayed_tasks`) to ensure atomicity.
    - Leveraged the nested critical section support in the port layer to allow safe recursive calls.

3.  **Logging Optimization**:
    - Identified UART bottlenecks and updated examples to use throttled logging (e.g., 10Hz log rate) when running high-frequency control loops.

## Result

The system is now robust against high-frequency asynchronous events and provides a much simpler API for hardware timing. Linked-list corruption is prevented at the primitive level, making the scheduler thread-safe.

## Validation Proof

### 1000Hz Loop Stability

The `block_wake_task` example now runs stably at 1000Hz (1ms period) with 10Hz throttled logging:

```text
[WARN 213004] 104
[WARN 213104] 104
[WARN 213204] 104
...
[WARN 218904] 104
```

(Note: The constant "104" indicates consistent timing performance).

## Lesson

Kernel primitives must always be atomic if they can be accessed from both task and interrupt contexts. At the HAL level, API convenience (like frequency-based init) significantly reduces the likelihood of configuration errors.
