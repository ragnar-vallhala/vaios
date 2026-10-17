/* Runner for the isolated shell test binary (see tests/CMakeLists.txt). */
#include "framework.h"

extern const test_suite_t shell_suite;
/* Registers /dev/console and pre-opens fd 0/1/2 on the synthetic task, so the
   shell's v_file_read(0, ...) runs the real devfs path (see shell_stubs.c). */
extern void shell_test_env_init(void);

int main(void) {
  shell_test_env_init();
  const test_suite_t *const suites[] = {&shell_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
