# Improve: Scheduler Hot Path — IRQ-Driven Wakeup and Call-Free Context Switch

**Date:** 2026-05-22
**Severity:** Improvement — eliminated per-cycle jitter and leaned out every context switch
**Files Modified:** `kernel/task.c`, `kernel/utils.c`, `include/task.h`

## Problem

Three issues compounded in the scheduler core:

1. **Tick-driven wakeup was lazy.** `wake_up_delayed_tasks()` ran only inside
   `get_next_task()` (on PendSV). A task that called `task_delay()` slept until
   the next yield or tick — periodic tasks were systematically a full tick
   late (`control_loop_1khz_jitter` measured ~983 µs).
2. **Every context switch scanned lists.** `get_next_task()` called
   `wake_up_delayed_tasks()`, walking the whole `delayed_list` and
   `blocked_list` on every single switch.
3. **`get_next_task()` went through the general list API** —
   `add_to_ready_list → enqueue_task` (with an O(n) tail walk) and
   `remove_from_ready_list → remove_task` — four nested calls per switch — and
   `highest_ready_prio()` was an up-to-8-iteration bit scan.

## Fix

- `delayed_list` is kept sorted by deadline; `wake_up_delayed_tasks_isr()`
  drains due sleepers and ejects timed-out semaphore waiters **once per tick
  in `SysTick_Handler`** — the only place wakeups now happen.
- `get_next_task()` no longer scans any list (deadlines are tick-granular and
  SysTick covers them exactly).
- Ready lists gained `ready_tails[]`; O(1) `rl_append`/`rl_pop_head`/
  `rl_remove` helpers replace the tail-walking `enqueue_task`. `get_next_task()`
  does the surgery inline — no calls, no nested critical section.
- `highest_ready_prio()` uses `__builtin_clz` (one Cortex-M4 instruction).

## Result

`control_loop_1khz_jitter` collapsed from ~983 µs to ~0. `delay_accuracy_5ms`
gained back a full tick of overshoot. The context-switch path is now call-free
apart from `v_get_ticks()` and cold panic checks.

## Lesson

Timer-driven events belong in the timer ISR, not in the scheduler decision.
And the hottest function in the kernel should manipulate its own data
structures directly — every cross-function call on that path is pure overhead.
