/**
 * @file test_task_list.c
 * @brief Unit tests for vaios task list management (task.c generic list ops
 *        and the priority-based ready list).
 *
 * Tests: enqueue, remove, dequeue, peek on generic lists;
 *        ready list add/remove, priority bitmap management;
 *        get_highest_priority_task across multiple priorities.
 */
#include "framework.h"
#include "memory.h"
#include "task.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * External globals from task.c
 * ---------------------------------------------------------------------- */
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;

/* -------------------------------------------------------------------------
 * Stubs declared in stubs.c
 * ---------------------------------------------------------------------- */
extern void stub_reset_heap(void);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
/* Statically allocate a fake TCB so we can test list ops without v_malloc */
static TCB make_task(uint32_t id, uint32_t prio) {
  TCB t;
  memset(&t, 0, sizeof(t));
  t.task_id = id;
  t.priority = prio;
  t.status = TASK_READY;
  return t;
}

/* Reset all task module globals between tests. get_task_by_id (used by
 * task_unblock) dereferences current_task, so point it at a sentinel TCB
 * with an id that will never collide with a real test task. */
static TCB _dummy_current = {0};
static void reset_task_state(void) {
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = NULL;
  delayed_list = NULL;
  ready_bitmap = 0;
  _dummy_current.task_id = UINT32_MAX;
  current_task = &_dummy_current;
}

/* -------------------------------------------------------------------------
 * Generic List Tests
 * ---------------------------------------------------------------------- */

/* enqueue single node → list head points to it */
static void test_enqueue_single(void) {
  TCB *list = NULL;
  TCB t = make_task(1, 0);
  enqueue_task(&list, &t);
  TEST_ASSERT_NOT_NULL(list);
  TEST_ASSERT_EQ(list->task_id, 1u);
  TEST_ASSERT_NULL(list->next);
}

/* enqueue two nodes → FIFO order */
static void test_enqueue_two(void) {
  TCB *list = NULL;
  TCB a = make_task(1, 0);
  TCB b = make_task(2, 0);
  enqueue_task(&list, &a);
  enqueue_task(&list, &b);
  TEST_ASSERT_EQ(list->task_id, 1u);
  TEST_ASSERT_NOT_NULL(list->next);
  TEST_ASSERT_EQ(list->next->task_id, 2u);
}

/* enqueue NULL is a no-op */
static void test_enqueue_null(void) {
  TCB *list = NULL;
  enqueue_task(&list, NULL);
  TEST_ASSERT_NULL(list);
}

/* peek returns head without removing */
static void test_peek(void) {
  TCB *list = NULL;
  TCB t = make_task(42, 0);
  enqueue_task(&list, &t);
  TCB *p = peek_task(&list);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(p->task_id, 42u);
  TEST_ASSERT_NOT_NULL(list); /* still in list */
}

/* dequeue removes and returns head */
static void test_dequeue(void) {
  TCB *list = NULL;
  TCB a = make_task(1, 0);
  TCB b = make_task(2, 0);
  enqueue_task(&list, &a);
  enqueue_task(&list, &b);

  TCB *got = dequeue_task(&list);
  TEST_ASSERT_NOT_NULL(got);
  TEST_ASSERT_EQ(got->task_id, 1u);
  TEST_ASSERT_EQ(list->task_id, 2u);
}

/* dequeue on empty list returns NULL */
static void test_dequeue_empty(void) {
  TCB *list = NULL;
  TCB *got = dequeue_task(&list);
  TEST_ASSERT_NULL(got);
}

/* remove head of list */
static void test_remove_head(void) {
  TCB *list = NULL;
  TCB a = make_task(1, 0);
  TCB b = make_task(2, 0);
  enqueue_task(&list, &a);
  enqueue_task(&list, &b);

  remove_task(&list, &a);
  TEST_ASSERT_EQ(list->task_id, 2u);
  TEST_ASSERT_NULL(a.next);
  TEST_ASSERT_NULL(a.prev);
}

/* remove tail of list */
static void test_remove_tail(void) {
  TCB *list = NULL;
  TCB a = make_task(1, 0);
  TCB b = make_task(2, 0);
  enqueue_task(&list, &a);
  enqueue_task(&list, &b);

  remove_task(&list, &b);
  TEST_ASSERT_EQ(list->task_id, 1u);
  TEST_ASSERT_NULL(list->next);
}

/* remove middle node */
static void test_remove_middle(void) {
  TCB *list = NULL;
  TCB a = make_task(1, 0);
  TCB b = make_task(2, 0);
  TCB c = make_task(3, 0);
  enqueue_task(&list, &a);
  enqueue_task(&list, &b);
  enqueue_task(&list, &c);

  remove_task(&list, &b);
  TEST_ASSERT_EQ(list->task_id, 1u);
  TEST_ASSERT_EQ(list->next->task_id, 3u);
  TEST_ASSERT_NULL(b.next);
  TEST_ASSERT_NULL(b.prev);
}

