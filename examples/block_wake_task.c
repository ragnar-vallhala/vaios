#include "navhal.h"
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
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  {
    hal_timer_init_freq(TIM5, 1000);
    hal_timer_attach_callback(TIM5, callback);
    hal_timer_enable_interrupt(TIM5);
    hal_timer_start(TIM5);
  }
  scheduler_init();
  task_id = task_create(task, NULL, 1024, 1);
  scheduler_start();
  while (1)
    ;
}