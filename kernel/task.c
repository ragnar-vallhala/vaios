#include "task.h"
#include "ipc.h"
#include "memory.h"
#include "perf_hooks.h"
#include "port.h" // ENTER_CRITICAL / EXIT_CRITICAL
#include "syscall.h" // SVC trap wrappers (VAIOS_SYSCALL_SVC)
#include "utils.h"
#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>
//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------
TCB *ready_lists[MAX_PRIORITY + 1];        // Ready list heads by priority
static TCB *ready_tails[MAX_PRIORITY + 1]; // Ready list tails — O(1) append
TCB *blocked_list = NULL;           // Blocked tasks list
/* Count of TASK_TERMINATED tasks parked on blocked_list awaiting the idle-task
 * garbage collector. Bumped at each termination, decremented on free, so the
 * idle GC can skip the blocked_list walk entirely when nothing is pending. */
static volatile uint32_t _terminated_count = 0;
TCB *delayed_list = NULL;           // Delayed tasks list
TCB *current_task = NULL;           // Currently running task
TCB *idle_task = NULL;              // Idle task pointer
uint32_t ready_bitmap = 0;          // Bitmap for O(1) priority search
uint32_t context_switch_count = 0;  // Context switch counter
uint32_t task_count = 0;            // Total created tasks
uint8_t scheduler_running = 0;
static uint32_t last_context_switch_tick = 0; // To track ticks for current task

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------
static inline void rb_set(uint32_t prio) { ready_bitmap |= (1u << prio); }
static inline void rb_clear(uint32_t prio) { ready_bitmap &= ~(1u << prio); }
static TCB *get_task_by_id(uint32_t task_id);

// O(1) ready-list primitives. Each priority's ready list is a doubly-linked
// queue with head (ready_lists[]) and tail (ready_tails[]) pointers;
// ready_bitmap bit p is set iff that list is non-empty. The caller must
// already hold a critical section, or run in the basepri-masked PendSV path.
static inline void rl_append(TCB *task) {
  uint32_t p = task->priority;
  task->next = NULL;
  task->prev = ready_tails[p];
  if (ready_lists[p])
    ready_tails[p]->next = task;
  else {
    ready_lists[p] = task;
    rb_set(p);
  }
  ready_tails[p] = task;
  task->status = TASK_READY;
}

// Pop the head of priority p's ready list. Caller ensures the list is
// non-empty (ready_bitmap bit p set).
static inline TCB *rl_pop_head(uint32_t p) {
  TCB *t = ready_lists[p];
  ready_lists[p] = t->next;
  if (ready_lists[p])
    ready_lists[p]->prev = NULL;
  else {
    ready_tails[p] = NULL;
    rb_clear(p);
  }
  t->next = NULL;
  t->prev = NULL;
  return t;
}

// Splice an arbitrary task out of its priority's ready list (caller ensures
// the task is actually on that list).
static inline void rl_remove(TCB *task) {
  uint32_t p = task->priority;
  if (task->prev)
    task->prev->next = task->next;
  else
    ready_lists[p] = task->next;
  if (task->next)
    task->next->prev = task->prev;
  else
    ready_tails[p] = task->prev;
  if (!ready_lists[p])
    rb_clear(p);
  task->next = NULL;
  task->prev = NULL;
}
// Highest set bit of the ready bitmap = highest ready priority. The Cortex-M4
// `clz` instruction (via __builtin_clz) makes this one instruction instead of
// an up-to-MAX_PRIORITY-iteration scan.
static inline int highest_ready_prio(void) {
  if (!ready_bitmap)
    return -1;
  return 31 - __builtin_clz(ready_bitmap);
}

