#ifndef TASK_H
#define TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vaios_config.h"
#include "perf.h"
#include "vfile.h" // v_fd_entry (per-task fd table)

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
  uint32_t *sp;           // Current stack pointer (PSP)
  uint32_t *mem_block;    // Base of allocated stack memory
  void *arg;              // Task argument
  void (*entry)(void *);  // Task entry function
  uint32_t stack_size;    // Stack size in bytes
  uint32_t task_id;       // Unique task identifier
  const char *name;       // Optional human-readable name (flash literal, not
                          // owned/copied; "" when unset). See task_get_name.
  uint32_t delay_ticks;   // Absolute wakeup tick (deadline)
  uint32_t ticks_run;     // Total ticks executed (stats)
  uint32_t priority;      // Current priority (0..MAX_PRIORITY)
  uint32_t base_priority; // Base priority for inheritance
  Task_Status status;     // Current status
  void *wait_sem;         // Semaphore this task is blocking on (NULL if none)
  void *wait_mutex;       // rmutex_t this task is blocking to acquire (NULL if
                          // none) — drives the transitive PI chain walk
  void *held_mutexes;     // rmutex_t head of the intrusive list of mutexes
                          // this task currently owns
  struct Task_Control_Block *next; // Next in list (ready/blocked/delayed)
  struct Task_Control_Block *prev; // Prev in list
  // Separate link for the semaphore/mutex wait queue. A task blocked on a
  // timed take is on both a sema wait queue AND blocked_list at once, so the
  // wait queue must not share next/prev with the scheduler lists.
  struct Task_Control_Block *wait_next;
#if VAIOS_MODULE_PERF
  v_perf_task_t perf; // Per-task perf counters (see perf.h)
#endif
#if VAIOS_MPU_STACK_GUARD
  // Pre-encoded MPU stack-guard region (RBAR/RASR word pair), applied on
  // context switch. Opaque here (mirrors NavHAL's hal_mpu_encoded_t) so task.h
  // stays architecture-independent. Valid only when mpu_guard_valid != 0.
  uint32_t mpu_guard[2];
  uint8_t mpu_guard_valid;
#endif
#if VAIOS_SYSCALL_SVC
  // Deferred blocking-syscall result: set by the waker (v_syscall_wake_result)
  // and written into this task's stacked r0 by PendSV (v_syscall_deliver_result)
  // when it is next scheduled in, so a blocked sem_take/mutex_lock returns it.
  int32_t syscall_result;
  uint8_t has_syscall_result;
#endif
#if VAIOS_DEVFS
  v_fd_entry fds[VAIOS_MAX_FDS]; // per-task file-descriptor table (Stage 2)
#endif
  uint32_t magic; // Sanity check (must be TCB_MAGIC)
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
// As task_create, but also tags the task with a human-readable name for
// diagnostics (perf dumps, telemetry). `name` must point at storage that
// outlives the task — typically a string literal in flash; the TCB keeps the
// pointer, not a copy. Pass NULL or "" for an unnamed task. task_create() is a
// thin wrapper over this with name = "".
uint32_t task_create_named(void (*entry)(void *), void *arg,
                           uint32_t stack_size, uint32_t priority,
                           const char *name);
// Tag an existing task by id (e.g. after task_create). `name` storage must
// outlive the task (string literal in flash). No-op if the id is unknown.
void task_set_name(uint32_t task_id, const char *name);
// Human-readable task name, or "" if unset. Never returns NULL.
const char *task_get_name(const TCB *task);
// Look up a task's name by id, or "" if the id is unknown/unset. Never returns
// NULL. Lets callers (e.g. a telemetry request handler) resolve names on demand
// without holding a TCB pointer.
const char *task_get_name_by_id(uint32_t task_id);
void task_exit_request(uint32_t task_id);
//-----------------------------------------------------------------------------
// Scheduler Core Functions
//-----------------------------------------------------------------------------
TCB *get_next_task(void);           // Called by context switch handler
void set_next_task(void);           // Select next task to run
void load_next_task_from_isr(void); // ISR-safe trigger to switch
void task_yield(void);              // Voluntary yield (port.c)
void task_change_priority(TCB *task, uint32_t new_priority);
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
// Drift-free periodic delay (cf. FreeRTOS vTaskDelayUntil). Blocks until the
// ABSOLUTE tick *last_wake + period, then advances *last_wake by period — so a
// loop runs at a fixed rate independent of its own execution time. Seed
// *last_wake with v_get_ticks() once before the loop. If the deadline has
// already passed (the iteration overran a period), it returns immediately
// without blocking so the loop catches up. Returns true if it actually
// blocked, false on overrun (or invalid args / idle task).
bool task_delay_until(uint32_t *last_wake, uint32_t period);
// SysTick wake path: drains due sleepers from the sorted delayed list and
// ejects timed-out semaphore waiters. The only place delayed/timeout wakeups
// happen — get_next_task does not scan. Returns nonzero if a woken task
// outranks the current task, so SysTick can pend an immediate switch.
int wake_up_delayed_tasks_isr(void);

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
// Snapshot live task pointers (running + ready + blocked + delayed) into
// out[] under a brief critical section; returns count (<= max). Used by the
// perf module's per-task dump.
int task_snapshot_list(TCB **out, int max);

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
