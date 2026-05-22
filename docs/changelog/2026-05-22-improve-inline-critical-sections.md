# Improve: Inline Critical Sections and Allocator Helpers

**Date:** 2026-05-22
**Severity:** Improvement — removed fixed per-call overhead from every kernel primitive
**Files Modified:** `portable/cortex-m4/port.h`, `portable/cortex-m4/port.c`, `kernel/memory.c`

## Problem

Disassembly of the optimized build showed `v_malloc` made 4 and `v_free` made
5 non-inlined `bl` calls on their hot paths — `v_enter_critical`,
`v_exit_critical`, `fl_remove`, `fl_insert`. `v_enter_critical`/
`v_exit_critical` lived in `port.c` (a separate translation unit) and so could
not be inlined; every critical section in the kernel — malloc, free, every
semaphore and mutex operation, the scheduler — paid a `bl` round-trip with
prologue/epilogue. FreeRTOS `heap_4` is a single flat function with an inline
critical-section macro; that fixed per-call overhead was the residual
malloc/free gap.

## Fix

- `v_enter_critical` / `v_exit_critical` moved into `port.h` as
  `always_inline static inline`; `critical_nesting` stays in `port.c` as an
  extern. Every `ENTER_CRITICAL`/`EXIT_CRITICAL` site now emits a bare
  `msr basepri` instead of a call.
- `size_class`, `fl_insert`, `fl_remove` in `memory.c` marked `always_inline`.

## Result

`v_malloc` and `v_free` have zero hot-path calls — only the cold
`v_log`/`v_panic` error paths remain. The inline critical sections also sped
up `sem_give_take` and `mutex_lock_unlock` measurably, since they touch the
same primitive. Cost: +1.7 KB `.text` for the inline expansion at every
critical-section site.

## Lesson

A critical-section primitive is called thousands of times across the kernel; if
it is a cross-translation-unit function it cannot be inlined and every call is
overhead. Define it inline in a header — the small code-size cost buys a
speedup on every kernel primitive at once.
