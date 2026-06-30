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
 * Phase 3: tests for new IPC behaviour
 *   - B2: priority-ordered semaphore wait queues
 *   - B3: transitive priority inheritance + recompute-on-unlock
 * ---------------------------------------------------------------------- */

/* B2 — wait_q_enqueue inserts by priority. Two consecutive blocking takes
 * by different-priority "tasks" leave the more urgent one at the head; the
 * less urgent one follows via wait_next (ties remain FIFO). */
static void test_wait_queue_priority_order(void) {
  full_reset();
  static TCB t_lo, t_hi;
  memset(&t_lo, 0, sizeof(t_lo));
  memset(&t_hi, 0, sizeof(t_hi));
  t_lo.task_id = 50;
  t_lo.priority = 2;
  t_lo.status = TASK_RUNNING;
  t_hi.task_id = 51;
  t_hi.priority = 5;
  t_hi.status = TASK_RUNNING;

  SemaphoreHandle_t sem = v_semaphore_create_binary(); /* initial count 0 */
  sema_t *s = (sema_t *)sem;

  current_task = &t_lo;
  v_semaphore_take(sem, 1000); /* blocks → t_lo on wait_q (sole) */
  TEST_ASSERT_EQ((TCB *)s->wait_q, &t_lo);

  current_task = &t_hi;
  v_semaphore_take(sem, 1000); /* blocks → t_hi inserted before t_lo */
  TEST_ASSERT_EQ((TCB *)s->wait_q, &t_hi);
  TEST_ASSERT_EQ(t_hi.wait_next, &t_lo);
  TEST_ASSERT_EQ((TCB *)s->tail, &t_lo);
}

/* B3 — transitive PI chain walk. H wants M1 (owner L1, mid-priority).
 * L1 is itself blocked acquiring M2 (owner L2, low-priority). Boosting only
 * L1 isn't enough — L2 must also be boosted, or L2 stays low-priority,
 * gets preempted by mids, never finishes, never releases M2, and the chain
 * never resolves. */
static void test_pi_transitive_chain(void) {
  full_reset();
  static TCB l1, l2, h;
  memset(&l1, 0, sizeof(l1));
  memset(&l2, 0, sizeof(l2));
  memset(&h, 0, sizeof(h));
  l1.task_id = 60;
  l1.priority = l1.base_priority = 2;
  l1.status = TASK_RUNNING;
  l2.task_id = 61;
  l2.priority = l2.base_priority = 1;
  l2.status = TASK_RUNNING;
  h.task_id = 62;
  h.priority = h.base_priority = 5;
  h.status = TASK_RUNNING;

  MutexHandle_t m1 = v_mutex_create();
  MutexHandle_t m2 = v_mutex_create();
  rmutex_t *rm2 = (rmutex_t *)m2;

  current_task = &l2;
  TEST_ASSERT_EQ(v_mutex_lock(m2, 0), VA_PASS);
  current_task = &l1;
  TEST_ASSERT_EQ(v_mutex_lock(m1, 0), VA_PASS);

  /* Stand in for L1 being blocked acquiring M2 — the host stub can't really
   * suspend a task, so we set the chain-walk hooks (status + wait_mutex)
   * manually. */
  l1.status = TASK_BLOCKED;
  l1.wait_mutex = m2;
  l1.wait_sem = &rm2->base;

  /* H tries to lock M1. The take returns VA_FAIL immediately (count==0,
   * ticks==0) but the chain walk runs inside its critical section first. */
  current_task = &h;
  v_mutex_lock(m1, 0);

  TEST_ASSERT_EQ(l1.priority, 5u); /* boosted by H */
  TEST_ASSERT_EQ(l2.priority, 5u); /* transitively boosted */
}

/* B3 — on unlock, the owner's priority is recomputed as
 * max(base_priority, top waiter across all STILL-held mutexes). The old
 * code unconditionally dropped to base, dropping below the priority of a
 * waiter on a different held mutex — silent inversion. */
