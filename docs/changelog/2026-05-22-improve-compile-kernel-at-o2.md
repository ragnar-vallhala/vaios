# Improve: Compile vaios and NavHAL at -O2

**Date:** 2026-05-22
**Severity:** Improvement — 2–6× across the board; corrected an unfair benchmark baseline
**Files Modified:** `CMakeLists.txt`

## Problem

The kernel, portable layer, examples, and NavHAL all hardcoded `-O0 -g` in
their `CMakeLists.txt` — a hard `set(CMAKE_C_FLAGS ...)`, so `CMAKE_BUILD_TYPE`
could not override it. Every benchmark number was an unoptimized number, and
the earlier "vaios is 8–100× slower than FreeRTOS/Zephyr" conclusion was
measured with vaios at `-O0` against FreeRTOS at `-O2` and Zephyr at `-Os` —
an unfair comparison that masked where the real algorithmic gaps were.

## Fix

- vaios `CMAKE_C_FLAGS`: `-O0` → `-O2 -g` (covers kernel, portable, examples).
- NavHAL: rather than editing the submodule, its `-O0` is overridden per
  target from the vaios parent after `add_subdirectory`:

  ```cmake
  target_compile_options(hal    PRIVATE -O2)
  target_compile_options(common PRIVATE -O2)
  ```

  gcc honours the last `-O` flag, so the trailing `-O2` wins over NavHAL's own
  `-O0`. The submodule (including the DWT cycle counter) is built optimized
  without being modified — the same principle as the externally-owned
  `navhal.config`.

## Result

A flat 2–6× speedup over the `-O0` build with no algorithmic change
(`ctx_switch_yield` 1404→697, `free_8B` 2874→651, `malloc_64B` 18033→3033,
`mutex_pi_basic` 833k→352k), and ~32% smaller `.text`. With a fair
`-O2`-vs-`-O2/-Os` comparison vaios is competitive — winning some metrics —
rather than 10–100× behind.

## Lesson

Always confirm the optimization level before drawing conclusions from a
benchmark. A cross-target comparison is meaningless unless every target is
built at a comparable optimization level.
