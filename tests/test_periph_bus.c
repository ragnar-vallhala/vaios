/**
 * @file test_periph_bus.c
 * @brief Unit tests for the bus arbiter (kernel/periph_bus.c). The "DMA" is a
 * fake: start() only records the job, and the test calls v_pbus_done_isr() to
 *        play the completion interrupt.
 */
#include "framework.h"
#include "periph_bus.h"
#include "task.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *current_task;

static TCB _fake_task;
static v_pbus_t bus;
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

static int fake_start(v_pbus_job_t *j) {
  order[n_order++] = (int)(long)j->arg;
  return 0;
}
static int failing_start(v_pbus_job_t *j) { return -5; }
static void record_done(v_pbus_job_t *j, int rc) { last_rc = rc; }

#define JOB(id, p)                                                             \
  {.start = fake_start, .done = record_done, .arg = (void *)(id), .prio = (p)}

/* Idle bus: submit starts at once; later jobs wait, then run by prio, FIFO. */
static void test_pbus_priority_then_fifo(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 1), b = JOB(2, 1), c = JOB(3, 5), d = JOB(4, 1);
  TEST_ASSERT_EQ(v_pbus_submit(&bus, &a), VA_PASS);
  v_pbus_submit(&bus, &b);
  v_pbus_submit(&bus, &c);
  v_pbus_submit(&bus, &d);
  TEST_ASSERT_EQ(n_order, 1); /* only a is on the bus */
  for (int i = 0; i < 3; i++)
    v_pbus_done_isr(&bus, 0); /* each completion chains the next start */
  TEST_ASSERT_EQ(n_order, 4);
  TEST_ASSERT_EQ(order[1], 3); /* high prio jumps the queue */
  TEST_ASSERT_EQ(order[2], 2); /* then FIFO among equals */
  TEST_ASSERT_EQ(order[3], 4);
  v_pbus_done_isr(&bus, 7);
  TEST_ASSERT_EQ(last_rc, 7);
  TEST_ASSERT_NULL(bus.active);
}

/* A job can't be queued twice; it can be resubmitted once done. */
static void test_pbus_double_submit_rejected(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0);
  v_pbus_submit(&bus, &a);
  TEST_ASSERT_EQ(v_pbus_submit(&bus, &a), VA_FAIL);
  v_pbus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(v_pbus_submit(&bus, &a), VA_PASS);
}

/* start() failure reports rc and hands the bus to the next job. */
static void test_pbus_start_failure_moves_on(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0), bad = JOB(2, 0), c = JOB(3, 0);
  bad.start = failing_start;
  v_pbus_submit(&bus, &a);
  v_pbus_submit(&bus, &bad);
  v_pbus_submit(&bus, &c);
  v_pbus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(n_order, 2);
  TEST_ASSERT_EQ(order[1], 3);
}

/* Sync lock on an idle bus is granted at once; async waits for the unlock. */
static void test_pbus_lock_unlock(void) {
  reset();
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 0, 0), VA_PASS);
  static v_pbus_job_t a = JOB(1, 9);
  v_pbus_submit(&bus, &a);
  TEST_ASSERT_EQ(n_order, 0);
  v_pbus_unlock(&bus);
  TEST_ASSERT_EQ(n_order, 1);
}

/* Lock on a busy bus times out and leaves the queue clean. */
static void test_pbus_lock_timeout(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0), b = JOB(2, 0);
  v_pbus_submit(&bus, &a);
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 5, 0), VA_FAIL);
  v_pbus_submit(&bus, &b);
  v_pbus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(order[1], 2);
  TEST_ASSERT_NULL(bus.queue);
}

/* Cyclic job fires every `period` ticks; a still-busy one is an overrun. */
static void test_pbus_cyclic(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0);
  a.period = 3;
  TEST_ASSERT_EQ(v_pbus_cyclic_add(&bus, &a), VA_PASS);
  v_pbus_tick_isr(&bus);
  v_pbus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 0);
  v_pbus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 1);
  for (int i = 0; i < 3; i++)
    v_pbus_tick_isr(&bus); /* never completed -> overrun */
  TEST_ASSERT_EQ(bus.overruns, 1u);
  v_pbus_done_isr(&bus, 0);
  v_pbus_cyclic_remove(&bus, &a);
  for (int i = 0; i < 6; i++)
    v_pbus_tick_isr(&bus);
  TEST_ASSERT_EQ(n_order, 1);
}

/* A wedged async job is recoverable: abort fails it and the queue moves on. */
static void test_pbus_abort_unsticks(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0), b = JOB(2, 0);
  TEST_ASSERT_EQ(v_pbus_abort_isr(&bus, -1), VA_FAIL); /* idle */
  v_pbus_submit(&bus, &a);                             /* never completes */
  v_pbus_submit(&bus, &b);
  TEST_ASSERT_EQ(v_pbus_abort_isr(&bus, -110), VA_PASS);
  TEST_ASSERT_EQ(last_rc, -110);
  TEST_ASSERT_EQ(n_order, 2);
  TEST_ASSERT_EQ(v_pbus_submit(&bus, &a), VA_PASS); /* a is reusable */
}

