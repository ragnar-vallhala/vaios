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
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vaios C ASM)
else()
    add_compile_definitions(SUBMODULE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_COMMANDS_OUTPUT "${CMAKE_SOURCE_DIR}/compile_commands.json")

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
set(VAIOS_CONFIG_FILE "" CACHE STRING "Path to the user-provided vaios_config.h file")

# ---------------------------------------------------------------------------
# Optional kernel modules. Each maps to one kernel/*.c file. OFF excludes that
# file from the build entirely (smaller flash) and defines the matching macro
# to 0 so C code can #if out init calls and APIs. Defaults mirror
# vaios_config_default.h. Override with -DVAIOS_MODULE_X=OFF.
# ---------------------------------------------------------------------------
option(VAIOS_MODULE_TERMINAL    "Build the interactive command terminal"   ON)
option(VAIOS_MODULE_VFS         "Build the virtual file system (FatFs)"    ON)
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

message(STATUS "VAIOS modules: terminal=${VAIOS_MODULE_TERMINAL} vfs=${VAIOS_MODULE_VFS} "
               "semihosting=${VAIOS_MODULE_SEMIHOSTING} fifo=${VAIOS_MODULE_FIFO} "
               "perf=${VAIOS_MODULE_PERF}")

if(VAIOS_CONFIG_FILE)
    add_compile_definitions(VAIOS_CONFIG_FILE="${VAIOS_CONFIG_FILE}")
    message(STATUS "VAIOS: Using custom config file ${VAIOS_CONFIG_FILE}")
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

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "cortex-m4")
  add_compile_definitions(CORTEX_M4)
endif()

if(NAVHAL)
  add_compile_definitions(NAVHAL)
  # NavHAL is always embedded inside vaios. Define SUBMODULE so NavHAL cedes
  # the SysTick / PendSV / SVCall / HardFault vectors to vaios (which owns
  # them in port.c / utils.c). Without it NavHAL's timebase.c emits a strong
  # SysTick_Handler that collides with vaios's at link time.
  add_compile_definitions(SUBMODULE)
  include_directories(extern/NavHAL/include)
  set(USE_FPU ${VAIOS_FPU} CACHE BOOL "Pass FPU setting to NavHAL" FORCE)
  # NavHAL's Kconfig reads extern/NavHAL/.config from a hardcoded path. Keep
  # the authoritative copy here in the vaios repo (navhal.config) so NavHAL
  # stays an unmodified submodule, and stage it into place before NavHAL's
  # CMake runs kconfig.py. .config is gitignored inside NavHAL, so this never
  # dirties the submodule. Editing navhal.config re-triggers configure.
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/navhal.config
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/.config
    COPYONLY)
  add_subdirectory(extern/NavHAL)
  # NavHAL hardcodes -O0 in its own CMAKE_C_FLAGS. Rather than editing the
  # submodule, override per target from here — gcc honours the last -O flag,
  # so a trailing -O2 wins. Keeps NavHAL (incl. the DWT cycle counter)
  # optimized without dirtying the submodule.
  target_compile_options(hal    PRIVATE -O2)
  target_compile_options(common PRIVATE -O2)
  set(LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/src/board/${BOARD}/linker.ld)
endif()


get_directory_property(has_parent PARENT_DIRECTORY)
if(has_parent)
  set(LINKER_FILE ${LINKER_FILE} PARENT_SCOPE)
  set(STARTUP_FILE ${STARTUP_FILE} PARENT_SCOPE)
endif()

message("LINKER_FILE for VAIOS: ${LINKER_FILE}")
message("STARTUP_FILE for VAIOS: ${STARTUP_FILE}")

add_subdirectory(portable) # implementations for specific architectures (context switching, interrupt handling)
add_subdirectory(kernel) # The heart of your RTOS (scheduling, tasks, synchronization, etc.) — totally architecture independent
if(EXAMPLES)
add_subdirectory(examples) # Small test apps showing how to use the RTOS
endif()
if(TEST)
  enable_testing()
  add_subdirectory(tests) # For unit tests 
endif()
