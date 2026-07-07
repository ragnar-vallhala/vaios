#include "ipc.h"
#include "atomic.h"
#include "memory.h"
#include "perf_hooks.h"
#include "port.h" // ENTER_CRITICAL / EXIT_CRITICAL[_FROM_ISR]
#include "syscall.h" // SVC trap wrappers (VAIOS_SYSCALL_SVC)
#include "task.h"
#include "utils.h"

//-----------------------------------------------------------------------------
// Wait Queue Helpers (priority-ordered, highest priority at head)
//-----------------------------------------------------------------------------
// Insert sorted by priority, most urgent at the head, so wait_q_dequeue
// (which pops the head) always hands the slot to the highest-priority
// waiter. Ties keep FIFO order among equal priorities, so a slow
// low-priority waiter can no longer delay a high-priority control task
// blocked on the same semaphore. O(n) in waiters on this one semaphore,
// which is bounded and typically 1-3.
// The wait queue threads through task->wait_next — never task->next — because
// a task blocked on a timed take also sits on blocked_list (via next/prev) at
// the same time.
static void wait_q_enqueue(sema_t *sem, TCB *task) {
  task->wait_next = NULL;

  // Empty queue, or task outranks the current head -> new head.
  if (!sem->wait_q || sem->wait_q->priority < task->priority) {
    task->wait_next = sem->wait_q;
    sem->wait_q = task;
    if (!task->wait_next)
      sem->tail = task;
    return;
  }

  // Walk past every waiter with priority >= task's (keeps ties FIFO).
  TCB *p = sem->wait_q;
  while (p->wait_next && p->wait_next->priority >= task->priority)
    p = p->wait_next;

  task->wait_next = p->wait_next;
  p->wait_next = task;
  if (!task->wait_next)
    sem->tail = task;
}

static TCB *wait_q_dequeue(sema_t *sem) {
  TCB *task = sem->wait_q;
  if (!task)
    return NULL;

  sem->wait_q = task->wait_next;
  if (!sem->wait_q)
    sem->tail = NULL;

  task->wait_next = NULL;
  return task;
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Creation
//-----------------------------------------------------------------------------
static sema_t *sema_init(sema_t *sem, uint32_t initial_count, uint32_t limit) {
  sem->wait_q = NULL;
  sem->tail = NULL;
  atomic_set(&sem->count, initial_count);
  atomic_set(&sem->limit, limit);
  return sem;
}

SemaphoreHandle_t v_semaphore_create_binary(void) {
  sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
  if (!sem)
    return NULL;
  return (SemaphoreHandle_t)sema_init(sem, 0, 1);
}

SemaphoreHandle_t
v_semaphore_create_binary_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, 0, 1);
}

SemaphoreHandle_t v_semaphore_create_counting(uint32_t max_count,
                                              uint32_t initial_count) {
  if (initial_count > max_count)
    return NULL;
  sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
  if (!sem)
    return NULL;
  return (SemaphoreHandle_t)sema_init(sem, initial_count, max_count);
}

SemaphoreHandle_t
v_semaphore_create_counting_static(uint32_t max_count, uint32_t initial_count,
                                   StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer || initial_count > max_count)
    return NULL;
  return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, initial_count,
                                      max_count);
}

