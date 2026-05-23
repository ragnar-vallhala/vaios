/**
 * @file main.c
 * @brief vaios RTOS test runner – runs all test suites and exits 0 on pass.
 */
#include "framework.h"

/* Define global counters (one translation unit only) */
TEST_GLOBALS_IMPL;

/* Suite prototypes */
void run_memory_tests(void);
void run_task_list_tests(void);
void run_scheduler_tests(void);
void run_ipc_tests(void);
void run_structure_tests(void);

int main(void) {
  run_memory_tests();
  run_task_list_tests();
  run_scheduler_tests();
  run_ipc_tests();
  run_structure_tests();

  TEST_GLOBAL_REPORT();
}
