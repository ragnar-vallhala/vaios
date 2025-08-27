#include "task.h"
#include "config.h"
#include "memory.h"
#include "structures.h"
#include "utils.h"
#include <stddef.h>
#include <stdint.h>

TCB *ready_queue = NULL;
// TCB *blocked_queue = NULL;
// TCB *sleep_queue = NULL;

Scheduler_Status_Type scheduler_state = SCHEDULER_STOPPED;
uint32_t task_count = 0;
uint32_t task_id = 0;
TCB *current_task = NULL;

// ---------------------- User functions ------------------------
void task_exit(void); // forward declaration

// --------------------------------------------------
// Stack Initialization
// --------------------------------------------------
void prepare_task_stack(TCB *task)
{
  if (!task || !task->stack_base)
    return;

  // Compute stack top (highest address)
  uint32_t *stack_top = task->stack_base + (task->stack_size / sizeof(uint32_t));

  // Align stack top to 8-byte boundary (required for Cortex-M)
  stack_top = (uint32_t *)((uintptr_t)stack_top & ~0x7UL);

  // --- Hardware-saved context for exception return ---
  *(--stack_top) = 0x01000000;                  // xPSR (Thumb bit)
  *(--stack_top) = (uint32_t)(task->entry) | 1; // PC = task entry | Thumb bit
  *(--stack_top) = (uint32_t)task_exit;         // LR = task_exit
  *(--stack_top) = 0x00000000;                  // R12
  *(--stack_top) = 0x00000000;                  // R3
  *(--stack_top) = 0x00000000;                  // R2
  *(--stack_top) = 0x00000000;                  // R1
  *(--stack_top) = (uint32_t)task->arg;         // R0 = argument

  // --- Space for callee-saved registers R4-R11 ---
  for (int i = 0; i < 8; i++)
    *(--stack_top) = 0;

  // Save PSP (stack pointer) to TCB
  task->stack_ptr = stack_top;
}

// Task exit function: called if a task returns
void task_exit(void)
{
  TCB *curr = task_get_current();
  v_free(curr->stack_base);
  v_free(curr);
  // PendSV_Handler();
  while (1)
    __asm volatile("wfi");
}

// --------------------------------------------------
// Create a new task
// --------------------------------------------------
uint32_t v_task_create(void (*entry)(void *), void *arg, uint8_t priority, uint32_t stack_size)
{
  if (!entry || stack_size < 32) // minimum stack
    return 0;

  // Allocate TCB
  TCB *task = (TCB *)v_malloc(sizeof(TCB));
  if (!task)
    return 0;

  task->task_id = ++task_id;
  task->entry = entry;
  task->arg = arg;
  task->priority = priority;
  task->stack_size = stack_size;
  task->state = TASK_READY;
  task->delay_ticks = 0;

  // Allocate stack (+8 bytes for alignment safety)
  uint32_t *alloc = (uint32_t *)v_malloc(stack_size + 8);
  if (!alloc)
  {
    v_free(task);
    return 0;
  }

  task->stack_allocation = alloc;
  task->stack_base = (uint32_t *)(((uintptr_t)alloc + 7) & ~0x7UL);

  // Initialize stack context
  prepare_task_stack(task);

  // Insert into ready queue
  task_insert_sorted(&ready_queue, task);

  // Log task creation
  v_log(LOG_INFO,
        "[TASK] Added task id=%d, priority=%d, stack_size=%d, stack_base=0x%x",
        task->task_id, task->priority, task->stack_size,
        (unsigned)task->stack_base);

  return task->task_id;
}
TCB *task_get_current(void) { return current_task; }
void task_set_current(TCB *task) { current_task = task; }
// ---------------------- Queue Operations ----------------------

// Add task at the end of a circular doubly-linked queue
void task_enqueue(TCB **head, TCB *task)
{
  if (!head || !task)
    return;

  if (*head == NULL)
  {
    // First node points to itself
    task->next = task;
    task->prev = task;
    *head = task;
    task_count = 1;
  }
  else
  {
    TCB *tail = (*head)->prev; // tail is head->prev
    tail->next = task;
    task->prev = tail;
    task->next = *head;
    (*head)->prev = task;
    task_count++;
  }
}

// Remove and return the head of the queue
TCB *task_dequeue(TCB **head)
{
  if (!head || !*head)
    return NULL;

  TCB *task = *head;

  if (task->next == task)
  {
    // Only one node in the queue
    *head = NULL;
  }
  else
  {
    TCB *tail = task->prev;
    *head = task->next;
    (*head)->prev = tail;
    tail->next = *head;
  }

  task->next = NULL;
  task->prev = NULL;
  task_count--;
  return task;
}

// Remove a specific task from a circular queue
void task_remove(TCB **head, TCB *task)
{
  if (!head || !*head || !task)
    return;

  if (task->next == task)
  {
    // Only one node
    *head = NULL;
  }
  else
  {
    task->prev->next = task->next;
    task->next->prev = task->prev;
    if (*head == task)
    {
      *head = task->next;
    }
  }

  task->next = NULL;
  task->prev = NULL;
  task_count--;
}

// --------------------------------------------------
// Insert task into ready queue sorted by priority
// --------------------------------------------------
void task_insert_sorted(TCB **head, TCB *task)
{
  if (!head || !task)
    return;

  if (*head == NULL)
  {
    // First task in queue
    task->next = task->prev = task;
    *head = task;
    return;
  }

  TCB *curr = *head;
  do
  {
    if (task->priority > curr->priority)
      break;
    curr = curr->next;
  } while (curr != *head);

  if (curr == *head && task->priority <= (*head)->prev->priority)
  {
    // Insert at end
    TCB *tail = (*head)->prev;
    tail->next = task;
    task->prev = tail;
    task->next = *head;
    (*head)->prev = task;
  }
  else if (curr == *head)
  {
    // Insert before head
    TCB *tail = (*head)->prev;
    task->next = *head;
    task->prev = tail;
    tail->next = task;
    (*head)->prev = task;
    *head = task;
  }
  else
  {
    // Insert before curr
    TCB *prev = curr->prev;
    prev->next = task;
    task->prev = prev;
    task->next = curr;
    curr->prev = task;
  }
}

// ---------------------- Scheduler Helpers ----------------------

// Return next READY task from ready_queue
TCB *task_get_next_ready(void)
{
  if (!ready_queue)
    return NULL;

  TCB *curr = ready_queue;
  do
  {
    if (curr->state == TASK_READY)
      return curr;
    curr = curr->next;
  } while (curr != ready_queue);

  return NULL;
}

// Move task from ready_queue to blocked_queue
void task_block(TCB *task)
{
  if (!task)
    return;
  task_remove(&ready_queue, task);
  task->state = TASK_BLOCKED;
  // task_enqueue(&blocked_queue, task);
}

// Move task from blocked_queue to ready_queue
void task_unblock(TCB *task)
{
  if (!task)
    return;
  // task_remove(&blocked_queue, task);
  task->state = TASK_READY;
  task_enqueue(&ready_queue, task);
}

// Move task to sleep_queue with delay_ticks
void task_sleep(TCB *task, uint32_t ticks)
{
  if (!task)
    return;
  task_remove(&ready_queue, task);
  task->state = TASK_SLEEPING;
  task->delay_ticks = ticks;
  // task_insert_sorted(&sleep_queue, task); // sorted by priority or wake-up
}
