/**
 * @file test_upipe.c
 * @brief v_upipe's user-space ring (include/upipe.h): ordering, full/empty,
 *        index wrap, and the kick only when the peer has said it sleeps.
 *
 * The ring is header-only, so this runs it on a plain buffer; the two syscalls
 * are stubs that count. The MPU mapping and the sleep itself are checked on
 * target (tools/renode_upipe.sh, tools/run_hw_tests.sh).
 */
#include "framework.h"
#include "upipe.h"
#include <string.h>

static int kicks, sleeps;
int v_upipe_kick(void) { return ++kicks; }
int v_upipe_sleep(uint32_t ticks) {
  (void)ticks;
  return ++sleeps;
}

static uint32_t mem[(sizeof(v_upipe_t) + 4 * 8) / 4];

static v_upipe_t *fresh(void) {
  memset(mem, 0, sizeof mem);
  v_upipe_t *p = (v_upipe_t *)mem;
  p->slot = 8;
  p->n = 4;
  kicks = sleeps = 0;
  return p;
}

static void test_upipe_fifo_and_full(void) {
  v_upipe_t *p = fresh();
  TEST_ASSERT_NULL(v_upipe_peek(p)); // empty
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t *s = v_upipe_reserve(p);
    TEST_ASSERT_NOT_NULL(s);
    *s = i;
    v_upipe_commit(p);
  }
  TEST_ASSERT_NULL(v_upipe_reserve(p)); // full at n
  for (uint32_t i = 0; i < 4; i++) {
    const uint32_t *s = v_upipe_peek(p);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ(*s, i);
    v_upipe_release(p);
  }
  TEST_ASSERT_NULL(v_upipe_peek(p));
  TEST_ASSERT_EQ(kicks, 0); // nobody slept: no syscall at all
}

static void test_upipe_wraps(void) {
  v_upipe_t *p = fresh();
  p->wr = p->rd = 0xFFFFFFFEu; // the free-running indices about to wrap
  for (uint32_t i = 0; i < 10; i++) {
    uint32_t *s = v_upipe_reserve(p);
    TEST_ASSERT_NOT_NULL(s);
    *s = i;
    v_upipe_commit(p);
    const uint32_t *r = v_upipe_peek(p);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQ(*r, i);
    v_upipe_release(p);
  }
  TEST_ASSERT_EQ(p->wr, 8u);
}

static void test_upipe_kicks_only_a_sleeper(void) {
  v_upipe_t *p = fresh();
  TEST_ASSERT_EQ(v_upipe_wait(p, 0, 5), 1); // empty: the consumer sleeps
  TEST_ASSERT_EQ(p->wait_rd, 0);            // ...and lowers its flag after
  p->wait_rd = 1;                           // asleep again
  v_upipe_reserve(p);
  v_upipe_commit(p);
  TEST_ASSERT_EQ(kicks, 1);
  TEST_ASSERT_EQ(v_upipe_wait(p, 0, 5), 0); // data there: no sleep
  TEST_ASSERT_EQ(sleeps, 1);
  p->wait_wr = 1; // the producer sleeps: the release kicks it
  v_upipe_release(p);
  TEST_ASSERT_EQ(kicks, 2);
}

static const test_case_t upipe_cases[] = {
    TEST_CASE(test_upipe_fifo_and_full),
    TEST_CASE(test_upipe_wraps),
    TEST_CASE(test_upipe_kicks_only_a_sleeper),
};
const test_suite_t upipe_suite = {
    .name = "upipe ring", .cases = upipe_cases, .count = TEST_COUNT(upipe_cases)};
