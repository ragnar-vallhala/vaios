# =========================================================================
# tests/CMakeLists.txt  –  Host-native test build for vaios RTOS
#
# Build:
#   cmake -S .. -B ../build_tests -DTEST=ON \
#         -DCMAKE_C_COMPILER=gcc -DCMAKE_SYSTEM_NAME=Linux
#   cmake --build ../build_tests
#   ctest --test-dir ../build_tests --output-on-failure
# =========================================================================
cmake_minimum_required(VERSION 3.20)

# -------------------------------------------------------------------------
# Include paths
#   1. The real <include/> headers (memory.h, task.h, ipc.h, etc.)
#   2. This directory's stub port.h – must shadow portable/cortex-m4/port.h
# -------------------------------------------------------------------------
set(VAIOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(TEST_INCLUDE_DIRS
    "${VAIOS_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"  # port_stub.h → included as "port.h"
)

# -------------------------------------------------------------------------
# Kernel source files compiled natively (no arm-none-eabi toolchain)
# -------------------------------------------------------------------------
set(KERNEL_SOURCES
    "${VAIOS_ROOT}/kernel/memory.c"
    "${VAIOS_ROOT}/kernel/task.c"
    "${VAIOS_ROOT}/kernel/ipc.c"
    "${VAIOS_ROOT}/kernel/structure.c"
    "${VAIOS_ROOT}/kernel/vfs.c"
    "${VAIOS_ROOT}/kernel/vaios.c"
    "${VAIOS_ROOT}/kernel/terminal.c"
    "${VAIOS_ROOT}/kernel/perf.c"
)

# -------------------------------------------------------------------------
# Stub sources
# -------------------------------------------------------------------------
set(STUB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/v_fs_stub.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"  # v_port_hw_* facade
)

# -------------------------------------------------------------------------
# Test sources per suite
# -------------------------------------------------------------------------
set(TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_task_list.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_ipc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_structure.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_vfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_vaios.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_perf.c"
)

# -------------------------------------------------------------------------
# Build the test executable
# -------------------------------------------------------------------------
add_executable(vaios_tests
    ${KERNEL_SOURCES}
    ${STUB_SOURCES}
    ${TEST_SOURCES}
)

target_include_directories(vaios_tests PRIVATE ${TEST_INCLUDE_DIRS})

target_compile_definitions(vaios_tests PRIVATE
    CORTEX_M4              # required by task.h build guard
    HEAP_WATERMARK_ENABLE=0 # don't trip the heap-watermark v_panic when a
                            # test deliberately exhausts the allocator
    TASK_STACK_WATERMARK_ENABLE=0 # likewise for the stack-overflow check —
                            # the host has no real task stack to overflow
    STATIC_SEMAPHORE_SIZE=64 # rmutex_t on a 64-bit host has 8-byte pointers
                             # (sema_t + owner + recursion_count + next_held),
                             # which exceeds the 32-byte target-side default.
                             # 64 is comfortably larger than both layouts.
)

target_compile_options(vaios_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    # Allow the stub port.h to shadow the real port.h via include order
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

# -------------------------------------------------------------------------
# Register with CTest
# -------------------------------------------------------------------------
enable_testing()
add_test(NAME vaios_unit_tests COMMAND vaios_tests)
set_tests_properties(vaios_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
)

# -------------------------------------------------------------------------
# vaios_utils_tests — second executable focused on kernel/utils.c.
#
# utils.c defines v_log / v_memset / v_memcpy / v_strlen / v_strcmp /
# v_panic; the main vaios_tests binary stubs those out so the other suites
# can compile cleanly. Linking utils.c into that binary would collide. Run
# this isolated executable to exercise the real formatter (print_fmt_buf,
# v_atof, and the static itoa/utoa it dispatches to).
# -------------------------------------------------------------------------
add_executable(vaios_utils_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/utils_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_utils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/utils_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"  # v_port_hw_* facade
    "${VAIOS_ROOT}/kernel/utils.c"
    "${VAIOS_ROOT}/kernel/perf.c"  # SysTick_Handler in utils.c hooks perf.
)
target_include_directories(vaios_utils_tests PRIVATE ${TEST_INCLUDE_DIRS})
# Force VFS off so kernel/perf.c's CSV writer doesn't drag vfs_open/etc into
# this binary — vaios_utils_tests does not link vfs.c.
target_compile_definitions(vaios_utils_tests PRIVATE CORTEX_M4 VAIOS_MODULE_VFS=0)
target_compile_options(vaios_utils_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_utils_unit_tests COMMAND vaios_utils_tests)
set_tests_properties(vaios_utils_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
)