//-----------------------------------------------------------------------------
// Task List Management Functions
//-----------------------------------------------------------------------------
void enqueue_task(TCB **list, TCB *task) {
  if (!task)
    return;

  ENTER_CRITICAL();
  task->next = NULL;
  task->prev = NULL;

  if (*list == NULL) {
    *list = task;
  } else {
    TCB *curr = *list;
    while (curr->next) {
      curr = curr->next;
    }
    curr->next = task;
    task->prev = curr;
  }
  EXIT_CRITICAL();
}

void remove_task(TCB **list, TCB *task) {
  if (!task || !*list)
    return;

  ENTER_CRITICAL();
  if (*list == task) {
    *list = task->next;
    if (*list)
      (*list)->prev = NULL;
    if (task->next)
      task->next->prev = NULL;
  } else {
    if (task->prev)
      task->prev->next = task->next;
    if (task->next)
      task->next->prev = task->prev;
  }

  task->next = NULL;
  task->prev = NULL;
  EXIT_CRITICAL();
}

TCB *dequeue_task(TCB **list) {
  TCB *task = peek_task(list);
  if (task)
    remove_task(list, task);
  return task;
}

TCB *peek_task(TCB **list) { return (list && *list) ? *list : NULL; }

//-----------------------------------------------------------------------------
// Ready List Management
//-----------------------------------------------------------------------------
void add_to_ready_list(TCB *task) {
  if (!task)
    return;
  ENTER_CRITICAL();
  rl_append(task);
  EXIT_CRITICAL();
}

void remove_from_ready_list(TCB *task) {
  if (!task)
    return;
  ENTER_CRITICAL();
  rl_remove(task);
  EXIT_CRITICAL();
}

TCB *get_highest_priority_task(void) {
  int p = highest_ready_prio();
  return (p < 0) ? NULL : ready_lists[p];
}

//-----------------------------------------------------------------------------
// Task Creation and Management
//-----------------------------------------------------------------------------
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t size,
                     uint32_t priority) {
  return task_create_named(entry, arg, size, priority, "");
}

