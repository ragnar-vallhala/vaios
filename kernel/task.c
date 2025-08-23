#include "task.h"
#include "utils.h"
#include <stddef.h>

TCB *ready_queue = NULL;
TCB *blocked_queue = NULL;
TCB *sleep_queue = NULL;
uint32_t task_count = 0;

// ---------------------- Queue Operations ----------------------

// Add task at the end of a circular doubly-linked queue
void task_enqueue(TCB **head, TCB *task) {
  if (!head || !task)
    return;

  if (*head == NULL) {
    // First node points to itself
    task->next = task;
    task->prev = task;
    *head = task;
    task_count = 1;
  } else {
    TCB *tail = (*head)->prev; // tail is head->prev
    tail->next = task;
    task->prev = tail;
    task->next = *head;
    (*head)->prev = task;
    task_count++;
  }
}

// Remove and return the head of the queue
TCB *task_dequeue(TCB **head) {
  if (!head || !*head)
    return NULL;

  TCB *task = *head;

  if (task->next == task) {
    // Only one node in the queue
    *head = NULL;
  } else {
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
void task_remove(TCB **head, TCB *task) {
  if (!head || !*head || !task)
    return;

  if (task->next == task) {
    // Only one node
    *head = NULL;
  } else {
    task->prev->next = task->next;
    task->next->prev = task->prev;
    if (*head == task) {
      *head = task->next;
    }
  }

  task->next = NULL;
  task->prev = NULL;
  task_count--;
}

// Insert task into queue sorted by priority (higher number = higher priority)
void task_insert_sorted(TCB **head, TCB *task) {
  if (!head || !task)
    return;

  if (*head == NULL) {
    task->next = task->prev = task;
    *head = task;
    task_count = 1;
    return;
  }

  TCB *curr = *head;
  do {
    if (task->priority > curr->priority)
      break;
    curr = curr->next;
  } while (curr != *head);

  if (curr == *head && task->priority <= (*head)->prev->priority) {
    // Insert at end
    TCB *tail = (*head)->prev;
    tail->next = task;
    task->prev = tail;
    task->next = *head;
    (*head)->prev = task;
  } else if (curr == *head) {
    // Insert before head
    TCB *tail = (*head)->prev;
    task->next = *head;
    task->prev = tail;
    tail->next = task;
    (*head)->prev = task;
    *head = task;
  } else {
    // Insert before curr
    TCB *prev = curr->prev;
    prev->next = task;
    task->prev = prev;
    task->next = curr;
    curr->prev = task;
  }

  task_count++;
}

// ---------------------- Scheduler Helpers ----------------------

// Return next READY task from ready_queue
TCB *task_get_next_ready(void) {
  if (!ready_queue)
    return NULL;

  TCB *curr = ready_queue;
  do {
    if (curr->state == TASK_READY)
      return curr;
    curr = curr->next;
  } while (curr != ready_queue);

  return NULL;
}

// Move task from ready_queue to blocked_queue
void task_block(TCB *task) {
  if (!task)
    return;
  task_remove(&ready_queue, task);
  task->state = TASK_BLOCKED;
  task_enqueue(&blocked_queue, task);
}

// Move task from blocked_queue to ready_queue
void task_unblock(TCB *task) {
  if (!task)
    return;
  task_remove(&blocked_queue, task);
  task->state = TASK_READY;
  task_enqueue(&ready_queue, task);
}

// Move task to sleep_queue with delay_ticks
void task_sleep(TCB *task, uint32_t ticks) {
  if (!task)
    return;
  task_remove(&ready_queue, task);
  task->state = TASK_SLEEPING;
  task->delay_ticks = ticks;
  task_insert_sorted(&sleep_queue, task); // sorted by priority or wake-up
}

