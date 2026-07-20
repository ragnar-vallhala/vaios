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
# AddressSanitizer + UBSan for the host suites. Directory-scoped so it applies
# to every test target defined below (they are all created after this call).
# This is what makes the Stage-5 memory-safety regressions observable: an
# out-of-bounds write (e.g. the SYS_wait wnodes overrun) or a wild-pointer
# dereference (unvalidated realloc) aborts instead of silently corrupting.
# -fno-sanitize-recover=all makes UB a hard failure so a bug can't slip past
# ctest's "0 failed" gate. Toggle off with -DVAIOS_TEST_SANITIZE=OFF.
# -------------------------------------------------------------------------
option(VAIOS_TEST_SANITIZE "Build host tests with AddressSanitizer + UBSan" ON)
if(VAIOS_TEST_SANITIZE)
  add_compile_options(-fsanitize=address,undefined
                      -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# -------------------------------------------------------------------------
# GCC static analyzer (-fanalyzer). Complements the runtime sanitizers above:
# ASan/UBSan only see paths the tests execute, whereas -fanalyzer reasons about
# the kernel sources statically (null derefs, leaks, use-after-free, bad
# variadics) on paths no test reaches. Off by default so the normal ASan test
# loop stays fast (~4s); tools/run_static_analysis.sh flips it ON for the gate.
# Known false positives (custom byte-arena heap, best-effort panic formatter)
# are suppressed at their sites with GCC-only pragmas.
# -------------------------------------------------------------------------
option(VAIOS_TEST_ANALYZER "Build host tests with the GCC -fanalyzer static analyzer" OFF)
if(VAIOS_TEST_ANALYZER)
  add_compile_options(-fanalyzer)
endif()

# ASan's leak detector would flag the RTOS's static heap (never freed by
# design) and libc allocations the tests don't own; we care about OOB/UAF here,
# not leaks. Applied to each test via set_tests_properties(... ENVIRONMENT ...).
set(VAIOS_SANITIZER_ENV
    "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1"
    "UBSAN_OPTIONS=print_stacktrace=1")

# -------------------------------------------------------------------------
# Kconfig-generated config. The host build has no NavHAL and no static default
# header any more, so it configures from the same Kconfig (standalone: NavHAL is
# not sourced) and force-includes the generated scalars + derived layer into
# every TU below. Binary-specific overrides (VFS, watermarks, semaphore size)
# stay as per-target -D and still win — precedence is -D > Kconfig > derived.
# -------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_vaios_autoconf "${CMAKE_BINARY_DIR}/vaios_autoconf.h")
# Settable like the main build (-DVAIOS_CONFIG=<path>); defaults build-local.
set(VAIOS_CONFIG "${CMAKE_BINARY_DIR}/.config" CACHE FILEPATH
    "Kconfig .config the host-test build reads")
set(VAIOS_DEFCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/host.defconfig" CACHE FILEPATH
    "Kconfig defconfig used to seed VAIOS_CONFIG (selects the host port)")
if(NOT EXISTS "${VAIOS_CONFIG}" AND EXISTS "${VAIOS_DEFCONFIG}")
  configure_file("${VAIOS_DEFCONFIG}" "${VAIOS_CONFIG}" COPYONLY)
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} "${VAIOS_ROOT}/tools/kconfig.py"
          --kconfig "${VAIOS_ROOT}/Kconfig"
          --config "${VAIOS_CONFIG}"
          --header-out "${_vaios_autoconf}"
  RESULT_VARIABLE _vaios_kconfig_rc)
if(NOT _vaios_kconfig_rc EQUAL 0)
  message(FATAL_ERROR "vaios Kconfig generation failed (rc=${_vaios_kconfig_rc})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VAIOS_CONFIG}")
add_compile_options("SHELL:-include ${_vaios_autoconf}")
add_compile_options("SHELL:-include ${VAIOS_ROOT}/include/vaios_config_derived.h")

