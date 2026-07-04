/**
 * @file test_vaios.c
 * @brief Unit tests for kernel/vaios.c.
 *
 * v_init / v_system_init are pure hardware bring-up (NavHAL clocks, UART,
 * IRQ priorities, optional SDIO) and are not meaningfully testable on the
 * host — they only need to link, which the stubbed semihosting helpers in
 * stubs.c provide. The host-testable surface is v_delay: it dispatches to
 * task_delay() when the scheduler is running, otherwise busy-waits on
 * systick_count.
 */
#include "framework.h"
#include "task.h"
#include "vaios.h"
#include <string.h>

extern void stub_reset_heap(void);
extern void stub_set_ticks(uint32_t t);
extern volatile uint32_t systick_count;
extern uint8_t scheduler_running;
extern TCB *current_task;
extern TCB *idle_task;
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern uint32_t task_count;

/* Distinct stand-ins so task_delay's `current_task == idle_task` short-circuit
 * doesn't fire unless a test deliberately points current at idle. */
static TCB _vaios_fake_task;
static TCB _vaios_idle_task;

static void full_reset(void) {
  stub_reset_heap();
  stub_set_ticks(0);
  scheduler_running = 0;
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = NULL;
  ready_bitmap = 0;
  task_count = 0;
  memset(&_vaios_fake_task, 0, sizeof(_vaios_fake_task));
  _vaios_fake_task.task_id = 1;
  _vaios_fake_task.priority = 3;
  _vaios_fake_task.status = TASK_RUNNING;
  memset(&_vaios_idle_task, 0, sizeof(_vaios_idle_task));
  _vaios_idle_task.task_id = 0;
  _vaios_idle_task.status = TASK_RUNNING;
  current_task = &_vaios_fake_task;
  idle_task = &_vaios_idle_task;
}

/* When the scheduler is running, v_delay(ms) computes ticks from
 * SYSTICK_PERIOD and delegates to task_delay(); task_delay parks the
 * current task on the delayed_list with delay_ticks = now + ticks. */
static void test_v_delay_delegates_to_task_delay_when_scheduler_running(void) {
  full_reset();
  scheduler_running = 1;
  stub_set_ticks(100);

  /* SYSTICK_PERIOD is 1000 us, so delay_ticks = ms (10 -> 10 ticks). */
  v_delay(10);

  TEST_ASSERT_EQ(_vaios_fake_task.status, TASK_DELAYED);
  TEST_ASSERT_EQ(_vaios_fake_task.delay_ticks, 110u); /* now (100) + 10 */
  TEST_ASSERT_NOT_NULL(delayed_list);
}

/* The idle task short-circuits task_delay (see kernel/task.c:task_delay).
 * v_delay on the idle task is a no-op when the scheduler is running. */
static void test_v_delay_noop_on_idle_task(void) {
  full_reset();
  scheduler_running = 1;
  current_task = idle_task; /* idle == fake here */
  stub_set_ticks(100);

  v_delay(5);

  /* delay_ticks must not have been set, idle stays RUNNING. */
  TEST_ASSERT_EQ(_vaios_fake_task.delay_ticks, 0u);
  TEST_ASSERT_EQ(_vaios_fake_task.status, TASK_RUNNING);
  TEST_ASSERT_NULL(delayed_list);
}

/* When scheduler is not yet running (early boot / sensor init), v_delay
 * busy-waits on systick_count. v_delay(0) computes 0 ticks and the loop
 * condition systick_count < initial+0 is immediately false — exits without
 * spinning. This both exercises the busy-wait branch and verifies the
 * boundary. */
static void test_v_delay_zero_returns_immediately_no_scheduler(void) {
  full_reset();
  scheduler_running = 0;
  stub_set_ticks(42);

  v_delay(0); /* must not hang */

  TEST_ASSERT_EQ(systick_count, 42u);
  /* No task state should have changed — task_delay was not called. */
  TEST_ASSERT_EQ(_vaios_fake_task.delay_ticks, 0u);
  TEST_ASSERT_NULL(delayed_list);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
static const test_case_t vaios_cases[] = {
    TEST_CASE(test_v_delay_delegates_to_task_delay_when_scheduler_running),
    TEST_CASE(test_v_delay_noop_on_idle_task),
    TEST_CASE(test_v_delay_zero_returns_immediately_no_scheduler),
};
const test_suite_t vaios_suite = {
    .name = "vaios (v_delay)",
    .cases = vaios_cases,
    .count = TEST_COUNT(vaios_cases),
};
