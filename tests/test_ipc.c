/**
 * @file test_ipc.c
 * @brief Unit tests for vaios IPC primitives (ipc.c):
 *        binary semaphores, counting semaphores, mutexes (static & dynamic).
 *
 * Because task_yield() and task_block() are stubbed to be no-ops / counters,
 * we test the count arithmetic and guard checks without needing a real
 * scheduler running.
 */
#include "framework.h"
#include "ipc.h"
#include "memory.h"
#include "task.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Stubs declared in stubs.c
 * ---------------------------------------------------------------------- */
extern void stub_reset_heap(void);
extern void stub_set_ticks(uint32_t t);

/* External kernel globals needed to set up a minimal current_task */
extern TCB *current_task;
extern TCB *idle_task;
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern uint32_t task_count;
extern uint32_t context_switch_count;

/* -------------------------------------------------------------------------
 * Helper: reset everything between tests
 * ---------------------------------------------------------------------- */
static TCB _fake_task;

static void full_reset(void) {
  stub_reset_heap();
  stub_set_ticks(0);

  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = NULL;
  ready_bitmap = 0;
  task_count = 0;
  context_switch_count = 0;

  /* Provide a minimal current_task so IPC code can call get_current_task() */
  memset(&_fake_task, 0, sizeof(_fake_task));
  _fake_task.task_id = 1;
  _fake_task.priority = 3;
  _fake_task.status = TASK_RUNNING;
  current_task = &_fake_task;
  idle_task = &_fake_task;
}

/* -------------------------------------------------------------------------
 * Binary Semaphore Tests
 * ---------------------------------------------------------------------- */

/* create_binary returns non-NULL handle */
static void test_binary_sem_create(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  TEST_ASSERT_NOT_NULL(sem);
}

/* New binary semaphore starts at 0 – take with timeout 0 returns FAIL */
static void test_binary_sem_take_empty(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  TEST_ASSERT_NOT_NULL(sem);
  int ret = v_semaphore_take(sem, 0);
  TEST_ASSERT_EQ(ret, VA_FAIL);
}

/* give then take succeeds */
static void test_binary_sem_give_take(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  TEST_ASSERT_NOT_NULL(sem);
  int g = v_semaphore_give(sem);
  TEST_ASSERT_EQ(g, VA_PASS);
  int t = v_semaphore_take(sem, 0);
  TEST_ASSERT_EQ(t, VA_PASS);
}

/* Binary semaphore cannot be given twice (limit 1) */
static void test_binary_sem_no_overflow(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  v_semaphore_give(sem);
  int ret = v_semaphore_give(sem); /* already full */
  TEST_ASSERT_EQ(ret, VA_FAIL);
}

/* NULL handle returns FAIL */
static void test_binary_sem_null_handle(void) {
  full_reset();
  TEST_ASSERT_EQ(v_semaphore_take(NULL, 0), VA_FAIL);
  TEST_ASSERT_EQ(v_semaphore_give(NULL), VA_FAIL);
}

/* Static binary semaphore allocation */
static void test_binary_sem_static(void) {
  full_reset();
  StaticSemaphore_t buf;
  SemaphoreHandle_t sem = v_semaphore_create_binary_static(&buf);
  TEST_ASSERT_NOT_NULL(sem);
  v_semaphore_give(sem);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
}

/* Static binary sem with NULL buffer returns NULL */
static void test_binary_sem_static_null_buf(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary_static(NULL);
  TEST_ASSERT_NULL(sem);
}

/* give_from_isr works like give */
static void test_sem_give_from_isr(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  int woken = 0;
  int ret = v_semaphore_give_from_isr(sem, &woken);
  TEST_ASSERT_EQ(ret, VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
}

/* -------------------------------------------------------------------------
 * Counting Semaphore Tests
 * ---------------------------------------------------------------------- */

/* create_counting with initial_count > max_count returns NULL */
static void test_counting_sem_invalid_init(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_counting(3, 5);
  TEST_ASSERT_NULL(sem);
}

/* counting semaphore respects limit */
static void test_counting_sem_limit(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_counting(3, 0);
  TEST_ASSERT_NOT_NULL(sem);

  TEST_ASSERT_EQ(v_semaphore_give(sem), VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_give(sem), VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_give(sem), VA_PASS);
  /* 4th give exceeds limit */
  TEST_ASSERT_EQ(v_semaphore_give(sem), VA_FAIL);
}

/* counting semaphore: take decrements count */
static void test_counting_sem_take(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_counting(5, 3);
  TEST_ASSERT_NOT_NULL(sem);

  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
  /* count now 0 */
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_FAIL);
}