uint32_t task_create_named(void (*entry)(void *), void *arg,
                           uint32_t size, uint32_t priority,
                           const char *name) {
  // Universal floor: a stack too small to hold the port's initial context frame
  // can't run. Rejected on every build, MPU or not.
  if (size < VAIOS_ARCH_MIN_STACK) {
    V_KLOG(LOG_ERROR,
           "[TASK] size %u below the %u B minimum stack; task not created",
           (unsigned)size, (unsigned)VAIOS_ARCH_MIN_STACK);
    return 0;
  }
#if VAIOS_MPU_STACK_GUARD || VAIOS_MPU_USER_SEPARATION
  // With per-task MPU regions on, each stack maps onto a single power-of-two MPU
  // region, so the size must be an exact power of two. Reject anything else
  // rather than silently rounding, which would desync the caller's view of the
  // stack from the region geometry. This constraint follows the *feature*, not
  // the chip: with MPU stack protection off, any size >= the minimum is fine.
  if ((size & (size - 1)) != 0) {
    V_KLOG(LOG_ERROR,
           "[TASK] size %u is not a power of two; task not created",
           (unsigned)size);
    return 0;
  }
#endif

  TCB *task = (TCB *)v_malloc(sizeof(TCB));
  if (!task)
    return 0;
  task->task_id = ++task_count;
  task->name = name ? name : "";
  task->stack_size = size;
#if VAIOS_MPU_USER_SEPARATION
  // The per-task RW-unprivileged region covers the WHOLE block as one MPU region,
  // which needs a size-aligned, power-of-two base. Align the block to its own
  // size (size is already required power-of-two). The guard at the base stays
  // size-aligned trivially. Encoded in init_task_stack.
  task->mem_block = (uint32_t *)v_memalign(size, size);
#elif VAIOS_MPU_STACK_GUARD
  // The MPU stack-guard region sits at the stack base, so the base must land on
  // the guard's size boundary. v_memalign gives that; the no-access guard region
  // is encoded in init_task_stack.
  task->mem_block = (uint32_t *)v_memalign(VAIOS_MPU_GUARD_SIZE, size);
#else
  task->mem_block = (uint32_t *)v_malloc(size);
#endif
  if (!task->mem_block) {
    v_panic(__FILE__, __LINE__, "failed to allocate stack for task %u",
            task->task_id);
  }
  task->ticks_run = 0;
  task->entry = entry;
  task->arg = arg;
  task->sp = task->mem_block + (size / sizeof(uint32_t));
  task->priority = priority;
  task->base_priority = priority;
#if VAIOS_MPU_USER_SEPARATION
  // User tasks run unprivileged by default (the flip). scheduler_init raises the
  // idle task back to privileged; a privileged system task would set this after
  // create.
  task->privileged = 0;
#endif
  task->delay_ticks = 0;
  task->next = NULL;
  task->prev = NULL;
  task->wait_next = NULL;
  task->wait_sem = NULL;
  task->wait_mutex = NULL;
  task->held_mutexes = NULL;
  task->status = TASK_READY;
#if VAIOS_MODULE_PERF
  task->perf.cycles_run = 0;
  task->perf.last_scheduled_cyc = 0;
  task->perf.max_burst_cyc = 0;
  task->perf.switches_in = 0;
  task->perf.stack_size = 0; /* computed on demand by v_perf_task_stats */
  task->perf.stack_peak = 0;
  /* Paint the whole stack so the high-water mark is measurable from the
   * untouched sentinel run. Done before init_task_stack lays the initial
   * frame at the top (that region then reads as "used"). */
  for (uint32_t i = 0; i < size / sizeof(uint32_t); i++)
    task->mem_block[i] = V_PERF_STACK_FILL;
#endif
  task->magic = TCB_MAGIC;
#if VAIOS_DEVFS
  v_fd_table_init(task); // fd 0/1/2 -> /dev/console
#endif
#if VAIOS_IPC_FD
  task->in_multiwait = 0; // not in v_wait()
  task->narm = 0;
#endif
#if VAIOS_TASK_HEAP
  // The heap sits at the LOW end of this task's block, just above the stack
  // guard; it grows up while the stack grows down from the top of the same
  // block. Empty until the first malloc.
  {
    uint32_t guard_off = 0;
#if VAIOS_MPU_STACK_GUARD
    guard_off = VAIOS_MPU_GUARD_SIZE;
#endif
    task->heap_base = (uint8_t *)task->mem_block + guard_off;
    task->heap_brk = task->heap_base;
    task->heap_peak_brk = task->heap_base;
  }
#endif
  init_task_stack(task);

  ENTER_CRITICAL();
  add_to_ready_list(task);
  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG,
        "[TASK] Created Task id: %u priority: %u memory block addr: %p stack "
        "size: 0x%x",
        task->task_id, priority, task->mem_block, size);
  return task->task_id;
}

void task_set_name(uint32_t task_id, const char *name) {
  TCB *task = get_task_by_id(task_id);
  if (task)
    task->name = name ? name : "";
}

const char *task_get_name(const TCB *task) {
  return (task && task->name) ? task->name : "";
}

const char *task_get_name_by_id(uint32_t task_id) {
  return task_get_name(get_task_by_id(task_id));
}

