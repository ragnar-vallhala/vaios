/**
 * @file suites.h
 * @brief Central registry of every test suite object.
 *
 * One `extern const test_suite_t` per suite (defined in the matching
 * test_*.c). Runners include this header and pick the suites they want into a
 * `const test_suite_t *const[]` — the host runner takes them all, the
 * on-target runner (examples/unit_tests.c) takes a curated subset.
 *
 * Adding a suite: define its `test_suite_t` in a test_*.c, declare it here,
 * and add `&its_suite` to the runner array(s) that should include it. That is
 * the only bookkeeping — no per-file run_*_tests() prototype to duplicate.
 */
#ifndef VAIOS_TEST_SUITES_H
#define VAIOS_TEST_SUITES_H

#include "framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const test_suite_t memory_suite;
extern const test_suite_t task_list_suite;
extern const test_suite_t scheduler_suite;
extern const test_suite_t ipc_suite;
extern const test_suite_t structure_suite;
extern const test_suite_t vfs_suite;
extern const test_suite_t vaios_suite;
extern const test_suite_t terminal_suite;
extern const test_suite_t perf_suite;
extern const test_suite_t utils_suite;

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_TEST_SUITES_H */
