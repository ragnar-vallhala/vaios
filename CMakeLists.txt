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
if(NOT DEFINED CMAKE_PROJECT_NAME)
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

option(VAIOS_FPU "Enable FPU support" OFF)

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
"-mcpu=${CMAKE_SYSTEM_PROCESSOR} -mthumb ${FPU_FLAGS} -Wall -O0 -g -std=c11") # C++
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
  include_directories(extern/NavHAL/include)
  add_subdirectory(extern/NavHAL)
  set(LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/extern/NavHAL/src/board/${BOARD}/linker.ld)
endif()


set(LINKER_FILE ${LINKER_FILE} PARENT_SCOPE)
set(STARTUP_FILE ${STARTUP_FILE} PARENT_SCOPE)

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
