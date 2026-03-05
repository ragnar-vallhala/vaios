# Fix: IPC Semaphore Take Timeout — Wrong Deadline Format

**Date:** 2026-03-05  
**Severity:** High — caused all IPC benchmarks to fail (semaphore ping-pong and mutex)  
**Files Modified:** `kernel/ipc.c`

## Problem

`semaphore_take_common()` stored the raw timeout duration directly into
`task->delay_ticks`:

```c
// BEFORE (broken)
current->delay_ticks = ticks_to_wait;  // e.g. 2000 (raw duration)
```

However, `wake_up_delayed_tasks()` in `task.c` uses `delay_ticks` as an
**absolute tick deadline**:

```c
if (task->delay_ticks < v_get_ticks()) {
    // wake the task
}
```

By the time the IPC benchmarks ran (~tick 7000+), a stored value of `2000`
was already less than the current tick count. This meant **every blocked
semaphore take would immediately time out** on the very next tick, unblocking
with an error before the paired task had any chance to `give` the semaphore.

### Symptom

```
[FAIL] IPC: semaphore ping-pong  (154 ticks, 6493 ops/s)
[FAIL] IPC: mutex shared counter  (261 ticks, 0 ops/s)
```

`ping_count` and `pong_count` were always much less than `SEM_PING_ITERS`
because the waiter returned immediately without receiving a valid signal.

## Fix

Store the **absolute deadline** instead of the raw duration:

```c
// AFTER (fixed)
// Store ABSOLUTE deadline so wake_up_delayed_tasks() works correctly
current->delay_ticks = v_get_ticks() + ticks_to_wait;
```

Added `#include "utils.h"` to `ipc.c` to expose the `v_get_ticks()` declaration.

## Result

```
[PASS] IPC: semaphore ping-pong  (154 ticks, 6493 ops/s)
[PASS] IPC: mutex shared counter  (261 ticks, 0 ops/s)
```

## Lesson

Any kernel component that uses `delay_ticks` for timeout tracking must store
the **absolute wakeup time** (`now + duration`), not the raw duration. This
convention must be documented at the `TCB` struct definition.
