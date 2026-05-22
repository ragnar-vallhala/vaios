# Fix: Semaphore Wait Queue Corruption and Recursive-Mutex Critical-Section Leak

**Date:** 2026-05-22
**Severity:** Critical — deadlocked any IPC pipeline; froze the kernel on recursive-mutex release
**Files Modified:** `include/task.h`, `kernel/task.c`, `kernel/ipc.c`

## Problem

**Bug 1 — wait queue / blocked list shared a pointer.** A timed
`v_semaphore_take()` puts the task on a semaphore wait queue *and* on
`blocked_list` at the same time, but `TCB` had a single `next`/`prev` pair
used by both. `semaphore_take_common()` did `wait_q_enqueue()` (links via
`->next`) and then `add_to_blocked_list()` → `enqueue_task()`, which clears
`->next`/`->prev` — destroying the wait-queue linkage just built. With 2+
waiters on one semaphore, or a non-empty `blocked_list`, a wait-queue walk
wandered into `blocked_list`: a waiter was lost or a cycle formed, deadlocking
pipelines that hand off between tasks via semaphores.

**Bug 2 — leaked critical section.** `v_mutex_unlock_recursive()` took the
`recursion_count == 0` branch and `return`ed `v_mutex_unlock(...)` without a
matching `EXIT_CRITICAL()`. `critical_nesting` never returned to zero, so
BASEPRI was never cleared and the kernel froze on the first recursive-mutex
release.

## Fix

- `TCB` gained a dedicated `wait_next` link for the semaphore wait queue,
  independent of the scheduler `next`/`prev`. `wait_q_enqueue`/`wait_q_dequeue`
  and the timeout-eject walk thread through `wait_next`. A task can now sit on
  `blocked_list` and a wait queue simultaneously without collision.
- `v_mutex_unlock_recursive()` calls `EXIT_CRITICAL()` before delegating to
  `v_mutex_unlock()` (which runs its own critical section).

## Result

Verified on hardware with two tasks contending one binary semaphore: both are
held on the wait queue and ejected cleanly on timeout across repeated
iterations. Recursive mutex lock/unlock no longer freezes the system.

## Lesson

A node that belongs to two lists at once needs two sets of links — never
overload one. And every `ENTER_CRITICAL()` needs a matching `EXIT_CRITICAL()`
on **every** return path; an early `return` that skips it leaks the lock and,
with nesting-counted BASEPRI, hangs the whole kernel.
