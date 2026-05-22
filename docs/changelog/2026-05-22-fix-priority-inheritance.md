# Fix: Priority Inheritance — Priority-Ordered Wait Queues and Transitive Chains

**Date:** 2026-05-22
**Severity:** High — priority inversion at the semaphore boundary and across chained mutexes
**Files Modified:** `kernel/ipc.c`, `include/ipc.h`, `include/task.h`, `kernel/task.c`, `include/vaios_config_default.h`

## Problem

Two distinct inversion bugs:

1. **FIFO wait queues.** `wait_q_enqueue()` appended unconditionally. A
   high-priority control task blocking on a semaphore behind a slow
   low-priority task was woken FIFO — it waited for the logger.
2. **One-shot, incorrect priority inheritance.** `v_mutex_lock()` boosted only
   the direct mutex owner. If that owner was itself blocked on a second mutex,
   the deeper owner was never boosted — classic transitive inversion.
   `v_mutex_unlock()` then unconditionally dropped the releaser to
   `base_priority`, which was wrong whenever it still held another mutex with
   a high-priority waiter.

## Fix

- `wait_q_enqueue()` inserts by priority (most urgent at the head, ties FIFO);
  `wait_q_dequeue()` still pops the head O(1), so the slot always goes to the
  most urgent waiter.
- `TCB` gained `wait_mutex` (the mutex a task is blocking to acquire) and
  `held_mutexes` (intrusive list head); `rmutex_t` gained `next_held`.
- `v_mutex_lock()` walks the ownership chain `owner → wait_mutex → owner`,
  boosting every outranked owner, capped at `MAX_PI_DEPTH` (new config,
  default 4) which panics on a deeper chain — a likely dependency cycle.
- `v_mutex_unlock()` recomputes the releaser's priority as
  `max(base_priority, top waiter across all still-held mutexes)`.

## Result

A high-priority task contending a semaphore or a chain of mutexes is no longer
delayed by lower-priority work. Verified on hardware: the low-priority mutex
owner is boosted, preempts the mid-priority task, releases, and the high task
acquires.

## Lesson

Priority inheritance is only correct if it is **transitive** (follow the whole
blocking chain) and if release **recomputes** the floor from every mutex still
held — not a blind drop to base. A priority-ordered wait queue is the
prerequisite that makes the boost actually change who runs next.

## Known limitation

Boosting a task already queued on another mutex does not re-sort that wait
queue. Single-waiter chains (the common case) resolve correctly; multiple
waiters on one mutex within a chain keep their original order.