MutexHandle_t v_mutex_create(void) {
  rmutex_t *mtx = (rmutex_t *)v_malloc(sizeof(rmutex_t));
  if (!mtx)
    return NULL;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive(void) {
  rmutex_t *mtx = (rmutex_t *)v_malloc(sizeof(rmutex_t));
  if (!mtx)
    return NULL;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Operations
//-----------------------------------------------------------------------------
static int semaphore_take_common(sema_t *s, uint32_t ticks_to_wait) {
  TCB *current = get_current_task();
  PERF_IPC_TAKE_ATTEMPT();
  ENTER_CRITICAL();

  if (atomic_get(&s->count) > 0) {
    atomic_dec(&s->count);
    EXIT_CRITICAL();
    return VA_PASS;
  }

  if (ticks_to_wait == 0) {
    EXIT_CRITICAL();
    return VA_FAIL; // would block
  }

  // Block task and enqueue in semaphore wait queue
  current->status = TASK_BLOCKED;
  current->delay_ticks = v_get_ticks() + ticks_to_wait;
  current->wait_sem = s; // track which semaphore we are waiting on
  wait_q_enqueue(s, current);
  add_to_blocked_list(
      current); // so wake_up_delayed_tasks can find us on timeout

  PERF_IPC_TAKE_BLOCKED();
  // Leave the critical section BEFORE pending the switch. Commit 5b3df99 had
  // reordered these (task_yield while still BASEPRI-masked, EXIT_CRITICAL after)
  // to make block-and-yield atomic. On real STM32F401 that opened a give<->take
  // race: a DMA-completion v_semaphore_give_from_isr landing between the
  // BLOCKED-mark and the taken switch left the waiting task's wake lost, so it
  // always fell through to its timeout — the IMU DMA reader got stuck in 20 Hz
  // recovery (task_count frozen) and the estimator diverged to a bank-angle
  // failsafe. Confirmed on-target; the NavHAL maskable-IRQ-priority fix is the
  // real orphan-bug cure, so dropping the reorder is safe.
  EXIT_CRITICAL();
  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)

#if VAIOS_SYSCALL_SVC
  if (!v_in_thread_mode()) {
    // Reached via a syscall (handler mode): the switch is deferred to SVCall
    // return, so we cannot resume-check inline (wait_sem is still set). Return a
    // placeholder; the waker (give / timeout) stashes the real result and PendSV
    // delivers it into our stacked r0 when we are next scheduled in.
    return V_SYSCALL_BLOCKED;
  }
#endif
  // Thread-mode (direct call) or SVC off: the switch already happened, so resume
  // here. semaphore_give cleared wait_sem → VA_PASS; a timeout left it set →
  // VA_FAIL.
  if (current->wait_sem != NULL) {
    current->wait_sem = NULL;
    PERF_IPC_TIMEOUT();
    return VA_FAIL;
  }
  return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks_to_wait) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc2(SYS_sem_take, (uint32_t)(uintptr_t)sem, ticks_to_wait);
#endif
  if (!sem)
    return VA_FAIL;
  return semaphore_take_common((sema_t *)sem, ticks_to_wait);
}

// Highest priority among the tasks queued on a mutex (0 if none). The wait
// queue is priority-ordered, so the head is the most urgent waiter.
static uint32_t mutex_top_waiter_prio(rmutex_t *rm) {
  return rm->base.wait_q ? rm->base.wait_q->priority : 0;
}

int v_mutex_lock(MutexHandle_t mtx, uint32_t ticks_to_wait) {
  if (!mtx)
    return VA_FAIL;
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc2(SYS_mutex_lock, (uint32_t)(uintptr_t)mtx, ticks_to_wait);
#endif
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  // Transitive priority inheritance: walk the ownership chain and boost
  // every owner outranked by `current`. Each hop follows the boosted owner's
  // own wait_mutex (the mutex it is itself blocked acquiring), so a chain
  // low -> mid -> high resolves end to end. Capped at MAX_PI_DEPTH — a
  // deeper chain is a dependency cycle and panics.
  {
    rmutex_t *m = rm;
    int depth = 0;
    while (m && m->owner && m->owner->priority < current->priority) {
      if (++depth > MAX_PI_DEPTH)
        v_panic(__FILE__, __LINE__,
                "priority-inheritance chain exceeded MAX_PI_DEPTH (cycle?)");
      TCB *o = m->owner;
      task_change_priority(o, current->priority);
      // Propagate only while the boosted owner is itself blocked on a mutex.
      if (o->status != TASK_BLOCKED || !o->wait_mutex)
        break;
      m = (rmutex_t *)o->wait_mutex;
    }
  }
  current->wait_mutex = rm; // visible to other chain walks while we block
  EXIT_CRITICAL();

  int res = semaphore_take_common(&rm->base, ticks_to_wait);

#if VAIOS_SYSCALL_SVC
  if (res == V_SYSCALL_BLOCKED) {
    // Blocked via syscall (handler mode). On grant, v_mutex_unlock's direct
    // handoff already set ownership and cleared wait_mutex; on timeout the
    // ejector clears both. The VA_PASS/VA_FAIL result reaches our stacked r0 on
    // resume — nothing more to do here.
    return res;
  }
#endif

  ENTER_CRITICAL();
  current->wait_mutex = NULL;
  // Set ownership only for an UNCONTENDED acquire; a contended-then-granted lock
  // already had ownership handed to it by v_mutex_unlock's direct handoff.
  if (res == VA_PASS && rm->owner != current) {
    rm->owner = current;
    rm->recursion_count = 1;
    // Track on the owner's held list for the unlock-time priority recompute.
    rm->next_held = (rmutex_t *)current->held_mutexes;
    current->held_mutexes = rm;
  }
  EXIT_CRITICAL();
  return res;
}

