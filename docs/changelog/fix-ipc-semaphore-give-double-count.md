# Fix: Semaphore Give — Double-Counting of Available Slots

**Date:** 2026-03-05  
**Severity:** Medium — caused semaphore count inconsistency and potential deadlocks  
**Files Modified:** `kernel/ipc.c`

## Problem

The original `semaphore_give_common()` always incremented the semaphore count
first, then also dequeued a waiting task and put it back on the ready list.
This resulted in a **double-spend**: the count went up by 1 *and* a waiting task
was unblocked as if it had consumed a token — but it hadn't actually decremented
the count.

```c
// BEFORE (broken)
atomic_inc(&s->count);              // count goes 0 → 1

TCB *to_unblock = wait_q_dequeue(s);
if (to_unblock) {
    add_to_ready_list(to_unblock); // task wakes up, but count is still 1!
}
```

If the unblocked task then tried to `take` again (or another task did), it
would find `count == 1` and succeed without actually receiving a proper signal —
corrupting the ping-pong protocol and making the mutex non-exclusive.

## Fix

Check for a waiting task **first**. If one exists, hand the slot **directly**
to that task without incrementing the count. Only increment the count if no
task is waiting.

```c
// AFTER (fixed)
TCB *to_unblock = wait_q_dequeue(s);
if (to_unblock) {
    // Slot goes directly to the waiting task — do NOT increment count
    to_unblock->status = TASK_READY;
    add_to_ready_list(to_unblock);
    EXIT_CRITICAL();
    return VA_PASS;
}

// No waiting task — place the token in the semaphore
if (atomic_get(&s->count) >= atomic_get(&s->limit)) {
    EXIT_CRITICAL();
    return VA_FAIL; // full
}
atomic_inc(&s->count);
```

## Result

Semaphore count remains consistent. The ping-pong and mutex benchmarks
both achieve the correct final trip counts and counter values.

## Lesson

When handing off a semaphore token to a blocked waiter, the token should
be transferred directly rather than incrementing the count and then
decrementing it again in the waiter — this avoids a transient window where
a third task could steal the token.
