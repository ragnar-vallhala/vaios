# Fix: Idle Task Stack Size Too Small

**Date:** 2026-03-05  
**Severity:** High — idle task stack overflow caused HardFault during garbage collection  
**Files Modified:** `include/config.h`

## Problem

The idle task was created with only **256 bytes** of stack. The idle task is
responsible for:
- Flushing the log double-buffer (`v_log_flush()` in a tight loop)
- Running the garbage collector (`v_free(task->mem_block)` + `v_free(task)`)

Both `v_log_flush()` and `v_log()` (called from the GC log message) allocate
large local buffers. `v_log` alone pushes ~200 bytes onto the stack. Combined
with the function call overhead, the 256-byte idle stack was reliably overflowing
during GC runs, corrupting the heap blocks immediately below the idle task's
stack region.

## Fix

Increased `IDLE_TASK_STACK_SIZE` in `config.h`:

```c
// BEFORE
#define IDLE_TASK_STACK_SIZE 256

// AFTER
#define IDLE_TASK_STACK_SIZE 2048
```

2 KB provides comfortable headroom for the log flush and GC operations,
including nested function calls and their local variables.

## Result

Idle task garbage collection runs without stack overflow. Memory is freed
correctly for all terminated tasks throughout the benchmark suite.

## Lesson

The idle task stack size should be sized for the **deepest call chain** it
executes, not for the task body alone. Log-intensive RTOS idle hooks need
at least 1-2 KB.
