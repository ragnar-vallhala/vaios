#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
// Dummy tasks
void task1(void *arg) { v_log(LOG_INFO, "Task 1 running"); }
void task2(void *arg) { v_log(LOG_INFO, "Task 2 running"); }
void task3(void *arg) { v_log(LOG_INFO, "Task 3 running"); }
// task.h
extern TCB *ready_queue ;
extern TCB *blocked_queue;
extern TCB *sleep_queue;

int main()
{

  v_init();
  v_log(LOG_INFO, "Starting task queue test...");

  // Create dummy TCBs
  TCB t1 = {.task_id = 1, .entry = task1, .priority = 5, .state = TASK_READY};
  TCB t2 = {.task_id = 2, .entry = task2, .priority = 10, .state = TASK_READY};
  TCB t3 = {.task_id = 3, .entry = task3, .priority = 7, .state = TASK_READY};

  // Enqueue tasks into ready_queue
  task_enqueue(&ready_queue, &t1);
  task_enqueue(&ready_queue, &t2);
  task_enqueue(&ready_queue, &t3);

  v_log(LOG_INFO, "Enqueued 3 tasks into ready_queue");

  // Print ready_queue
  TCB *curr = ready_queue;
  if (curr)
  {
    do
    {
      v_log(LOG_INFO, "Task ID: %d, Priority: %d, State: %d", curr->task_id,
            curr->priority, curr->state);
      curr = curr->next;
    } while (curr != ready_queue);
  }

  // Dequeue a task and run it
  TCB *next_task = task_dequeue(&ready_queue);
  if (next_task)
  {
    v_log(LOG_INFO, "Dequeued task ID: %d", next_task->task_id);
    next_task->entry(NULL); // call the task function
  }

  // Print ready_queue after dequeue
  v_log(LOG_INFO, "Ready queue after dequeue:");
  curr = ready_queue;
  if (curr)
  {
    do
    {
      v_log(LOG_INFO, "Task ID: %d, Priority: %d, State: %d", curr->task_id,
            curr->priority, curr->state);
      curr = curr->next;
    } while (curr != ready_queue);
  }

  // Loop forever
  while (1)
    ;
}
