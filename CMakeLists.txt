cmake_minimum_required(VERSION 3.20)

# Prevent macOS-specific flags
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_INIT "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "" CACHE STRING "" FORCE)

# Toolchain settings
set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_BINARY_SIZE ${TOOLCHAIN_PREFIX}size)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Enable $<LINK_GROUP:RESCAN,...> for C (GNU ld --start-group/--end-group). Used
# to resolve the static-lib cycle between the kernel (vaios) and portable libs
# introduced by the SVC syscall dispatch. Not predefined for the bare 'C'
# language, so define it here.
set(CMAKE_C_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_C_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vaios C ASM)
else()
    add_compile_definitions(SUBMODULE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Mirror the generated compile_commands.json to the repo root on every build, so
# clangd / editors (which look at the source root) always see the current DB.
# ALL runs it each `cmake --build`; copy_if_different is a no-op when unchanged.
# (copy_compile_commands is NavHAL's own target — use a distinct name.)
add_custom_target(vaios_copy_compile_commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_BINARY_DIR}/compile_commands.json"
          "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
  BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
  COMMENT "Mirroring compile_commands.json to repo root")

# Defaults
set(CMAKE_SYSTEM_PROCESSOR
    cortex-m4
    CACHE STRING "Target processor" FORCE) # Processor on board
set(BOARD
    nucleo_f401re
    CACHE STRING "Target board")
set(VENDOR
    stm32
    CACHE STRING "Vendor")

# Flasher configuration
if(NOT DEFINED FLASHER)
  set(FLASHER st-flash CACHE STRING "Tool used for flashing")
endif()

if(NOT DEFINED FLASH_ADDRESS)
  set(FLASH_ADDRESS 0x8000000 CACHE STRING "Address to flash the binary to")
endif()

message(STATUS "Selected flasher: ${FLASHER}")
message(STATUS "Selected flash address: ${FLASH_ADDRESS}")

option(VAIOS_FPU "Enable FPU support" ON)
set(VAIOS_EXAMPLE "" CACHE STRING "The example to build (e.g., FPU_USAGE, DMA_DUMP)")

# ---------------------------------------------------------------------------
# Optional kernel modules. Each maps to one kernel/*.c file. OFF excludes that
# file from the build entirely (smaller flash) and defines the matching macro
# to 0 so C code can #if out init calls and APIs. Defaults mirror
# vaios_config_default.h. Override with -DVAIOS_MODULE_X=OFF.
# ---------------------------------------------------------------------------
option(VAIOS_MODULE_TERMINAL    "Build the interactive command terminal"   ON)
option(VAIOS_MODULE_VFS         "Build the virtual file system (FatFs)"    OFF)
option(VAIOS_MODULE_SEMIHOSTING "Build the semihosting debug I/O module"   ON)
option(VAIOS_MODULE_FIFO        "Build the SPSC/MPMC FIFO data structures" ON)
option(VAIOS_MODULE_PERF        "Build the kernel performance counters"    ON)

# Dependency resolution (Kconfig-style 'select').
if(VAIOS_MODULE_TERMINAL AND NOT VAIOS_MODULE_SEMIHOSTING)
  message(WARNING "VAIOS_MODULE_TERMINAL needs semihosting (sh_readc) -- forcing VAIOS_MODULE_SEMIHOSTING ON.")
  set(VAIOS_MODULE_SEMIHOSTING ON CACHE BOOL "Build the semihosting debug I/O module" FORCE)
endif()
if(NOT NAVHAL AND NOT VAIOS_MODULE_SEMIHOSTING)
  message(WARNING "Non-NAVHAL build needs semihosting for systick/pendsv setup -- forcing VAIOS_MODULE_SEMIHOSTING ON.")
  set(VAIOS_MODULE_SEMIHOSTING ON CACHE BOOL "Build the semihosting debug I/O module" FORCE)
endif()

