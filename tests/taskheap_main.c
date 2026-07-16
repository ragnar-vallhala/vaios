/* Runner for the isolated per-task-heap test binary (see tests/CMakeLists.txt).
 * Built with VAIOS_TASK_HEAP=1 + a synthetic current_task so memory.c's per-task
 * allocator is exercised on the host. */
#include "framework.h"

extern const test_suite_t taskheap_suite;

int main(void) {
  const test_suite_t *const suites[] = {&taskheap_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