//-----------------------------------------------------------------------------
// Scheduler Core Functions
//-----------------------------------------------------------------------------
TCB *get_next_task(void) {
#if VAIOS_MODULE_PERF
  TCB *_perf_prev = current_task;
#endif
  uint32_t now = v_get_ticks();
  current_task->ticks_run += now - last_context_switch_tick;
  last_context_switch_tick = now;
  // Delayed-task and semaphore-timeout wakeups are driven once per tick by
  // SysTick (wake_up_delayed_tasks_isr) — no per-context-switch list scan.

  // Runs inside PendSV / load_next_task_from_isr — basepri already masks
  // syscall-level IRQs and nothing else can touch the ready lists here, so
  // the surgery is done inline with the O(1) rl_* helpers (no calls, no
  // nested critical section).
  if (current_task && current_task->status == TASK_RUNNING)
    rl_append(current_task);

  int p = highest_ready_prio();
  if (p >= 0) {
    TCB *t = rl_pop_head((uint32_t)p);
    /* Corruption sanity check on the popped TCB. The RAM-range test lives in
     * the port (v_port_ptr_is_ram); the host stub returns 1 since host TCBs
     * come from the host heap, not a known target map. */
    if (!v_port_ptr_is_ram(t) || t->priority > MAX_PRIORITY) {
      v_panic(__FILE__, __LINE__, "invalid task pointer: %p", (void *)t);
    }
    current_task = t;
  } else {
    if (current_task->status != TASK_RUNNING) {
      current_task = idle_task;
    }
  }
  current_task->status = TASK_RUNNING;
  context_switch_count++;
  PERF_SCHED_SWITCH(_perf_prev, current_task);
#if TASK_STACK_WATERMARK_ENABLE == 1
  if ((uint32_t)(current_task->sp) <
      ((uint32_t)current_task->mem_block + TASK_STACK_OVERFLOW_THRESHOLD)) {
    v_panic(__FILE__, __LINE__,
            "stack overflow in task %d | SP: 0x%x, Base: 0x%x",
            current_task->task_id, (uint32_t)current_task->sp,
            (uint32_t)current_task->mem_block);
  }
#endif
  return current_task;
}

void set_next_task(void) {
  if (get_next_task() == NULL)
    v_panic(__FILE__, __LINE__, "current_task is NULL");
}

// Privileged body: mark the caller terminated, park it for the idle GC, and pend
// the switch. RETURNS to its caller (the SVC dispatch or task_exit's spin), which
// is why task_exit — not this — is the noreturn entry.
void v_task_exit_impl(void) {
  ENTER_CRITICAL();
  v_ipc_task_teardown(current_task); // release held mutexes / wait memberships
  current_task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, current_task);
  _terminated_count++;
  EXIT_CRITICAL();
  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
}

// A task's function-return LR. Under the unprivileged flip the body touches
// kernel state (ready lists, ICSR), so an unprivileged task must trap; the
// dispatch runs v_task_exit_impl privileged. Flag off (or privileged), run it
// directly. Either way we never return — PendSV switches the terminated task out.
__attribute__((noreturn)) void task_exit(void) {
#if VAIOS_MPU_USER_SEPARATION && VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    v_svc0(SYS_exit); // traps; kernel terminates us; we never resume
  else
#endif
    v_task_exit_impl();
  while (1)
    ;
}

// --- Syscall-boundary pointer validation (Stage 5, 5c) ----------------------
// Always compiled (pure, host-testable); only the USE in the syscall dispatch is
// gated on VAIOS_MPU_USER_SEPARATION.
// A user pointer is valid only if [p, p+len) lies wholly within the calling
// task's own block [mem_block, mem_block+stack_size). Overflow-safe. `write` is
// a hook for a future finer split; the whole block is RW today.
int v_access_ok(const void *p, uint32_t len, int write) {
  (void)write;
  TCB *t = current_task;
  if (!t || !t->mem_block)
    return 0;
  uintptr_t base = (uintptr_t)t->mem_block;
#if VAIOS_MPU_STACK_GUARD
  // The bottom VAIOS_MPU_GUARD_SIZE bytes are the no-access stack guard (MPU
  // AP_NONE) — not readable/writable even by the kernel, and no user object
  // lives there. Exclude it, or a pointer into the guard would pass validation
  // and then fault the kernel's own copy. (STAGE5_REVIEW_FINDINGS #11.)
  base += VAIOS_MPU_GUARD_SIZE;
#endif
  uintptr_t end = (uintptr_t)t->mem_block + t->stack_size;
  uintptr_t a = (uintptr_t)p;
  if (a < base || a > end)
    return 0;
  return len <= (uint32_t)(end - a); // a+len <= end, no wrap (a<=end already)
}