/* Abort leaves a sync holder alone; a lock on one bus doesn't touch another. */
static void test_pbus_lock_is_per_bus(void) {
  reset();
  static v_pbus_t other = {0};
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 0, 0), VA_PASS);
  TEST_ASSERT_EQ(v_pbus_abort_isr(&bus, -1), VA_FAIL);
  v_pbus_unlock(&other); /* not locked: no-op */
  TEST_ASSERT_EQ(bus.locked, 1);
  static v_pbus_job_t a = JOB(1, 0);
  v_pbus_submit(&other, &a);
  TEST_ASSERT_EQ(n_order, 1); /* other bus is free */
  v_pbus_unlock(&bus);
  TEST_ASSERT_EQ(bus.locked, 0);
}

/* A stray DMA-complete (late IRQ, or a sync holder's own DMA on the same
 * callback) must not release a v_pbus_lock holder and start a queued job. */
static void test_pbus_stray_done_keeps_lock(void) {
  reset();
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 0, 0), VA_PASS);
  static v_pbus_job_t a = JOB(1, 0);
  v_pbus_submit(&bus, &a);
  v_pbus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(bus.locked, 1);
  TEST_ASSERT_EQ(n_order, 0);
  v_pbus_unlock(&bus);
  TEST_ASSERT_EQ(n_order, 1);
}

/* Registering a cyclic job twice is rejected, not a self-linked list that
 * spins v_pbus_tick_isr forever. */
static void test_pbus_cyclic_double_add_rejected(void) {
  reset();
  static v_pbus_job_t a = JOB(1, 0);
  a.period = 1;
  TEST_ASSERT_EQ(v_pbus_cyclic_add(&bus, &a), VA_PASS);
  TEST_ASSERT_EQ(v_pbus_cyclic_add(&bus, &a), VA_FAIL);
  TEST_ASSERT_NULL(a.cyc_next);
}

/* A task killed while HOLDING the lock (task_exit_request) must not leave the
 * bus locked forever: teardown releases it and the queued job starts. */
static void test_pbus_teardown_releases_dead_holder(void) {
  reset();
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 0, 0), VA_PASS);
  static v_pbus_job_t a = JOB(1, 0);
  v_pbus_submit(&bus, &a);
  TEST_ASSERT_EQ(n_order, 0);
  v_pbus_task_teardown(current_task);
  TEST_ASSERT_EQ(bus.locked, 0);
  TEST_ASSERT_EQ(n_order, 1);
  v_pbus_done_isr(&bus, 0);
}

/* Only the task that took the lock can release it. */
static void test_pbus_unlock_by_non_owner_ignored(void) {
  reset();
  TEST_ASSERT_EQ(v_pbus_lock(&bus, 0, 0), VA_PASS);
  static TCB other_task;
  current_task = &other_task;
  v_pbus_unlock(&bus);
  TEST_ASSERT_EQ(bus.locked, 1);
  current_task = &_fake_task;
  v_pbus_unlock(&bus);
  TEST_ASSERT_EQ(bus.locked, 0);
}

/* Lock slots are a bounded pool: one more locker than it holds gets VA_FAIL,
 * and unlocking hands the slots back. */
static void test_pbus_lock_slots_bounded(void) {
  reset();
  static v_pbus_t buses[VAIOS_PBUS_MAX_LOCKERS + 1];
  memset(buses, 0, sizeof buses);
  for (int i = 0; i < VAIOS_PBUS_MAX_LOCKERS; i++)
    TEST_ASSERT_EQ(v_pbus_lock(&buses[i], 0, 0), VA_PASS);
  TEST_ASSERT_EQ(v_pbus_lock(&buses[VAIOS_PBUS_MAX_LOCKERS], 0, 0), VA_FAIL);
  for (int i = 0; i < VAIOS_PBUS_MAX_LOCKERS; i++)
    v_pbus_unlock(&buses[i]);
  TEST_ASSERT_EQ(v_pbus_lock(&buses[VAIOS_PBUS_MAX_LOCKERS], 0, 0), VA_PASS);
  v_pbus_unlock(&buses[VAIOS_PBUS_MAX_LOCKERS]);
}

static const test_case_t bus_cases[] = {
    TEST_CASE(test_pbus_priority_then_fifo),
    TEST_CASE(test_pbus_double_submit_rejected),
    TEST_CASE(test_pbus_start_failure_moves_on),
    TEST_CASE(test_pbus_lock_unlock),
    TEST_CASE(test_pbus_lock_timeout),
    TEST_CASE(test_pbus_cyclic),
    TEST_CASE(test_pbus_abort_unsticks),
    TEST_CASE(test_pbus_lock_is_per_bus),
    TEST_CASE(test_pbus_stray_done_keeps_lock),
    TEST_CASE(test_pbus_cyclic_double_add_rejected),
    TEST_CASE(test_pbus_teardown_releases_dead_holder),
    TEST_CASE(test_pbus_unlock_by_non_owner_ignored),
    TEST_CASE(test_pbus_lock_slots_bounded),
};
const test_suite_t pbus_suite = {
    .name = "Bus arbiter",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
