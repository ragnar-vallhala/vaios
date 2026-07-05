/**
 * @file framework.c
 * @brief Test-framework state + suite runners (compiled once per test binary).
 *
 * The analogue of NavHAL's navtest_state.c: owns the global pass/fail counters
 * and the two suite walkers declared in framework.h. Output is plain printf so
 * it works both on the host (libc stdout) and on target (newlib retargeted to
 * the UART console by examples/unit_tests.c).
 *
 * The per-case line format —
 *     "  <case_name><pad>PASS (n)"   /   "FAIL (p/t)"
 * and the "=== Suite: NAME ===" banner are load-bearing: tools/lib/
 * test_summary.awk parses them to build the cross-suite summary table. Do not
 * change them without updating that parser.
 */
#include "framework.h"

int _test_pass = 0;
int _test_fail = 0;
const char *_current_suite = "";

int run_test_suite(const test_suite_t *suite) {
  _current_suite = suite->name;
  printf("\n\033[1;34m=== Suite: %s ===\033[0m\n", suite->name);

  int suite_pass = 0, suite_fail = 0;
  for (size_t i = 0; i < suite->count; i++) {
    int before_pass = _test_pass;
    int before_fail = _test_fail;

    printf("  %-52s", suite->cases[i].name);
    fflush(stdout);
    suite->cases[i].fn();

    int dpass = _test_pass - before_pass;
    int dfail = _test_fail - before_fail;
    int dtotal = dpass + dfail;
    if (dfail == 0)
      printf("\033[32mPASS\033[0m (%d)\n", dtotal);
    else
      printf("\033[31mFAIL\033[0m (%d/%d)\n", dpass, dtotal);

    suite_pass += dpass;
    suite_fail += dfail;
  }

  printf("\n\033[1m[%s] Results: \033[32m%d passed\033[0m, "
         "\033[31m%d failed\033[0m\n",
         suite->name, suite_pass, suite_fail);
  return suite_fail;
}

int run_test_suites(const test_suite_t *const *suites, size_t n) {
  for (size_t i = 0; i < n; i++)
    run_test_suite(suites[i]);

  printf("\n\033[1m========== TOTAL: %d passed, %d failed ==========\033[0m\n",
         _test_pass, _test_fail);
  return (_test_fail > 0) ? 1 : 0;
}