// Bounded NUL scan for a user string, never reading past the caller's block.
// Returns the length (excluding NUL), or -1 if the pointer is out of bounds or
// no NUL is found within `max` / the block.
long v_strnlen_user(const char *s, uint32_t max) {
  TCB *t = current_task;
  if (!t || !t->mem_block)
    return -1;
  uintptr_t base = (uintptr_t)t->mem_block;
#if VAIOS_MPU_STACK_GUARD
  base += VAIOS_MPU_GUARD_SIZE; // exclude the no-access guard (see v_access_ok)
#endif
  uintptr_t end = (uintptr_t)t->mem_block + t->stack_size;
  uintptr_t a = (uintptr_t)s;
  if (a < base || a >= end)
    return -1;
  uint32_t avail = (uint32_t)(end - a);
  uint32_t limit = avail < max ? avail : max;
  for (uint32_t i = 0; i < limit; i++)
    if (s[i] == '\0')
      return (long)i;
  return -1; // unterminated within bounds
}
void task_exit_request(uint32_t task_id) {
  TCB *task = get_task_by_id(task_id);
  if (!task || task->status == TASK_TERMINATED)
    return;

  V_KLOG(LOG_DEBUG, "[TASK] Task %u Exit Requested", task_id);

  ENTER_CRITICAL();
  if (task->status == TASK_READY)
    remove_from_ready_list(task);
  else if (task->status == TASK_DELAYED)
    remove_from_delayed_list(task);
  else if (task->status == TASK_BLOCKED)
    // A blocked task already sits on blocked_list (via next/prev); unlink it
    // before re-enqueuing below, or enqueue_task self-links the node and
    // corrupts the list. Its wait-queue membership is cleared by the teardown.
    remove_from_blocked_list(task);

  // Release held mutexes and unlink any wait-queue memberships so a later
  // give/unlock/wake never touches this soon-freed TCB.
  v_ipc_task_teardown(task);

  task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, task);
  _terminated_count++;
  EXIT_CRITICAL();

  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
}
//-----------------------------------------------------------------------------
// Idle Task Function
//-----------------------------------------------------------------------------
extern void v_log_flush(void);
void idle_task_function(void *arg) {
  while (1) {
#if LOGGING_ENABLED == 1
    /* One attempt per pass: v_log_flush() is DMA-driven and self-gated by its
     * read_lock — it kicks a flush if one isn't already in flight, else returns.
     * The old 64x loop just took 63 extra critical sections per pass while the
     * first flush's DMA was still running. At interrupt-cadence wakes this keeps
     * the log buffer drained. */
    v_log_flush();
#endif

    /* Dead-task GC: only walk blocked_list when a task has actually terminated.
     * The common case (nothing pending) costs no critical section / list walk,
     * removing the per-pass interrupt-off window the unconditional scan added. */
    if (_terminated_count > 0) {
      TCB *to_free = NULL;
      ENTER_CRITICAL();
      TCB *task = blocked_list;
      while (task) {
        if (task->status == TASK_TERMINATED) {
          to_free = task;
          remove_from_blocked_list(to_free);
          _terminated_count--;
          break; // free this one; the next pass collects any others
        }
        task = task->next;
      }
      EXIT_CRITICAL();

      if (to_free) {
        V_KLOG(LOG_DEBUG, "[TASK] Garbage Collector freeing task %u",
              to_free->task_id);
        v_port_free_task_stack(to_free); // release any separate port context
        if (to_free->mem_block) {
          v_free(to_free->mem_block);
          to_free->mem_block = NULL;
        }
        // The per-task heap lives inside mem_block, so it was already freed
        // above — no separate release needed.
        v_free(to_free);
      }
    }

    /* Sleep the core until the next interrupt instead of busy-spinning. Each
     * wake does one housekeeping pass (log flush + dead-task GC) then sleeps
     * again, so idle CPU/power collapses from a full-clock spin to interrupt
     * cadence. A wakeup event pending at entry returns immediately, so the GC
     * and flush still keep up. The port facade WFIs on HAL targets and is a
     * no-op (busy-spin) on no-HAL/host builds — no NavHAL knowledge here. */
    v_port_hw_cpu_idle();
  }
}