# The VFS examples call vfs_* directly, so they only link when the VFS module
# (kernel/vfs.c) is compiled into libvaios. Gate it on the selected example:
# auto-enable VAIOS_MODULE_VFS for those examples so a plain
# -DVAIOS_EXAMPLE=VFS_PERF build links without a manual -DVAIOS_MODULE_VFS=ON,
# while every other example keeps VFS out of flash.
if(VAIOS_EXAMPLE STREQUAL "VFS_PERF" OR
   VAIOS_EXAMPLE STREQUAL "VFS_CONCURRENT" OR
   VAIOS_EXAMPLE STREQUAL "SD_WRITE_TEST")
  if(NOT VAIOS_MODULE_VFS)
    message(STATUS "VAIOS_EXAMPLE=${VAIOS_EXAMPLE} uses the VFS API -- forcing VAIOS_MODULE_VFS ON.")
    set(VAIOS_MODULE_VFS ON CACHE BOOL "Build the virtual file system (FatFs)" FORCE)
  endif()
endif()

message(STATUS "VAIOS modules: terminal=${VAIOS_MODULE_TERMINAL} vfs=${VAIOS_MODULE_VFS} "
               "semihosting=${VAIOS_MODULE_SEMIHOSTING} fifo=${VAIOS_MODULE_FIFO} "
               "perf=${VAIOS_MODULE_PERF}")

# Heap allocator selection. The facade kernel/memory/memory.c always compiles;
# the free-block index backends kernel/memory/seglist.c (segregated fit) and
# kernel/memory/tlsf.c (TLSF) are both in the build, and VAIOS_HEAP_ALGO decides
# which one compiles a live body. The primary selector is the Kconfig `Heap
# allocator` choice (surfaced as VAIOS_HEAP_ALGO by vaios_config_derived.h), so
# by default this CMake var is empty and defers to it. Set -DVAIOS_HEAP_ALGO=TLSF
# (or =SEGLIST) to FORCE it from the command line instead — emitted via
# add_compile_definitions, which (unlike CMAKE_C_FLAGS, overwritten below)
# survives, and overrides the header.
set(VAIOS_HEAP_ALGO "" CACHE STRING "Heap allocator: SEGLIST, TLSF, or empty (use vaios_config)")
set_property(CACHE VAIOS_HEAP_ALGO PROPERTY STRINGS "" SEGLIST TLSF)
if(VAIOS_HEAP_ALGO STREQUAL "TLSF")
  add_compile_definitions(VAIOS_HEAP_ALGO=1)
  message(STATUS "VAIOS heap allocator: TLSF (forced by CMake)")
elseif(VAIOS_HEAP_ALGO STREQUAL "SEGLIST")
  add_compile_definitions(VAIOS_HEAP_ALGO=0)
  message(STATUS "VAIOS heap allocator: segregated-fit (forced by CMake)")
elseif(VAIOS_HEAP_ALGO STREQUAL "")
  message(STATUS "VAIOS heap allocator: from Kconfig (default segregated-fit)")
else()
  message(FATAL_ERROR "VAIOS_HEAP_ALGO must be SEGLIST, TLSF, or empty (got '${VAIOS_HEAP_ALGO}')")
endif()

# ---------------------------------------------------------------------------
# Kconfig-driven configuration. At configure time, generate vaios_autoconf.h
# (the kernel's config macros) + config.cmake from Kconfig, then force-include
# the header into vaios's own TUs below (Linux `-include autoconf.h` style,
# mirroring NavHAL's navhal_target.h). Precedence is -D > Kconfig > derived.
#
# When NAVHAL is on, vaios's Kconfig `osource`s NavHAL's, so ONE .config /
# menuconfig covers both. That single unified .config is also what drives
# navhal_target.h (staged as extern/NavHAL/.config in the NAVHAL block below) —
# a single source of truth, no NavHAL change (kconfiglib load_config ignores the
# vaios symbols in NavHAL's run and vice-versa via the source filter in
# tools/kconfig.py). See docs/plan/KCONFIG_CONFIG_SYSTEM_PLAN.md.
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VAIOS_AUTOCONF "${CMAKE_BINARY_DIR}/vaios_autoconf.h")
# Which Kconfig .config the build reads. Default is a build-local artifact
# (gitignored), seeded once from defconfig; -DVAIOS_CONFIG=<path> overrides it to
# a shared/checked-in config. The config is NOT bound to the build dir.
#
# Per-example feature needs (fd IPC, the per-task heap, devfs, the MPU faults)
# are NOT separate .config files: each such example is a `config EXAMPLE_<TOKEN>`
# entry in examples/Kconfig whose `select`s pull in the feature closure it needs.
# The generator is told which example is being built (--example-override below),
# activates that choice, and the selects cascade — layered transiently on top of
# the base .config, never written back. So a plain -DVAIOS_EXAMPLE=<X> builds
# with the right features and examples with no special needs are a no-op. This
# mirrors NavHAL's samples/Kconfig SAMPLE_* / --sample-override mechanism.
set(VAIOS_CONFIG "${CMAKE_BINARY_DIR}/.config" CACHE FILEPATH
    "Kconfig .config the build reads (build-local artifact by default)")
