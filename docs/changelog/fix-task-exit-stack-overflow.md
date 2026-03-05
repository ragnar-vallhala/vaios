# Fix: Stack Overflow in `task_exit()` via `v_log()` Call

**Date:** 2026-03-05  
**Severity:** Critical — caused HardFault and heap corruption  
**Files Modified:** `kernel/task.c`

## Problem

`task_exit()` called `v_log()` before marking the task as terminated:

```c
// BEFORE (broken)
void task_exit(void) {
  uint32_t tid = current_task->task_id;
  v_log(LOG_DEBUG, "[TASK] Task %u Exiting", tid); // <-- stack overflow!
  ...
}
```

`v_log()` allocates a local buffer of `char final_msg[LOG_MSG_MAX_LEN + 64]`
(192 bytes) on the calling task's stack. For small tasks (e.g., `ctx_switch_task`
with a 512-byte stack), by the time `task_exit` was reached the usable stack
space was already consumed by the Cortex-M4 exception frame (32 bytes) and
PendSV register save (36 bytes), leaving insufficient room for `v_log`'s frame.

The stack pointer underflowed below the task's allocated stack region, writing
into the adjacent `Heap_Mem_Block` header block in the heap, corrupting its
`magic_number` field. This caused subsequent `v_free()` calls to report
`"Heap corruption detected while walking during free"` and ultimately a
HardFault with `PC: 0x00000000` and `CFSR: 0x00020000` (INVSTATE fault).

## Stack Layout at Crash Time

```
0x20004c20  <-- task 6 stack base (bottom of stack region)
0x20004c10  <-- Heap_Mem_Block header for task 6 stack ← CORRUPTED HERE
0x20004be0  <-- TCB for task 6 ← ALSO CORRUPTED
```

Stack grew downward past `0x20004c20` into the heap header and TCB.

## Fix

Removed the `v_log` call from `task_exit()`. The idle task garbage collector
already logs `"Garbage Collector freeing task N"` which is sufficient for
debugging.

```c
// AFTER (fixed)
__attribute__((noreturn)) void task_exit(void) {
  ENTER_CRITICAL();
  current_task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, current_task);
  EXIT_CRITICAL();

  task_yield();
  while (1);
}
```

## Result

No more HardFault during context switching benchmark. All 14 benchmark
sub-tests pass cleanly.

## Lesson

Any function called from within a task at exit time must not significantly
expand the stack. Tasks with small stacks (< 1 KB) are especially vulnerable.
Consider enforcing a minimum task stack size that accounts for the deepest
call chain reachable from `task_exit`.
