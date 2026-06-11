/**
 * @file test_scheduler.c
 * @brief Unit tests for vaios scheduler state machine (task.c).
 *
 * Tests: scheduler_init, task_create (memory allocation), wake_up_delayed_tasks
 *        tick interaction, delayed list manipulation, task status transitions.
 */
#include "../framework.h"
#include "memory.h"
#include "task.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Stubs declared in stubs.c
 * ---------------------------------------------------------------------- */
extern void stub_reset_heap(void);
extern void stub_set_ticks(uint32_t t);
extern void stub_reset_yield_count(void);
extern int stub_yield_count(void);

/* External task globals */
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern uint32_t context_switch_count;

/* -------------------------------------------------------------------------
 * Helper: fully reset the kernel state between tests
 * ---------------------------------------------------------------------- */
static void full_reset(void) {
  stub_reset_heap();
  stub_set_ticks(0);
  stub_reset_yield_count();

  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  ready_bitmap = 0;
  task_count = 0;
  context_switch_count = 0;
}

/* Dummy task function for use in task_create */
static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

/* scheduler_init creates the idle task */
static void test_scheduler_init_creates_idle(void) {
  full_reset();
  scheduler_init();
  TEST_ASSERT_NOT_NULL(idle_task);
  TEST_ASSERT_EQ(idle_task->priority, (uint32_t)IDLE_TASK_PRIORITY);
}

/* scheduler_init sets current_task to idle */
static void test_scheduler_init_current_is_idle(void) {
  full_reset();
  scheduler_init();
  TEST_ASSERT(current_task == idle_task);
}

/* scheduler_init with ready_bitmap cleared */
static void test_scheduler_init_bitmap(void) {
  full_reset();
  scheduler_init();
  /* Only the idle task is queued; its priority bit must be set */
  TEST_ASSERT(ready_bitmap & (1u << IDLE_TASK_PRIORITY));
}

/* task_create returns non-zero task ID on success */
static void test_task_create_returns_id(void) {
  full_reset();
  scheduler_init();
  uint32_t id = task_create(dummy_task, NULL, 256, 1);
  TEST_ASSERT(id != 0u);
}

/* task_create increments task_count */
static void test_task_create_increments_count(void) {
  full_reset();
  scheduler_init();
  uint32_t before = task_count;
  task_create(dummy_task, NULL, 256, 1);
  TEST_ASSERT_EQ(task_count, before + 1u);
}

/* task_create places task in ready list at correct priority */
static void test_task_create_in_ready_list(void) {
  full_reset();
  scheduler_init();
  uint32_t id = task_create(dummy_task, NULL, 256, 3);
  TEST_ASSERT(id != 0u);
  /* Priority 3 ready list must be non-NULL */
  TEST_ASSERT_NOT_NULL(ready_lists[3]);
  TEST_ASSERT_EQ(ready_lists[3]->task_id, id);
}

/* task_create with heap exhausted returns 0 */
static void test_task_create_oom(void) {
  full_reset();
  scheduler_init();
  /* Exhaust heap at finest granularity — segregated free lists may leave
   * small fragments after large allocations, but 8-byte allocs drain all
   * size classes including small ones. */
  while (v_malloc(8))
    ;
  uint32_t id = task_create(dummy_task, NULL, 256, 1);
  TEST_ASSERT_EQ(id, 0u);
}

/* Multiple tasks created at different priorities */
static void test_task_create_multi_priority(void) {
  full_reset();
  scheduler_init();
  uint32_t id1 = task_create(dummy_task, NULL, 256, 1);
  uint32_t id2 = task_create(dummy_task, NULL, 256, 5);
  TEST_ASSERT(id1 != 0u);
  TEST_ASSERT(id2 != 0u);
  TEST_ASSERT_NOT_NULL(ready_lists[1]);
  TEST_ASSERT_NOT_NULL(ready_lists[5]);
}

/* wake_up_delayed_tasks_isr moves due sleepers from delayed_list to ready */
static void test_wake_up_delayed_tasks(void) {
  full_reset();
  scheduler_init();

  /* Manually park a TCB on the delayed list with deadline tick 50. */
  TCB t;
  memset(&t, 0, sizeof(t));
  t.task_id = 99;
  t.priority = 2;
  t.delay_ticks = 50;
  t.status = TASK_DELAYED;
  enqueue_task(&delayed_list, &t);
  t.status = TASK_DELAYED;

  /* Below the deadline — stays delayed. */
  stub_set_ticks(30);
  wake_up_delayed_tasks_isr();
  TEST_ASSERT_EQ(t.status, TASK_DELAYED);

  /* At/past the deadline — wakes (the ISR variant uses <=). */
  stub_set_ticks(51);
  wake_up_delayed_tasks_isr();
  TEST_ASSERT_EQ(t.status, TASK_READY);
  TEST_ASSERT_NOT_NULL(ready_lists[2]);
}

/* add/remove from delayed list */
static void test_delayed_add_remove(void) {
  full_reset();
  TCB t;
  memset(&t, 0, sizeof(t));
  t.task_id = 7;
  t.priority = 1;
  add_to_delayed_list(&t);
  TEST_ASSERT_EQ(t.status, TASK_DELAYED);
  TEST_ASSERT_NOT_NULL(delayed_list);

  remove_from_delayed_list(&t);
  TEST_ASSERT_NULL(delayed_list);
}

/* context_switch_count starts at zero after scheduler_init */
static void test_context_switch_count_zero(void) {
  full_reset();
  scheduler_init();
  TEST_ASSERT_EQ(get_context_switch_count(), 0u);
}