set(VAIOS_DEFCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/defconfig" CACHE FILEPATH
    "Kconfig defconfig used to seed VAIOS_CONFIG on first configure")
set(_vaios_dotconfig "${VAIOS_CONFIG}")
# Env the generator needs to pull NavHAL's tree in: NAVHAL_KCONFIG names the file
# to osource (unset -> standalone, sources nothing); srctree roots NavHAL's own
# `source "src/..."` lines. Standalone builds set neither.
set(_vaios_gen_env "")
if(NAVHAL)
  set(_vaios_gen_env ${CMAKE_COMMAND} -E env
      "NAVHAL_KCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/Kconfig"
      "srctree=${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL")
endif()
# Seed the build .config once: vaios defaults, plus (NAVHAL) the NavHAL driver
# set so the single file carries both symbol worlds. menuconfig edits persist.
if(NOT EXISTS "${_vaios_dotconfig}")
  set(_seed "")
  if(EXISTS "${VAIOS_DEFCONFIG}")
    file(READ "${VAIOS_DEFCONFIG}" _d)
    string(APPEND _seed "${_d}\n")
  endif()
  if(NAVHAL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/navhal.config")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/navhal.config" _n)
    string(APPEND _seed "${_n}\n")
  endif()
  file(WRITE "${_vaios_dotconfig}" "${_seed}")
endif()
execute_process(
  COMMAND ${_vaios_gen_env} ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig.py"
          --kconfig "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig"
          --config "${_vaios_dotconfig}"
          --header-out "${VAIOS_AUTOCONF}"
          --cmake-out "${CMAKE_BINARY_DIR}/vaios_config.cmake"
          --example-override "${VAIOS_EXAMPLE}"
  RESULT_VARIABLE _vaios_kconfig_rc)
if(NOT _vaios_kconfig_rc EQUAL 0)
  message(FATAL_ERROR "vaios Kconfig generation failed (rc=${_vaios_kconfig_rc})")
endif()
include("${CMAKE_BINARY_DIR}/vaios_config.cmake")

# Re-run configure (hence regenerate the headers) when the .config changes, so
# editing it or running the menuconfig target takes effect on the next cmake
# without deleting the build dir.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vaios_dotconfig}")

# Interactive configuration: `cmake --build <dir> --target menuconfig` edits the
# unified build .config (vaios symbols + NavHAL's when NAVHAL=ON). Re-run cmake
# afterwards to regenerate the headers. Needs the `menuconfig` python module.
add_custom_target(menuconfig
  COMMAND ${_vaios_gen_env} ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig.py" --menuconfig
          --kconfig "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig" --config "${_vaios_dotconfig}"
  USES_TERMINAL
  COMMENT "vaios menuconfig — re-run cmake afterwards to regenerate config headers")
message(STATUS "VAIOS: Kconfig config generated -> ${VAIOS_AUTOCONF}")

