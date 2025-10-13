#include "ipc.h"
#include "memory.h"
#include "task.h"
#include "atomic.h"

//-----------------------------------------------------------------------------
// Wait Queue Helpers (FIFO)
//-----------------------------------------------------------------------------
static void wait_q_enqueue(sema_t *sem, TCB *task)
{
    task->next = NULL;

    if (!sem->wait_q)
    {
        sem->wait_q = task;
        sem->tail = task;
    }
    else
    {
        sem->tail->next = task;
        sem->tail = task;
    }
}

static TCB *wait_q_dequeue(sema_t *sem)
{
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
static sema_t *sema_init(sema_t *sem, uint32_t initial_count, uint32_t limit)
{
    sem->wait_q = NULL;
    sem->tail = NULL;
    atomic_set(&sem->count, initial_count);
    atomic_set(&sem->limit, limit);
    return sem;
}

SemaphoreHandle_t v_semaphore_create_binary(void)
{
    sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
    if (!sem)
        return NULL;
    return (SemaphoreHandle_t)sema_init(sem, 0, 1);
}

SemaphoreHandle_t v_semaphore_create_binary_static(StaticSemaphore_t *pxBuffer)
{
    if (!pxBuffer)
        return NULL;
    return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, 0, 1);
}

SemaphoreHandle_t v_semaphore_create_counting(uint32_t max_count, uint32_t initial_count)
{
    if (initial_count > max_count)
        return NULL;
    sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
    if (!sem)
        return NULL;
    return (SemaphoreHandle_t)sema_init(sem, initial_count, max_count);
}

SemaphoreHandle_t v_semaphore_create_counting_static(uint32_t max_count, uint32_t initial_count, StaticSemaphore_t *pxBuffer)
{
    if (!pxBuffer || initial_count > max_count)
        return NULL;
    return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, initial_count, max_count);
}

MutexHandle_t v_mutex_create(void)
{
    sema_t *mtx = (sema_t *)v_malloc(sizeof(sema_t));
    if (!mtx)
        return NULL;
    return (MutexHandle_t)sema_init(mtx, 1, 1); // unlocked
}

MutexHandle_t v_mutex_create_static(StaticSemaphore_t *pxBuffer)
{
    if (!pxBuffer)
        return NULL;
    return (MutexHandle_t)sema_init((sema_t *)pxBuffer, 1, 1);
}

MutexHandle_t v_mutex_create_recursive(void)
{
    sema_t *mtx = (sema_t *)v_malloc(sizeof(sema_t));
    if (!mtx)
        return NULL;
    return (MutexHandle_t)sema_init(mtx, 1, 1);
}

MutexHandle_t v_mutex_create_recursive_static(StaticSemaphore_t *pxBuffer)
{
    if (!pxBuffer)
        return NULL;
    return (MutexHandle_t)sema_init((sema_t *)pxBuffer, 1, 1);
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Operations
//-----------------------------------------------------------------------------
static int semaphore_take_common(sema_t *s, uint32_t ticks_to_wait)
{
    TCB *current = get_current_task();
    ENTER_CRITICAL();

    if (atomic_get(&s->count) > 0)
    {
        atomic_dec(&s->count);
        EXIT_CRITICAL();
        return VA_PASS;
    }

    if (ticks_to_wait == 0)
    {
        EXIT_CRITICAL();
        return VA_FAIL; // would block
    }

    // Block task and enqueue
    current->status = TASK_BLOCKED;
    current->delay_ticks = ticks_to_wait;
    wait_q_enqueue(s, current);

    EXIT_CRITICAL();
    task_yield();

    // When unblocked, semaphore must be granted
    return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks_to_wait)
{
    if (!sem)
        return VA_FAIL;
    return semaphore_take_common((sema_t *)sem, ticks_to_wait);
}

int v_mutex_lock(MutexHandle_t mtx, uint32_t ticks_to_wait)
{
    if (!mtx)
        return VA_FAIL;
    return semaphore_take_common((sema_t *)mtx, ticks_to_wait);
}

static int semaphore_give_common(sema_t *s, int *pxHigherPriorityTaskWoken)
{
    ENTER_CRITICAL();

    if (atomic_get(&s->count) >= atomic_get(&s->limit))
    {
        EXIT_CRITICAL();
        return VA_FAIL; // full / already unlocked
    }

    atomic_inc(&s->count);

    // Unblock first waiting task if any
    TCB *to_unblock = wait_q_dequeue(s);
    if (to_unblock)
    {
        to_unblock->status = TASK_READY;
        add_to_ready_list(to_unblock);

        if (pxHigherPriorityTaskWoken && to_unblock->priority > get_current_task()->priority)
            *pxHigherPriorityTaskWoken = 1;
    }

    EXIT_CRITICAL();
    return VA_PASS;
}

int v_semaphore_give(SemaphoreHandle_t sem)
{
    if (!sem)
        return VA_FAIL;
    return semaphore_give_common((sema_t *)sem, NULL);
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem, int *pxHigherPriorityTaskWoken)
{
    if (!sem)
        return VA_FAIL;
    return semaphore_give_common((sema_t *)sem, pxHigherPriorityTaskWoken);
}

int v_mutex_unlock(MutexHandle_t mtx)
{
    if (!mtx)
        return VA_FAIL;
    return semaphore_give_common((sema_t *)mtx, NULL);
}

int v_mutex_lock_recursive(MutexHandle_t mtx, uint32_t ticks_to_wait)
{
    rmutex_t *rm = (rmutex_t *)mtx;
    TCB *current = get_current_task();

    ENTER_CRITICAL();
    if (rm->owner == current)
    {
        rm->recursion_count++;
        EXIT_CRITICAL();
        return 0;
    }
    EXIT_CRITICAL();

    // otherwise, lock normally
    if (v_mutex_lock((MutexHandle_t)&rm->base, ticks_to_wait) == 0)
    {
        ENTER_CRITICAL();
        rm->owner = current;
        rm->recursion_count = 1;
        EXIT_CRITICAL();
        return 0;
    }
    return -1;
}

int v_mutex_unlock_recursive(MutexHandle_t mtx)
{
    rmutex_t *rm = (rmutex_t *)mtx;

    ENTER_CRITICAL();
    if (rm->owner != get_current_task())
    {
        EXIT_CRITICAL();
        return -1; // not owner
    }

    rm->recursion_count--;
    if (rm->recursion_count == 0)
    {
        rm->owner = NULL;
        EXIT_CRITICAL();
        return v_mutex_unlock((MutexHandle_t)&rm->base);
    }
    EXIT_CRITICAL();
    return 0;
}
