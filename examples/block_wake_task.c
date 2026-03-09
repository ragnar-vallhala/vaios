#include "core/cortex-m4/timer.h"
#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

uint32_t task_id = 0;
uint32_t count = 0;
void task(void *args) {
  while (1) {
    count++;
    if (count % 100 == 0)
      v_log(LOG_WARN, "%d", (v_get_ticks() - count));
    task_block();
  }
}

void callback() {
  if (task_id != 0)
    task_unblock(task_id);
}

int main() {
  v_init();
  v_heap_memory_init();
  {
    timer_init_freq(TIM5, 1000);
    timer_attach_callback(TIM5, callback);
    timer_enable_interrupt(TIM5);
    timer_start(TIM5);
  }
  scheduler_init();
  task_id = task_create(task, NULL, 1024, 1);
  scheduler_start();
  while (1)
    ;
}