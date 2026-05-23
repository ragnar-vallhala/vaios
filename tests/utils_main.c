/**
 * @file utils_main.c
 * @brief Runner for the vaios_utils_tests executable (kernel/utils.c
 *        formatter + v_atof). Separate from the main vaios_tests binary
 *        because real utils.c collides with that binary's v_log /
 *        v_memset / v_panic stubs.
 */
#include "framework.h"

TEST_GLOBALS_IMPL;

void run_utils_tests(void);

int main(void) {
  run_utils_tests();
  TEST_GLOBAL_REPORT();
}
