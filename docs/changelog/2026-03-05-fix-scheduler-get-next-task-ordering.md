# Fix: Scheduler `get_next_task` — Current Task Re-enqueued Before Priority Selection

**Date:** 2026-03-05  
**Severity:** Medium — caused spurious idle task execution when a single task yielded  
**Files Modified:** `kernel/task.c`

## Problem

The PendSV context switch handler pops the `current_task` off the `ready_lists`
while it is running (i.e. the running task is never in the ready list during
execution). When `get_next_task()` was called, the old logic was:

```c
// BEFORE (broken ordering)
TCB *t = get_highest_priority_task(); // current_task NOT in list yet
if (t) {
    remove_from_ready_list(t);
    add_to_ready_list(current_task);  // re-add current_task AFTER picking t
    current_task = t;
} else {
    // current_task->status is TASK_RUNNING, not captured yet
    current_task = idle_task;         // wrong fallback!
}
```

**The problem:** If `current_task` was the only runnable task and called
`task_yield()`, `get_highest_priority_task()` would see an empty list (because
`current_task` hadn't been re-added yet) and incorrectly fall through to the
`idle_task`. The idle task would then run needlessly and could begin garbage
collecting tasks prematurely.

## Fix

Re-enqueue `current_task` into `ready_lists` **before** calling
`get_highest_priority_task()`:

```c
// AFTER (fixed ordering)
if (current_task && current_task->status == TASK_RUNNING) {
    add_to_ready_list(current_task); // put current back FIRST
}

TCB *t = get_highest_priority_task(); // now sees current_task if it's the only one
if (t) {
    remove_from_ready_list(t);
    current_task = t;
} else {
    if (current_task->status != TASK_RUNNING) {
        current_task = idle_task;    // only fall back if truly no runnable task
    }
}
```

## Result

Single-task yield correctly re-schedules the same task instead of switching
to idle. Priority preemption and round-robin scheduling remain correct.

## Lesson

The scheduler must restore `current_task` to the ready list before computing
the next highest-priority task to run. The "running" state should be treated
as logically equivalent to "ready" during the scheduling decision.
