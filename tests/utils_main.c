/**
 * @file utils_main.c
 * @brief Runner for the vaios_utils_tests executable (kernel/utils.c
 *        formatter + v_atof). Separate from the main vaios_tests binary
 *        because real utils.c collides with that binary's v_log /
 *        v_memset / v_panic stubs.
 */
#include "framework.h"
#include "suites.h"

static const test_suite_t *const utils_suites[] = {
    &utils_suite,
};

int main(void) {
  return run_test_suites(utils_suites, TEST_COUNT(utils_suites));
}
