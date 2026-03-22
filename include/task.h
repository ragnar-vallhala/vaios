#ifndef TASK_H
#define TASK_H

#include <stddef.h>
#include <stdint.h>

//-----------------------------------------------------------------------------
// Architecture Validation
// Require CORTEX_M to be defined by the build system/port.
//-----------------------------------------------------------------------------
#ifndef CORTEX_M4
#error                                                                         \
    "Define a valid architecture macro (e.g., CORTEX_M) before including task.h"
#endif

//-----------------------------------------------------------------------------
// Task Status Enumeration
//-----------------------------------------------------------------------------
typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_DELAYED = 3,
  TASK_TERMINATED = 4
} Task_Status;

//-----------------------------------------------------------------------------
// Task Control Block (TCB) Structure
//-----------------------------------------------------------------------------
typedef struct Task_Control_Block {
  uint32_t *sp;          // Current stack pointer (PSP)
  uint32_t *mem_block;   // Base of allocated stack memory
  void *arg;             // Task argument
  void (*entry)(void *); // Task entry function
  uint32_t stack_size;   // Stack size in bytes
  uint32_t task_id;      // Unique task identifier
  uint32_t delay_ticks;  // Absolute wakeup tick (deadline)
  uint32_t ticks_run;    // Total ticks executed (stats)
  uint32_t priority;     // Priority (0..MAX_PRIORITY)
  Task_Status status;    // Current status
  void *wait_sem;        // Semaphore this task is blocking on (NULL if none)
  struct Task_Control_Block *next; // Next in list (ready/blocked/etc.)
  struct Task_Control_Block *prev; // Prev in list
  uint32_t magic;                  // Sanity check (must be TCB_MAGIC)
} TCB;

#define TCB_MAGIC 0x54434221 // "TCB!"

//-----------------------------------------------------------------------------
// Scheduler Configuration Constants
//-----------------------------------------------------------------------------
#define MAX_PRIORITY MAX_TASK_PRIORITY      // Highest usable priority
#define IDLE_PRIORITY IDLE_TASK_PRIORITY    // Priority for idle task
#define TASK_EXIT task_exit                 // Called if a task returns
#define TCB_SP_OFF ((int)offsetof(TCB, sp)) // Used by assembly

//-----------------------------------------------------------------------------
// Task List Management
//-----------------------------------------------------------------------------
void enqueue_task(TCB **list, TCB *task);
void remove_task(TCB **list, TCB *task);
TCB *dequeue_task(TCB **list);
TCB *peek_task(TCB **list);

void add_to_ready_list(TCB *task);
void remove_from_ready_list(TCB *task);
TCB *get_highest_priority_task(void);

//-----------------------------------------------------------------------------
// Task Stack Initialization (port-specific)
//-----------------------------------------------------------------------------
void init_task_stack(TCB *task); // Implemented in port.c

//-----------------------------------------------------------------------------
// Task Creation and Management
//-----------------------------------------------------------------------------
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size,
                     uint32_t priority);
void task_exit_request(uint32_t task_id);
//-----------------------------------------------------------------------------
// Scheduler Core Functions
//-----------------------------------------------------------------------------
TCB *get_next_task(void);           // Called by context switch handler
void set_next_task(void);           // Select next task to run
void load_next_task_from_isr(void); // ISR-safe trigger to switch
void task_yield(void);              // Voluntary yield (port.c)
__attribute__((noreturn)) void task_exit(void);

//-----------------------------------------------------------------------------
// Idle Task
//-----------------------------------------------------------------------------
void idle_task_function(void *arg);

//-----------------------------------------------------------------------------
// Delayed Task Management
//-----------------------------------------------------------------------------
void add_to_delayed_list(TCB *task);
void remove_from_delayed_list(TCB *task);
void task_delay(uint32_t ticks);
void wake_up_delayed_tasks(void); // Decrement and wake as needed

//-----------------------------------------------------------------------------
// Blocked Task Management
//-----------------------------------------------------------------------------
void task_block(void); // Block current task
void add_to_blocked_list(TCB *task);
void remove_from_blocked_list(TCB *task);
void task_unblock(uint32_t task_id);

//-----------------------------------------------------------------------------
// Scheduler Initialization and Control
//-----------------------------------------------------------------------------
void scheduler_init(void);
void scheduler_start(void);

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
TCB *get_current_task(void);
uint32_t get_context_switch_count(void);
uint32_t get_idle_tick_count(void);

extern TCB *current_task;

//-----------------------------------------------------------------------------
// Macros
//-----------------------------------------------------------------------------
#define IS_VALID_PRIORITY(p) ((p) <= MAX_PRIORITY)
#define IS_TASK_READY(t) ((t) && (t)->status == TASK_READY)
#define IS_TASK_RUNNING(t) ((t) && (t)->status == TASK_RUNNING)
#define IS_TASK_BLOCKED(t) ((t) && (t)->status == TASK_BLOCKED)
#define IS_TASK_DELAYED(t) ((t) && (t)->status == TASK_DELAYED)
#define IS_TASK_TERMINATED(t) ((t) && (t)->status == TASK_TERMINATED)

#define GET_CURRENT_TASK_ID() (current_task ? current_task->task_id : -1)
#define GET_CURRENT_PRIORITY() (current_task ? current_task->priority : -1)

// Stack alignment (STACK_ALIGN_SIZE provided by config.h)
#define ALIGN_STACK_SIZE(sz)                                                   \
  (((sz) + (STACK_ALIGN_SIZE - 1u)) & ~(STACK_ALIGN_SIZE - 1u))

// Delay helpers (map to ticks; define MS_TO_TICKS in config.h if desired)
#ifndef MS_TO_TICKS
#define MS_TO_TICKS(ms)                                                        \
  ((ms * 1000 )/ SYSTICK_PERIOD) // systick in us
#endif

#define TASK_DELAY_MS(ms) task_delay(MS_TO_TICKS(ms))
#define TASK_DELAY_TICKS(ticks) task_delay((ticks))

//-----------------------------------------------------------------------------
// Error Codes for Task Operations
//-----------------------------------------------------------------------------
typedef enum {
  TASK_SUCCESS = 0,
  TASK_ERROR_NULL_POINTER,
  TASK_ERROR_INVALID_PRIORITY,
  TASK_ERROR_INSUFFICIENT_MEMORY,
  TASK_ERROR_MAX_TASKS_EXCEEDED,
  TASK_ERROR_TASK_NOT_FOUND,
  TASK_ERROR_INVALID_STATE
} task_error_t;

#endif // TASK_H
