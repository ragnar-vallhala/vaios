#include "task.h"
#include "memory.h"
#include "utils.h"
#include <stddef.h>
#include <stdint.h>

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------
TCB *ready_lists[MAX_PRIORITY + 1]; // Ready task lists by priority
TCB *blocked_list = NULL;           // Blocked tasks list
TCB *delayed_list = NULL;           // Delayed tasks list
TCB *current_task;                  // Currently running task
TCB *idle_task;                     // Idle task pointer
uint32_t ready_bitmap;              // Bitmap for O(1) priority search
uint32_t context_switch_count;      // Context switch counter
uint32_t task_count;
uint8_t scheduler_running = 0;
static uint32_t last_context_switch_tick = 0; // To track ticks for current task

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------
static inline void rb_set(uint32_t prio) { ready_bitmap |= (1u << prio); }
static inline void rb_clear(uint32_t prio) { ready_bitmap &= ~(1u << prio); }
static inline int rb_any(void) { return ready_bitmap != 0; }

// Return highest set bit index (portable fallback)
static inline int highest_ready_prio(void)
{
  if (!ready_bitmap)
    return -1;
  for (int p = (int)MAX_PRIORITY; p >= 0; --p)
    if (ready_bitmap & (1u << p))
      return p;
  return -1;
}

//-----------------------------------------------------------------------------
// Task List Management Functions
//-----------------------------------------------------------------------------
void enqueue_task(TCB **list, TCB *task)
{
  if (!task)
    return;

  task->next = NULL;
  task->prev = NULL;

  if (*list == NULL)
  {
    *list = task;
  }
  else
  {
    TCB *curr = *list;
    while (curr->next)
    {
      curr = curr->next;
    }
    curr->next = task;
    task->prev = curr;
  }
}

void remove_task(TCB **list, TCB *task)
{
  if (!task || !*list)
    return;

  if (*list == task)
  {
    *list = task->next;
    if (*list)
      (*list)->prev = NULL;
    if (task->next)
      task->next->prev = NULL;
  }
  else
  {
    if (task->prev)
      task->prev->next = task->next;
    if (task->next)
      task->next->prev = task->prev;
  }

  task->next = NULL;
  task->prev = NULL;
}

TCB *dequeue_task(TCB **list)
{
  TCB *task = peek_task(list);
  if (task)
    remove_task(list, task);
  return task;
}

TCB *peek_task(TCB **list)
{
  return (list && *list) ? *list : NULL;
}

//-----------------------------------------------------------------------------
// Ready List Management
//-----------------------------------------------------------------------------
void add_to_ready_list(TCB *task)
{
  if (!task)
    return;
  TCB **head = &ready_lists[task->priority];

  enqueue_task(head, task);
  rb_set(task->priority);
  task->status = TASK_READY;
}

void remove_from_ready_list(TCB *task)
{
  if (!task)
    return;

  TCB **head = &ready_lists[task->priority];

  if (*head == NULL)
    return;

  remove_task(head, task);
  if (*head == NULL)
    rb_clear(task->priority);

  task->next = NULL;
  task->prev = NULL;
}

TCB *get_highest_priority_task(void)
{
  int p = highest_ready_prio();
  return (p < 0) ? NULL : ready_lists[p];
}

//-----------------------------------------------------------------------------
// Task Creation and Management
//-----------------------------------------------------------------------------
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size, uint32_t priority)
{
  ENTER_CRITICAL();
  if (stack_size < 128)
    stack_size = 128;
  stack_size &= ~(3); // Align to 4 bytes

  TCB *task = (TCB *)v_malloc(sizeof(TCB));
  if (!task)
    return 0;
  task->stack_size = stack_size;
  task->mem_block = (uint32_t *)v_malloc(stack_size);
  if (!task->mem_block)
  {
    v_free(task);
    return 0;
  }
  task->task_id = ++task_count;
  task->ticks_run = 0;
  task->entry = entry;
  task->arg = arg;
  task->sp = task->mem_block + (stack_size / sizeof(uint32_t));
  task->priority = priority;
  task->delay_ticks = 0;
  task->next = NULL;
  task->prev = NULL;
  task->status = TASK_READY;
  init_task_stack(task);

  add_to_ready_list(task);
  EXIT_CRITICAL();
  return task->task_id;
}

//-----------------------------------------------------------------------------
// Scheduler Core Functions
//-----------------------------------------------------------------------------
TCB *get_next_task(void)
{
  current_task->ticks_run += v_get_ticks() - last_context_switch_tick;
  last_context_switch_tick = v_get_ticks();
  wake_up_delayed_tasks();

  TCB *t = get_highest_priority_task();
  if (t)
  {
    remove_from_ready_list(t);
    if (current_task->status == TASK_RUNNING)
      add_to_ready_list(current_task);
    current_task = t;
  }
  else
    current_task = idle_task;
  current_task->status = TASK_RUNNING;
  context_switch_count++;
  return current_task;
}

