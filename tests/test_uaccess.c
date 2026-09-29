/*
 * test_uaccess — the Stage-5 syscall-boundary pointer validators
 * (v_access_ok / v_strnlen_user, kernel/task.c). These decide whether the kernel
 * will dereference a pointer an unprivileged task handed it, so their bounds and
 * overflow behaviour are security-critical. Pure functions over current_task's
 * block — host-testable with a synthetic TCB.
 */
#include "framework.h"
#include "task.h"
#include <stdint.h>
#include <string.h>

extern TCB *current_task;

static uint32_t g_block[64]; /* 256-byte synthetic task block */
static TCB g_task;

#define BLK_BASE ((uintptr_t)g_block)
#define BLK_SIZE ((uint32_t)sizeof(g_block)) /* 256 */

static void setup(void) {
  memset(&g_task, 0, sizeof g_task);
  g_task.magic = TCB_MAGIC;
  g_task.mem_block = g_block;
  g_task.stack_size = BLK_SIZE;
  current_task = &g_task;
}

/* ---- v_access_ok ---------------------------------------------------------- */

static void test_access_in_block(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)BLK_BASE, 16, 0), 1);
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + 100), 32, 1), 1);
}

static void test_access_whole_block(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)BLK_BASE, BLK_SIZE, 0), 1);
}

static void test_access_one_past_end(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)BLK_BASE, BLK_SIZE + 1, 0), 0);
}

static void test_access_below_base(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE - 4), 4, 0), 0);
}

static void test_access_above_end(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + BLK_SIZE + 4), 4, 0), 0);
}

static void test_access_end_zero_len(void) {
  setup();
  /* a pointer at the exact end with zero length reads nothing -> allowed */
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + BLK_SIZE), 0, 0), 1);
}

static void test_access_len_overflow(void) {
  setup();
  /* in-block pointer but a len that would wrap ptr+len: must reject, not pass */
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + 8), 0xFFFFFFFFu, 0), 0);
}

static void test_access_mid_reaching_end(void) {
  setup();
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + BLK_SIZE - 16), 16, 0), 1);
  TEST_ASSERT_EQ(v_access_ok((void *)(BLK_BASE + BLK_SIZE - 16), 17, 0), 0);
}

static void test_access_null_task(void) {
  current_task = NULL;
  TEST_ASSERT_EQ(v_access_ok((void *)BLK_BASE, 4, 0), 0);
}

static void test_access_null_memblock(void) {
  setup();
  g_task.mem_block = NULL;
  TEST_ASSERT_EQ(v_access_ok((void *)BLK_BASE, 4, 0), 0);
}

/* ---- v_strnlen_user ------------------------------------------------------- */

static void test_strnlen_terminated(void) {
  setup();
  char *s = (char *)g_block;
  strcpy(s, "hello");
  TEST_ASSERT_EQ(v_strnlen_user(s, 256), 5);
}

static void test_strnlen_empty(void) {
  setup();
  char *s = (char *)g_block;
  s[0] = '\0';
  TEST_ASSERT_EQ(v_strnlen_user(s, 256), 0);
}

static void test_strnlen_unterminated(void) {
  setup();
  char *s = (char *)g_block;
  memset(s, 'A', BLK_SIZE); /* no NUL anywhere in the block */
  TEST_ASSERT_EQ(v_strnlen_user(s, 1024), -1);
}

static void test_strnlen_below_base(void) {
  setup();
  TEST_ASSERT_EQ(v_strnlen_user((char *)(BLK_BASE - 4), 256), -1);
}

static void test_strnlen_above_end(void) {
  setup();
  TEST_ASSERT_EQ(v_strnlen_user((char *)(BLK_BASE + BLK_SIZE), 256), -1);
}

static void test_strnlen_nul_at_last_byte(void) {
  setup();
  char *s = (char *)g_block;
  memset(s, 'B', BLK_SIZE);
  s[BLK_SIZE - 1] = '\0'; /* NUL exactly at the last in-block byte */
  TEST_ASSERT_EQ(v_strnlen_user(s, 1024), (long)(BLK_SIZE - 1));
}

static void test_strnlen_max_bounds_scan(void) {
  setup();
  char *s = (char *)g_block;
  strcpy(s, "abcdefgh"); /* 8 chars, NUL at index 8 */
  TEST_ASSERT_EQ(v_strnlen_user(s, 4), -1); /* no NUL within max=4 */
  TEST_ASSERT_EQ(v_strnlen_user(s, 8), -1); /* loop 0..7, NUL@8 not reached */
  TEST_ASSERT_EQ(v_strnlen_user(s, 9), 8);  /* NUL@8 within max=9 */
}

static void test_strnlen_null_task(void) {
  current_task = NULL;
  TEST_ASSERT_EQ(v_strnlen_user("x", 4), -1);
}

static const test_case_t uaccess_cases[] = {
    TEST_CASE(test_access_in_block),
    TEST_CASE(test_access_whole_block),
    TEST_CASE(test_access_one_past_end),
    TEST_CASE(test_access_below_base),
    TEST_CASE(test_access_above_end),
    TEST_CASE(test_access_end_zero_len),
    TEST_CASE(test_access_len_overflow),
    TEST_CASE(test_access_mid_reaching_end),
    TEST_CASE(test_access_null_task),
    TEST_CASE(test_access_null_memblock),
    TEST_CASE(test_strnlen_terminated),
    TEST_CASE(test_strnlen_empty),
    TEST_CASE(test_strnlen_unterminated),
    TEST_CASE(test_strnlen_below_base),
    TEST_CASE(test_strnlen_above_end),
    TEST_CASE(test_strnlen_nul_at_last_byte),
    TEST_CASE(test_strnlen_max_bounds_scan),
    TEST_CASE(test_strnlen_null_task),
};
const test_suite_t uaccess_suite = {
    .name = "uaccess (syscall pointer validation)",
    .cases = uaccess_cases,
    .count = TEST_COUNT(uaccess_cases),
};
