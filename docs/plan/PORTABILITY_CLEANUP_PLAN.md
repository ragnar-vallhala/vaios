# vaios — Portability Cleanup

Phased plan to remove hardware-specific code that has leaked outside
`portable/`, so that adding a second architecture is a matter of writing a new
`portable/<arch>/` rather than editing `kernel/`, `include/`, the root
`CMakeLists.txt`, and 12 build scripts. Every `file:line` anchor below was
verified against the tree at time of writing (branch `dev`, 2026-07-21).

> **Status: IN PROGRESS.** Phase 1 implemented (arch-capability Kconfig); Phase 2
> items 2a–2d implemented (2e deferred); Phase 3 implemented (3a header
> relocations + 3b NVIC-priority cluster); Phase 4 port-selection mechanism
> implemented (Kconfig symbol moves/renames deferred); Phase 5 implemented;
> Phase 6 implemented (full portability tripwire + CI, its MMIO fix having landed
> early in commit `3b2ad1b`). All six phases done. Remaining, as deferred
> sub-items: 2e (stack-size validation) and Phase 4's Kconfig symbol
> moves/renames — both tracked in their sections.

---

## Framing

This is a **migration, not a redesign.** The port facade already exists and is
already good: 28 `v_port_*` / `v_port_hw_*` entry points, and `kernel/vaios.c`
routes every bit of bring-up through it (`v_port_hw_clock_init`,
`v_port_hw_fpu_enable`, `v_port_hw_systick_init`, `v_port_mpu_init`,
`v_port_cpu_relax`) with no-op fallbacks on backends that lack the hardware.
That file is the model. The findings below are code that predates the facade or
bypassed it, not evidence that the facade is wrong.

Two consequences for how we sequence:

- We are **adding to an existing seam**, so each phase can land independently
  and keep the host suite green. There is no big-bang cutover.
- The most valuable early fix is not the scariest one. `include/task.h:16` is a
  one-line `#error`, but it forces `-DCORTEX_M4` into a dozen *host* builds.
  Removing it is mechanical and immediately shrinks the blast radius of
  everything else.

---

## Goals

1. **No architecture-specific code outside `portable/`** — no inline asm, no
   peripheral addresses, no CMSIS/vendor symbols, no arch `#ifdef`s in
   `kernel/` or `include/`.
2. **`portable/` is the port-selection point** — the build picks a port and
   consumes what that port exports, rather than the root `CMakeLists.txt`
   hardcoding ARM flags and reaching into `portable/cortex-m4/` by path.
3. **Host builds stop pretending to be ARM** — no `-DCORTEX_M4` in any
   `CMAKE_SYSTEM_NAME=Linux` build, test script, or CI job.
4. **Regressions are caught mechanically** — a tripwire that fails CI when new
   hardware code appears outside `portable/`.