# Mirror NavHAL's FPU setting so vaios's float ABI can never diverge from the HAL
# it links against. NavHAL carries the FPU decision in two places that must agree:
# the CMake `USE_FPU` var (sets -mfloat-abi in cmake/arch/armv7e-m.cmake) and the
# Kconfig `CONFIG_USE_FPU` (-> navhal_target.h NAVHAL_HAS_FPU -> defines
# _FPU_ENABLED, which port.c's context-switch asm keys off). If vaios builds
# soft-float while NavHAL's .config still has CONFIG_USE_FPU=y, _FPU_ENABLED stays
# defined and the `vstmdb/vldmia {s16-s31}` asm fails to assemble. So take the
# canonical NavHAL flag (CONFIG_USE_FPU in the .config NavHAL is configured from)
# as the single source of truth and force VAIOS_FPU to match it. Non-NAVHAL builds
# keep VAIOS_FPU as an independent knob.
if(NAVHAL)
    if(DEFINED NAVHAL_CONFIG_FILE)
        set(_nav_cfg "${NAVHAL_CONFIG_FILE}")
    else()
        set(_nav_cfg "${CMAKE_CURRENT_SOURCE_DIR}/navhal.config")
    endif()
    if(EXISTS "${_nav_cfg}")
        file(STRINGS "${_nav_cfg}" _nav_fpu REGEX "^CONFIG_USE_FPU=y$")
        if(_nav_fpu)
            set(VAIOS_FPU ON CACHE BOOL "Enable FPU support" FORCE)
        else()
            set(VAIOS_FPU OFF CACHE BOOL "Enable FPU support" FORCE)
        endif()
        message(STATUS "VAIOS: FPU mirrored from NavHAL (${_nav_cfg}) -> VAIOS_FPU=${VAIOS_FPU}")
    endif()
endif()

if(VAIOS_FPU)
    set(FPU_FLAGS "-mfloat-abi=hard -mfpu=fpv4-sp-d16")
    add_compile_definitions(_FPU_ENABLED __FPU_PRESENT=1)
    message(STATUS "VAIOS: FPU Support Enabled")
else()
    set(FPU_FLAGS "-mfloat-abi=soft")
    message(STATUS "VAIOS: FPU Support Disabled")
endif()

set(CMAKE_ASM_FLAGS "-mcpu=${CMAKE_SYSTEM_PROCESSOR} -mthumb ${FPU_FLAGS}")

set(CMAKE_C_STANDARD 11)

set(CMAKE_C_FLAGS
"-mcpu=${CMAKE_SYSTEM_PROCESSOR} -mthumb ${FPU_FLAGS} -Wall -O2 -g -std=c11") # C++
# standard
# 11

set(CMAKE_ASM_FLAGS "-mcpu=${CMAKE_SYSTEM_PROCESSOR} -mthumb ${FPU_FLAGS}")

set(CMAKE_EXE_LINKER_FLAGS "-T ${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/src/board/${BOARD}/linker.ld -nostdlib ${FPU_FLAGS} --specs=nosys.specs --specs=nano.specs") # can be

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include  ${CMAKE_CURRENT_SOURCE_DIR}/portable/${CMAKE_SYSTEM_PROCESSOR})

# Arch is now a Kconfig choice (VAIOS_ARCH_CORTEX_M4, emitted into
# vaios_autoconf.h); the kernel reads capability symbols, not a CORTEX_M4
# compile macro, so no add_compile_definitions is needed here. Phase 4 of
# docs/plan/PORTABILITY_CLEANUP_PLAN.md moves the remaining arch flags into a
# per-port arch.cmake.

