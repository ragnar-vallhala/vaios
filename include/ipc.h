#ifndef VAIOS_IPC_H
#define VAIOS_IPC_H

#include "atomic.h"
#include "task.h"
#include "vaios_config.h"
#include <stdint.h>
#define _wait_q TCB
#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Type Definitions
//-----------------------------------------------------------------------------
typedef struct {
  _wait_q *wait_q;
  _wait_q *tail; // For mutexes
  atomic_t count;
  atomic_t limit;
} sema_t;

typedef struct rmutex {
  sema_t base;
  TCB *owner;
  uint32_t recursion_count;
  // Intrusive link for the owner's held-mutex list (TCB.held_mutexes).
  // Used by priority inheritance to recompute the owner's priority on
  // unlock from the mutexes it still holds.
  struct rmutex *next_held;
} rmutex_t;
typedef void *SemaphoreHandle_t; // Opaque handle for semaphore/mutex
typedef void *MutexHandle_t;

typedef struct StaticSemaphore {
  uint8_t reserved[STATIC_SEMAPHORE_SIZE]; // Adjust depending on your kernel
                                           // internals
} StaticSemaphore_t;

//-----------------------------------------------------------------------------
// Creation Functions
//-----------------------------------------------------------------------------

// Binary Semaphore
SemaphoreHandle_t v_semaphore_create_binary(void);
SemaphoreHandle_t v_semaphore_create_binary_static(StaticSemaphore_t *pxBuffer);

// Counting Semaphore
SemaphoreHandle_t v_semaphore_create_counting(uint32_t max_count,
                                              uint32_t initial_count);
SemaphoreHandle_t
v_semaphore_create_counting_static(uint32_t max_count, uint32_t initial_count,
                                   StaticSemaphore_t *pxBuffer);

// Mutex
MutexHandle_t v_mutex_create(void);
MutexHandle_t v_mutex_create_static(StaticSemaphore_t *pxBuffer);

// Recursive Mutex
MutexHandle_t v_mutex_create_recursive(void);
MutexHandle_t v_mutex_create_recursive_static(StaticSemaphore_t *pxBuffer);

//-----------------------------------------------------------------------------
// Operations
//-----------------------------------------------------------------------------

// Semaphore operations
int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks_to_wait);
int v_semaphore_give(SemaphoreHandle_t sem);

#if VAIOS_IPC_FD
// fd-typed named semaphores (Phase 3, Stage 3). A named sem lives in the kernel
// and is referenced by a per-task fd. v_sem_open find-or-creates by name and
// returns an fd (or < 0); v_sem_take/give resolve fd -> object; close(fd) drops
// the reference (object destroyed at refcount 0). Binary semaphores for now.
#define V_IPC_CREATE 0x1 // create the named object if it does not exist
int v_sem_open(const char *name, int flags);
int v_sem_take(int fd, uint32_t ticks_to_wait);
int v_sem_give(int fd);
#endif

// ISR-safe give
int v_semaphore_give_from_isr(SemaphoreHandle_t sem,
                              int *pxHigherPriorityTaskWoken);

// Predicate behind the FromISR priority assert (pure; host-testable). Given the
// active exception number (ICSR.VECTACTIVE) and the active IRQ's NVIC priority
// byte, returns 1 when calling a *_from_isr API here is safe: thread mode / a
// system handler (vectactive < 16), or an external IRQ whose priority is
// numerically >= MAX_SYSCALL_INTERRUPT_PRIORITY (so the kernel's BASEPRI masks
// it). An IRQ more urgent than that can preempt a kernel critical section and
// silently corrupt the scheduler's lists — the assert turns that into a panic.
int vaios_isr_priority_is_safe(uint32_t vectactive, uint32_t irq_prio);

// Mutex operations
int v_mutex_lock(MutexHandle_t mtx, uint32_t ticks_to_wait);
int v_mutex_unlock(MutexHandle_t mtx);

// Recursive Mutex operations
int v_mutex_lock_recursive(MutexHandle_t mtx, uint32_t ticks_to_wait);
int v_mutex_unlock_recursive(MutexHandle_t mtx);

//-----------------------------------------------------------------------------
// Return Codes
//-----------------------------------------------------------------------------

#define VA_PASS 1
#define VA_FAIL 0

#ifdef __cplusplus
}
#endif

#endif // VAIOS_IPC_H