/* static counting semaphore */
static void test_counting_sem_static(void) {
  full_reset();
  StaticSemaphore_t buf;
  SemaphoreHandle_t sem = v_semaphore_create_counting_static(2, 1, &buf);
  TEST_ASSERT_NOT_NULL(sem);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_PASS);
  TEST_ASSERT_EQ(v_semaphore_take(sem, 0), VA_FAIL);
}

/* -------------------------------------------------------------------------
 * Mutex Tests
 * ---------------------------------------------------------------------- */

/* create_mutex returns non-NULL handle */
static void test_mutex_create(void) {
  full_reset();
  MutexHandle_t mtx = v_mutex_create();
  TEST_ASSERT_NOT_NULL(mtx);
}

/* Mutex starts unlocked; lock succeeds */
static void test_mutex_lock_succeeds(void) {
  full_reset();
  MutexHandle_t mtx = v_mutex_create();
  TEST_ASSERT_EQ(v_mutex_lock(mtx, 0), VA_PASS);
}

/* After locking, second immediate lock (no wait) fails */
static void test_mutex_double_lock_fails(void) {
  full_reset();
  MutexHandle_t mtx = v_mutex_create();
  v_mutex_lock(mtx, 0);
  TEST_ASSERT_EQ(v_mutex_lock(mtx, 0), VA_FAIL);
}

/* Lock then unlock then lock again succeeds */
static void test_mutex_unlock_relocks(void) {
  full_reset();
  MutexHandle_t mtx = v_mutex_create();
  v_mutex_lock(mtx, 0);
  TEST_ASSERT_EQ(v_mutex_unlock(mtx), VA_PASS);
  TEST_ASSERT_EQ(v_mutex_lock(mtx, 0), VA_PASS);
}

/* NULL mutex lock/unlock returns FAIL */
static void test_mutex_null_handle(void) {
  full_reset();
  TEST_ASSERT_EQ(v_mutex_lock(NULL, 0), VA_FAIL);
  TEST_ASSERT_EQ(v_mutex_unlock(NULL), VA_FAIL);
}

/* Static mutex allocation */
static void test_mutex_static(void) {
  full_reset();
  StaticSemaphore_t buf;
  MutexHandle_t mtx = v_mutex_create_static(&buf);
  TEST_ASSERT_NOT_NULL(mtx);
  TEST_ASSERT_EQ(v_mutex_lock(mtx, 0), VA_PASS);
  TEST_ASSERT_EQ(v_mutex_unlock(mtx), VA_PASS);
}

/* -------------------------------------------------------------------------
 * Phase 2: regression tests for the two critical bug fixes
 *   - wait_next separates the sema wait queue from blocked_list
 *   - v_mutex_unlock_recursive used to leak a critical section on the
 *     recursion-zero branch
 * ---------------------------------------------------------------------- */

/* Static TCBs the wait-queue tests construct manually. Static so they
 * outlive the wake_up_delayed_tasks_isr call. */
static TCB _phase2_t1, _phase2_t2;

/* Park two tasks on `sem`'s wait queue (via wait_next) AND on blocked_list
 * (via next/prev), both with the same timeout deadline. Wait-queue order:
 * priority-ordered, head = highest priority. */
static void phase2_setup_two_waiters(SemaphoreHandle_t sem, uint32_t deadline) {
  sema_t *s = (sema_t *)sem;
  memset(&_phase2_t1, 0, sizeof(_phase2_t1));
  memset(&_phase2_t2, 0, sizeof(_phase2_t2));
  _phase2_t1.task_id = 10;
  _phase2_t1.priority = 2;
  _phase2_t1.status = TASK_BLOCKED;
  _phase2_t1.wait_sem = sem;
  _phase2_t1.delay_ticks = deadline;
  _phase2_t2.task_id = 11;
  _phase2_t2.priority = 3;
  _phase2_t2.status = TASK_BLOCKED;
  _phase2_t2.wait_sem = sem;
  _phase2_t2.delay_ticks = deadline;

  s->wait_q = &_phase2_t2;
  s->tail = &_phase2_t1;
  _phase2_t2.wait_next = &_phase2_t1;
  _phase2_t1.wait_next = NULL;

  add_to_blocked_list(&_phase2_t1);
  add_to_blocked_list(&_phase2_t2);
}

/* Wait queue (wait_next) and blocked_list (next/prev) carry the same tasks
 * at once without corrupting each other. Pre-fix, add_to_blocked_list cleared
 * task->next — which was the wait_q link — and lost waiters. */