/* -------------------------------------------------------------------------
 * Ready List Tests
 * ---------------------------------------------------------------------- */

/* add_to_ready_list sets task status to READY and updates bitmap */
static void test_ready_add_single(void) {
  reset_task_state();
  TCB t = make_task(1, 3);
  add_to_ready_list(&t);
  TEST_ASSERT_EQ(t.status, TASK_READY);
  TEST_ASSERT(ready_bitmap & (1u << 3u));
  TEST_ASSERT_NOT_NULL(ready_lists[3]);
}

/* remove_from_ready_list clears bitmap when list becomes empty */
static void test_ready_remove_clears_bitmap(void) {
  reset_task_state();
  TCB t = make_task(1, 5);
  add_to_ready_list(&t);
  TEST_ASSERT(ready_bitmap & (1u << 5u));

  remove_from_ready_list(&t);
  TEST_ASSERT_EQ(ready_lists[5], (TCB *)NULL);
  TEST_ASSERT(!(ready_bitmap & (1u << 5u)));
}

/* get_highest_priority_task returns task at highest set priority */
static void test_get_highest_priority(void) {
  reset_task_state();
  TCB lo = make_task(1, 1);
  TCB hi = make_task(2, 6);
  add_to_ready_list(&lo);
  add_to_ready_list(&hi);

  TCB *best = get_highest_priority_task();
  TEST_ASSERT_NOT_NULL(best);
  TEST_ASSERT_EQ(best->task_id, 2u);
}

/* get_highest_priority_task returns NULL when no tasks ready */
static void test_get_highest_empty(void) {
  reset_task_state();
  TCB *best = get_highest_priority_task();
  TEST_ASSERT_NULL(best);
}

/* Multiple tasks at same priority – first enqueued is returned */
static void test_ready_fifo_same_priority(void) {
  reset_task_state();
  TCB a = make_task(1, 4);
  TCB b = make_task(2, 4);
  add_to_ready_list(&a);
  add_to_ready_list(&b);

  TCB *best = get_highest_priority_task();
  TEST_ASSERT_EQ(best->task_id, 1u); /* FIFO */
}

/* -------------------------------------------------------------------------
 * Blocked / Delayed list tests
 * ---------------------------------------------------------------------- */

/* add_to_blocked_list sets TASK_BLOCKED status */
static void test_blocked_add(void) {
  reset_task_state();
  TCB t = make_task(1, 2);
  add_to_blocked_list(&t);
  TEST_ASSERT_EQ(t.status, TASK_BLOCKED);
  TEST_ASSERT_NOT_NULL(blocked_list);
}

/* task_unblock moves task from blocked to ready */
static void test_task_unblock(void) {
  reset_task_state();
  TCB t = make_task(1, 3);
  t.status = TASK_BLOCKED;
  add_to_blocked_list(&t);
  TEST_ASSERT_NULL(ready_lists[3]);

  task_unblock(t.task_id);
  TEST_ASSERT_EQ(t.status, TASK_READY);
  TEST_ASSERT_NOT_NULL(ready_lists[3]);
  TEST_ASSERT_NULL(blocked_list);
}

/* task_unblock on non-blocked task is a no-op */
static void test_task_unblock_noop(void) {
  reset_task_state();
  TCB t = make_task(1, 2);
  t.status = TASK_READY;
  task_unblock(t.task_id);          /* should not crash */
  TEST_ASSERT_NULL(ready_lists[2]); /* not added to ready list */
}

/* add_to_delayed_list sets TASK_DELAYED status */
static void test_delayed_add(void) {
  reset_task_state();
  TCB t = make_task(1, 1);
  add_to_delayed_list(&t);
  TEST_ASSERT_EQ(t.status, TASK_DELAYED);
  TEST_ASSERT_NOT_NULL(delayed_list);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
static const test_case_t task_list_cases[] = {
    TEST_CASE(test_enqueue_single),
    TEST_CASE(test_enqueue_two),
    TEST_CASE(test_enqueue_null),
    TEST_CASE(test_peek),
    TEST_CASE(test_dequeue),
    TEST_CASE(test_dequeue_empty),
    TEST_CASE(test_remove_head),
    TEST_CASE(test_remove_tail),
    TEST_CASE(test_remove_middle),
    TEST_CASE(test_ready_add_single),
    TEST_CASE(test_ready_remove_clears_bitmap),
    TEST_CASE(test_get_highest_priority),
    TEST_CASE(test_get_highest_empty),
    TEST_CASE(test_ready_fifo_same_priority),
    TEST_CASE(test_blocked_add),
    TEST_CASE(test_task_unblock),
    TEST_CASE(test_task_unblock_noop),
    TEST_CASE(test_delayed_add),
};
const test_suite_t task_list_suite = {
    .name = "Task List Management",
    .cases = task_list_cases,
    .count = TEST_COUNT(task_list_cases),
};
