/**
 * @file test_stage5_regress.c
 * @brief Regression tests for STAGE5_REVIEW_FINDINGS.md task-teardown bugs.
 *
 * These are EXPECTED TO FAIL against the current (unfixed) code — they assert
 * the fixed contract. They fail *gracefully* (an assertion on kernel state, not
 * a crash) so tools/run_tests.sh still prints the summary. Each corresponds to
 * a finding; the fix flips it green.
 *
 * Scope: only the raw-primitive teardown bugs are here (#5, #6). The fd-typed
 * ones (#1 wnodes overrun, #4 named-object refcount leak, #7 name truncation,
 * #12 fd-mutex recursion) need VAIOS_IPC_FD, which this binary does not build.
 */
#include "framework.h"
#include "ipc.h"
#include "memory.h"
#include "task.h"
#include <string.h>

extern void stub_reset_heap(void);
extern void stub_set_ticks(uint32_t t);

extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern uint32_t context_switch_count;

static void full_reset(void) {
  stub_reset_heap();
  stub_set_ticks(0);
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  ready_bitmap = 0;
  task_count = 0;
  context_switch_count = 0;
}

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

/* Finding #5: task_exit_request() only unlinks READY/DELAYED tasks. A task
 * parked in TASK_BLOCKED is left on blocked_list, then re-enqueued by
 * enqueue_task() — which nulls the node's own links and re-appends it, forming
 * a self-cycle (node->next == node) and severing the list. Correct teardown
 * unlinks the blocked task first, so it never points at itself. */
static void test_bug5_blocked_task_exit_corrupts_blocked_list(void) {
  full_reset();
  scheduler_init(); /* idle task on ready_lists[0], current = idle */

  uint32_t id = task_create(dummy_task, NULL, 256, 3);
  TEST_ASSERT(id != 0);
  TCB *b = ready_lists[3];
  TEST_ASSERT_NOT_NULL(b);

  /* Move it into the blocked state on blocked_list (as v_sem_take would). */
  remove_from_ready_list(b);
  add_to_blocked_list(b);
  TEST_ASSERT(blocked_list == b);
  TEST_ASSERT(b->next == NULL); /* only node so far */

  task_exit_request(id);

  /* FAILS today: enqueue_task self-links the already-listed node. */
  TEST_ASSERT(b->next != b);
  TEST_ASSERT(b->prev != b);
}

/* Finding #6: a task that exits while holding a mutex never releases it — the
 * exit paths don't walk held_mutexes. The mutex stays owned by the dead task
 * (count 0), so a later lock from any other context can never succeed. Correct
 * teardown releases held mutexes on exit. */
static void test_bug6_mutex_held_on_exit_not_released(void) {
  full_reset();
  scheduler_init();

  uint32_t id = task_create(dummy_task, NULL, 256, 3);
  TEST_ASSERT(id != 0);
  TCB *holder = ready_lists[3];
  TEST_ASSERT_NOT_NULL(holder);

  MutexHandle_t m = v_mutex_create();
  TEST_ASSERT_NOT_NULL(m);

  /* The holder locks it (uncontended -> owner = holder, count 0). */
  current_task = holder;
  TEST_ASSERT_EQ(v_mutex_lock(m, 0), VA_PASS);

  /* The holder is terminated without unlocking. */
  task_exit_request(id);

  /* Another context tries to acquire it non-blocking. A released mutex grants;
   * the leaked one never will. FAILS today (returns VA_FAIL). */
  current_task = idle_task;
  TEST_ASSERT_EQ(v_mutex_lock(m, 0), VA_PASS);
}

/* Finding #14: a finite timeout whose absolute deadline (v_get_ticks() + ticks)
 * sums exactly to 0 — the 32-bit tick counter wrapping past UINT32_MAX — was
 * stored as delay_ticks == 0, which is the V_WAIT_FOREVER "no deadline"
 * sentinel. A finite wait would then never time out. The fix nudges such a
 * deadline to 1. */
static void test_bug14_finite_wait_not_infinite_on_tick_wrap(void) {
  full_reset();
  scheduler_init();
  current_task = idle_task; /* a running task to block */
  stub_set_ticks(0xFFFFFFFFu); /* +1 wraps to 0 */

  SemaphoreHandle_t s = v_semaphore_create_binary(); /* starts empty (count 0) */
  TEST_ASSERT_NOT_NULL(s);
  (void)v_semaphore_take(s, 1); /* finite wait; deadline = 0xFFFFFFFF + 1 = 0 */

  /* A finite wait must not be parked with the FOREVER sentinel. */
  TEST_ASSERT(current_task->delay_ticks != 0);
}

static const test_case_t stage5_regress_cases[] = {
    TEST_CASE(test_bug5_blocked_task_exit_corrupts_blocked_list),
    TEST_CASE(test_bug6_mutex_held_on_exit_not_released),
    TEST_CASE(test_bug14_finite_wait_not_infinite_on_tick_wrap),
};

const test_suite_t stage5_regress_suite = {
    .name = "stage5 regressions (EXPECTED FAIL until fixed)",
    .cases = stage5_regress_cases,
    .count = TEST_COUNT(stage5_regress_cases),
};