static void test_pi_unlock_keeps_priority_floor(void) {
  full_reset();
  static TCB owner_t, mid, high;
  memset(&owner_t, 0, sizeof(owner_t));
  memset(&mid, 0, sizeof(mid));
  memset(&high, 0, sizeof(high));
  owner_t.task_id = 70;
  owner_t.priority = owner_t.base_priority = 1;
  owner_t.status = TASK_RUNNING;
  mid.task_id = 71;
  mid.priority = mid.base_priority = 3;
  high.task_id = 72;
  high.priority = high.base_priority = 5;

  MutexHandle_t m1 = v_mutex_create();
  MutexHandle_t m2 = v_mutex_create();
  rmutex_t *rm1 = (rmutex_t *)m1;
  rmutex_t *rm2 = (rmutex_t *)m2;

  current_task = &owner_t;
  TEST_ASSERT_EQ(v_mutex_lock(m1, 0), VA_PASS);
  TEST_ASSERT_EQ(v_mutex_lock(m2, 0), VA_PASS);

  /* Park `mid` on M1's wait queue and `high` on M2's. */
  sema_t *s1 = &rm1->base;
  sema_t *s2 = &rm2->base;
  s1->wait_q = &mid;
  s1->tail = &mid;
  mid.wait_next = NULL;
  s2->wait_q = &high;
  s2->tail = &high;
  high.wait_next = NULL;

  /* Pretend the chain walk has already boosted owner to high's priority. */
  owner_t.priority = 5;

  /* Release M2. Recompute: max(base=1, top of M1 = mid=3) = 3. Must NOT
   * collapse to base while M1 still has a higher-priority waiter. */
  TEST_ASSERT_EQ(v_mutex_unlock(m2), VA_PASS);
  TEST_ASSERT_EQ(owner_t.priority, 3u);

  /* Release M1. No more held mutexes → drop to base. */
  TEST_ASSERT_EQ(v_mutex_unlock(m1), VA_PASS);
  TEST_ASSERT_EQ(owner_t.priority, 1u);
}

/* -------------------------------------------------------------------------
 * FromISR priority assert predicate (problems/vaios-issues.md). An IRQ more
 * urgent than MAX_SYSCALL_INTERRUPT_PRIORITY must not call *_from_isr APIs.
 * ---------------------------------------------------------------------- */
static void test_isr_prio_thread_mode_is_safe(void) {
  /* VECTACTIVE < 16: thread mode / a system handler — no NVIC priority to break. */
  TEST_ASSERT_EQ(vaios_isr_priority_is_safe(0, 0), 1);
  TEST_ASSERT_EQ(vaios_isr_priority_is_safe(15, 0), 1);
}
static void test_isr_prio_maskable_irq_is_safe(void) {
  /* External IRQ (>=16) at or below the syscall band (numerically >= threshold). */
  TEST_ASSERT_EQ(vaios_isr_priority_is_safe(16, MAX_SYSCALL_INTERRUPT_PRIORITY), 1);
  TEST_ASSERT_EQ(
      vaios_isr_priority_is_safe(42, MAX_SYSCALL_INTERRUPT_PRIORITY + 0x10), 1);
}
static void test_isr_prio_urgent_irq_is_unsafe(void) {
  /* External IRQ more urgent than the threshold (numerically lower) — unmaskable. */
  TEST_ASSERT_EQ(vaios_isr_priority_is_safe(16, 0), 0);
  TEST_ASSERT_EQ(
      vaios_isr_priority_is_safe(50, MAX_SYSCALL_INTERRUPT_PRIORITY - 0x10), 0);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
void run_ipc_tests(void) {
  TEST_SUITE_BEGIN("IPC (Semaphores & Mutexes)");
  /* FromISR priority assert predicate */
  TEST_RUN(test_isr_prio_thread_mode_is_safe);
  TEST_RUN(test_isr_prio_maskable_irq_is_safe);
  TEST_RUN(test_isr_prio_urgent_irq_is_unsafe);
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
  /* Phase 3: new IPC behaviour */
  TEST_RUN(test_wait_queue_priority_order);
  TEST_RUN(test_pi_transitive_chain);
  TEST_RUN(test_pi_unlock_keeps_priority_floor);
  TEST_SUITE_END();
}
