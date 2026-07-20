#ifndef TASK_H
#define TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vaios_config.h"
#include "perf.h"
#include "vfile.h" // v_fd_entry (per-task fd table)

//-----------------------------------------------------------------------------
// Architecture validation
// The Kconfig arch `choice` always selects exactly one port, so this is a
// sanity check that the generated config actually reached this TU (i.e. the
// build force-includes vaios_autoconf.h) rather than a hand-maintained arch
// gate. The kernel below reads capability symbols (VAIOS_ARCH_HAS_MPU, ...),
// never an arch name.
//-----------------------------------------------------------------------------
#if !defined(VAIOS_ARCH_CORTEX_M4) && !defined(VAIOS_ARCH_HOST) &&             \
    !defined(VAIOS_ARCH_AVR)
#error                                                                         \
    "No vaios arch selected -- is vaios_autoconf.h on the include path? Run the Kconfig step."
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
#if VAIOS_IPC_FD
// One multi-wait registration: a v_wait()ing task links a node into each
// watched semaphore's observer list. When that sem becomes ready (count goes
// positive) the give path walks the list and wakes each node's owner. `sem` is
// the sema_t the node is linked into (opaque here); `idx` is unused by the
// kernel but kept for symmetry with the fds[] the caller passed.
struct Task_Control_Block;
typedef struct v_wnode {
  struct v_wnode *next; // next observer in the sem's list
  void *sem;            // sema_t this node is linked into
  struct Task_Control_Block *owner;
  int32_t idx;
} v_wnode;
#endif

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
#if VAIOS_MPU_USER_SEPARATION
  // Pre-encoded per-task RW-unprivileged region over the WHOLE block, applied on
  // context switch alongside the guard. Grants the running (unprivileged) task
  // access to only its own stack+heap; the higher-numbered guard still wins at
  // the base. Valid only when mpu_block_valid != 0.
  uint32_t mpu_block[2];
  uint8_t mpu_block_valid;
  // Privilege of this task's thread-mode execution: 0 = unprivileged (the
  // default for user tasks), 1 = privileged. Applied to CONTROL.nPRIV on
  // switch-in. The idle task is privileged (it does kernel housekeeping).
  uint8_t privileged;
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
#if VAIOS_TASK_HEAP
  // Per-task heap (Stage 4). The heap lives at the LOW end of this task's own
  // mem_block (just above the stack guard) and grows UP; the stack grows down
  // from the top of the same block. heap_base is fixed (block base + guard);
  // heap_brk is the current top of the heap, bounded at malloc time by the live
  // stack pointer. malloc/free/calloc/realloc (memory.c) operate here.
  uint8_t *heap_base;
  uint8_t *heap_brk;
  uint8_t *heap_peak_brk; // highest heap_brk ever reached (peak footprint)
#endif
#if VAIOS_IPC_FD
  // Multi-fd wait (v_wait). While blocked in v_wait, in_multiwait == 1 and
  // wnodes[0..narm) are linked into the watched sems' observer lists; the give
  // path or the timeout scan clears in_multiwait and wakes the task, which then
  // unlinks the nodes. Bounded by the fd-table size (can't watch more fds than
  // it holds).
  v_wnode wnodes[VAIOS_MAX_FDS];
  uint8_t in_multiwait;
  uint8_t narm; // number of currently armed wnodes
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
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t size,
                     uint32_t priority);
// As task_create, but also tags the task with a human-readable name for
// diagnostics (perf dumps, telemetry). `name` must point at storage that
// outlives the task — typically a string literal in flash; the TCB keeps the
// pointer, not a copy. Pass NULL or "" for an unnamed task. task_create() is a
// thin wrapper over this with name = "".
uint32_t task_create_named(void (*entry)(void *), void *arg, uint32_t size,
                           uint32_t priority, const char *name);
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
// Privileged terminate-self body (runs from the SYS_exit dispatch, or directly
// when the flip is off). Returns; task_exit is the noreturn entry point.
void v_task_exit_impl(void);
// Syscall-boundary pointer validation (5c): is [p, p+len) within the current
// task's own block? v_strnlen_user bounds a user C string to the block. Always
// available (pure); the syscall dispatch only calls them under the flip.
int v_access_ok(const void *p, uint32_t len, int write);
long v_strnlen_user(const char *s, uint32_t max);

//-----------------------------------------------------------------------------
// Idle Task
//-----------------------------------------------------------------------------
void idle_task_function(void *arg);

//-----------------------------------------------------------------------------
// Blocking-wait timeout sentinel
//-----------------------------------------------------------------------------
// Pass as the ticks_to_wait of a blocking wait (v_semaphore_take, v_mutex_lock,
// v_sem_take, ...) to block until signalled with no timeout. Ordinary timeouts
// are a `now + ticks` deadline, so a literal 0xFFFFFFFF would overflow into the
// past and expire immediately; the kernel special-cases this value to park the
// waiter with no deadline instead.
#define V_WAIT_FOREVER 0xFFFFFFFFu

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