if(NAVHAL)
  add_compile_definitions(NAVHAL)
  # NavHAL is always embedded inside vaios. Define SUBMODULE so NavHAL cedes
  # the SysTick / PendSV / SVCall / HardFault vectors to vaios (which owns
  # them in port.c / utils.c). Without it NavHAL's timebase.c emits a strong
  # SysTick_Handler that collides with vaios's at link time.
  add_compile_definitions(SUBMODULE)
  include_directories(extern/NavHAL/include)

  # Who provides NavHAL? When a superproject has already added it (e.g. a nav
  # firmware project that does add_subdirectory(extern/NavHAL) before pulling
  # vaios in as a [dependencies] library), the `hal` target already exists —
  # share it instead of adding NavHAL a second time, which would collide on the
  # `hal`/`common` targets. Only embed our own NavHAL when nobody else has.
  # See nav/docs/vaios-as-library.md.
  if(NOT TARGET hal)
    set(USE_FPU ${VAIOS_FPU} CACHE BOOL "Pass FPU setting to NavHAL" FORCE)
    # Stage the config NavHAL reads (extern/NavHAL/.config): the unified vaios
    # .config (its CONFIG_VAIOS_* are ignored by NavHAL's load_config) with
    # navhal.config appended so NavHAL's driver set is ALWAYS present and
    # authoritative — appended last, its values win. This is the robustness
    # fix: a build .config first seeded WITHOUT -DNAVHAL=ON has no CONFIG_DRV_*,
    # and copying it verbatim silently compiled every NavHAL driver out (SDIO,
    # MPU, ...). navhal.config remains the source of truth for the HAL driver
    # set; the unified menuconfig still shows both trees. The file is gitignored,
    # so NavHAL stays an unmodified submodule; rewriting it re-triggers configure.
    set(_nav_staged "")
    if(EXISTS "${_vaios_dotconfig}")
      file(READ "${_vaios_dotconfig}" _vc)
      string(APPEND _nav_staged "${_vc}\n")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/navhal.config")
      file(READ "${CMAKE_CURRENT_SOURCE_DIR}/navhal.config" _nc)
      string(APPEND _nav_staged "${_nc}\n")
    endif()
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/.config" "${_nav_staged}")
    add_subdirectory(extern/NavHAL)
    # NavHAL hardcodes -O0 in its own CMAKE_C_FLAGS. Rather than editing the
    # submodule, override per target from here — gcc honours the last -O flag,
    # so a trailing -O2 wins. Keeps NavHAL (incl. the DWT cycle counter)
    # optimized without dirtying the submodule.
    target_compile_options(hal    PRIVATE -O2)
    target_compile_options(common PRIVATE -O2)
    set(LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/src/board/${BOARD}/linker.ld)
  endif()

  # Force-include the Kconfig-generated navhal_target.h into vaios's OWN
  # translation units, mirroring NavHAL's own `-include` (which is scoped to the
  # submodule). NavHAL's capability flags (NAVHAL_CONFIG_DRV_*) live only in this
  # generated header — the hal_config.h/navhal_port_config.h chain is an inert
  # stub. Capability-gated NavHAL headers wrap their whole body in
  # `#if NAVHAL_CONFIG_DRV_*` (common/hal_sdio.h today, hal_eth.h next), so
  # without the flag ambient in vaios sources those APIs compile out and their
  # symbols vanish from consumers like portable/cortex-m4/port_hw.c. The header
  # is generated at configure time (before any TU compiles) and its dir is
  # already on the include path. Applies to portable/kernel/examples added below.
  # SHELL: keeps the two-token option intact — CMake otherwise de-duplicates the
  # bare `-include` against vaios's own force-include below, silently turning the
  # second header into an input file ("cannot specify -o with -c ... multiple
  # files"). See the vaios_autoconf force-include near add_subdirectory(portable).
  add_compile_options("SHELL:-include ${CMAKE_BINARY_DIR}/navhal_target.h")
endif()


get_directory_property(has_parent PARENT_DIRECTORY)
if(has_parent)
  set(LINKER_FILE ${LINKER_FILE} PARENT_SCOPE)
  set(STARTUP_FILE ${STARTUP_FILE} PARENT_SCOPE)
endif()

message("LINKER_FILE for VAIOS: ${LINKER_FILE}")
message("STARTUP_FILE for VAIOS: ${STARTUP_FILE}")

# The on-target unit-test runner (VAIOS_EXAMPLE=UNIT_TESTS) runs the host
# suites against the real kernel. Some allocator/stack tests deliberately
# exhaust the heap / shrink stacks; disable the watermark panics for that
# build only, exactly as tests/CMakeLists.txt does for the host suite.
if(VAIOS_EXAMPLE STREQUAL "UNIT_TESTS")
  add_compile_definitions(HEAP_WATERMARK_ENABLE=0 TASK_STACK_WATERMARK_ENABLE=0)
