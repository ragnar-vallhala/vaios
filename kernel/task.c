#include "task.h"
#include "memory.h"
#include "utils.h"
#include <stddef.h>
#include <stdint.h>

TCB *ready_lists[MAX_PRIORITY + 1]; // Ready task lists by priority
TCB *current_task;                  // Currently running task
TCB *idle_task;                     // Idle task pointer
uint32_t ready_bitmap;              // Bitmap for O(1) priority search
uint32_t context_switch_count;      // Context switch counter
uint32_t idle_tick_count;
uint32_t task_count;
uint8_t scheduler_running = 0;

static inline void rb_set(uint32_t prio) { ready_bitmap |= (1u << prio); }
static inline void rb_clear(uint32_t prio) { ready_bitmap &= ~(1u << prio); }
static inline int rb_any(void) { return ready_bitmap != 0; }

// Return highest set bit index (portable fallback)
static inline int highest_ready_prio(void)
{
  if (!ready_bitmap)
    return -1;
  // If you have __builtin_clz, use it; below is a simple scan
  for (int p = (int)MAX_PRIORITY; p >= 0; --p)
    if (ready_bitmap & (1u << p))
      return p;
  return -1;
}

void add_to_ready_list(TCB *task)
{
  if (!task)
    return;

  task->next = NULL;
  task->prev = NULL;

  TCB **head = &ready_lists[task->priority];

  if (*head == NULL)
  {
    // First task at this priority
    *head = task;
    rb_set(task->priority);
  }
  else
  {
    // Append at the tail
    TCB *curr = *head;
    while (curr->next)
    {
      curr = curr->next;
    }
    curr->next = task;
    task->prev = curr;
  }
  task->status = TASK_READY;
}

void remove_from_ready_list(TCB *task)
{
  if (!task)
    return;

  TCB **head = &ready_lists[task->priority];

  if (*head == NULL)
    return;

  // If removing head of the list
  if (*head == task)
  {
    *head = task->next;
    if (*head)
      (*head)->prev = NULL;
    else
      rb_clear(task->priority); // list became empty
    if (task->next)
    {
      task->next->prev = NULL;
    }
  }
  else
  {
    // Middle or tail
    if (task->prev)
    {
      task->prev->next = task->next;
    }
    if (task->next)
    {
      task->next->prev = task->prev;
    }
  }

  task->next = NULL;
  task->prev = NULL;
}

TCB *get_highest_priority_task(void)
{
  int p = highest_ready_prio();
  return (p < 0) ? NULL : ready_lists[p];
}

void idle_task_function(void *arg)
{
  uint32_t last_tick = v_get_ticks();
  while (1)
  {
    uint32_t ticks = v_get_ticks();
    if (ticks != last_tick)
    {
      idle_tick_count++;
      last_tick = ticks;
    }
  }
}

// Task stack initialization
void init_task_stack(TCB *task)
{
  // Align sp to 8 bytes
  uint32_t *sp = (uint32_t *)((uint32_t)(task->sp) & (~7UL));
  sp--;
  *sp = INITIAL_XPSR;
  sp--;
  *sp = ((uint32_t)task->entry) & TASK_ENTRY_MASK;
  sp--;
  *sp = (uint32_t)TASK_EXIT;
  sp -= 5;
  *sp = (uint32_t)task->arg;
  sp--;
  *sp = 0xfffffffd;
  sp -= 8;
  task->sp = sp;
}

// Task creation and management
uint32_t task_create(void (*entry)(void *), void *arg, uint32_t stack_size,
                     uint32_t priority)
{
  if (stack_size < 128)
  {
    // tiny guard: require a sane minimum stack (caller can override)
    stack_size = 128;
  }
  stack_size &= ~(3); // make it multiple of 4
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
  return task->task_id;
}

TCB *get_next_task(void)
{
  TCB *t = get_highest_priority_task();
  if (t)
  {
    remove_from_ready_list(t);
    if (current_task)
    {
      add_to_ready_list(current_task);
    }
    current_task = t;
  }
  else
    current_task = idle_task; // no ready tasks
  return current_task;
}

void set_next_task(void) { get_next_task(); }
__attribute__((noreturn)) void task_exit(void)
{
  // Mark the current task as finished
  current_task->status =
      TASK_TERMINATED; // 0 = TASK_TERMINATED (define as you like)

  // Remove it from the scheduler's ready list if needed
  // (for circular list, you can just skip terminated tasks in PendSV)

  // Yield CPU so scheduler can switch to next task
  task_yield();

  // Should never return here
  while (1)
    ;
}
#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04)
#define PENDSVSET (1U << 28)
void task_yield(void)
{
  SCB_ICSR |= PENDSVSET;

  /* Data/Instruction barriers manually */
  asm volatile("dsb");
  asm volatile("isb");
}

void scheduler_init(void)
{
  // Initialize scheduler

  for (uint8_t i = 0; i <= MAX_PRIORITY; i++)
  {
    ready_lists[i] = NULL;
  }
  ready_bitmap = 0;         // Bitmap for O(1) priority search
  context_switch_count = 0; // Context switch counter
  idle_tick_count = 0;
  task_count = 0;
  scheduler_running = 0;
  task_create(idle_task_function, NULL, 256, 0);
  idle_task = ready_lists[0];
  ;                         // Idle task pointer
  current_task = idle_task; // Currently running task
}
__attribute__((naked)) void scheduler_start(void)
{
  __asm volatile(
      "ldr r0, =scheduler_running\n"
      "mov r1, #123             \n"
      "strb r1, [r0]          \n"
      " ldr r0, =0xE000ED08   \n" /* Use the NVIC offset register to locate the
                                     stack. */
      " ldr r0, [r0]          \n"
      " ldr r0, [r0]          \n"
      " msr msp, r0           \n" /* Set the msp back to the start of the stack.
                                   */
      " mov r0, #0            \n" /* Clear the bit that indicates the FPU is in
                                     use, see comment above. */
      " msr control, r0       \n"
      " cpsie i               \n" /* Globally enable interrupts. */
      " cpsie f               \n"
      " dsb                   \n"
      "svc 0                  \n"
      " nop                   \n"
      " .ltorg                \n");
}