void set_next_task(void)
{
  get_next_task();
}

__attribute__((noreturn)) void task_exit(void)
{
  ENTER_CRITICAL();
  v_log(LOG_INFO, "Task %u Exiting", current_task->task_id);
  current_task->status = TASK_TERMINATED;
  enqueue_task(&blocked_list, current_task);
  EXIT_CRITICAL();
  task_yield();
  while (1)
    ;
}

//-----------------------------------------------------------------------------
// Idle Task Function
//-----------------------------------------------------------------------------
void idle_task_function(void *arg)
{
  TCB *task = blocked_list;
  while (1)
  {
    task = blocked_list;
    if (task == NULL)
    {
      task_delay(100); // Sleep for a while if no blocked tasks
      continue;
    }
    while (task)
    {
      v_log(LOG_DEBUG, "GC Checking Task %u Status: %d", task->task_id, task->status);
      if (task->status == TASK_TERMINATED)
      {
        v_log(LOG_DEBUG, "Idle Task Running. CPU_Usage %d, %d/%d",
              ((v_get_ticks() - idle_task->ticks_run + 1) * 100) / (v_get_ticks()),
              idle_task->ticks_run, v_get_ticks());
        TCB *to_free = task;
        task = task->next;
        remove_from_blocked_list(to_free);
        if (to_free->mem_block)
          v_free(to_free->mem_block);
        v_free(to_free);
        continue;
      }
      task = task->next;
    }
  }
}

//-----------------------------------------------------------------------------
// Delayed Task Management
//-----------------------------------------------------------------------------
void add_to_delayed_list(TCB *task)
{
  enqueue_task(&delayed_list, task);
  task->status = TASK_DELAYED;
}

void remove_from_delayed_list(TCB *task)
{
  remove_task(&delayed_list, task);
  task->next = NULL;
  task->prev = NULL;
}

void task_delay(uint32_t ticks)
{
  if (current_task == idle_task)
    return;
  if (ticks == 0)
    return;
  current_task->delay_ticks = v_get_ticks() + ticks;
  current_task->status = TASK_DELAYED;
  add_to_delayed_list(current_task);
  task_yield();
}

void wake_up_delayed_tasks(void)
{
  TCB *task = delayed_list;
  while (task)
  {
    if (task->delay_ticks < v_get_ticks())
    {
      TCB *to_wake = task;
      task = task->next;
      remove_from_delayed_list(to_wake);
      to_wake->delay_ticks = 0;
      to_wake->status = TASK_READY;
      add_to_ready_list(to_wake);
      continue;
    }
    task = task->next;
  }
}

//-----------------------------------------------------------------------------
// Blocked Task Management
//-----------------------------------------------------------------------------
void task_block(void)
{
  if (current_task == idle_task)
    return;
  current_task->status = TASK_BLOCKED;
  add_to_blocked_list(current_task);
  task_yield();
}

void add_to_blocked_list(TCB *task)
{
  enqueue_task(&blocked_list, task);
  task->status = TASK_BLOCKED;
}

void remove_from_blocked_list(TCB *task)
{
  remove_task(&blocked_list, task);
  task->next = NULL;
  task->prev = NULL;
}

void task_unblock(TCB *task)
{
  if (!task || task->status != TASK_BLOCKED)
    return;
  remove_from_blocked_list(task);
  task->status = TASK_READY;
  add_to_ready_list(task);
}

//-----------------------------------------------------------------------------
// Scheduler Initialization and Control
//-----------------------------------------------------------------------------
void scheduler_init(void)
{
  for (uint8_t i = 0; i <= MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = NULL;
  delayed_list = NULL;
  ready_bitmap = 0;
  context_switch_count = 0;
  last_context_switch_tick = v_get_ticks();
  task_count = 0;
  scheduler_running = 0;
  task_create(idle_task_function, NULL, 512, 0);
  idle_task = ready_lists[0];
  current_task = idle_task;
}

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
TCB *get_current_task(void)
{
  return current_task;
}

uint32_t get_context_switch_count(void)
{
  return context_switch_count;
}

uint32_t get_idle_tick_count(void)
{
  return idle_task->ticks_run;
}

uint32_t get_task_run_time(TCB *task)
{
  if (!task)
    return 0;
  return task->ticks_run;
}

void reset_task_run_time(TCB *task)
{
  if (task)
    task->ticks_run = 0;
}

void reset_idle_task_timer(void)
{
  if (idle_task)
    idle_task->ticks_run = 0;
}
