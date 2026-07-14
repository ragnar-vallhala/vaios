/* Runner for the isolated syscall-dispatch test binary (see tests/CMakeLists.txt).
 * Built with VAIOS_SYSCALL_SVC=1 + VAIOS_MPU_USER_SEPARATION=1 so syscall.c's
 * validation switch and privileged-only gate are exercised host-side. */
#include "framework.h"

extern const test_suite_t syscall_suite;

int main(void) {
  const test_suite_t *const suites[] = {&syscall_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
