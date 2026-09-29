/* Runner for the isolated devfs test binary (see tests/CMakeLists.txt). */
#include "framework.h"

extern const test_suite_t devfs_suite;

int main(void) {
  const test_suite_t *const suites[] = {&devfs_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