static int semaphore_give_common(sema_t *s, int *pxHigherPriorityTaskWoken) {
  PERF_IPC_GIVE();
  // FromISR-safe critical section: this function is shared by v_semaphore_give
  // (task context) and v_semaphore_give_from_isr (ISR context). Saving/
  // restoring BASEPRI in a local — instead of bumping the global
  // critical_nesting — keeps the ISR path from corrupting a preempted task's
  // critical-section accounting. The atomic_* below are lock-free, so they take
  // no critical section of their own.
  uint32_t isr_state = ENTER_CRITICAL_FROM_ISR();

  // Unblock first waiting task if any — hand off the slot directly
  TCB *to_unblock = wait_q_dequeue(s);
  if (to_unblock) {
    // Clear wait_sem so take() knows it was given (not a timeout).
    // Also remove from blocked_list since we are no longer waiting.
    to_unblock->wait_sem = NULL;
    remove_from_blocked_list(to_unblock);
    to_unblock->status = TASK_READY;
    add_to_ready_list(to_unblock);
#if VAIOS_SYSCALL_SVC
    // Deliver VA_PASS to a syscall-blocked take() when it next runs.
    v_syscall_wake_result(to_unblock, VA_PASS);
#endif

    if (pxHigherPriorityTaskWoken &&
        to_unblock->priority > get_current_task()->priority)
      *pxHigherPriorityTaskWoken = 1;

    EXIT_CRITICAL_FROM_ISR(isr_state);
    return VA_PASS;
  }

  if (atomic_get(&s->count) >= atomic_get(&s->limit)) {
    EXIT_CRITICAL_FROM_ISR(isr_state);
    return VA_FAIL; // full
  }

  atomic_inc(&s->count);
  EXIT_CRITICAL_FROM_ISR(isr_state);
  return VA_PASS;
}

int v_semaphore_give(SemaphoreHandle_t sem) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc1(SYS_sem_give, (uint32_t)(uintptr_t)sem);
#endif
  if (!sem)
    return VA_FAIL;
  return semaphore_give_common((sema_t *)sem, NULL);
}

int vaios_isr_priority_is_safe(uint32_t vectactive, uint32_t irq_prio) {
  if (vectactive < 16u)
    return 1; // thread mode or a system handler — no NVIC priority to violate
  return irq_prio >= (uint32_t)MAX_SYSCALL_INTERRUPT_PRIORITY;
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem,
                              int *pxHigherPriorityTaskWoken) {
  if (!sem)
    return VA_FAIL;
#if VAIOS_FROMISR_PRIO_CHECK
  // Enforce the FromISR priority contract the scheduler's correctness relies on:
  // an IRQ more urgent than MAX_SYSCALL_INTERRUPT_PRIORITY is not masked by the
  // kernel's critical sections, so calling this from there can tear the
  // scheduler lists. Fail loudly and locatably instead of corrupting silently.
  {
    uint32_t vectactive = 0;
    uint32_t prio = v_port_hw_active_irq_priority(&vectactive);
    if (!vaios_isr_priority_is_safe(vectactive, prio))
      v_panic(__FILE__, __LINE__,
              "v_semaphore_give_from_isr called from an IRQ above the syscall "
              "priority; raise its NVIC priority to the maskable band");
  }
#endif
  return semaphore_give_common((sema_t *)sem, pxHigherPriorityTaskWoken);
}

