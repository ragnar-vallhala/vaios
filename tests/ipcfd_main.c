/* Runner for the fd-typed IPC test binary (see tests/CMakeLists.txt). Built with
 * DEVFS + IPC_FD on, SVC off, so the fd-typed IPC bodies run directly. */
#include "framework.h"

extern const test_suite_t ipcfd_suite;
extern int v_test_in_handler; // syscall_stubs.c

int main(void) {
  // Run the IPC bodies directly ("handler mode") rather than trapping through
  // the svc trampolines, whose uint32_t ABI would truncate the 64-bit host
  // pointers these tests pass (names, fd buffers). This exercises the same fix
  // logic without the pointer-width mismatch.
  v_test_in_handler = 1;
  const test_suite_t *const suites[] = {&ipcfd_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
