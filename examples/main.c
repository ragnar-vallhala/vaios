#include "memory.h"
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

  for (int i = 0;; i++) {
    // v_log(LOG_INFO, "Task %d Entered. Count: %d", get_current_task()->task_id,
          // count);
    count++;
    task_delay(10);
  }
  v_log(LOG_INFO, "Task %d completed. Count: %d", get_current_task()->task_id,
        count);
  // return;
}

extern TCB *idle_task;
void kernel_task(void *arg) {
  uint32_t t1 = task_create(task1_func, NULL, 512, 0);
  uint32_t t2 = task_create(task1_func, NULL, 512, 0);

  v_log(LOG_DEBUG, "Count: %d", count);

  while (1) {
    v_log(LOG_DEBUG, "Idle Task Running. CPU_Usage %d, %d/%d",
          ((v_get_ticks() - idle_task->ticks_run + 1) * 100) / (v_get_ticks()),
          idle_task->ticks_run, v_get_ticks());
  }
}

int main(void) {

  v_init();
  heap_memory_init();
  scheduler_init();
  count = 0;

  task_create(kernel_task, NULL, 1024, 0);

  scheduler_start();
  while (1)
    ;
}
