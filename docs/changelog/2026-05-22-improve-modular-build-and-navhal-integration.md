# Improve: Modular Build Config and NavHAL Integration

**Date:** 2026-05-22
**Severity:** Improvement — smaller flight binaries; clean integration of the restructured NavHAL
**Files Modified:** `CMakeLists.txt`, `kernel/CMakeLists.txt`, `portable/CMakeLists.txt`, `include/vaios_config_default.h`, `kernel/vaios.c`, `kernel/utils.c`, `navhal.config` (new)

## Problem

Two build-system issues:

1. The kernel `GLOB`-compiled every module unconditionally, so flash always
   carried `terminal`, `vfs`, `semihosting`, and the SPSC/MPMC structures even
   when an application never used them.
2. NavHAL's M-series restructure broke the vaios build at several layers:
   relocated vendor headers (`family/*_reg.h`), a soft-float `libhal.a` vs
   vaios's hard-float ABI, Kconfig-gated drivers (FPU, SDIO, FatFs) not
   enabled, and a strong `SysTick_Handler` that collided with vaios's.

## Fix

**Modular config.** A CMake `option()` per optional module
(`VAIOS_MODULE_TERMINAL`, `_VFS`, `_SEMIHOSTING`, `_FIFO`). When OFF, the
module's `.c` is filtered out of the build entirely and the option is passed
as a `0`/`1` compile definition so C code can `#if` out init calls.
Dependency resolution (Kconfig-`select` style) keeps `semihosting` enabled when
`terminal` needs it.

**NavHAL integration.** NavHAL's Kconfig `.config` is now owned in the vaios
repo as `navhal.config` and copied into the submodule at configure time (it is
gitignored there), enabling `USE_FPU`/`DRV_FPU`/`DRV_SDIO`/`DRV_DWT`. `portable`
links `hal` to inherit the relocated include paths. `SUBMODULE` is defined so
NavHAL cedes the SysTick/PendSV/SVCall/HardFault vectors to vaios.

## Result

Turning off `terminal` + `vfs` + `semihosting` drops the example's `.text` by
~32% — a flight build lands well under 40 KB. NavHAL builds cleanly as an
unmodified submodule; the authoritative configuration lives in the vaios repo.

## Lesson

Configuration that a submodule reads from a fixed path should be owned by the
parent and staged in at build time — that keeps the submodule pristine and the
real config under version control where it belongs.
