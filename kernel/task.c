#include "task.h"
#include "ipc.h"
#include "memory.h"
#include "perf_hooks.h"
#include "port.h" // ENTER_CRITICAL / EXIT_CRITICAL
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
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size,
                     uint32_t priority) {
  if (stack_size < 128)
    stack_size = 128;
  stack_size &= ~(3); // Align to 4 bytes

  TCB *task = (TCB *)v_malloc(sizeof(TCB));
  if (!task)
    return 0;
  task->task_id = ++task_count;
  task->stack_size = stack_size;
  task->mem_block = (uint32_t *)v_malloc(stack_size);
  if (!task->mem_block) {
    v_panic(__FILE__, __LINE__, "failed to allocate stack for task %u",
            task->task_id);
  }
  task->ticks_run = 0;
  task->entry = entry;
  task->arg = arg;
  task->sp = task->mem_block + (stack_size / sizeof(uint32_t));
  task->priority = priority;
  task->base_priority = priority;
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
  for (uint32_t i = 0; i < stack_size / sizeof(uint32_t); i++)
    task->mem_block[i] = V_PERF_STACK_FILL;
#endif
  task->magic = TCB_MAGIC;
  init_task_stack(task);

  ENTER_CRITICAL();
  add_to_ready_list(task);
  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG,
        "[TASK] Created Task id: %u priority: %u memory block addr: 0x%x stack "
        "size: 0x%x",
        task->task_id, priority, task->mem_block, stack_size);
  return task->task_id;
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
#if defined(VAIOS_HOST_TEST)
    /* Host TCBs come from the host heap, not target SRAM, so the address
     * range check below is meaningless (and (uint32_t)-truncating a 64-bit
     * host pointer is undefined). Keep the priority sanity check only. */
    if (t->priority > MAX_PRIORITY) {
      v_panic(__FILE__, __LINE__, "invalid task priority: %u", t->priority);
    }
#else
    if ((uint32_t)t < 0x20000000 || (uint32_t)t > 0x20020000 ||
        t->priority > MAX_PRIORITY) {
      v_panic(__FILE__, __LINE__, "invalid task pointer: 0x%x", (uint32_t)t);
    }
#endif
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

__attribute__((noreturn)) void task_exit(void) {
  ENTER_CRITICAL();
  current_task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, current_task);
  EXIT_CRITICAL();

  task_yield();
  while (1)
    ;
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

  task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, task);
  EXIT_CRITICAL();

  task_yield();
}
//-----------------------------------------------------------------------------
// Idle Task Function
//-----------------------------------------------------------------------------
extern void v_log_flush(void);
void idle_task_function(void *arg) {
  while (1) {
#if LOGGING_ENABLED == 1
    for (int i = 0; i < 64; i++)
      v_log_flush();
#endif
    TCB *to_free = NULL;

    // Find one terminated task to free under critical section
    ENTER_CRITICAL();
    TCB *task = blocked_list;
    while (task) {
      if (task->status == TASK_TERMINATED) {
        to_free = task;
        remove_from_blocked_list(to_free);
        break; // we will free this one, then check again next loop
      }
      task = task->next;
    }
    EXIT_CRITICAL();

    if (to_free) {
      V_KLOG(LOG_DEBUG, "[TASK] Garbage Collector freeing task %u",
            to_free->task_id);
      if (to_free->mem_block) {
        v_free(to_free->mem_block);
        to_free->mem_block = NULL;
      }
      v_free(to_free);
    }
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

  task_yield();
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
      if (current_task && task->priority > current_task->priority)
        higher_woken = 1;
    }
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

  task_yield();
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

  task_yield();
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
  current_task = idle_task;
}

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
TCB *get_current_task(void) { return current_task; }

uint32_t get_context_switch_count(void) { return context_switch_count; }

uint32_t get_idle_tick_count(void) { return idle_task->ticks_run; }

// Snapshot the set of live tasks into out[] (running + ready + blocked +
// delayed; the running task is off the scheduler lists, so no duplicates).
// Copies POINTERS under a brief critical section — bounded and fast — so the
// caller can read each task's stats/stack outside the lock (racy but
// tolerable for diagnostics). Returns the count written (<= max).
int task_snapshot_list(TCB **out, int max) {
  if (!out || max <= 0)
    return 0;
  int n = 0;
  ENTER_CRITICAL();
  if (current_task && n < max)
    out[n++] = current_task;
  for (uint32_t p = 0; p <= MAX_PRIORITY && n < max; p++)
    for (TCB *t = ready_lists[p]; t && n < max; t = t->next)
      out[n++] = t;
  for (TCB *t = blocked_list; t && n < max; t = t->next)
    out[n++] = t;
  for (TCB *t = delayed_list; t && n < max; t = t->next)
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