int v_mutex_unlock(MutexHandle_t mtx) {
  if (!mtx)
    return VA_FAIL;
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc1(SYS_mutex_unlock, (uint32_t)(uintptr_t)mtx);
#endif
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  if (rm->owner != current) {
    EXIT_CRITICAL();
    return VA_FAIL; // Not the owner
  }

  // Unlink rm from the owner's held-mutex list.
  rmutex_t **pp = (rmutex_t **)&current->held_mutexes;
  while (*pp && *pp != rm)
    pp = &(*pp)->next_held;
  if (*pp)
    *pp = rm->next_held;
  rm->next_held = NULL;

  // Recompute the owner's priority: its base, raised to the highest-priority
  // waiter across the mutexes it STILL holds. Replaces the old unconditional
  // drop-to-base, which was wrong whenever the owner held another mutex that
  // still had a high-priority task waiting on it.
  uint32_t newp = current->base_priority;
  for (rmutex_t *m = (rmutex_t *)current->held_mutexes; m; m = m->next_held) {
    uint32_t w = mutex_top_waiter_prio(m);
    if (w > newp)
      newp = w;
  }
  if (newp != current->priority)
    task_change_priority(current, newp);

  // Direct handoff: transfer the mutex straight to the highest-priority waiter
  // so it resumes already owning it (its lock post-process never runs on the
  // syscall path — and on the thread path it sees rm->owner already == itself,
  // so it skips). Clear the waiter's wait_sem/wait_mutex here (it is no longer
  // blocked), keeping transitive-PI visibility correct until the actual grant.
  TCB *w = wait_q_dequeue(&rm->base);
  if (w) {
    w->wait_sem = NULL;
    w->wait_mutex = NULL;
    remove_from_blocked_list(w);
    w->status = TASK_READY;
    add_to_ready_list(w);
    rm->owner = w;
    rm->recursion_count = 1;
    rm->next_held = (rmutex_t *)w->held_mutexes;
    w->held_mutexes = rm;
#if VAIOS_SYSCALL_SVC
    v_syscall_wake_result(w, VA_PASS); // deliver to the blocked lock()
#endif
    EXIT_CRITICAL();
    return VA_PASS;
  }

  // No waiter: release the mutex (free the binary base slot).
  rm->owner = NULL;
  rm->recursion_count = 0;
  atomic_inc(&rm->base.count);
  EXIT_CRITICAL();
  return VA_PASS;
}

int v_mutex_lock_recursive(MutexHandle_t mtx, uint32_t ticks_to_wait) {
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  if (rm->owner == current) {
    rm->recursion_count++;
    EXIT_CRITICAL();
    return VA_PASS;
  }
  EXIT_CRITICAL();

  // otherwise, lock normally
  if (v_mutex_lock((MutexHandle_t)rm, ticks_to_wait) == VA_PASS) {
    return VA_PASS;
  }
  return VA_FAIL;
}

int v_mutex_unlock_recursive(MutexHandle_t mtx) {
  rmutex_t *rm = (rmutex_t *)mtx;

  ENTER_CRITICAL();
  if (rm->owner != get_current_task()) {
    EXIT_CRITICAL();
    return VA_FAIL; // not owner
  }

  rm->recursion_count--;
  if (rm->recursion_count == 0) {
    // Balance this function's ENTER_CRITICAL before delegating — v_mutex_unlock
    // runs its own critical section. Without this the nesting count never
    // returns to 0, BASEPRI is never cleared, and the kernel freezes.
    EXIT_CRITICAL();
    return v_mutex_unlock((MutexHandle_t)rm);
  }
  EXIT_CRITICAL();
  return VA_PASS;
}
