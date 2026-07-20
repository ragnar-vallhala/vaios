# vaios — Portability Cleanup

Phased plan to remove hardware-specific code that has leaked outside
`portable/`, so that adding a second architecture is a matter of writing a new
`portable/<arch>/` rather than editing `kernel/`, `include/`, the root
`CMakeLists.txt`, and 12 build scripts. Every `file:line` anchor below was
verified against the tree at time of writing (branch `dev`, 2026-07-21).

> **Status: PROPOSED.** No phase implemented yet.

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

`portable/<arch>/Kconfig` declares the arch and `select`s what it can do; the
capability symbols are hidden (no prompt), so only the arch sets them:

```kconfig
choice
    prompt "Target architecture"
    default ARCH_CORTEX_M4

config ARCH_CORTEX_M4
    bool "ARM Cortex-M4"
    select ARCH_HAS_MPU
    select ARCH_HAS_IRQ_PRIORITY
    select ARCH_STACK_DESCENDING

config ARCH_HOST
    bool "Host (POSIX — test and analysis builds)"
    select ARCH_STACK_DESCENDING

config ARCH_AVR
    bool "AVR (8-bit)"
endchoice

config ARCH_HAS_MPU          # hidden — selected by the arch, never by the user
    bool
config ARCH_HAS_IRQ_PRIORITY
    bool
config ARCH_STACK_DESCENDING
    bool
config ARCH_MPU_MIN_REGION
    int
    default 128 if ARCH_CORTEX_M4   # ARMv7-M: power-of-two, ≥ exception frame
```

### What this fixes beyond the `#error`

The existing MPU menu (`Kconfig:163-240`) is well built — seven symbols with
correct `depends on` chains — but nothing ties it to the target actually having
an MPU. `VAIOS_MPU_ENABLE` (`Kconfig:164`) is titled "Enable the ARMv7-M MPU"
and its help text defers the question to a **runtime** check
(`hal_mpu_present`, `Kconfig:169`). Add one line:

```kconfig
config VAIOS_MPU_ENABLE
    bool "Enable hardware memory protection (MPU)"
    depends on ARCH_HAS_MPU          # <-- new
```

and the entire MPU subtree — stack guard, static protect, flash RO, null guard,
user separation — disappears from `menuconfig` on an arch that has no MPU. That
is strictly better than an `#error` or a port `#define`: the invalid
configuration becomes unrepresentable rather than diagnosed after the fact. The
same applies to `NVIC_PRIO_BITS` (`Kconfig:36-38`, "implemented by the
silicon"), which moves under `depends on ARCH_HAS_IRQ_PRIORITY`.

### The `task.h` guard

With a Kconfig `choice`, exactly one arch is always selected — the invariant the
`#error` was hand-checking is now structural. The guard reduces to a sanity
check that generated config is actually on the include path:

```c
#include "vaios_autoconf.h"
#ifndef CONFIG_ARCH_STACK_DESCENDING   /* any always-defined arch symbol */
#error "vaios_autoconf.h not found — run the Kconfig step before building"
#endif
```

Kernel code then reads `CONFIG_ARCH_HAS_MPU` / `CONFIG_VAIOS_MPU_ENABLE`, never
an arch name. **`CORTEX_M4` stops being a compile flag entirely** — it exists
only as the Kconfig symbol `ARCH_CORTEX_M4`, consumed by
`portable/cortex-m4/`.

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
3 KB stack. Gate on `CONFIG_VAIOS_MPU_ENABLE` and take the bound from
`CONFIG_ARCH_MPU_MIN_REGION` (Phase 1). Note this makes the constraint follow
the *feature*, not the *chip*: a Cortex-M4 build with the MPU switched off also
regains arbitrary stack sizes, which is correct and is not true today.

**Acceptance:** `grep -rnE '0xE000|0x2000[0-9A-F]{4}|__asm|SysTick_Handler' kernel/ include/`
returns nothing.

---

## Phase 3 — Relocate arch-owned headers out of `include/`

These don't hard-break other ports (nothing forces their inclusion) but they
violate the layering rule outright, and they're what makes the seam ambiguous
to a contributor.

| Move | Why |
|---|---|
| `include/semihosting.h` → `portable/cortex-m4/` | Whole file is ARM: semihosting op numbers `SYS_OPEN 0x01 … SYS_EXIT 0x18` (lines 6-30) plus `set_systick_interrupt_priority()` / `set_pendsv_interrupt_priority()` (37-38) — Cortex-M system exceptions declared in the portable tree. |
| `include/syscall.h:108-143` → `portable/cortex-m4/port_syscall.h` | `mrs %0, ipsr`, `svc 1`, and `register uint32_t r12 __asm__("r12")` pin the ARMv7-M trap ABI. Today's two configs work (`#if VAIOS_HOST_TEST` stub + `#if VAIOS_SYSCALL_SVC`), but there is no third branch, so no other port can have SVC syscalls. `include/syscall.h` keeps the portable prototypes and includes the port header. |
| `include/vaios_config_derived.h:17-26` → port | `#define __NVIC_PRIO_BITS` and `MAX_SYSCALL_INTERRUPT_PRIORITY (… << (8 - __NVIC_PRIO_BITS))` hardcode ARM's 8-bit, high-bit-justified, lower-is-more-urgent priority register — meaningless where `CONFIG_ARCH_HAS_IRQ_PRIORITY` is unset. Also adopts the CMSIS reserved identifier `__NVIC_PRIO_BITS`. The derived composite moves to the port; the *input* stays the `NVIC_PRIO_BITS` Kconfig symbol. |
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
Leave the logic, but source the `16` from `CONFIG_ARCH_FIRST_EXTERNAL_IRQ` (arch
Kconfig, `default 16 if ARCH_CORTEX_M4`) and put the inverted comparison behind a
`v_port_prio_is_more_urgent(a, b)` inline — code-bodied, so it belongs in the
port header per the Phase 1 rule. The block sits under
`#if CONFIG_ARCH_HAS_IRQ_PRIORITY`: an arch with no priority levels has no
question to answer.

---

## Phase 4 — Make `portable/` the real port-selection point

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
  assumes full-descending growth (`CONFIG_ARCH_STACK_DESCENDING` from Phase 1
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
