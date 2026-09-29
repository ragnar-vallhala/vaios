/**
 * @file framework.h
 * @brief Minimal single-header test framework for vaios RTOS unit tests.
 *
 * Suite-registry model (mirrors NavHAL's navtest): a test file exports one
 * `test_suite_t` describing its cases as data; every runner just walks an array
 * of suite pointers. There is no per-file `run_x_tests()` boilerplate and no
 * hand-maintained list of prototypes in each runner.
 *
 * Writing a suite:
 *   static void test_foo(void) { TEST_ASSERT(...); }
 *   static const test_case_t my_cases[] = {
 *     TEST_CASE(test_foo),
 *     TEST_CASE(test_bar),
 *   };
 *   const test_suite_t my_suite = {
 *     .name = "My Thing", .cases = my_cases, .count = TEST_COUNT(my_cases),
 *   };
 * Declare `extern const test_suite_t my_suite;` in suites.h, then add
 * `&my_suite` to a runner's array.
 *
 * Assertions (fail => log + return from the current case):
 *   TEST_ASSERT(cond)            - fail if cond is false
 *   TEST_ASSERT_EQ(a, b)         - fail if a != b
 *   TEST_ASSERT_NULL(ptr)        - fail if ptr != NULL
 *   TEST_ASSERT_NOT_NULL(ptr)    - fail if ptr == NULL
 *
 * The pass/fail counters and the two run_test_suite* runners live in
 * framework.c (compiled once into each test binary) — the analogue of NavHAL's
 * navtest_state.c.
 */
#ifndef VAIOS_TEST_FRAMEWORK_H
#define VAIOS_TEST_FRAMEWORK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Pull in the kernel config so test sources see MAX_TASK_PRIORITY /
 * IDLE_TASK_PRIORITY / etc. — task.h's MAX_PRIORITY macro expands to them.
 * Kernel sources get this transitively via utils.h; tests don't. */
#include "vaios_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Global counters (defined once in framework.c). Assertions bump these; the
 * runner reads deltas per case to annotate PASS/FAIL with the assert count.
 * ---------------------------------------------------------------------- */
extern int _test_pass;
extern int _test_fail;
extern const char *_current_suite;

/* -------------------------------------------------------------------------
 * Assertion macros
 * ---------------------------------------------------------------------- */
#define TEST_ASSERT(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "  \033[31mFAIL\033[0m [%s:%d] %s\n", __FILE__,          \
              __LINE__, #cond);                                                \
      _test_fail++;                                                            \
      return;                                                                  \
    } else {                                                                   \
      _test_pass++;                                                            \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_EQ(a, b)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      fprintf(stderr,                                                          \
              "  \033[31mFAIL\033[0m [%s:%d] %s == %s  "                       \
              "(got %lld vs %lld)\n",                                          \
              __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b));     \
      _test_fail++;                                                            \
      return;                                                                  \
    } else {                                                                   \
      _test_pass++;                                                            \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_NULL(ptr)                                                  \
  do {                                                                         \
    if ((ptr) != NULL) {                                                       \
      fprintf(stderr,                                                          \
              "  \033[31mFAIL\033[0m [%s:%d] expected NULL: "                  \
              "%s\n",                                                          \
              __FILE__, __LINE__, #ptr);                                       \
      _test_fail++;                                                            \
      return;                                                                  \
    } else {                                                                   \
      _test_pass++;                                                            \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_NOT_NULL(ptr)                                              \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      fprintf(stderr,                                                          \
              "  \033[31mFAIL\033[0m [%s:%d] unexpected "                      \
              "NULL: %s\n",                                                    \
              __FILE__, __LINE__, #ptr);                                       \
      _test_fail++;                                                            \
      return;                                                                  \
    } else {                                                                   \
      _test_pass++;                                                            \
    }                                                                          \
  } while (0)

/* -------------------------------------------------------------------------
 * Suite registry — suites are data, not per-file runner functions.
 * ---------------------------------------------------------------------- */
typedef void (*test_fn_t)(void);

typedef struct {
  test_fn_t fn;
  const char *name;
} test_case_t;

typedef struct {
  const char *name;
  const test_case_t *cases;
  size_t count;
} test_suite_t;

/* Build a test_case_t from a function; the case name is the function name. */
#define TEST_CASE(fn) {(fn), #fn}

/* Case-array element count, for the suite's `.count` initializer. */
#define TEST_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Run one suite: prints "=== Suite: NAME ===", each case as
 * "  case_name  PASS (n)" / "FAIL (p/t)" (the format tools/lib/test_summary.awk
 * parses), then a per-suite Results line. Returns the number of failed asserts
 * this suite added. */
int run_test_suite(const test_suite_t *suite);

/* Run an array of suites in order, then print the per-binary TOTAL line.
 * Returns total failures (0 => all passed), so a runner's main can
 * `return run_test_suites(suites, n);` for a CI-friendly exit code. */
int run_test_suites(const test_suite_t *const *suites, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_TEST_FRAMEWORK_H */