/* -------------------------------------------------------------------------
 * Scheduler pick: get_next_task / set_next_task (the O(1) ready-list core).
 *
 * Priority convention: higher number = higher priority (highest_ready_prio is
 * 31 - clz(ready_bitmap)); FIFO round-robin within a level. The host-test
 * build drops get_next_task's target-SRAM pointer assertion (VAIOS_HOST_TEST).
 * ---------------------------------------------------------------------- */

/* The highest-priority ready task is selected over lower ones. */
static void test_get_next_task_picks_highest_priority(void) {
  full_reset();
  scheduler_init(); /* idle @ prio 0, current_task = idle (READY) */
  task_create(dummy_task, NULL, 256, 3);
  task_create(dummy_task, NULL, 256, 7);
  TCB *hi = ready_lists[7];
  TEST_ASSERT_NOT_NULL(hi);

  TCB *next = get_next_task();
  TEST_ASSERT(next == hi); /* prio 7 beats prio 3 and idle */
  TEST_ASSERT_EQ(next->priority, 7u);
  TEST_ASSERT(current_task == hi);
}

/* Two tasks at the same priority are served round-robin (FIFO). */
static void test_get_next_task_round_robin_same_priority(void) {
  full_reset();
  scheduler_init();
  task_create(dummy_task, NULL, 256, 4);
  task_create(dummy_task, NULL, 256, 4);
  TCB *t1 = ready_lists[4];
  TCB *t2 = ready_lists[4]->next;
  TEST_ASSERT(t1 != NULL && t2 != NULL && t1 != t2);

  TEST_ASSERT(get_next_task() == t1); /* head first */
  TEST_ASSERT(get_next_task() == t2); /* t1 re-queued at tail, t2 now head */
  TEST_ASSERT(get_next_task() == t1); /* back to t1 — round robin */
}

/* With no user tasks, the scheduler falls back to the idle task. */
static void test_get_next_task_returns_idle_when_no_tasks(void) {
  full_reset();
  scheduler_init();
  TEST_ASSERT(get_next_task() == idle_task);
  TEST_ASSERT(current_task == idle_task);
}

/* Each pick advances the context-switch counter. */
static void test_get_next_task_bumps_switch_count(void) {
  full_reset();
  scheduler_init();
  task_create(dummy_task, NULL, 256, 5);
  uint32_t before = get_context_switch_count();
  get_next_task();
  get_next_task();
  TEST_ASSERT_EQ(get_context_switch_count(), before + 2u);
}

/* set_next_task is a thin wrapper that updates current_task via get_next_task. */
static void test_set_next_task_updates_current(void) {
  full_reset();
  scheduler_init();
  task_create(dummy_task, NULL, 256, 6);
  TCB *hi = ready_lists[6];
  set_next_task();
  TEST_ASSERT(current_task == hi);
}

/* task_snapshot_list returns every live task exactly once (covers the
 * scheduler_init case where the idle task is both current_task and on a ready
 * list — must be deduplicated), spanning ready + delayed, and honours `max`. */
static void test_task_snapshot_list_covers_all_once(void) {
  full_reset();
  scheduler_init(); /* idle_task: current_task AND in the ready list */
  uint32_t a = task_create(dummy_task, NULL, 256, 1);
  uint32_t b = task_create(dummy_task, NULL, 256, 2);
  uint32_t c = task_create(dummy_task, NULL, 256, 3);

  TCB *out[8];
  int n = task_snapshot_list(out, 8);
  TEST_ASSERT_EQ(n, 4); /* idle + a + b + c, idle NOT double-counted */

  int idle = 0, sa = 0, sb = 0, sc = 0;
  for (int i = 0; i < n; i++) {
    if (out[i] == idle_task)
      idle++;
    if (out[i]->task_id == a)
      sa++;
    if (out[i]->task_id == b)
      sb++;
    if (out[i]->task_id == c)
      sc++;
  }
  TEST_ASSERT_EQ(idle, 1); /* exactly once despite being current + ready */
  TEST_ASSERT_EQ(sa, 1);
  TEST_ASSERT_EQ(sb, 1);
  TEST_ASSERT_EQ(sc, 1);

  /* `max` is honoured. */
  TEST_ASSERT_EQ(task_snapshot_list(out, 2), 2);

  /* A task on the delayed list is still covered. Move `a` ready->delayed. */
  TCB *ta = ready_lists[1];
  remove_from_ready_list(ta);
  add_to_delayed_list(ta);
  n = task_snapshot_list(out, 8);
  TEST_ASSERT_EQ(n, 4); /* same four, a now found via delayed_list */
  int found_a = 0;
  for (int i = 0; i < n; i++)
    if (out[i] == ta)
      found_a++;
  TEST_ASSERT_EQ(found_a, 1);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
void run_scheduler_tests(void) {
  TEST_SUITE_BEGIN("Scheduler");
  TEST_RUN(test_task_snapshot_list_covers_all_once);
  TEST_RUN(test_scheduler_init_creates_idle);
  TEST_RUN(test_scheduler_init_current_is_idle);
  TEST_RUN(test_scheduler_init_bitmap);
  TEST_RUN(test_task_create_returns_id);
  TEST_RUN(test_task_create_increments_count);
  TEST_RUN(test_task_create_in_ready_list);
  TEST_RUN(test_task_create_oom);
  TEST_RUN(test_task_create_multi_priority);
  TEST_RUN(test_wake_up_delayed_tasks);
  TEST_RUN(test_delayed_add_remove);
  TEST_RUN(test_context_switch_count_zero);
  TEST_RUN(test_get_next_task_picks_highest_priority);
  TEST_RUN(test_get_next_task_round_robin_same_priority);
  TEST_RUN(test_get_next_task_returns_idle_when_no_tasks);
  TEST_RUN(test_get_next_task_bumps_switch_count);
  TEST_RUN(test_set_next_task_updates_current);
  TEST_SUITE_END();
}