//-----------------------------------------------------------------------------
// Delayed Task Management
//-----------------------------------------------------------------------------
void add_to_delayed_list(TCB *task) {
  if (!task)
    return;

  ENTER_CRITICAL();
  task->next = NULL;
  task->prev = NULL;
  task->status = TASK_DELAYED;

  // Insert sorted by absolute wakeup tick (ascending) so the SysTick ISR
  // only needs an O(1) head check to find the next-due task.
  if (!delayed_list || task->delay_ticks < delayed_list->delay_ticks) {
    task->next = delayed_list;
    if (delayed_list)
      delayed_list->prev = task;
    delayed_list = task;
    EXIT_CRITICAL();
    return;
  }

  TCB *cur = delayed_list;
  while (cur->next && cur->next->delay_ticks <= task->delay_ticks)
    cur = cur->next;

  task->next = cur->next;
  task->prev = cur;
  if (cur->next)
    cur->next->prev = task;
  cur->next = task;
  EXIT_CRITICAL();
}

void remove_from_delayed_list(TCB *task) {
  remove_task(&delayed_list, task);
  task->next = NULL;
  task->prev = NULL;
}

void task_delay(uint32_t ticks) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) { // task-facing: trap into the kernel
    v_svc1(SYS_delay, ticks);
    return;
  }
#endif
  if (current_task == NULL)
    v_panic(__FILE__, __LINE__, "current_task is NULL");
  if (current_task == idle_task)
    return;
  if (ticks == 0)
    return;

  ENTER_CRITICAL();
  current_task->delay_ticks = v_get_ticks() + ticks;
  current_task->status = TASK_DELAYED;
  add_to_delayed_list(current_task);
  EXIT_CRITICAL();

  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
}

bool task_delay_until(uint32_t *last_wake, uint32_t period) {
  if (current_task == NULL)
    v_panic(__FILE__, __LINE__, "current_task is NULL");
  if (current_task == idle_task || last_wake == NULL || period == 0)
    return false;

  ENTER_CRITICAL();
  uint32_t wake = *last_wake + period;
  *last_wake = wake;
  /* Signed delta so the comparison is wrap-safe: if the deadline already
   * passed (the loop body overran a period), don't block — let it catch up. */
  if ((int32_t)(wake - v_get_ticks()) <= 0) {
    EXIT_CRITICAL();
    return false;
  }
  current_task->delay_ticks = wake; /* absolute deadline */
  current_task->status = TASK_DELAYED;
  add_to_delayed_list(current_task);
  EXIT_CRITICAL();

  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
  return true;
}

