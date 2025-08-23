#ifndef VAIOS_STRUCTURE_H
#define VAIOS_STRUCTURE_H

#include <stdint.h>

typedef enum {
  TASK_READY,
  TASK_RUNNING,
  TASK_BLOCKED,
  TASK_SLEEPING
} Task_State;

typedef struct TaskControlBlock {
  int task_id;           // Unique task identifier
  void (*entry)(void *); // Task entry function
  void *arg;             // Function argument
  uint32_t *stack_ptr;   // Saved stack pointer (for context switch)
  uint32_t *stack_base;  // Base of task's stack memory
  uint32_t stack_size;   // Stack size (bytes)
  Task_State state;      // Current state
  int priority;          // Priority (for priority-based schedulers)
  uint32_t delay_ticks;  // For sleep/delay handling

  struct TaskControlBlock *next; // Pointer to next task (for queue/list)
  struct TaskControlBlock *prev; // Pointer to prev task (for queue/list)
} TCB;
#endif // !VAIOS_STRUCTURE_H
