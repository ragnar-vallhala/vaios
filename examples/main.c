#include "memory.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>
#include <task.h>

int count = 0;

// Example main function
void delay(volatile uint32_t count)
{
  while (count--)
    ;
}

void task1_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 1, %d", count);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
    delay(1000000);
  }
}
void task2_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 2, %d", count);
    delay(1000000);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
  }
}

void task3_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 3, %d", count);
    delay(1000000);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
  }
}

void task4_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 4, %d", count);
    delay(1000000);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
  }
}

void task5_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 5, %d", count);
    delay(1000000);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
  }
}

void task6_func(void *arg)
{
  while (1)
  {
    count++;
    v_log(LOG_WARN, "Running 6, %d", count);
    delay(1000000);
    // Example work: toggle LED or simulate
    // yield CPU
    // task_yield(); // optional: can call task_yield()
  }
}

int main(void)
{

  v_init();
  heap_memory_init();
  scheduler_init();
  count = 0;
  uint32_t t1 = task_create(task1_func, NULL, 512, 1);
  uint32_t t2 = task_create(task2_func, NULL, 512, 1);

  uint32_t t3 = task_create(task3_func, NULL, 512, 1);
  uint32_t t4 = task_create(task4_func, NULL, 512, 1);

  uint32_t t5 = task_create(task5_func, NULL, 512, 1);
  uint32_t t6 = task_create(task6_func, NULL, 512, 1);
  scheduler_start();
  while (1)
    ;
}