# -------------------------------------------------------------------------
# Kernel source files compiled natively (no arm-none-eabi toolchain)
# -------------------------------------------------------------------------
set(KERNEL_SOURCES
    # Heap facade + both index backends. VAIOS_HEAP_ALGO (vaios_config) makes
    # exactly one backend compile a body, the other an empty TU — no clash.
    "${VAIOS_ROOT}/kernel/memory/memory.c"
    "${VAIOS_ROOT}/kernel/memory/seglist.c"
    "${VAIOS_ROOT}/kernel/memory/tlsf.c"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"  # counters + suite runners (once)
    "${CMAKE_CURRENT_SOURCE_DIR}/test_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_task_list.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_ipc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_structure.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_vfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_vaios.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_perf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_uaccess.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_stage5_regress.c"  # expected-fail regressions
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
    VAIOS_MODULE_VFS=1     # VFS is off by default now; this binary links
                           # kernel/vfs.c + test_vfs.c and exercises perf.c's
                           # VFS-gated CSV path, so opt it in explicitly.
    VAIOS_HOST_TEST        # drops target-SRAM pointer assertions so the
                           # scheduler core (get_next_task) is host-testable
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
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"  # counters + suite runners (once)
    "${CMAKE_CURRENT_SOURCE_DIR}/test_utils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/utils_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"  # v_port_hw_* facade
    "${VAIOS_ROOT}/kernel/utils.c"
    "${VAIOS_ROOT}/kernel/perf.c"  # v_kernel_tick in utils.c hooks perf.
)
target_include_directories(vaios_utils_tests PRIVATE ${TEST_INCLUDE_DIRS})
# Force VFS off so kernel/perf.c's CSV writer doesn't drag vfs_open/etc into
# this binary — vaios_utils_tests does not link vfs.c.
target_compile_definitions(vaios_utils_tests PRIVATE VAIOS_MODULE_VFS=0)
target_compile_options(vaios_utils_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_utils_unit_tests COMMAND vaios_utils_tests)
set_tests_properties(vaios_utils_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)