// SysTick wake path — runs once per tick, and is the ONLY place delayed-task
// and semaphore-timeout wakeups happen (get_next_task no longer scans, so a
// context switch stays lean). delayed_list is sorted ascending by delay_ticks
// so a head-only walk drains every due sleeper. Returns nonzero if any woken
// task outranks the current task, letting SysTick pend an immediate switch.
int wake_up_delayed_tasks_isr(void) {
  int higher_woken = 0;
  uint32_t now = v_get_ticks();

  ENTER_CRITICAL();

  // 1. Sleepers from task_delay(). <= (not <) so an N-tick delay wakes on
  //    the Nth tick, not the N+1th.
  while (delayed_list && delayed_list->delay_ticks <= now) {
    TCB *to_wake = delayed_list;
    delayed_list = to_wake->next;
    if (delayed_list)
      delayed_list->prev = NULL;
    to_wake->next = NULL;
    to_wake->prev = NULL;
    to_wake->delay_ticks = 0;
    to_wake->status = TASK_READY;
    add_to_ready_list(to_wake);
    if (current_task && to_wake->priority > current_task->priority)
      higher_woken = 1;
  }

  // 2. Eject semaphore waiters whose take() timeout has expired. wait_sem is
  //    left set so the resumed take() returns VA_FAIL.
  TCB *task = blocked_list;
  while (task) {
    TCB *next = task->next;
    if (task->wait_sem != NULL && task->delay_ticks != 0 &&
        task->delay_ticks < now) {
      // Remove from the semaphore wait queue (threaded via wait_next).
      sema_t *s = (sema_t *)task->wait_sem;
      if (s->wait_q == task) {
        s->wait_q = task->wait_next;
        if (!s->wait_q)
          s->tail = NULL;
      } else {
        TCB *prev_node = s->wait_q;
        while (prev_node && prev_node->wait_next != task)
          prev_node = prev_node->wait_next;
        if (prev_node) {
          prev_node->wait_next = task->wait_next;
          if (s->tail == task)
            s->tail = prev_node;
        }
      }
      remove_from_blocked_list(task);
      task->delay_ticks = 0;
      task->status = TASK_READY;
      add_to_ready_list(task);
      task->wait_mutex = NULL; // clear for a timed-out mutex waiter (no-op for sems)
#if VAIOS_SYSCALL_SVC
      // Deliver VA_FAIL to a syscall-blocked take()/lock() that timed out.
      v_syscall_wake_result(task, VA_FAIL);
#endif
      if (current_task && task->priority > current_task->priority)
        higher_woken = 1;
    }
#if VAIOS_IPC_FD
    // 2b. Eject a v_wait() multi-waiter whose timeout expired. Its observer
    // nodes are unlinked later by mwait_disarm (thread mode); clearing
    // in_multiwait makes any give on a watched sem skip it in the meantime. The
    // result must be negative (not VA_FAIL/0, which is a valid ready index) so
    // v_wait treats it as "timed out" and its disarm reports no ready fd.
    else if (task->in_multiwait && task->delay_ticks != 0 &&
             task->delay_ticks < now) {
      task->in_multiwait = 0;
      remove_from_blocked_list(task);
      task->delay_ticks = 0;
      task->status = TASK_READY;
      add_to_ready_list(task);
#if VAIOS_SYSCALL_SVC
      v_syscall_wake_result(task, -1);
#endif
      if (current_task && task->priority > current_task->priority)
        higher_woken = 1;
    }
#endif
    task = next;
  }

  EXIT_CRITICAL();
  return higher_woken;
}

//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Blocked Task Management
//-----------------------------------------------------------------------------
void task_block(void) {
  if (current_task == idle_task)
    return;

  ENTER_CRITICAL();
  if (current_task->status == TASK_BLOCKED) {
    EXIT_CRITICAL();
    return;
  }
  if (current_task->status == TASK_DELAYED)
    remove_from_delayed_list(current_task);
  if (current_task->status == TASK_READY)
    remove_from_ready_list(current_task);
  add_to_blocked_list(current_task);
  EXIT_CRITICAL();

  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
}

void add_to_blocked_list(TCB *task) {
  enqueue_task(&blocked_list, task);
  task->status = TASK_BLOCKED;
}

void remove_from_blocked_list(TCB *task) {
  remove_task(&blocked_list, task);
  task->next = NULL;
  task->prev = NULL;
}

void task_unblock(uint32_t task_id) {
  ENTER_CRITICAL();
  TCB *task = get_task_by_id(task_id);
  if (!task || task->status != TASK_BLOCKED) {
    EXIT_CRITICAL();
    return;
  }
  remove_from_blocked_list(task);
  task->status = TASK_READY;
  add_to_ready_list(task);
  EXIT_CRITICAL();

  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)
}
void task_change_priority(TCB *task, uint32_t new_priority) {
  if (!task || new_priority > MAX_PRIORITY)
    return;

  ENTER_CRITICAL();
  if (task->status == TASK_READY) {
    remove_from_ready_list(task);
    task->priority = new_priority;
    add_to_ready_list(task);
  } else {
    task->priority = new_priority;
  }
  EXIT_CRITICAL();
}

