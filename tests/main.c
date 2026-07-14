/**
 * @file main.c
 * @brief vaios RTOS host test runner — walks the suite registry and exits 0
 *        on pass (non-zero = failures), suitable for ctest / CI.
 *
 * The full host suite set. utils lives in a separate binary
 * (vaios_utils_tests, see utils_main.c) because kernel/utils.c collides with
 * this binary's v_log / v_memset / v_panic stubs.
 */
#include "framework.h"
#include "suites.h"

static const test_suite_t *const host_suites[] = {
    &memory_suite, &task_list_suite, &scheduler_suite,
    &ipc_suite,    &structure_suite, &vfs_suite,
    &vaios_suite,  &terminal_suite,  &perf_suite,
    &uaccess_suite, &stage5_regress_suite,
};

int main(void) {
  return run_test_suites(host_suites, TEST_COUNT(host_suites));
}
