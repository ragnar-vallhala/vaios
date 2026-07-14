# On-target gcov coverage — session handoff

**Date:** 2026-07-14
**Branch:** `feat/stage5-unpriv-flip` (PR #30 context; unmerged)
**Status:** infrastructure built + validated on Renode; scheduler runner built but only viable on real hardware. **Nothing committed or pushed.**

---

## 1. Goal

Extend gcov coverage to the **ARM-only** files that host `tools/coverage.sh` structurally cannot reach because they are never compiled on the host:
`portable/cortex-m4/{port.c, port_hw.c, semihosting.c, syscall.c, qemu_irq.c}`.

## 2. What was built (all NEW/uncommitted)

| File | Role |
|------|------|
| `portable/cortex-m4/gcov_dump.c` | On-target dumper. Overrides `__gcov_init` to collect `gcov_info` (with real embedded `.gcda` paths), stubs `__gcov_exit`, weak `strlen`, runs `.init_array` ctors itself, serializes each TU with `__gcov_info_to_gcda()` and streams framed base64 over UART. Whole file is `#if defined(VAIOS_GCOV)` — inert otherwise. |
| `include/gcov_dump.h` | Declares `void vaios_gcov_dump(void)`. |
| `tools/gcov_sections.ld` | Linker fragment (`INSERT AFTER .text`) bounding `.init_array` with `__init_array_start/end`, kept in FLASH. No NavHAL submodule edit. |
| `tools/gcov_uart_decode.py` | Host-side: strips Renode UART prefixes/ANSI, base64-decodes each `@@VAIOS_GCDA_BEGIN..END` frame, writes byte-exact `.gcda` to its embedded path (next to the `.gcno`). |
| `tools/coverage_target.sh` | Full pipeline: build `-DVAIOS_GCOV=ON` UNIT_TESTS → run in Renode → decode → gcov table of ARM-only files. |
| `examples/98_coverage_sched.c` | Scheduler-exercising runner (see §5). Compiles; **not runtime-validated**. |

Modified: `CMakeLists.txt` (VAIOS_GCOV option block), `examples/99_unit_tests.c` (calls `vaios_gcov_dump()` under `VAIOS_GCOV`), `examples/CMakeLists.txt` + `examples/Kconfig` (COVERAGE_SCHED entry).

## 3. How the mechanism works (key facts — don't re-derive)

- arm-none-eabi **libgcov is freestanding: NO file I/O** (only external ref is `strlen`). So `__gcov_dump()` writes nothing; you must serialize via `__gcov_info_to_gcda()` yourself.
- Build with plain **`--coverage` (NOT `-fprofile-info-section`)** — the section variant NULLs the embedded `.gcda` filename; the constructor path keeps it. `__gcov_init` is overridden to capture the list; `__gcov_exit` stubbed; ctors run manually because vaios/NavHAL startup never runs `.init_array`.
- `.gcda` is streamed over **UART**, not semihosting (see §4). Frames: `@@VAIOS_GCDA_DUMP_BEGIN` / per-file `@@VAIOS_GCDA_BEGIN <abspath>` + base64 lines + `@@VAIOS_GCDA_END` / `@@VAIOS_GCDA_DUMP_END`.
- Linker: base script via `-dT`, fragment via `-T` (ordering matters); section stays in FLASH (one harmless "redeclaration of FLASH" warning is expected).

## 4. Two hard constraints discovered

1. **Renode 1.16.1 has NO ARM semihosting** — `semihosting_enabled()` is a tlib stub returning false; the `DoSemihosting`/`SemihostingUart` support is Xtensa-only. This killed the original "write `.gcda` to a host file via semihosting" plan → UART transport instead (identical result). QEMU *does* have ARM semihosting, but its `netduinoplus2` can't run the NAVHAL F401 image (unimplemented RCC).
2. **Instrumented scheduler code is impractically slow under Renode.** Renode advances virtual time in lock-step with executed instructions; `--coverage` + context-switch traffic inflates the count so that ~3 s of virtual time took >2 min wall clock, never reaching the dump. Not a bug — Renode's model vs instrumentation.

Also fixed: gcov counters bloat `.bss`, so the fixed 88 KB `HEAP_SIZE` overran the 96 KB SRAM (`v_heap_memory_init` memset faulted pre-boot). `VAIOS_GCOV` now forces `HEAP_SIZE=32768`.

## 5. Validated result

`bash tools/coverage_target.sh` → `ON-TARGET RESULT: ALL PASS`, 17 `.gcda` reconstructed, 0 errors:

```
port_hw.c    44.83% line / 20.00% branch   (host gcov: 0% — ARM-only)
port.c        0.00%                         (no scheduler in UNIT_TESTS runner)
semihosting.c  —   (not linked in this runner)
syscall.c      —   (not linked: no SVC syscalls without a scheduler)
```

Normal builds unaffected: verified 0 gcov symbols in a MULTI_TASK build, host suite green (206/206, 830 asserts). VAIOS_GCOV defaults OFF.

## 6. The open item — port.c / syscall.c coverage

`port.c` is entirely scheduler/exception machinery (PendSV, SVCall, context switch, fault handlers) → 0% because the UNIT_TESTS runner has no scheduler. `syscall.c` isn't even linked without SVC syscalls.

`examples/98_coverage_sched.c` is the answer: 2 privileged worker tasks doing yield/semaphore/mutex/malloc/exit via SVC, last worker dumps. Config `MPU_ENABLE + SYSCALL_SVC + TASK_HEAP` (mirrors the working FD_IPC examples; STATIC_PROTECT was dropped after a SVCall_Handler fault trace). **It compiles but was never run to completion** — Renode is too slow (§4-2), and this session couldn't drive the physical board.

### Next steps (in priority order)
1. **Run `98_coverage_sched.c` on the real STM32F401** (connected in prior sessions). At 84 MHz the instrumented image + UART dump finishes in ms. Build: `cmake -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=COVERAGE_SCHED -DVAIOS_GCOV=ON`. Flash, capture UART (e.g. `tools/run_hw_tests.sh` capture style / `PORT=/dev/ttyACM0`), pipe the capture to `tools/gcov_uart_decode.py`, then `arm-none-eabi-gcov` the port.c/syscall.c objects. **TODO: write `tools/coverage_hw.sh`** to automate flash→capture→decode→report (user offered; not yet built).
2. If HW confirms it, decide whether to also keep a Renode CI gate on just the UNIT_TESTS coverage (already fast, <120 s) — could add a `coverage-target` step to `.github/workflows/ci.yml` mirroring the `sitl` job.
3. Then merge the whole thing into PR #30 (or a fresh PR).

## 7. Gotchas for whoever picks this up

- **Environment flakiness this session:** files written by bash heredoc to `/tmp` or `/home/ragnar` did NOT persist across tool calls; only the repo working dir persisted. Renode runs >120 s exceed the bash tool cap; background renode output wasn't reliably captured. Use committed `tools/renode.resc` (takes `$bin`, `$run`) and keep runs short, or drive from a real terminal.
- Kill stray renode: `pkill -9 -f Renode.dll` (verify with `ps aux | grep -i '/Renode.dll' | grep -v grep`; `pgrep -f Renode.dll` matches its own cmdline — false positive).
- `-DVAIOS_GCOV` uses `-O0` (faithful attribution). An instrumented `-O1` image is faster under Renode but I reverted to `-O0` since that's the validated setting for the UNIT_TESTS path.
- Constraint reminders: NavHAL builds need `export srctree=$PWD/extern/NavHAL`. Trunk is `dev`; PRs target `dev`. **Never add Claude trailers to commits/PRs.** Don't push without explicit confirmation.

## 8. Build dirs
All `build_*` are gitignored and were cleaned. `build_cov_target` is what `coverage_target.sh` (re)creates.
