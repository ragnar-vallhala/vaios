#include "memory.h"
#include "port.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>
#include <task.h>

int count = 0;

void task1_func(void *arg)
{
  for (int i = 0; i < 100000; i++)
  {
    count++;
  }
  v_log(LOG_INFO, "Task %d completed. Count: %d", get_current_task()->task_id,
        count);
}

void kernel_task(void *arg)
{
  uint32_t t1 = task_create(task1_func, NULL, 512, 1);
  uint32_t t2 = task_create(task1_func, NULL, 512, 1);

  v_log(LOG_DEBUG, "Count: %d", count);

  while (1)
  {
  }
}

int main(void)
{

  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();
  count = 0;
  task_create(kernel_task, NULL, 1024, 0);

  scheduler_start();
  while (1)
    ;
}
