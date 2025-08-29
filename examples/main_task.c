#include "memory.h"
#include "port.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>
#include <task.h>

int count = 0;

// Example main function
void delay(volatile uint32_t count) {
  while (count--)
    ;
}

void task1_func(void *arg) {
  while (1) {
    ENTER_CRITICAL();
    count++;
    v_log(LOG_WARN, "Running %d, %d", GET_CURRENT_TASK_ID(),count);
    // Example work: toggle LED or simulate
    // yield CPU
    task_yield(); // optional: can call task_yield()
    // delay(1000000);
    EXIT_CRITICAL();
  }
}

int main(void) {

  v_init();
  heap_memory_init();
  scheduler_init();
  count = 0;
  uint32_t t1 = task_create(task1_func, NULL, 512, 1);
  uint32_t t2 = task_create(task1_func, NULL, 512, 1);

  // uint32_t t3 = task_create(task3_func, NULL, 512, 2);
  // uint32_t t4 = task_create(task4_func, NULL, 512, 2);
  //
  // uint32_t t5 = task_create(task5_func, NULL, 512, 3);
  // uint32_t t6 = task_create(task6_func, NULL, 512, 3);
  scheduler_start();
  while (1)
    ;
}
