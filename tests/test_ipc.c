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
  TEST_SUITE_END();
}