//-----------------------------------------------------------------------------
// Scheduler Initialization and Control
//-----------------------------------------------------------------------------
void scheduler_init(void) {
  for (uint8_t i = 0; i <= MAX_PRIORITY; i++) {
    ready_lists[i] = NULL;
    ready_tails[i] = NULL;
  }
  blocked_list = NULL;
  delayed_list = NULL;
  ready_bitmap = 0;
  context_switch_count = 0;
  last_context_switch_tick = v_get_ticks();
  task_count = 0;
  scheduler_running = 0;
  task_create(idle_task_function, NULL, IDLE_TASK_STACK_SIZE, 0);
  idle_task = ready_lists[0];
#if VAIOS_MPU_USER_SEPARATION
  // The idle task runs kernel housekeeping (log flush, dead-task GC), so it must
  // stay privileged even though task_create defaults new tasks to unprivileged.
  idle_task->privileged = 1;
#endif
  current_task = idle_task;
}

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
TCB *get_current_task(void) { return current_task; }

uint32_t get_context_switch_count(void) { return context_switch_count; }

uint32_t get_idle_tick_count(void) { return idle_task->ticks_run; }

// Snapshot the set of live tasks into out[] (running + ready + blocked +
// delayed). The running task is normally off the scheduler lists, but at
// scheduler_init (before the first switch) the idle task is both current_task
// AND on a ready list, so current_task is recorded first and then skipped in
// the list walks to guarantee no duplicates. Copies POINTERS under a brief
// critical section — bounded and fast — so the caller can read each task's
// stats/stack outside the lock (racy but tolerable for diagnostics). Returns
// the count written (<= max).
int task_snapshot_list(TCB **out, int max) {
  if (!out || max <= 0)
    return 0;
  int n = 0;
  ENTER_CRITICAL();
  if (current_task && n < max)
    out[n++] = current_task;
  for (uint32_t p = 0; p <= MAX_PRIORITY && n < max; p++)
    for (TCB *t = ready_lists[p]; t && n < max; t = t->next)
      if (t != current_task)
        out[n++] = t;
  for (TCB *t = blocked_list; t && n < max; t = t->next)
    if (t != current_task)
      out[n++] = t;
  for (TCB *t = delayed_list; t && n < max; t = t->next)
    if (t != current_task)
      out[n++] = t;
  EXIT_CRITICAL();
  return n;
}
TCB *get_task_by_id(uint32_t task_id) {
  ENTER_CRITICAL();
  if (current_task->task_id == task_id) {
    EXIT_CRITICAL();
    return current_task;
  }
  for (int i = 0; i < MAX_PRIORITY; i++) {
    TCB *curr = ready_lists[i];
    while (curr) {
      if (curr->task_id == task_id) {
        EXIT_CRITICAL();
        return curr;
      }
      curr = curr->next;
    }
  }
  TCB *curr = blocked_list;
  while (curr) {
    if (curr->task_id == task_id) {
      EXIT_CRITICAL();
      return curr;
    }
    curr = curr->next;
  }
  curr = delayed_list;
  while (curr) {
    if (curr->task_id == task_id) {
      EXIT_CRITICAL();
      return curr;
    }
    curr = curr->next;
  }
  EXIT_CRITICAL();
  return NULL;
}
uint32_t get_task_run_time(TCB *task) {
  if (!task)
    return 0;
  return task->ticks_run;
}

void reset_task_run_time(TCB *task) {
  if (task)
    task->ticks_run = 0;
}

void reset_idle_task_timer(void) {
  if (idle_task)
    idle_task->ticks_run = 0;
}
