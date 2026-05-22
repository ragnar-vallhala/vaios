# Improve: Compile-Time Gate for Kernel Hot-Path Logging

**Date:** 2026-05-21
**Severity:** Improvement — removed orders-of-magnitude latency inflation from malloc/sem/ctx-switch paths
**Files Modified:** `include/utils.h`, `include/vaios_config_default.h`, `kernel/memory.c`, `kernel/task.c`

## Problem

Every kernel hot path called `v_log()` directly — `v_malloc`/`v_free` logged on
each allocation, `task_create`/`task_exit` on each task event. Even when a
message was below `MIN_LOG_LEVEL`, `v_log()` still paid its prologue: a PSP
read, a stack-overflow check, a `module_allowed()` string scan, and vararg
promotion. The benchmark `bench_quiet_flag` only silenced `bench_info()`
printing — not these kernel `v_log()` calls — so the kernel was effectively
printf-ing inside every measured operation.

## Fix

Introduced `V_KLOG(level, ...)` in `utils.h`, gated by a compile-time
`VAIOS_KERNEL_LOG_LEVEL` (default `LOG_WARN`):

```c
#define V_KLOG(level, ...) \
  do { if ((int)(level) >= (int)(VAIOS_KERNEL_LOG_LEVEL)) \
         v_log((level), __VA_ARGS__); } while (0)
```

Every kernel hot-path `v_log()` in `memory.c`/`task.c` was replaced with
`V_KLOG()`. Below the configured level the whole call site — format string
included — is dead-code-eliminated by the compiler.

## Result

The example application's `.text` dropped ~22% immediately, purely from
`--gc-sections` reclaiming the now-unreferenced `LOG_DEBUG` format strings.
A flight build can set `VAIOS_KERNEL_LOG_LEVEL=LOG_ERROR` to drop everything
but panics.

## Lesson

A logging call is never free, even when the message is filtered: the argument
marshalling and the function prologue still run. Gate hot-path logging at the
call site, at compile time — not inside the logger.
