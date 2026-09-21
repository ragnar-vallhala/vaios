/**
 * @file test_bus.c
 * @brief Unit tests for the bus arbiter (kernel/bus.c). The "DMA" is a fake:
 *        start() only records the job, and the test calls v_bus_done_isr() to
 *        play the completion interrupt.
 */
#include "bus.h"
#include "framework.h"
#include "task.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *current_task;

static TCB _fake_task;
static v_bus_t bus;
static int order[8], n_order, last_rc;

static void reset(void) {
  stub_reset_heap();
  memset(&_fake_task, 0, sizeof(_fake_task));
  _fake_task.priority = 3;
  _fake_task.status = TASK_RUNNING;
  current_task = &_fake_task;
  memset(&bus, 0, sizeof(bus));
  n_order = 0;
  last_rc = 1;
}

static int fake_start(v_bus_job_t *j) {
  order[n_order++] = (int)(long)j->arg;
  return 0;
}
static int failing_start(v_bus_job_t *j) { return -5; }
static void record_done(v_bus_job_t *j, int rc) { last_rc = rc; }

#define JOB(id, p) {.start = fake_start, .done = record_done, .arg = (void *)(id), .prio = (p)}

/* Idle bus: submit starts at once; later jobs wait, then run by prio, FIFO. */
static void test_bus_priority_then_fifo(void) {
  reset();
  v_bus_job_t a = JOB(1, 1), b = JOB(2, 1), c = JOB(3, 5), d = JOB(4, 1);
  TEST_ASSERT_EQ(v_bus_submit(&bus, &a), VA_PASS);
  v_bus_submit(&bus, &b);
  v_bus_submit(&bus, &c);
  v_bus_submit(&bus, &d);
  TEST_ASSERT_EQ(n_order, 1); /* only a is on the bus */
  for (int i = 0; i < 3; i++)
    v_bus_done_isr(&bus, 0); /* each completion chains the next start */
  TEST_ASSERT_EQ(n_order, 4);
  TEST_ASSERT_EQ(order[1], 3); /* high prio jumps the queue */
  TEST_ASSERT_EQ(order[2], 2); /* then FIFO among equals */
  TEST_ASSERT_EQ(order[3], 4);
  v_bus_done_isr(&bus, 7);
  TEST_ASSERT_EQ(last_rc, 7);
  TEST_ASSERT_NULL(bus.active);
}

/* A job can't be queued twice; it can be resubmitted once done. */
static void test_bus_double_submit_rejected(void) {
  reset();
  v_bus_job_t a = JOB(1, 0);
  v_bus_submit(&bus, &a);
  TEST_ASSERT_EQ(v_bus_submit(&bus, &a), VA_FAIL);
  v_bus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(v_bus_submit(&bus, &a), VA_PASS);
}

/* start() failure reports rc and hands the bus to the next job. */
static void test_bus_start_failure_moves_on(void) {
  reset();
  v_bus_job_t a = JOB(1, 0), bad = JOB(2, 0), c = JOB(3, 0);
  bad.start = failing_start;
  v_bus_submit(&bus, &a);
  v_bus_submit(&bus, &bad);
  v_bus_submit(&bus, &c);
  v_bus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(n_order, 2);
  TEST_ASSERT_EQ(order[1], 3);
}

/* Sync lock on an idle bus is granted at once; async waits for the unlock. */
static void test_bus_lock_unlock(void) {
  reset();
  TEST_ASSERT_EQ(v_bus_lock(&bus, 0, 0), VA_PASS);
  v_bus_job_t a = JOB(1, 9);
  v_bus_submit(&bus, &a);
  TEST_ASSERT_EQ(n_order, 0);
  v_bus_unlock(&bus);
  TEST_ASSERT_EQ(n_order, 1);
}

/* Lock on a busy bus times out and leaves the queue clean. */
static void test_bus_lock_timeout(void) {
  reset();
  v_bus_job_t a = JOB(1, 0), b = JOB(2, 0);
  v_bus_submit(&bus, &a);
  TEST_ASSERT_EQ(v_bus_lock(&bus, 5, 0), VA_FAIL);
  v_bus_submit(&bus, &b);
  v_bus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(order[1], 2);
  TEST_ASSERT_NULL(bus.queue);
}

/* Cyclic job fires every `period` ticks; a still-busy instance is an overrun. */
static void test_bus_cyclic(void) {
  reset();
  v_bus_job_t a = JOB(1, 0);
  a.period = 3;
  TEST_ASSERT_EQ(v_bus_cyclic_add(&bus, &a), VA_PASS);
  v_bus_tick_isr(&bus);
  v_bus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 0);
  v_bus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 1);
  for (int i = 0; i < 3; i++)
    v_bus_tick_isr(&bus); /* never completed -> overrun */
  TEST_ASSERT_EQ(bus.overruns, 1u);
  v_bus_done_isr(&bus, 0);
  v_bus_cyclic_remove(&bus, &a);
  for (int i = 0; i < 6; i++)
    v_bus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 1);
}

/* A wedged async job is recoverable: abort fails it and the queue moves on. */
static void test_bus_abort_unsticks(void) {
  reset();
  v_bus_job_t a = JOB(1, 0), b = JOB(2, 0);
  TEST_ASSERT_EQ(v_bus_abort_isr(&bus, -1), VA_FAIL); /* idle */
  v_bus_submit(&bus, &a); /* never completes */
  v_bus_submit(&bus, &b);
  TEST_ASSERT_EQ(v_bus_abort_isr(&bus, -110), VA_PASS);
  TEST_ASSERT_EQ(last_rc, -110);
  TEST_ASSERT_EQ(n_order, 2);
  TEST_ASSERT_EQ(v_bus_submit(&bus, &a), VA_PASS); /* a is reusable */
}

/* Abort leaves a sync holder alone; a lock on one bus doesn't touch another. */
static void test_bus_lock_is_per_bus(void) {
  reset();
  v_bus_t other = {0};
  TEST_ASSERT_EQ(v_bus_lock(&bus, 0, 0), VA_PASS);
  TEST_ASSERT_EQ(v_bus_abort_isr(&bus, -1), VA_FAIL);
  v_bus_unlock(&other); /* not locked: no-op */
  TEST_ASSERT_EQ(bus.locked, 1);
  v_bus_job_t a = JOB(1, 0);
  v_bus_submit(&other, &a);
  TEST_ASSERT_EQ(n_order, 1); /* other bus is free */
  v_bus_unlock(&bus);
  TEST_ASSERT_EQ(bus.locked, 0);
}

static const test_case_t bus_cases[] = {
    TEST_CASE(test_bus_priority_then_fifo),
    TEST_CASE(test_bus_double_submit_rejected),
    TEST_CASE(test_bus_start_failure_moves_on),
    TEST_CASE(test_bus_lock_unlock),
    TEST_CASE(test_bus_lock_timeout),
    TEST_CASE(test_bus_cyclic),
    TEST_CASE(test_bus_abort_unsticks),
    TEST_CASE(test_bus_lock_is_per_bus),
};
const test_suite_t bus_suite = {
    .name = "Bus arbiter",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
