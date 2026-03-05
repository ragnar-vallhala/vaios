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

typedef struct {
  sema_t base;
  TCB *owner;
  uint32_t recursion_count;
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

// ISR-safe give
int v_semaphore_give_from_isr(SemaphoreHandle_t sem,
                              int *pxHigherPriorityTaskWoken);

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
