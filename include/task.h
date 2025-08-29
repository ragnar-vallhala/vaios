#ifndef TASK_H
#define TASK_H

#include <stdint.h>

// Task status enumeration
typedef enum {
  TASK_READY = 0,
  TASK_RUNNING = 1,
  TASK_BLOCKED = 2,
  TASK_SUSPENDED = 3,
  TASK_TERMINATED = 4
} Task_Status;

// Task Control Block structure
typedef struct Task_Control_Block {
  uint32_t *sp;          // Stack pointer
  uint32_t *mem_block;   // Stack memory block
  void *arg;             // Task argument
  void (*entry)(void *); // Task entry point
  uint32_t stack_size;   // Stack size in bytes
  uint32_t task_id;      // Unique task identifier
  uint32_t delay_ticks;  // Delay remaining in ticks
  uint32_t priority;     // Task priority (0-7)
  Task_Status status;    // Current task status
  // Links for doubly-linked lists
  struct Task_Control_Block *next; // Next task in list
  struct Task_Control_Block *prev; // Previous task in list
} TCB;

// Scheduler configuration constants
#define MAX_PRIORITY 7  // Highest priority level
#define IDLE_PRIORITY 0 // Priority for idle task

#define INITIAL_XPSR 0x01000000UL    // Thumb bit set
#define TASK_ENTRY_MASK 0xFFFFFFFEUL // Set last bit 0
#define TASK_EXIT task_exit          // Handler if task returns
#define TCB_SP_OFF ((int)offsetof(TCB, sp))

// Task list management functions
void add_to_ready_list(TCB *task);
void remove_from_ready_list(TCB *task);
TCB *get_highest_priority_task(void);

// Task stack initialization
void init_task_stack(TCB *task);

// Task creation and management
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size,
                     uint32_t priority);

// Scheduler core functions
TCB *get_next_task(void);     // Called from context switch handler
void task_yield(void);        // Voluntary CPU yield
void task_block(void);        // Block current task
void task_unblock(TCB *task); // Unblock specified task
__attribute__((noreturn)) void task_exit(void);
// Idle task function
void idle_task_function(void *arg);

// Delayed task management
void add_to_delayed_list(TCB *task);
void remove_from_delayed_list(TCB *task);
void task_delay(uint32_t ticks);

// Scheduler initialization and control
void scheduler_init(void);             // Initialize scheduler
void scheduler_start(void);            // Start scheduler
void scheduler_add_task(TCB *task);    // Add task to ready queue
void scheduler_remove_task(TCB *task); // Remove task from scheduler

// Utility functions
TCB *get_current_task(void);             // Get current running task
uint32_t get_tick_count(void);           // Get system tick count
uint32_t get_context_switch_count(void); // Get context switch statistics

// Assembly functions (implemented in context_switch.s)
extern void start_first_task(void) __attribute__((noreturn));
extern void PendSV_Handler(void);

// Critical section macros
#define ENTER_CRITICAL() __asm volatile("cpsid i" ::: "memory")
#define EXIT_CRITICAL() __asm volatile("cpsie i" ::: "memory")

// SysTick register definitions
#define SYSTICK_BASE 0xE000E010UL
#define SYSTICK_CSR (*(volatile uint32_t *)(SYSTICK_BASE + 0x00))
#define SYSTICK_RVR (*(volatile uint32_t *)(SYSTICK_BASE + 0x04))
#define SYSTICK_CVR (*(volatile uint32_t *)(SYSTICK_BASE + 0x08))

// SysTick Control and Status Register bits
#define SYSTICK_CSR_ENABLE (1UL << 0)
#define SYSTICK_CSR_TICKINT (1UL << 1)
#define SYSTICK_CSR_CLKSOURCE (1UL << 2)
#define SYSTICK_CSR_COUNTFLAG (1UL << 16)

// NVIC System Priority Registers
#define NVIC_SYSPRI2 (*(volatile uint32_t *)0xE000ED1C)
#define NVIC_SYSPRI3 (*(volatile uint32_t *)0xE000ED20)

// System interrupt priority levels
#define PENDSV_PRIORITY 0xFF  // Lowest priority for PendSV
#define SYSTICK_PRIORITY 0x80 // Medium priority for SysTick

// Task creation helper macro
#define CREATE_TASK(name, entry_func, arg_ptr, stack_array, prio, id)          \
  task_create((entry_func), (arg_ptr), (stack_array), sizeof(stack_array),     \
              (prio), (id))

// Priority validation macro
#define IS_VALID_PRIORITY(p) ((p) <= MAX_PRIORITY)

// Task status check macros
#define IS_TASK_READY(task) ((task) && (task)->status == TASK_READY)
#define IS_TASK_RUNNING(task) ((task) && (task)->status == TASK_RUNNING)
#define IS_TASK_BLOCKED(task) ((task) && (task)->status == TASK_BLOCKED)
#define IS_TASK_SUSPENDED(task) ((task) && (task)->status == TASK_SUSPENDED)
#define IS_TASK_TERMINATED(task) ((task) && (task)->status == TASK_TERMINATED)

// Utility macros for common operations
#define TASK_DELAY_MS(ms) task_delay((ms))
#define TASK_DELAY_TICKS(ticks) task_delay((ticks))
#define GET_CURRENT_TASK_ID() (current_task ? current_task->task_id : 0)
#define GET_CURRENT_PRIORITY()                                                 \
  (current_task ? current_task->priority : IDLE_PRIORITY)

// Memory alignment for Cortex-M4 (8-byte aligned stacks)
#define STACK_ALIGN_SIZE 8
#define ALIGN_STACK_SIZE(size)                                                 \
  (((size) + STACK_ALIGN_SIZE - 1) & ~(STACK_ALIGN_SIZE - 1))

// Minimum stack size recommendation (in words, not bytes)
#define MIN_STACK_SIZE_WORDS 64   // 256 bytes minimum
#define IDLE_STACK_SIZE_WORDS 256 // 1KB for idle task

// Function attributes for optimization
#define SCHEDULER_INLINE inline __attribute__((always_inline))
#define SCHEDULER_NOINLINE __attribute__((noinline))

// Error codes for task operations
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
