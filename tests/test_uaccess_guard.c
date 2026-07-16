/*
 * test_uaccess_guard — v_access_ok / v_strnlen_user with the MPU stack guard
 * enabled (VAIOS_MPU_STACK_GUARD=1, GUARD_SIZE=32). Separate binary because the
 * base host suite builds guard-off; here the bottom GUARD_SIZE bytes of the
 * block are the no-access guard and must be excluded from the valid range
 * (STAGE5_REVIEW_FINDINGS #11).
 */
#include "framework.h"
#include "task.h"
#include <stdint.h>
#include <string.h>

extern TCB *current_task;

static uint32_t g_block[64]; /* 256-byte synthetic task block */
static TCB g_task;

#define GUARD 32u /* must match VAIOS_MPU_GUARD_SIZE for this binary */

static void setup(void) {
  memset(&g_task, 0, sizeof g_task);
  g_task.magic = TCB_MAGIC;
  g_task.mem_block = g_block;
  g_task.stack_size = (uint32_t)sizeof g_block; /* 256 */
  current_task = &g_task;
}

/* A pointer inside the no-access guard [mem_block, +GUARD) must be rejected;
 * one at/after the guard boundary is accepted. */
static void test_bug11_guard_region_rejected(void) {
  setup();
  uintptr_t base = (uintptr_t)g_block;
  TEST_ASSERT_EQ(v_access_ok((void *)base, 4, 0), 0);              /* in guard */
  TEST_ASSERT_EQ(v_access_ok((void *)(base + GUARD - 4), 8, 0), 0); /* straddles */
  TEST_ASSERT_EQ(v_access_ok((void *)(base + GUARD), 4, 0), 1);    /* past guard */
  TEST_ASSERT_EQ(v_access_ok((void *)(base + GUARD + 100), 32, 1), 1);
}

/* The whole post-guard region up to the block end stays accessible. */
static void test_guard_upper_region_ok(void) {
  setup();
  uintptr_t base = (uintptr_t)g_block;
  TEST_ASSERT_EQ(
      v_access_ok((void *)(base + GUARD), (uint32_t)sizeof g_block - GUARD, 0),
      1);
  /* one past the end still rejected */
  TEST_ASSERT_EQ(
      v_access_ok((void *)(base + GUARD), (uint32_t)sizeof g_block - GUARD + 1,
                  0),
      0);
}

/* v_strnlen_user rejects a string that starts inside the guard. */
static void test_bug11_strnlen_guard_rejected(void) {
  setup();
  uintptr_t base = (uintptr_t)g_block;
  TEST_ASSERT(v_strnlen_user((const char *)base, 16) < 0);
  /* a NUL-terminated string past the guard is measured normally */
  char *s = (char *)(base + GUARD);
  memcpy(s, "hi", 3);
  TEST_ASSERT_EQ(v_strnlen_user(s, 16), 2);
}

static const test_case_t uaccess_guard_cases[] = {
    TEST_CASE(test_bug11_guard_region_rejected),
    TEST_CASE(test_guard_upper_region_ok),
    TEST_CASE(test_bug11_strnlen_guard_rejected),
};

const test_suite_t uaccess_guard_suite = {
    .name = "uaccess with MPU stack guard (stage5 #11)",
    .cases = uaccess_guard_cases,
    .count = TEST_COUNT(uaccess_guard_cases),
};

int main(void) {
  const test_suite_t *const suites[] = {&uaccess_guard_suite};
  return run_test_suites(suites, TEST_COUNT(suites));
}
