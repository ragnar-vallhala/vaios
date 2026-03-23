#include "ipc.h"
#include "atomic.h"
#include "memory.h"
#include "task.h"
#include "utils.h"

//-----------------------------------------------------------------------------
// Wait Queue Helpers (FIFO)
//-----------------------------------------------------------------------------
static void wait_q_enqueue(sema_t *sem, TCB *task) {
  task->next = NULL;

  if (!sem->wait_q) {
    sem->wait_q = task;
    sem->tail = task;
  } else {
    sem->tail->next = task;
    sem->tail = task;
  }
}

static TCB *wait_q_dequeue(sema_t *sem) {
  TCB *task = sem->wait_q;
  if (!task)
    return NULL;

  sem->wait_q = task->next;
  if (!sem->wait_q)
    sem->tail = NULL;

  task->next = NULL;
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
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive(void) {
  rmutex_t *mtx = (rmutex_t *)v_malloc(sizeof(rmutex_t));
  if (!mtx)
    return NULL;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  return (MutexHandle_t)mtx;
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Operations
//-----------------------------------------------------------------------------
static int semaphore_take_common(sema_t *s, uint32_t ticks_to_wait) {
  TCB *current = get_current_task();
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

  EXIT_CRITICAL();
  task_yield();

  // On resume: semaphore_give clears wait_sem → VA_PASS (slot granted)
  //            wake_up_delayed_tasks() ejects us with wait_sem still set →
  //            VA_FAIL (timeout)
  if (current->wait_sem != NULL) {
    current->wait_sem = NULL;
    return VA_FAIL;
  }
  return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks_to_wait) {
  if (!sem)
    return VA_FAIL;
  return semaphore_take_common((sema_t *)sem, ticks_to_wait);
}

int v_mutex_lock(MutexHandle_t mtx, uint32_t ticks_to_wait) {
  if (!mtx)
    return VA_FAIL;
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  // Priority Inheritance: Boost owner if current task has higher priority
  if (rm->owner && rm->owner->priority < current->priority) {
    task_change_priority(rm->owner, current->priority);
  }
  EXIT_CRITICAL();

  int res = semaphore_take_common(&rm->base, ticks_to_wait);

  if (res == VA_PASS) {
    ENTER_CRITICAL();
    rm->owner = current;
    rm->recursion_count = 1;
    EXIT_CRITICAL();
  }
  return res;
}

static int semaphore_give_common(sema_t *s, int *pxHigherPriorityTaskWoken) {
  ENTER_CRITICAL();

  // Unblock first waiting task if any — hand off the slot directly
  TCB *to_unblock = wait_q_dequeue(s);
  if (to_unblock) {
    // Clear wait_sem so take() knows it was given (not a timeout).
    // Also remove from blocked_list since we are no longer waiting.
    to_unblock->wait_sem = NULL;
    remove_from_blocked_list(to_unblock);
    to_unblock->status = TASK_READY;
    add_to_ready_list(to_unblock);

    if (pxHigherPriorityTaskWoken &&
        to_unblock->priority > get_current_task()->priority)
      *pxHigherPriorityTaskWoken = 1;

    EXIT_CRITICAL();
    return VA_PASS;
  }

  if (atomic_get(&s->count) >= atomic_get(&s->limit)) {
    EXIT_CRITICAL();
    return VA_FAIL; // full
  }

  atomic_inc(&s->count);
  EXIT_CRITICAL();
  return VA_PASS;
}

int v_semaphore_give(SemaphoreHandle_t sem) {
  if (!sem)
    return VA_FAIL;
  return semaphore_give_common((sema_t *)sem, NULL);
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem,
                              int *pxHigherPriorityTaskWoken) {
  if (!sem)
    return VA_FAIL;
  return semaphore_give_common((sema_t *)sem, pxHigherPriorityTaskWoken);
}

int v_mutex_unlock(MutexHandle_t mtx) {
  if (!mtx)
    return VA_FAIL;
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  if (rm->owner != current) {
    EXIT_CRITICAL();
    return VA_FAIL; // Not the owner
  }

  // Priority Inheritance: Restore priority if boosted
  if (current->priority > current->base_priority) {
    task_change_priority(current, current->base_priority);
  }

  rm->owner = NULL;
  rm->recursion_count = 0;
  EXIT_CRITICAL();

  return v_semaphore_give((SemaphoreHandle_t)&rm->base);
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
    return v_mutex_unlock((MutexHandle_t)rm);
  }
  EXIT_CRITICAL();
  return VA_PASS;
}