endif()

# Force-include the generated vaios config into vaios's OWN sources only
# (portable/kernel/examples/tests, added below) — NOT NavHAL, which was added
# above and owns its own config via navhal_target.h. Order matters: the
# Kconfig-generated scalars first, then the derived/composite layer that builds
# on them. SHELL: prevents CMake from de-duplicating the `-include` tokens
# against each other and NavHAL's force-include (which would drop a header's
# -include and make it an input file).
add_compile_options("SHELL:-include ${VAIOS_AUTOCONF}")
add_compile_options("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/vaios_config_derived.h")

# ---------------------------------------------------------------------------
# On-target gcov coverage (VAIOS_GCOV). Instruments vaios's OWN translation
# units — portable/kernel/examples, added below — with --coverage so a run under
# semihosting (QEMU/Renode) can measure the ARM-only enforcement code host gcov
# cannot reach (context switch, MPU, SVC dispatch, fault handlers, semihosting).
# NavHAL was added above and keeps its own uninstrumented flags. Plain
# --coverage (NOT -fprofile-info-section) so the embedded .gcda path survives for
# __gcov_info_to_gcda; tools/gcov_sections.ld (prepended -> parsed first) bounds
# .init_array; --coverage at link pulls the correct multilib libgcov. The dumper
# lives in portable/cortex-m4/gcov_dump.c; drive it via tools/coverage_target.sh.
# Default OFF: normal builds are byte-for-byte unaffected.
# ---------------------------------------------------------------------------
option(VAIOS_GCOV "Instrument vaios for on-target gcov coverage (.gcda streamed over UART)" OFF)
if(VAIOS_GCOV)
  # -O0 (trailing, overrides the -O2 in CMAKE_C_FLAGS) so gcov attributes
  # lines/branches to source faithfully, matching the host coverage.sh. Note the
  # instruction-count cost: an instrumented image runs slowly under Renode's
  # virtual-time model, so keep on-target coverage runners lean (the no-scheduler
  # UNIT_TESTS runner is the validated CI path; scheduler-heavy runners are far
  # slower and better exercised on real hardware, which runs at full speed).
  string(APPEND CMAKE_C_FLAGS " --coverage -O0")
  set(CMAKE_EXE_LINKER_FLAGS
      "-T ${CMAKE_CURRENT_SOURCE_DIR}/tools/gcov_sections.ld ${CMAKE_EXE_LINKER_FLAGS} --coverage")
  add_compile_definitions(VAIOS_GCOV)
  # gcov's per-arc counter arrays inflate .bss substantially, pushing _heap_start
  # up. The default 88 KB heap (memset in full by v_heap_memory_init) then runs
  # off the end of the 96 KB SRAM and faults before the suite starts. Shrink the
  # heap for coverage builds so .bss + heap + stack fit; still ample for the
  # on-target unit suite's allocations. Tunable via -DVAIOS_GCOV_HEAP_SIZE.
  set(VAIOS_GCOV_HEAP_SIZE 32768 CACHE STRING
      "Heap size (bytes) for -DVAIOS_GCOV builds (smaller so gcov .bss + heap fit RAM)")
  add_compile_definitions(HEAP_SIZE=${VAIOS_GCOV_HEAP_SIZE})
  message(STATUS "VAIOS: on-target gcov instrumentation ENABLED (UART .gcda dump); "
                 "HEAP_SIZE forced to ${VAIOS_GCOV_HEAP_SIZE} for RAM fit")
endif()

add_subdirectory(portable) # implementations for specific architectures (context switching, interrupt handling)
add_subdirectory(kernel) # The heart of your RTOS (scheduling, tasks, synchronization, etc.) — totally architecture independent
if(EXAMPLES)
add_subdirectory(examples) # Small test apps showing how to use the RTOS
endif()
if(TEST)
  enable_testing()
  add_subdirectory(tests) # For unit tests 
endif()
