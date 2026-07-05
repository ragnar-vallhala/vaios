# vaios — Kconfig-Based Configuration System

Design + phased plan to replace vaios's static `include/vaios_config_default.h`
(a ~55-symbol `#ifndef` header) with a **Kconfig-driven** configuration system
modelled on NavHAL's, that **unifies with NavHAL's config into a single source
of truth** when `NAVHAL` is enabled. One `Kconfig` tree, one `.config`, one
`menuconfig`, generated headers. Every `file:line` anchor verified against the
tree at time of writing (branch `dev`, 2026-07-05).

> **Status: IMPLEMENTED (Option 1 single source of truth + full migration).**
> `Kconfig` + `tools/kconfig.py` + `defconfig` generate `vaios_autoconf.h`;
> composites live in `include/vaios_config_derived.h`; `include/
> vaios_config_default.h` is deleted. Standalone (host / non-NAVHAL) and the
> NAVHAL-unified path (vaios `osource`s NavHAL's Kconfig → one `.config` drives
> both `vaios_autoconf.h` and `navhal_target.h`) all build; host suite 162/682
> green; zero redefinition warnings. `menuconfig` CMake target added.

---

## Goals

1. **Rich, discoverable config** — menus, help text, types, ranges, and
   `depends on` relationships instead of a flat header of `#ifndef` defaults.
2. **Single source of truth with NavHAL** — when `NAVHAL=ON`, vaios's Kconfig
   *sources* NavHAL's, so one `menuconfig` and one `.config` cover both the
   kernel and the HAL driver set. No more parallel `navhal.config` +
   `vaios_config` worlds, and no more FPU-mirroring hack (CMakeLists.txt:120-146).
3. **Zero NavHAL changes** — reuse NavHAL's existing external-`.config` hook.
4. **Standalone operation** — a no-HAL / host-test build (no NavHAL) still
   configures from the same Kconfig, driven by a checked-in `defconfig`.

---

## What's reusable (verified)

- **`kconfiglib`** (pip, already a NavHAL dependency —
  `~/.local/.../kconfiglib.py`) is the engine: native `source`/`rsource`/glob
  includes, `$(srctree)` paths, `menuconfig`, and `write_autoconf()`.
- **NavHAL's `tools/kconfig.py`** (248 lines) is a thin kconfiglib wrapper with
  three emitters: `generate_cmake` (CONFIG_* cache vars, generic),
  `generate_header` (standard autoconf.h via `write_autoconf`, generic), and
  `generate_navhal_target_header` (NavHAL-specific `NAVHAL_CONFIG_*`/`NAVHAL_HAS_*`
  + `NAVHAL_HAS_MAP`). The first two are directly adaptable for vaios.