**Non-goal:** a working AVR port. `portable/avr/` is two files and is not
reachable from any build path. This plan makes the *kernel* port-ready and
proves it with a compile-only second target; finishing AVR is separate work
(see [Deferred](#deferred-word-size--stack-model)).

---

## Phase 1 — Break the arch-macro monopoly

> **Status: IMPLEMENTED** (branch `feat/arch-capability-kconfig`). Host suite
> 245/245; ARM firmware builds clean under both `NAVHAL=OFF` and `NAVHAL=ON`;
> `grep CORTEX_M4 kernel/ include/ tests/ tools/ .github/ examples/` is empty.
> Three deltas from the design as first written, all forced by the config
> system and reflected in the snippets below:
>
> 1. **Symbols are `VAIOS_ARCH_*`, not bare `ARCH_*`.** NavHAL already defines
>    `config ARCH_CORTEX_M4` inside its own `choice`
>    (`extern/NavHAL/src/arch/armv7e-m/Kconfig.choice`); a second bare
>    `ARCH_CORTEX_M4` in vaios's choice would put one symbol in two choices,
>    which kconfiglib rejects — the `NAVHAL=ON` config would not parse. The
>    `VAIOS_` prefix avoids the collision, and the arch **follows** NavHAL via
>    the choice default `default VAIOS_ARCH_CORTEX_M4 if ARCH_CORTEX_M4` (single
>    source of truth: NavHAL picks, vaios mirrors; standalone/QEMU fall through
>    to the plain default; the host build overrides to `VAIOS_ARCH_HOST`).
> 2. **The autoconf macros are unprefixed** (`VAIOS_ARCH_HAS_MPU`, not
>    `CONFIG_VAIOS_ARCH_HAS_MPU`). `tools/kconfig.py` emits vaios symbols into
>    `vaios_autoconf.h` without the `CONFIG_` prefix (it keeps `CONFIG_` only
>    for the CMake cache-var emitter). C code reads `VAIOS_ARCH_*`; CMake reads
>    `CONFIG_VAIOS_ARCH_*`.
> 3. **`NVIC_PRIO_BITS` was *not* moved under `depends on
>    VAIOS_ARCH_HAS_IRQ_PRIORITY`.** It feeds `MAX_SYSCALL_INTERRUPT_PRIORITY`
>    in `vaios_config_derived.h`, which is compiled on *every* build including
>    the host ipc tests; gating it off on host breaks them. It moves with the
>    derived macro in Phase 3. The MPU menu *was* gated (`depends on
>    VAIOS_ARCH_HAS_MPU`) — safe because the host MPU tests force
>    `-DVAIOS_MPU_*` per target, bypassing Kconfig.
>
> The host build selects the port through a new `tests/host.defconfig`
> (`CONFIG_VAIOS_ARCH_HOST=y`), replacing the `-DCORTEX_M4` it used to inject.

**Problem.** `include/task.h:14-19` gates the central kernel header on an
architecture macro:

```c
#ifndef CORTEX_M4
#error "Define a valid architecture macro (e.g., CORTEX_M) before including task.h"
#endif
```

(The message says `CORTEX_M`, the test says `CORTEX_M4` — they've drifted.)

Because *any* translation unit that includes `task.h` needs this, every host
build has to assert it is ARM. Confirmed sites: `tests/CMakeLists.txt:150, 204,
234, 267, 327, 377, 418`; `tools/run_tests.sh:26`; `tools/coverage.sh:37`;
`tools/run_static_analysis.sh:52`; `.github/workflows/codeql.yml:92`. Three
examples work around it by defining the macro in application source —
`examples/benchmark/main.c:28`, `examples/28_vfs_concurrent.c:10`,
`examples/20_uart_comm.c:4` — which is the worst case: it forces
arch-conditional kernel code on regardless of what the build selected, so a
wrong-arch build miscompiles instead of failing.

**Fix.** Gate on *capability*, not architecture — and express capability in
**Kconfig**, not in a hand-written port header.

### The rule

The config system is already Kconfig-driven with a single source of truth
(`vaios_autoconf.h`; `include/vaios_config_default.h` was deleted for exactly
this reason — see `KCONFIG_CONFIG_SYSTEM_PLAN.md`). A parallel `V_PORT_*`
`#define` namespace would re-fragment it. So:

| Kind of fact | Where it lives |
|---|---|
| Anything expressible as a bool or int — capability, enablement, silicon parameter | **Kconfig** → `CONFIG_*` in `vaios_autoconf.h` |
| C types (`v_stack_t`), macros with code bodies (`V_PORT_MB()`), inline asm accessors | Port header — Kconfig structurally cannot express these |

"Immutable ISA fact" is *not* a reason to bypass Kconfig. Stack-growth direction
can't be changed by the user, but it is still a bool, so it belongs in the same
namespace as everything else — discoverable in `menuconfig`, one place to look.

### Arch capability symbols

The arch `choice` lives in the main `Kconfig` (Phase 4 may split per-arch
members into `portable/<arch>/Kconfig` once the sourcing machinery exists). Each
member `select`s what it can do; the capability symbols are hidden (no prompt),
so only the arch sets them. As shipped (`Kconfig:38-79`):

```kconfig
choice
    prompt "Target architecture"
    default VAIOS_ARCH_CORTEX_M4 if ARCH_CORTEX_M4   # follow NavHAL when sourced
    default VAIOS_ARCH_AVR if ARCH_AVR8
    default VAIOS_ARCH_CORTEX_M4

config VAIOS_ARCH_CORTEX_M4
    bool "ARM Cortex-M4"
    select VAIOS_ARCH_HAS_MPU
    select VAIOS_ARCH_HAS_IRQ_PRIORITY
    select VAIOS_ARCH_STACK_DESCENDING

config VAIOS_ARCH_HOST
    bool "Host (POSIX — test and static-analysis builds)"
    select VAIOS_ARCH_STACK_DESCENDING

config VAIOS_ARCH_AVR
    bool "AVR (8-bit)"
endchoice

config VAIOS_ARCH_HAS_MPU          # hidden — selected by the arch, never the user
    bool
config VAIOS_ARCH_HAS_IRQ_PRIORITY
    bool
config VAIOS_ARCH_STACK_DESCENDING
    bool
config VAIOS_ARCH_MPU_MIN_REGION
    int
    default 128 if VAIOS_ARCH_CORTEX_M4   # ARMv7-M: power-of-two, ≥ frame
    default 0
```

`default VAIOS_ARCH_CORTEX_M4 if ARCH_CORTEX_M4` references NavHAL's symbol,
which is in scope after the `osource` at the bottom of `Kconfig` (kconfiglib
resolves references post-parse). Under `NAVHAL=OFF` that symbol is undefined and
evaluates to `n`, so the plain default wins.

### What this fixes beyond the `#error`

The existing MPU menu (`Kconfig:163-240`) is well built — seven symbols with
correct `depends on` chains — but nothing ties it to the target actually having
an MPU. `VAIOS_MPU_ENABLE` (`Kconfig:164`) is titled "Enable the ARMv7-M MPU"
and its help text defers the question to a **runtime** check
(`hal_mpu_present`, `Kconfig:169`). Add one line:

```kconfig
config VAIOS_MPU_ENABLE
    bool "Enable hardware memory protection (MPU)"
    depends on VAIOS_ARCH_HAS_MPU          # <-- new
```

and the entire MPU subtree — stack guard, static protect, flash RO, null guard,
user separation — disappears from `menuconfig` on an arch that has no MPU. That
is strictly better than an `#error` or a port `#define`: the invalid
configuration becomes unrepresentable rather than diagnosed after the fact. The
same applies to `NVIC_PRIO_BITS` (`Kconfig:36-38`, "implemented by the
silicon"), which moves under `depends on VAIOS_ARCH_HAS_IRQ_PRIORITY`.

### The `task.h` guard

With a Kconfig `choice`, exactly one arch is always selected — the invariant the
`#error` was hand-checking is now structural. The guard reduces to a sanity
check that generated config actually reached this TU. As shipped
(`include/task.h`), it tests that some arch member is defined — bools emit as
explicit `0`/`1`, so all three are present once autoconf is force-included, and
their absence means the config step didn't run:

```c
#if !defined(VAIOS_ARCH_CORTEX_M4) && !defined(VAIOS_ARCH_HOST) &&             \
    !defined(VAIOS_ARCH_AVR)
#error "No vaios arch selected -- is vaios_autoconf.h on the include path? ..."
#endif
```

Kernel code then reads `VAIOS_ARCH_HAS_MPU` / `VAIOS_MPU_ENABLE`, never an arch
name. **`CORTEX_M4` stops being a compile flag entirely** — the dead
`add_compile_definitions(CORTEX_M4)` at `CMakeLists.txt:266` was removed; the
name now exists only as the Kconfig symbol `VAIOS_ARCH_CORTEX_M4` and
`portable/cortex-m4/`'s own include guards.

### What remains in a port header

Only the two things Kconfig can't hold — and neither is a feature flag:

```c
/* portable/cortex-m4/v_port_arch.h */
typedef uint32_t v_stack_t;                        /* Phase 7 consumer */
#define V_PORT_MB() __asm volatile("dsb; isb" ::: "memory")
```

**Then delete** all 12 `-DCORTEX_M4` injections and the 3 in-source `#define`s.

**Acceptance:** host suite green; `grep -rn CORTEX_M4 kernel/ include/ tests/
tools/ .github/` is empty (the symbol survives only as `ARCH_CORTEX_M4` in
Kconfig and inside `portable/cortex-m4/`); `menuconfig` with `ARCH_HOST`
selected shows no MPU menu.

---

## Phase 2 — Migrate the three unguarded leaks in `kernel/`

> **Status: 2a–2d IMPLEMENTED; 2e DEFERRED.** Host suite 245/245; ARM firmware
> clean under `NAVHAL=OFF` and `NAVHAL=ON`. 2a shipped earlier with the MMIO
> gate (commit `3b2ad1b`, `v_port_hw_in_isr`). 2b/2c/2d migrated here. Notes:
>
> - **2c** uses the linker's `_estack` for the upper bound and `0x20000000` —
>   the ARMv7-M *architectural* SRAM region base, not a vendor literal — for the
>   lower, so `v_port_ptr_is_ram` is correct for any Cortex-M4 board, not just
>   the STM32F4 the old `0x20020000` literal assumed.
> - **2d** revealed the host stub `v_port_get_psp()` returns 0, not the
>   `UINTPTR_MAX` the old `task_live_sp()` used. The migration adds an explicit
>   `live_sp != 0` sentinel at the one grow-guard caller (mirroring the `psp !=
>   0` guard already in `utils.c`) so host stays a clean no-op rather than
>   relying on pointer-underflow wraparound. The core `v_port_get_psp` /
>   `v_port_ptr_is_ram` stubs moved to the shared `tests/stubs/stubs.c` so every
>   target linking `memory.c`/`task.c` resolves them.
> - **2e DEFERRED.** Gating the power-of-two rule on the MPU feature is correct,
>   but it changes the contract of `test_task_create_rejects_non_power_of_two`:
>   under a non-MPU build, sizes like 700 B become *valid*, so the test's "no
>   task created, no heap used" assertions break, not just its return-value
>   check. Doing it right means separating the universal minimum-stack floor
>   (every port needs room for the initial context frame) from the MPU-only
>   power-of-two rule, and rewriting the test to assert per-regime. That is a
>   deliberate behaviour + test-semantics change deserving its own commit, not a
>   silent rider on the clean migrations. The `VAIOS_ARCH_MPU_MIN_REGION` symbol
>   Phase 1 added is ready for it.

Each of these has a facade to migrate *to*; none needs a new concept.

### 2a. `kernel/utils.c:811` — raw ICSR dereference

```c
uint32_t is_in_isr = (*(volatile uint32_t *)0xE000ED04) & 0x1FF;
```

Cortex-M SCB→ICSR, masked for VECTACTIVE, guarded only by `#if LOGGING_ENABLED`
— **no arch guard at all**, so it is a wild dereference on host and AVR. The
identical need is already served properly at `kernel/ipc.c:764` via
`v_port_hw_active_irq_priority()`.

Add `int v_port_in_isr(void)` to the facade (Cortex-M: the ICSR read; host stub:
return 0) and call it.

### 2b. `kernel/utils.c:977` — kernel owns the ARM vector symbol

```c
void SysTick_Handler(void) { PERF_ISR_SYSTICK_BEGIN(); systick_count++; ...
```

The body is portable; the *name* is CMSIS. An AVR port has no such vector
(`ISR(TIMER0_COMPA_vect)`), so the kernel tick silently never runs.

Rename to `void v_kernel_tick(void)` in `kernel/utils.c`; `portable/cortex-m4/`
supplies `void SysTick_Handler(void) { v_kernel_tick(); }`. This also relocates
the `#ifdef NAVHAL hal_timebase_tick()` call at `kernel/utils.c:970-981` into
the port, where a vendor HAL call belongs.

### 2c. `kernel/task.c:336` — hardcoded STM32F4 SRAM window

```c
if ((uint32_t)t < 0x20000000 || (uint32_t)t > 0x20020000 || t->priority > MAX_PRIORITY)
  v_panic(__FILE__, __LINE__, "invalid task pointer: 0x%x", (uint32_t)t);
```

Assumes 128 KB of SRAM at `0x20000000`; panics on every context switch on an
STM32H7, a part with CCM, or anything non-ST. Note this is already
`#if defined(VAIOS_HOST_TEST)`-excluded at `kernel/task.c:329-335` with a
comment explaining the check isn't portable — the non-portability was known,
just never generalized.

Add `int v_port_ptr_is_ram(const void *p)` — Cortex-M4 keeps the range check
(sourced from the linker script, not a literal), host and any port without a
known map return 1. Kernel keeps the priority check unconditionally, drops the
`#if VAIOS_HOST_TEST` fork.

### 2d. `kernel/memory/memory.c:336` — duplicate PSP read

```c
#if defined(__arm__) || defined(__thumb__) || defined(__thumb2__)
  __asm volatile("mrs %0, psp" : "=r"(sp));
```

Correctly `#ifdef`-guarded with a `UINTPTR_MAX` fallback, so it does not break a
port — but it is raw arch `#ifdef` + inline asm in `kernel/`, and it duplicates
`v_port_get_psp()`, which `kernel/utils.c:713` already uses for the same
purpose. Two mechanisms for one operation. Collapse onto `v_port_get_psp()`.

### 2e. `kernel/task.c:192` — MPU geometry applied unconditionally

```c
if (size < 128 || (size & (size - 1)) != 0) { ... return 0; }
```

Power-of-two and ≥128 B are ARMv7-M MPU region constraints, but the check is
**not** under `#if VAIOS_MPU_*` — so a port with no MPU still can't create a
3 KB stack. Gate on `VAIOS_MPU_ENABLE` and take the bound from
`VAIOS_ARCH_MPU_MIN_REGION` (Phase 1). Note this makes the constraint follow
the *feature*, not the *chip*: a Cortex-M4 build with the MPU switched off also
regains arbitrary stack sizes, which is correct and is not true today.

**Acceptance:** `grep -rnE '0xE000|0x2000[0-9A-F]{4}|__asm|SysTick_Handler' kernel/ include/`
returns nothing.

---

## Phase 3 — Relocate arch-owned headers out of `include/`

> **Status: 3a and 3b both IMPLEMENTED.** Split by cohesion into the arch-owned
> *header* relocations (3a) and the NVIC *priority-model* cluster (3b); each
> kept the host suite green and both ARM builds clean.
>
> **3a done**: `include/semihosting.h` → `portable/cortex-m4/`; the
> `#if VAIOS_SYSCALL_SVC` block of `include/syscall.h` → a port-supplied
> `port_syscall.h` (ARM asm in `portable/cortex-m4/`, host model in
> `tests/stubs/`, selected by include-path shadowing since `portable/cortex-m4`
> is not on the host path); `include/qemu_irq.h` deleted with its two vestigial
> includes; `kernel/CMakeLists.txt` NavHAL include export narrowed `PUBLIC` →
> `PRIVATE`.
>
> **3b done**: `__NVIC_PRIO_BITS` + `MAX_SYSCALL_INTERRUPT_PRIORITY` moved from
> `include/vaios_config_derived.h` into `portable/cortex-m4/port.h` (from
> Kconfig) and `tests/stubs/port.h` (host-emulated, hardcoded); `ipc.c`'s
> FromISR predicate now sources the first-external-IRQ number and the priority
> comparison from the port. Phase 1's deferred `NVIC_PRIO_BITS depends on
> VAIOS_ARCH_HAS_IRQ_PRIORITY` landed here too. Three deviations from the
> written design, all forced by the port/host mechanics:
>
> 1. **`VAIOS_ARCH_FIRST_EXTERNAL_IRQ` and `v_port_prio_is_more_urgent` live in
>    the port headers, not Kconfig.** A Kconfig int would evaluate to 0 on the
>    host (host doesn't select the arch), but the host build *emulates* the ARM
>    exception model to run `test_ipc.c`. Keeping both in `port.h` + the stub
>    (real: 16 / `a < b`; host: mirror) puts the whole priority model in one
>    place per build.
> 2. **`ipc.c`'s predicate is left ungated, not wrapped in
>    `#if VAIOS_ARCH_HAS_IRQ_PRIORITY`.** A gate would force host emulation `-D`s
>    and give a non-priority port a silently-different function; instead each
>    port supplies the constants/comparator, so a priority-less arch defines
>    them to make the predicate trivially safe. More portable, and host stays
>    green with no per-target flags.
> 3. **The real `port.h` carries an `#ifndef NVIC_PRIO_BITS` fallback.** With the
>    Kconfig symbol now gated to ARM, the host build still compiles the *real*
>    `port.h` through `portable/cortex-m4/syscall.c` (a same-directory `"port.h"`
>    include that outranks the `tests/stubs` shadow), where the symbol is absent.
>    The fallback covers exactly that TU; ARM builds get the Kconfig value and
>    skip it.

These don't hard-break other ports (nothing forces their inclusion) but they
violate the layering rule outright, and they're what makes the seam ambiguous
to a contributor.

| Move | Why |
|---|---|
| `include/semihosting.h` → `portable/cortex-m4/` | Whole file is ARM: semihosting op numbers `SYS_OPEN 0x01 … SYS_EXIT 0x18` (lines 6-30) plus `set_systick_interrupt_priority()` / `set_pendsv_interrupt_priority()` (37-38) — Cortex-M system exceptions declared in the portable tree. |
| `include/syscall.h:108-143` → `portable/cortex-m4/port_syscall.h` | `mrs %0, ipsr`, `svc 1`, and `register uint32_t r12 __asm__("r12")` pin the ARMv7-M trap ABI. Today's two configs work (`#if VAIOS_HOST_TEST` stub + `#if VAIOS_SYSCALL_SVC`), but there is no third branch, so no other port can have SVC syscalls. `include/syscall.h` keeps the portable prototypes and includes the port header. |
| `include/vaios_config_derived.h:17-26` → port | `#define __NVIC_PRIO_BITS` and `MAX_SYSCALL_INTERRUPT_PRIORITY (… << (8 - __NVIC_PRIO_BITS))` hardcode ARM's 8-bit, high-bit-justified, lower-is-more-urgent priority register — meaningless where `VAIOS_ARCH_HAS_IRQ_PRIORITY` is unset. Also adopts the CMSIS reserved identifier `__NVIC_PRIO_BITS`. The derived composite moves to the port; the *input* stays the `NVIC_PRIO_BITS` Kconfig symbol. |
| `include/qemu_irq.h` | Zero-byte file. Delete. |

Also **narrow `kernel/CMakeLists.txt:44-46`**: the `vaios` target
`PUBLIC`-exports NavHAL's include directory, so every consumer inherits vendor
headers on its include path. That is precisely why an accidental `hal_*` call in
`kernel/` compiles silently instead of erroring. Change to `PRIVATE`, or scope
it to the port target.

**Interpretation left in portable code (accept, document).** `kernel/ipc.c:746-750`
encodes ARM exception numbering — `vectactive < 16` means system handler, and
`>=` on priority encodes ARM's inverted ordering. Acquisition is properly
abstracted; only the interpretation is arch-specific, and it was written this
way deliberately for host testability (`tests/test_ipc.c:493-509` asserts it).
Leave the logic, but source the `16` from `VAIOS_ARCH_FIRST_EXTERNAL_IRQ` (arch
Kconfig, `default 16 if ARCH_CORTEX_M4`) and put the inverted comparison behind a
`v_port_prio_is_more_urgent(a, b)` inline — code-bodied, so it belongs in the
port header per the Phase 1 rule. The block sits under
`#if VAIOS_ARCH_HAS_IRQ_PRIORITY`: an arch with no priority levels has no
question to answer.

---

## Phase 4 — Make `portable/` the real port-selection point

> **Status: port-selection mechanism IMPLEMENTED; Kconfig symbol moves/renames
> DEFERRED.** Host 245/245; ARM firmware byte-comparable (identical `-mcpu`/
> `-mthumb`/FPU flags and `arm-none-eabi-` toolchain) under `NAVHAL=OFF`/`ON`;
> the non-NAVHAL example and a `VAIOS_GCOV` build compile; `cmake
> -DVAIOS_PORT=nonsense` fails with the explicit `FATAL_ERROR`.
>
> **Done**: a per-port `portable/cortex-m4/arch.cmake` exports
> `PORT_TOOLCHAIN_PREFIX`, `PORT_ARCH_FLAGS` (`-mcpu=cortex-m4 -mthumb`), and
> `PORT_STARTUP_SOURCE`; the root `CMakeLists.txt` `include()`s it before
> `project()` and consumes those instead of hardcoding the toolchain, `-mcpu`/
> `-mthumb`, or a startup path (the duplicate `CMAKE_ASM_FLAGS` is gone too).
> `portable/CMakeLists.txt` selects `portable/${VAIOS_PORT}/` explicitly and
> `FATAL_ERROR`s on an unknown port, replacing the empty-glob that swept every
> port's sources into one library. `examples/CMakeLists.txt` consumes
> `PORT_STARTUP_SOURCE`. `tools/gcov_sections.ld` → `portable/cortex-m4/`, beside
> its companion `gcov_dump.c`.
>
> **Three deviations from the written design**, forced by how this project's
> build is actually layered:
>
> 1. **Port is selected by `VAIOS_PORT` (a cache var), not by Kconfig.** The
>    plan wanted `VAIOS_PORT` to derive from `CONFIG_ARCH_*`, but the cross
>    toolchain must be set *before* `project()`, and Kconfig generation runs
>    *after* it — an ordering the CMake language can't invert here. So the arch
>    is chosen by `-DVAIOS_PORT=` (default `cortex-m4`), and `arch.cmake` (not
>    Kconfig) is the source of truth for the toolchain.
> 2. **The linker script and FPU flags stay in the root, not `arch.cmake`.**
>    Neither is a pure arch fact on this project: the linker script is a
>    *per-board* file under `extern/NavHAL/src/board/<board>/`, and the float ABI
>    is *config*-derived (the NavHAL `CONFIG_USE_FPU` mirror). `arch.cmake` owns
>    only what is genuinely arch; the root appends the board/config parts. The
>    plan's `PORT_LINKER_SCRIPT` would misattribute a board fact to the arch.
> 3. **Kconfig symbol moves/renames deferred.** Relocating `CORTEX_M4`/
>    `NVIC_PRIO_BITS` into a `portable/cortex-m4/Kconfig` needs a Kconfig
>    `source` mechanism, and renaming `SYSTICK_PERIOD`→`TICK_PERIOD_US` /
>    `UART_BAUDRATE`→`CONSOLE_BAUDRATE` touches every macro consumer across the
>    tree — a config-schema change, separable from the CMake port-selection work
>    and better done on its own.

**Problem.** `portable/CMakeLists.txt:1-7` is the only arch conditional, and it
fails unsafely:

```cmake
set(PORTABLE_DIR "")
if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "cortex-m4")
  set(PORTABLE_DIR "cortex-m4")
endif()
FILE(GLOB_RECURSE PORTABLE_SOURCES .../${PORTABLE_DIR}/*.c .../${PORTABLE_DIR}/*.s)
```

There is no `avr` branch, so on any other processor `PORTABLE_DIR` stays empty
and the recursive glob matches **all of `portable/**`** — compiling `avr/avr.c`
and every `cortex-m4/*.c` plus `startup.s` into one library. And
`CMakeLists.txt:41` sets `CMAKE_SYSTEM_PROCESSOR cortex-m4 … FORCE`, so
`-DCMAKE_SYSTEM_PROCESSOR=` on the command line is overwritten and that branch
can never be taken anyway.

Meanwhile the root `CMakeLists.txt` owns everything the port should:

- `:10-14` — `TOOLCHAIN_PREFIX arm-none-eabi-`, duplicating
  `CONFIG_TOOLCHAIN_PREFIX` which already exists in `Kconfig` and
  `navhal.config` and is ignored here (third copy of one decision).
- `:242-259` — `-mcpu -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16`; also sets
  `CMAKE_ASM_FLAGS` twice, identically (`:250` and `:259`).
- `:261`, `:312` — linker script path into `extern/NavHAL/src/board/${BOARD}/`,
  plus `--specs=nano.specs`/`nosys.specs` (newlib/ARM-GCC only). Memory layout
  is owned by a vendor submodule, not by a port.
- `:265-266` — `add_compile_definitions(CORTEX_M4)`, driven from the root
  rather than exported by the port.
- `examples/CMakeLists.txt:4-6` reaches directly into
  `portable/cortex-m4/startup.s`, inherited by ~35 `add_executable` lines.
- `tools/gcov_sections.ld:19-22` holds `FLASH ORIGIN = 0x08000000, LENGTH = 512K`
  and an ARM `INSERT AFTER .text` — outside `portable/`, even though its
  companion dumper correctly lives at `portable/cortex-m4/gcov_dump.c`.

**Fix.** Each port ships an `arch.cmake` that sets, and `portable/CMakeLists.txt`
selects it and re-exports:

```cmake
# portable/CMakeLists.txt
set(VAIOS_PORT "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "Target port")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}/arch.cmake")
  message(FATAL_ERROR "No port for '${VAIOS_PORT}'. Available: cortex-m4, avr, host")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}/arch.cmake")
```

exporting `PORT_STARTUP_SOURCE`, `PORT_ARCH_C_FLAGS`, `PORT_ARCH_ASM_FLAGS`,
`PORT_LINKER_SCRIPT`, `PORT_TOOLCHAIN_PREFIX`, `PORT_COMPILE_DEFINITIONS`.
Root CMake and `examples/CMakeLists.txt` consume those. Drop the `FORCE` on
`CMAKE_SYSTEM_PROCESSOR`. Move `tools/gcov_sections.ld` →
`portable/cortex-m4/gcov_sections.ld` (update `CMakeLists.txt:382`).

**Kconfig is the source of truth for port selection, not CMake.** Phase 1
establishes the arch `choice`; this phase makes the build *consume* it rather
than duplicate it. `VAIOS_PORT` above derives from the selected
`CONFIG_ARCH_*` symbol (the Kconfig step already emits CMake cache vars via
`tools/kconfig.py`'s `generate_cmake` emitter), so the arch decision is made in
exactly one place. That retires the third copy noted in
[Phase 4's problem statement](#phase-4--make-portable-the-real-port-selection-point):
today `CMakeLists.txt:10`, `Kconfig`, and `navhal.config` each independently
name the toolchain and target.

Remaining `Kconfig:33-44` cleanup: `CORTEX_M4` and `NVIC_PRIO_BITS` move into
`portable/cortex-m4/Kconfig` (per Phase 1). `SYSTICK_PERIOD` and
`UART_BAUDRATE` are named after peripherals but are genuinely generic knobs —
rename to `TICK_PERIOD_US` and `CONSOLE_BAUDRATE`, keep central.

**Acceptance:** `cmake -DVAIOS_PORT=nonsense` fails with the explicit
`FATAL_ERROR`; the cortex-m4 firmware build is byte-comparable to before.

---

## Phase 5 — Examples and tools

> **Status: IMPLEMENTED.** Host 245/245; the touched examples build (11 now
> compiles under `NAVHAL=OFF`, which was the bug). Fixed: `33_mpu_user_demo.c`
> reads privilege through a new `v_port_is_privileged()` port accessor instead of
> `mrs control`; `11_block_wake_task.c` guards its `navhal.h` include and `TIM5`
> timer block with `#ifdef NAVHAL`; `tools/coverage_target.sh` derives the port
> subdir from the build cache's `VAIOS_PORT` instead of hardcoding `cortex-m4`.
>
> Two deviations from the written list:
>
> 1. **`22_mpu_fault.c`'s `dsb`/`isb` stay.** The plan suggested replacing them
>    with `V_PORT_MB()`, but that macro is `dmb` (data memory barrier only),
>    while the example injects a Thumb `BX LR` (`0x4770`) into RAM and executes
>    it — self-modifying code needs `dsb` (complete the write) + `isb` (flush the
>    pipeline), which `dmb` does not provide. The whole example is an irreducibly
>    ARMv7-M W^X/MPU probe (Thumb opcode, MMFSR values), so it belongs in the
>    "legitimately arch-specific" bucket; the barriers are correct as written.
> 2. **The stub-drift item was fixed by deletion, not by include/assert.**
>    `TASK_ENTRY_MASK` is used by no `.c` in the tree, and `INITIAL_XPSR`'s only
>    consumer is `port.c` (via the real `port.h`) — so the copies in
>    `tests/stubs/port.h` and `tests/stubs/port_stub.h` were dead. Removed them.
>    (A shared header can't fix this cleanly anyway: `portable/cortex-m4` is not
>    on the host include path, so the stub can't include a port-side constants
>    header.) The one live duplicate, `INITIAL_XPSR`, is a fixed ARMv7-M ABI
>    constant (the xPSR Thumb bit) that does not change; left as-is.
>
> Skipped as optional polish: the repeated/inconsistent QEMU machine name across
> the `qemu_*.sh` scripts, and the literal `arm-none-eabi-gcc` toolchain checks
> (now that `PORT_TOOLCHAIN_PREFIX` exists, they could read it).

**Genuinely misplaced (fix):**

- `examples/22_mpu_fault.c:26-30` — raw Thumb opcode `ram_code[0] = 0x4770u;
  /* BX LR */` and bare `__asm volatile("dsb")` / `("isb")` where `V_PORT_MB()`
  already exists in the port.
- `examples/33_mpu_user_demo.c:22-25` — `__asm volatile("mrs %0, control")`;
  use a `v_port_is_privileged()` accessor.
- `examples/11_block_wake_task.c:1, 30-33` — unconditional `#include "navhal.h"`
  and literal `TIM5`, with **no** `#ifdef NAVHAL` guard, so it fails to compile
  on any non-NavHAL build. `examples/benchmark/main.c` already does this
  correctly; match it.
- `tools/coverage_target.sh:90` — hardcoded
  `build/portable/CMakeFiles/portable.dir/cortex-m4`; derive from `VAIOS_PORT`.

**Legitimately arch-specific (leave, they *are* the Cortex-M4 port's tooling):**
`tools/renode.resc` (DWT at `0xE0001000`, 84 MHz), `tools/flash.sh` (ST device-ID
table), `tools/qemu_*.sh`, `tools/run_hw_tests.sh`, `tools/mem_stress_hw.sh`,
`tests/configs/navhal_softfp.config`, `examples/benchmark/bench_dma.c` (STM32F4
DMA2-only M2M is a real silicon constraint). Two nits worth folding in while
we're here: the QEMU machine name is repeated across five scripts and
inconsistent (`netduinoplus2` vs `olimex-stm32-h405` in `tools/qemu_hal.sh:7`),
and the toolchain-existence checks hard-code `arm-none-eabi-gcc` by literal name
in seven places rather than reading `CONFIG_TOOLCHAIN_PREFIX`.

**Test-side drift risk (fix):** `tests/stubs/port.h:32-33` duplicates
`INITIAL_XPSR 0x01000000UL` and `TASK_ENTRY_MASK 0xFFFFFFFEUL` from the real
port header, with a comment admitting it ("same values as the real header"). If
`portable/cortex-m4/port.h` changes, the host suite silently validates the wrong
constants. Have the stub include the port header, or assert equality at compile
time.

---

## Phase 6 — Tripwire

> **Status: IMPLEMENTED.** `tools/check_portability.sh` is the standalone
> portability gate: it fails if inline asm, an arch macro/`#ifdef`, a vendor/
> CMSIS include, or an integer-literal-cast-to-pointer (MMIO) appears in
> `kernel/` or `include/`. Four detectors, all verified against injected
> violations; the clean tree passes with **no allowlist** — the exception the
> plan anticipated (the `ipc.c` priority comparator) is moot, since Phase 3b
> moved it into the port as `v_port_prio_is_more_urgent`. Wired into CI two ways:
> a dedicated fast-failing `Portability tripwire` step in `ci.yml`'s host job,
> and inside `run_static_analysis.sh` (which absorbed and replaced its earlier
> inline MMIO pass — one authoritative script now).
>
> **On the "second arch in CI" acceptance:** already satisfied without a new
> target. The host-tests job builds the entire kernel with x86 gcc under
> `VAIOS_ARCH_HOST` (no ARM macro anywhere) — that *is* a non-ARM build of the
> kernel, exactly the property the plan wanted proven. A `VAIOS_PORT=host`
> compile-only target would need a `portable/host/` port that doesn't exist
> (Phase 4's `FATAL_ERROR` would fire) and would only duplicate what the host
> suite already proves, so it was not added.

Add `tools/check_portability.sh`, run in CI:

```sh
# Fail if hardware-specific patterns appear outside portable/
git grep -nE '__asm|asm volatile|0xE000[0-9A-F]{4}|SysTick|NVIC_|PRIMASK|BASEPRI|\bMSP\b|\bPSP\b|<avr/io\.h>|__arm__|__AVR__|CORTEX_M4' \
  -- kernel/ include/ && exit 1
```

with a small allowlist file for the deliberate exceptions agreed in Phase 3
(e.g. the `ipc.c` priority comparator). Wire into `.github/workflows/ci.yml`.

**The real acceptance test** is a second target in CI. `ci.yml:45-57` installs
only `gcc-arm-none-eabi`, and there is no second arch in the matrix, so nothing
would ever catch a portability regression. Add a **compile-only** `host` port
build (`VAIOS_PORT=host`, `CMAKE_SYSTEM_NAME=Linux`) to the matrix. That is
cheap, needs no new toolchain, and proves the kernel builds without any ARM
macro — which is exactly the property Phases 1-4 establish.

---

## Deferred: word size & stack model

Out of scope for this plan, flagged so nobody assumes it's done. A real AVR port
additionally needs:

- `include/task.h:51` — `uint32_t *sp` bakes in a 32-bit machine word. AVR has
  16-bit pointers; a 64-bit host build is also wrong. Needs a port-defined
  `v_stack_t`, which touches `kernel/task.c:227`, `kernel/task.c:351-357`,
  `kernel/perf.c:257-274`, and the syscall ABI.
- `kernel/task.c:227` — `task->sp = task->mem_block + (size / sizeof(uint32_t))`
  assumes full-descending growth (`VAIOS_ARCH_STACK_DESCENDING` from Phase 1
  gives us the flag; the arithmetic still needs writing).
- `include/task.h:79-95` — `mpu_guard[2]` is an ARMv7-M RBAR/RASR pair. The
  comment claims it "stays architecture-independent" because it's opaque; the
  size `[2]` and semantics say otherwise. It is `#if VAIOS_MPU_*`-gated, so it's
  containable, but the honest fix is a port-sized opaque blob.
- The syscall ABI carries pointers as `uint32_t` (`tests/stubs/syscall_stubs.c:37`,
  `tests/test_syscall.c:26`), with explicit 64-bit-host workarounds already in
  the tests.

Cosmetic, low priority: `include/perf.h:27-35` names ARM peripherals in a
portable struct (`systick_count`, `systick_min_cyc`), and the 84 MHz / ~51 s
CYCCNT wrap figure is repeated in three places (`kernel/perf.c:31`,
`include/perf.h:13, 110-113`) even though the read itself correctly goes through
`v_port_hw_cycle_counter_read()`.

---

## Sequencing

Phases 1 → 2 → 3 are strictly ordered (1 supplies `v_port_arch.h`, which 2 and 3
consume). Phase 4 is independent and can land in parallel. Phase 5 depends on 4
for `VAIOS_PORT`. Phase 6 must be last — the tripwire can only pass on a clean
tree.

Each phase keeps the host suite green and the cortex-m4 firmware building; none
requires a flag day.
