#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "config.h"
#include "port.h"

//-----------------------------------------------------------------------------
// Architecture Validation
//-----------------------------------------------------------------------------
#ifndef CORTEX_M
#else
#error "Define valid architecture"
#endif

//-----------------------------------------------------------------------------
// Task Status Enumeration
//-----------------------------------------------------------------------------
typedef enum
{
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_DELAYED = 3,
  TASK_TERMINATED = 4
} Task_Status;

//-----------------------------------------------------------------------------
// Task Control Block (TCB) Structure
//-----------------------------------------------------------------------------
typedef struct Task_Control_Block
{
  uint32_t *sp;          // Stack pointer
  uint32_t *mem_block;   // Stack memory block
  void *arg;             // Task argument
  void (*entry)(void *); // Task entry point
  uint32_t stack_size;   // Stack size in bytes
  uint32_t task_id;      // Unique task identifier
  uint32_t delay_ticks;  // Delay remaining in ticks
  uint32_t ticks_run;    // Total ticks run (for stats)
  uint32_t priority;     // Task priority (0-7)
  Task_Status status;    // Current task status
  struct Task_Control_Block *next; // Next task in list
  struct Task_Control_Block *prev; // Previous task in list
} TCB;

//-----------------------------------------------------------------------------
// Scheduler Configuration Constants
//-----------------------------------------------------------------------------
#define MAX_PRIORITY      MAX_TASK_PRIORITY   // Highest priority level
#define IDLE_PRIORITY    IDLE_TASK_PRIORITY  // Priority for idle task
#define TASK_EXIT        task_exit           // Handler if task returns
#define TCB_SP_OFF       ((int)offsetof(TCB, sp))

//-----------------------------------------------------------------------------
// Task List Management Functions
//-----------------------------------------------------------------------------
void enqueue_task(TCB **list, TCB *task);
void remove_task(TCB **list, TCB *task);
TCB *dequeue_task(TCB **list);
TCB *peek_task(TCB **list);

void add_to_ready_list(TCB *task);
void remove_from_ready_list(TCB *task);
TCB *get_highest_priority_task(void);

//-----------------------------------------------------------------------------
// Task Stack Initialization
//-----------------------------------------------------------------------------
void init_task_stack(TCB *task); // Defined in port.c

//-----------------------------------------------------------------------------
// Task Creation and Management
//-----------------------------------------------------------------------------
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size, uint32_t priority);

//-----------------------------------------------------------------------------
// Scheduler Core Functions
//-----------------------------------------------------------------------------
TCB *get_next_task(void);           // Called from context switch handler
void set_next_task(void);           // Select next task to run
void load_next_task_from_isr(void); // From ISR context
void task_yield(void);              // Voluntary CPU yield (defined in port.c)
__attribute__((noreturn)) void task_exit(void);

//-----------------------------------------------------------------------------
// Idle Task Function
//-----------------------------------------------------------------------------
void idle_task_function(void *arg);

//-----------------------------------------------------------------------------
// Delayed Task Management
//-----------------------------------------------------------------------------
void add_to_delayed_list(TCB *task);
void remove_from_delayed_list(TCB *task);
void task_delay(uint32_t ticks);
void wake_up_delayed_tasks(void); // Check and wake delayed tasks

//-----------------------------------------------------------------------------
// Blocked Task Management
//-----------------------------------------------------------------------------
void task_block(void); // Block current task
void add_to_blocked_list(TCB *task);
void remove_from_blocked_list(TCB *task);
void task_unblock(TCB *task); // Unblock specified task

//-----------------------------------------------------------------------------
// Scheduler Initialization and Control
//-----------------------------------------------------------------------------
void scheduler_init(void);  // Initialize scheduler
void scheduler_start(void); // Start scheduler

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
TCB *get_current_task(void);             // Get current running task
uint32_t get_context_switch_count(void); // Get context switch statistics
uint32_t get_idle_tick_count(void);

//-----------------------------------------------------------------------------
// Macros
//-----------------------------------------------------------------------------
#define IS_VALID_PRIORITY(p)        ((p) <= MAX_PRIORITY)
#define IS_TASK_READY(task)         ((task) && (task)->status == TASK_READY)
#define IS_TASK_RUNNING(task)       ((task) && (task)->status == TASK_RUNNING)
#define IS_TASK_BLOCKED(task)       ((task) && (task)->status == TASK_BLOCKED)
#define IS_TASK_SUSPENDED(task)     ((task) && (task)->status == TASK_SUSPENDED)
#define IS_TASK_TERMINATED(task)    ((task) && (task)->status == TASK_TERMINATED)

#define TASK_DELAY_MS(ms)           task_delay((ms))
#define TASK_DELAY_TICKS(ticks)     task_delay((ticks) * SYSTICK_PERIOD)

extern TCB *current_task;

#define GET_CURRENT_TASK_ID()       (current_task ? current_task->task_id : 0)
#define GET_CURRENT_PRIORITY()      (current_task ? current_task->priority : IDLE_PRIORITY)

// Memory alignment for Cortex-M4 (8-byte aligned stacks)
#define ALIGN_STACK_SIZE(size)      (((size) + STACK_ALIGN_SIZE - 1) & ~(STACK_ALIGN_SIZE - 1))

//-----------------------------------------------------------------------------
// Error Codes for Task Operations
//-----------------------------------------------------------------------------
typedef enum
{
  TASK_SUCCESS = 0,
  TASK_ERROR_NULL_POINTER,
  TASK_ERROR_INVALID_PRIORITY,
  TASK_ERROR_INSUFFICIENT_MEMORY,
  TASK_ERROR_MAX_TASKS_EXCEEDED,
  TASK_ERROR_TASK_NOT_FOUND,
  TASK_ERROR_INVALID_STATE
} task_error_t;

#endif // TASK_H