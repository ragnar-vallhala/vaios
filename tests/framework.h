/**
 * @file framework.h
 * @brief Minimal single-header test framework for vaios RTOS unit tests.
 *
 * Usage:
 *   TEST_ASSERT(cond)              - fail if cond is false
 *   TEST_ASSERT_EQ(a, b)          - fail if a != b
 *   TEST_ASSERT_NULL(ptr)         - fail if ptr != NULL
 *   TEST_ASSERT_NOT_NULL(ptr)     - fail if ptr == NULL
 *   TEST_RUN(fn)                  - run a void(void) test function
 *   TEST_SUITE_BEGIN(name)        - begin a named suite
 *   TEST_SUITE_END()              - print suite summary
 */
#ifndef VAIOS_TEST_FRAMEWORK_H
#define VAIOS_TEST_FRAMEWORK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Global counters (defined once via TEST_MAIN_IMPL in one .c file)
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
 * Suite / runner macros
 * ---------------------------------------------------------------------- */
#define TEST_SUITE_BEGIN(name)                                                 \
  do {                                                                         \
    _current_suite = (name);                                                   \
    printf("\n\033[1;34m=== Suite: %s ===\033[0m\n", _current_suite);          \
  } while (0)

#define TEST_RUN(fn)                                                           \
  do {                                                                         \
    int _before_fail = _test_fail;                                             \
    printf("  %-52s", #fn);                                                    \
    fflush(stdout);                                                            \
    fn();                                                                      \
    if (_test_fail == _before_fail)                                            \
      printf("\033[32mPASS\033[0m\n");                                         \
    else                                                                       \
      printf("\033[31mFAIL\033[0m\n");                                         \
  } while (0)

#define TEST_SUITE_END()                                                       \
  do {                                                                         \
    printf("\n\033[1m[%s] Results: \033[32m%d passed\033[0m, "                 \
           "\033[31m%d failed\033[0m\n",                                       \
           _current_suite, _test_pass, _test_fail);                            \
  } while (0)

/* -------------------------------------------------------------------------
 * Global report (call once at end of main)
 * ---------------------------------------------------------------------- */
#define TEST_GLOBAL_REPORT()                                                   \
  do {                                                                         \
    printf("\n\033[1m========== TOTAL: %d passed, %d failed =========="        \
           "\033[0m\n",                                                        \
           _test_pass, _test_fail);                                            \
    return (_test_fail > 0) ? 1 : 0;                                           \
  } while (0)

/* -------------------------------------------------------------------------
 * Place in exactly ONE .c file to define the global counters
 * ---------------------------------------------------------------------- */
#define TEST_GLOBALS_IMPL                                                      \
  int _test_pass = 0;                                                          \
  int _test_fail = 0;                                                          \
  const char *_current_suite = ""

#endif /* VAIOS_TEST_FRAMEWORK_H */