# -------------------------------------------------------------------------
# vaios_devfs_tests — third executable for kernel/devfs.c.
#
# devfs.c is #if VAIOS_DEVFS and ties into the per-task fd table in the TCB, so
# it's built here in isolation with DEVFS forced on and SVC off (the trap-once
# shims compile out; the bodies run directly). Doesn't link task.c — a synthetic
# current_task (devfs_stubs.c) plus the console facade (port_hw_stub) suffice.
# -------------------------------------------------------------------------
add_executable(vaios_devfs_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/devfs_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_devfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/devfs_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"  # console facade
    "${VAIOS_ROOT}/kernel/devfs.c"
)
target_include_directories(vaios_devfs_tests PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(vaios_devfs_tests PRIVATE
    VAIOS_DEVFS=1 VAIOS_MAX_FDS=8 VAIOS_MODULE_VFS=0)
target_compile_options(vaios_devfs_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_devfs_unit_tests COMMAND vaios_devfs_tests)
set_tests_properties(vaios_devfs_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)

# -------------------------------------------------------------------------
# vaios_taskheap_tests — fourth executable for the per-task heap.
#
# The main binary leaves VAIOS_TASK_HEAP off, so memory.c's per-task
# malloc/free/calloc/realloc never ran host-side (STAGE5_REVIEW_FINDINGS #3, #9).
# Build memory.c with VAIOS_TASK_HEAP=1 and SVC off (bodies run directly) against
# a synthetic current_task (taskheap_stubs.c) — no task.c/scheduler needed. The
# task block is a real ASan object, so an allocator that escapes it aborts.
# -------------------------------------------------------------------------
add_executable(vaios_taskheap_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/taskheap_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_taskheap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/taskheap_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs.c"          # _heap_start, v_mem*, v_panic
    "${VAIOS_ROOT}/kernel/memory/memory.c"
    "${VAIOS_ROOT}/kernel/memory/seglist.c"
    "${VAIOS_ROOT}/kernel/memory/tlsf.c"
)
target_include_directories(vaios_taskheap_tests PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(vaios_taskheap_tests PRIVATE
    VAIOS_HOST_TEST
    VAIOS_TASK_HEAP=1          # opt the per-task allocator in
    VAIOS_MODULE_VFS=0
    HEAP_WATERMARK_ENABLE=0
    TASK_STACK_WATERMARK_ENABLE=0
    STATIC_SEMAPHORE_SIZE=64
    # The per-task allocator is literally named malloc/free/calloc/realloc; on
    # the host those would override libc (ASan's own init calls malloc), so
    # rename them for this binary. memory.c defines the renamed symbols and
    # test_taskheap.c calls them via the same macro — libc's malloc stays intact.
    # Safe target-wide: no sibling TU here calls these identifiers as code.
    malloc=vtask_malloc
    free=vtask_free
    calloc=vtask_calloc
    realloc=vtask_realloc)
target_compile_options(vaios_taskheap_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_taskheap_unit_tests COMMAND vaios_taskheap_tests)
set_tests_properties(vaios_taskheap_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)

# -------------------------------------------------------------------------
# vaios_syscall_tests — fifth executable for the SVC syscall dispatch.
#
# The main binary does not link portable/cortex-m4/syscall.c, so
# v_syscall_dispatch — the Stage-5 pointer-validation switch + privileged-only
# IPC gate — was never host-tested (STAGE5_REVIEW_FINDINGS #2, and the dispatch
# side of #1/#3). Build it with SVC + MPU_USER_SEPARATION on (DEVFS/IPC_FD/
# TASK_HEAP off, so the validation runs while device/fd/heap cases fall through
# to `default: -1`), against the real validators in task.c and a synthetic
# caller whose privilege the test flips (syscall_stubs.c). The two ARM-asm seams
# (CONTROL.nPRIV read, the svc trampolines) resolve to host hooks under
# VAIOS_HOST_TEST — see syscall.c and include/syscall.h.
# -------------------------------------------------------------------------
add_executable(vaios_syscall_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/syscall_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_syscall.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/syscall_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"
    "${VAIOS_ROOT}/portable/cortex-m4/syscall.c"
    "${VAIOS_ROOT}/kernel/task.c"
    "${VAIOS_ROOT}/kernel/ipc.c"
    "${VAIOS_ROOT}/kernel/devfs.c"                    # fd table (IPC_FD/DEVFS)
    "${VAIOS_ROOT}/kernel/memory/memory.c"
    "${VAIOS_ROOT}/kernel/memory/seglist.c"
    "${VAIOS_ROOT}/kernel/memory/tlsf.c"
)
target_include_directories(vaios_syscall_tests PRIVATE ${TEST_INCLUDE_DIRS})
# The faithful Stage-5 unprivileged config: unprivileged tasks use fd-typed IPC,
# so DEVFS + IPC_FD are on (that's also where VAIOS_MAX_FDS — the SYS_wait bound —
# is defined). TASK_HEAP stays off to avoid the libc malloc-symbol clash.
target_compile_definitions(vaios_syscall_tests PRIVATE
    VAIOS_HOST_TEST
    VAIOS_SYSCALL_SVC=1            # compile the dispatch + svc host hooks
    VAIOS_MPU_USER_SEPARATION=1    # compile the validation switch + priv gate
    VAIOS_DEVFS=1                  # fd table
    VAIOS_IPC_FD=1                 # fd-typed IPC + SYS_wait validation
    VAIOS_MAX_FDS=8
    VAIOS_MODULE_VFS=0
    HEAP_WATERMARK_ENABLE=0
    TASK_STACK_WATERMARK_ENABLE=0
    STATIC_SEMAPHORE_SIZE=64)
target_compile_options(vaios_syscall_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_syscall_unit_tests COMMAND vaios_syscall_tests)
set_tests_properties(vaios_syscall_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)

# -------------------------------------------------------------------------
# vaios_ipcfd_tests — sixth executable for the fd-typed IPC layer.
#
# DEVFS + IPC_FD on, SVC off (bodies run directly against a real current_task
# built via task_create). Covers the teardown/name/recursion findings
# (STAGE5_REVIEW_FINDINGS #4 named-object leak on exit, #7 over-long name,
# #12 fd-mutex owner re-lock).
# -------------------------------------------------------------------------
add_executable(vaios_ipcfd_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/ipcfd_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_ipcfd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/syscall_stubs.c"  # svc host hooks + perf/kmsg stubs
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"
    "${VAIOS_ROOT}/portable/cortex-m4/syscall.c"          # v_wait etc. trap via svc
    "${VAIOS_ROOT}/kernel/task.c"
    "${VAIOS_ROOT}/kernel/ipc.c"
    "${VAIOS_ROOT}/kernel/devfs.c"
    "${VAIOS_ROOT}/kernel/memory/memory.c"
    "${VAIOS_ROOT}/kernel/memory/seglist.c"
    "${VAIOS_ROOT}/kernel/memory/tlsf.c"
)
target_include_directories(vaios_ipcfd_tests PRIVATE ${TEST_INCLUDE_DIRS})
# IPC_FD implies SVC (the fd-IPC wrappers like v_wait trap unconditionally). SVC
# on; MPU_USER off, so the dispatch routes straight to the bodies (no validation
# switch). The test calls the public IPC API, which funnels through the dispatch.
target_compile_definitions(vaios_ipcfd_tests PRIVATE
    VAIOS_HOST_TEST
    VAIOS_SYSCALL_SVC=1
    VAIOS_DEVFS=1
    VAIOS_IPC_FD=1
    VAIOS_MAX_FDS=8
    VAIOS_MODULE_VFS=0
    HEAP_WATERMARK_ENABLE=0
    TASK_STACK_WATERMARK_ENABLE=0
    STATIC_SEMAPHORE_SIZE=64)
target_compile_options(vaios_ipcfd_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_ipcfd_unit_tests COMMAND vaios_ipcfd_tests)
set_tests_properties(vaios_ipcfd_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)

# -------------------------------------------------------------------------
# vaios_uaccess_guard_tests — seventh executable: v_access_ok / v_strnlen_user
# with VAIOS_MPU_STACK_GUARD on, so the no-access guard region is excluded from
# the valid range (STAGE5_REVIEW_FINDINGS #11). The base suite builds guard-off,
# so this needs its own binary.
# -------------------------------------------------------------------------
add_executable(vaios_uaccess_guard_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/test_uaccess_guard.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/port_hw_stub.c"
    "${VAIOS_ROOT}/kernel/task.c"
    "${VAIOS_ROOT}/kernel/ipc.c"
    "${VAIOS_ROOT}/kernel/perf.c"
    "${VAIOS_ROOT}/kernel/memory/memory.c"
    "${VAIOS_ROOT}/kernel/memory/seglist.c"
    "${VAIOS_ROOT}/kernel/memory/tlsf.c"
)
target_include_directories(vaios_uaccess_guard_tests PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(vaios_uaccess_guard_tests PRIVATE
    VAIOS_HOST_TEST
    VAIOS_MPU_STACK_GUARD=1
    VAIOS_MPU_GUARD_SIZE=32
    VAIOS_MODULE_VFS=0
    HEAP_WATERMARK_ENABLE=0
    TASK_STACK_WATERMARK_ENABLE=0
    STATIC_SEMAPHORE_SIZE=64)
target_compile_options(vaios_uaccess_guard_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -g -O0
    -I"${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_test(NAME vaios_uaccess_guard_unit_tests COMMAND vaios_uaccess_guard_tests)
set_tests_properties(vaios_uaccess_guard_unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "0 failed"
    ENVIRONMENT "${VAIOS_SANITIZER_ENV}"
)