- **NavHAL already accepts an incoming `.config`** — this is what makes Option 1
  free:
  - NavHAL reads `extern/NavHAL/.config` (`extern/NavHAL/CMakeLists.txt:23`), and
    vaios already stages one there via `configure_file(navhal.config → .config)`
    (`CMakeLists.txt:199-204`). `NAVHAL_CONFIG_FILE` is the documented
    external-config override ("a superproject FORCE-sets it to OWN the driver
    set without writing into the vaios tree").
  - NavHAL loads it with `kconfiglib.Kconfig(..., warn=False)` +
    `kb.load_config()` (`extern/NavHAL/tools/kconfig.py:40,44`). `load_config`
    **silently ignores symbols not defined in NavHAL's tree** — so a *unified*
    `.config` holding both `CONFIG_VAIOS_*` and `CONFIG_DRV_*` feeds NavHAL fine;
    it picks out only its own symbols.

---

## Architecture

```
Kconfig                    vaios top menu (Kernel/Memory/Sched/IPC/Log/Modules…)
  └─ if NAVHAL:            one menuconfig, one tree covering both
        source "$(srctree)/extern/NavHAL/Kconfig"
                    │
              menuconfig / defconfig
                    ▼
          ONE unified .config          (CONFIG_VAIOS_* + CONFIG_DRV_*)
             │                    │
   tools/kconfig.py          staged as extern/NavHAL/.config
   (vaios generator)          via existing NAVHAL_CONFIG_FILE hook
     │        │                     ▼
     ▼        ▼            NavHAL's own kconfig run → navhal_target.h
 vaios_autoconf.h   config.cmake        (reads only its symbols; ignores ours)
 (force-included    (module gating →
  into every TU)     CMake cache vars)
```

Both generated headers are force-included (`-include`), exactly as NavHAL
already does for `navhal_target.h` (`CMakeLists.txt:224`, added this session).

---

## The one real wrinkle: `source` path resolution

NavHAL's `Kconfig` uses `source "src/arch/*/Kconfig.choice"` — **srctree-relative**
(kconfiglib resolves `source` against `$srctree`, default `.`). A single kconfig
run has one `$srctree`, so vaios's own `source` lines and NavHAL's cannot both be
srctree-relative to different roots.

Resolution (no NavHAL edit): from vaios's `Kconfig`, pull NavHAL in with an
explicitly rooted include —
```kconfig
if NAVHAL
    orsource "$(srctree)/extern/NavHAL/Kconfig"
endif
```
and run the generator with `srctree` = repo root (vaios already exports
`srctree` for NavHAL builds — see project memory `navhal-build-needs-srctree`).
NavHAL's inner `source "src/..."` then needs `$srctree` to reach its files;
handled by setting `srctree=extern/NavHAL` for the NavHAL subtree via kconfiglib's
per-file srctree, or by pre-seeding `KCONFIG_SOURCE` roots. **Phase 1 spikes this
exact resolution before anything else — it is the make-or-break detail.**

---

## Symbol migration map (`vaios_config_default.h` → Kconfig menus)

The ~55 current `#ifndef` symbols become typed Kconfig symbols, grouped:

| Menu | Symbols |
|------|---------|
| **Version** | `VERSION_MAJOR/MINOR/PATCH`, `VERSION`, `AUTHOR` |
| **Platform / Init** | `CORTEX_M4`, `__NVIC_PRIO_BITS`, `SYSTICK_PERIOD`, `UART_BAUDRATE` |
| **Scheduling** | `MAX_TASK_PRIORITY`, `IDLE_TASK_PRIORITY`, `IDLE_TASK_STACK_SIZE`, `TIME_SLICE`, `MAIN_STACK_SIZE`, `STACK_ALIGN_SIZE`, `TASK_STACK_OVERFLOW_THRESHOLD`, `TASK_STACK_WATERMARK_ENABLE` |
| **Interrupts / Critical** | `MAX_SYSCALL_INTERRUPT_PRIORITY`, `VAIOS_USE_BASEPRI`, `VAIOS_FROMISR_PRIO_CHECK` |
| **Memory / Heap** | `HEAP_SIZE`, `VAIOS_HEAP_ALGO` (→ `choice` SEGLIST/TLSF, replacing the `VAIOS_HEAP_SEGLIST/TLSF/ALGO` triad), `HEAP_WATERMARK_ENABLE`, `HEAP_WATERMARK_THRESHOLD`, `DMA_MIN_THRESHOLD` |
| **IPC** | `MAX_SEMAPHORE_COUNT`, `STATIC_SEMAPHORE_SIZE`, `MAX_PI_DEPTH` |
| **Modules** (bool) | `VAIOS_MODULE_TERMINAL/VFS/SEMIHOSTING/FIFO/PERF` — today split between `option()` in `CMakeLists.txt:58-62` and header `#ifndef`; Kconfig unifies them |
| **Logging** | `LOGGING_ENABLED`, `UART_LOGGING_ENABLE`, `BUFFERED_LOGGING`, `COLOR_LOGGING`, `MIN_LOG_LEVEL`, `VAIOS_KERNEL_LOG_LEVEL`, `LOG_BUFFER_SIZE`, `LOG_BUFFER_STORAGE_SIZE`, `LOG_MSG_MAX_LEN` |
| **Terminal** (`depends on VAIOS_MODULE_TERMINAL`) | `ENABLE_TERMINAL`, `CMD_BUFFER_SIZE`, `CMD_MAX_LEN`, `MAX_CMD_NUMBER`, `ESCAPE_SEQ_LEN`, `TERMINAL_LOG_LEVEL`, `TERMINAL_TASK_STACK_SIZE`, `ALLOWED_MODULES` |
| **From NavHAL** (sourced) | `USE_FPU` (retires the FPU mirror at `CMakeLists.txt:120-146` — vaios reads the one NavHAL symbol), `DRV_*`, target identity |

`depends on` relationships that were prose comments become enforced: e.g.
`VAIOS_USE_BASEPRI` help documents the NVIC bands (today `vaios_config_default.h`
comments), terminal sizing hides unless the terminal module is on, TLSF/seglist
is a real mutually-exclusive `choice`.

---

## Consumer impact (kept minimal)

- Kernel/port sources keep `#include "vaios_config.h"`. That aggregator
  (`include/vaios_config.h`) is rewritten to include the **generated**
  `vaios_autoconf.h` instead of the static default header — most TUs are
  untouched.
- `VAIOS_CONFIG_FILE` / `vaios_app_config.h` overrides (`vaios_config.h:5-16`)
  are superseded by `.config` + `defconfig`; kept as a deprecation shim for one
  release, then removed.
- `-D` overrides in CMake (heap algo `CMakeLists.txt:101`, module `option()`s)
  are replaced by `.config` values surfaced through `config.cmake`.

---

## Host-test build (the standalone case)

`tools/run_tests.sh` configures with `-S tests` and **no NavHAL**. It must still
get a config without interactive menuconfig:

- Check in `tests/defconfig` (host profile: modules the suite needs, watermarks
  off — mirroring today's `tests/CMakeLists.txt:78-92` `-D` block).
- `tests/CMakeLists.txt` runs `tools/kconfig.py` at configure time over vaios's
  Kconfig with `defconfig`, emits `vaios_autoconf.h`, force-includes it — dropping
  the hand-maintained `add_definitions` list.
- No NavHAL is sourced (the `if NAVHAL` block is inactive), proving the tree
  configures standalone.

---

## Phased plan

**Phase 0 — Generator + spike.** Vendor `tools/kconfig.py` into vaios (adapt
NavHAL's: VAIOS prefix, drop the NAVHAL target emitter). Write a throwaway 3-symbol
`Kconfig` that `orsource`s NavHAL's, and prove the **srctree/source resolution**
end-to-end (the wrinkle above). Gate everything after on this working.

**Phase 1 — vaios Kconfig tree, no NavHAL.** Author `Kconfig` for all ~55
symbols with types/ranges/help/`depends on`. Generate `vaios_autoconf.h` +
`config.cmake`; rewrite `include/vaios_config.h` to include the generated header.
Wire configure-time generation + force-include into the top `CMakeLists.txt`.
Add `tests/defconfig` and switch the host build. **Gate: host suite 162/682
still green.**

**Phase 2 — unify NavHAL.** Add `if NAVHAL: orsource extern/NavHAL/Kconfig`.
Point the existing `NAVHAL_CONFIG_FILE` hook (`CMakeLists.txt:199`) at the unified
`.config` so NavHAL regenerates `navhal_target.h` from it. Retire the FPU mirror.
**Gate: NAVHAL `UNIT_TESTS` firmware still links; QEMU + SITL build.**

**Phase 3 — cleanup.** Delete `vaios_config_default.h`; remove the
`VAIOS_HEAP_ALGO` CMake shim and per-module `option()`s now owned by Kconfig;
deprecate `VAIOS_CONFIG_FILE`. Add a `menuconfig` CMake target. Document in
README / a config guide.

---

## Risks

- **srctree/source resolution** (Phase 0) — the one true unknown. If NavHAL's
  `source` lines can't be rooted from a parent without editing NavHAL, fall back
  to a two-run bridge (still one `.config`, two generator invocations — the
  "shared .config" variant) rather than blocking.
- **Host-build coupling** — the host suite must never require NavHAL or
  interactive config; `defconfig` + standalone generation is a hard invariant
  (guarded by the Phase 1 gate).
- **Churn surface** — ~55 symbols + every `#include "vaios_config.h"` consumer.
  Mitigated by keeping the aggregator's name/contract stable so TUs don't change.
- **`.config` in VCS** — decide what's tracked: `defconfig`s (yes) vs generated
  `.config`/headers (no, `.gitignore` like NavHAL's).
```