static void test_wait_queue_independent_of_blocked_list(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  phase2_setup_two_waiters(sem, 1000);
  sema_t *s = (sema_t *)sem;

  /* Wait-queue walk via wait_next still works. */
  TEST_ASSERT_EQ((TCB *)s->wait_q, &_phase2_t2);
  TEST_ASSERT_EQ(_phase2_t2.wait_next, &_phase2_t1);
  TEST_ASSERT_NULL(_phase2_t1.wait_next);

  /* blocked_list walk via next contains both. */
  int found_t1 = 0, found_t2 = 0;
  for (TCB *p = blocked_list; p; p = p->next) {
    if (p == &_phase2_t1)
      found_t1 = 1;
    if (p == &_phase2_t2)
      found_t2 = 1;
  }
  TEST_ASSERT(found_t1);
  TEST_ASSERT(found_t2);
}

/* SysTick's timeout-eject walks the wait queue via wait_next and removes
 * both waiters from the sema and from blocked_list. Pre-fix, the eject walk
 * via task->next wandered into blocked_list and lost a waiter. */
static void test_sema_timeout_ejects_multiple_waiters(void) {
  full_reset();
  SemaphoreHandle_t sem = v_semaphore_create_binary();
  phase2_setup_two_waiters(sem, 1000);
  sema_t *s = (sema_t *)sem;

  stub_set_ticks(2000); /* past deadline */
  wake_up_delayed_tasks_isr();

  TEST_ASSERT_EQ(_phase2_t1.status, TASK_READY);
  TEST_ASSERT_EQ(_phase2_t2.status, TASK_READY);
  TEST_ASSERT_NULL(s->wait_q);
  TEST_ASSERT_NULL(blocked_list);
}

/* v_mutex_lock_recursive / v_mutex_unlock_recursive functional coverage —
 * the recursion-zero branch must hand off cleanly to v_mutex_unlock and
 * leave the mutex unowned. The BASEPRI critical-section leak the campaign
 * fixed is target-only (host ENTER/EXIT_CRITICAL are no-ops); this verifies
 * the end-to-end behaviour the leak prevented. */
static void test_mutex_recursive_lock_unlock(void) {
  full_reset();
  MutexHandle_t mtx = v_mutex_create_recursive();
  TEST_ASSERT_NOT_NULL(mtx);
  rmutex_t *rm = (rmutex_t *)mtx;

  TEST_ASSERT_EQ(v_mutex_lock_recursive(mtx, 0), VA_PASS);
  TEST_ASSERT_EQ(rm->owner, current_task);
  TEST_ASSERT_EQ(rm->recursion_count, 1u);

  TEST_ASSERT_EQ(v_mutex_lock_recursive(mtx, 0), VA_PASS);
  TEST_ASSERT_EQ(rm->recursion_count, 2u);

  TEST_ASSERT_EQ(v_mutex_unlock_recursive(mtx), VA_PASS);
  TEST_ASSERT_EQ(rm->recursion_count, 1u);
  TEST_ASSERT_EQ(rm->owner, current_task);

  TEST_ASSERT_EQ(v_mutex_unlock_recursive(mtx), VA_PASS);
  TEST_ASSERT_EQ(rm->recursion_count, 0u);
  TEST_ASSERT_NULL(rm->owner);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
void run_ipc_tests(void) {
  TEST_SUITE_BEGIN("IPC (Semaphores & Mutexes)");
  /* Binary semaphore */
  TEST_RUN(test_binary_sem_create);
  TEST_RUN(test_binary_sem_take_empty);
  TEST_RUN(test_binary_sem_give_take);
  TEST_RUN(test_binary_sem_no_overflow);
  TEST_RUN(test_binary_sem_null_handle);
  TEST_RUN(test_binary_sem_static);
  TEST_RUN(test_binary_sem_static_null_buf);
  TEST_RUN(test_sem_give_from_isr);
  /* Counting semaphore */
  TEST_RUN(test_counting_sem_invalid_init);
  TEST_RUN(test_counting_sem_limit);
  TEST_RUN(test_counting_sem_take);
  TEST_RUN(test_counting_sem_static);
  /* Mutex */
  TEST_RUN(test_mutex_create);
  TEST_RUN(test_mutex_lock_succeeds);
  TEST_RUN(test_mutex_double_lock_fails);
  TEST_RUN(test_mutex_unlock_relocks);
  TEST_RUN(test_mutex_null_handle);
  TEST_RUN(test_mutex_static);
  /* Phase 2: campaign bug-fix regressions */
  TEST_RUN(test_wait_queue_independent_of_blocked_list);
  TEST_RUN(test_sema_timeout_ejects_multiple_waiters);
  TEST_RUN(test_mutex_recursive_lock_unlock);
  TEST_SUITE_END();
}
