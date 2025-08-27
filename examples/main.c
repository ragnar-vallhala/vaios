#include "task.h"
#include "structures.h"
#include "memory.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

void task_fn1(void *arg)
{
  while (1)
    v_log(LOG_INFO, "Task 1 running");
}
void task_fn2(void *arg)
{
  while (1)
    v_log(LOG_INFO, "Task 2 running");
}
void task_fn3(void *arg)
{
  while (1)
    v_log(LOG_INFO, "Task 3 running");
}

// Main
int main(void)
{
  v_init();
  heap_memory_init();
  v_task_create(task_fn1, NULL, 1, 128);
  v_task_create(task_fn2, NULL, 1, 128);
  v_task_create(task_fn3, NULL, 1, 128);
  v_start();
  while (1)
    ;

  return 0;
}
