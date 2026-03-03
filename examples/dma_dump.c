#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

void dma_dump(void *args) {
  while (1) {
    int x = 100;
    while (x--)
      v_log(LOG_ERROR, "Running dump");
    v_delay(20);
  }
}

void heavy_math_operation(void *args) {
  int num_op = 0;
  while (1) {
    if (num_op % 100 == 0) {
      v_log(LOG_WARN, "Math op count %d, in time %d", num_op, v_get_ticks());
    }
    num_op++;
  }
}
void monitor_task(void *args) {
  uint32_t *task_id = (uint32_t *)args;
  uint32_t task_id_1 = task_id[0];
  uint32_t task_id_2 = task_id[1];

  while (1) {
    if (v_get_ticks() >= 10000) {
      task_exit_request(task_id_1);
      task_exit_request(task_id_2);
      v_log(LOG_INFO, "Exiting tasks");
      break; // Exit the monitor task itself after requesting exit
    }
    v_log(LOG_INFO, "Monitor Task ticks: %d", v_get_ticks());
    v_delay(500); // Check every 500ms
  }
}
int main() {
  v_init();
  v_heap_memory_init();
  scheduler_init();
  v_log(LOG_ERROR, "Running OS");
  uint32_t t1 = task_create(dma_dump, NULL, 1024, 0);
  uint32_t t2 = task_create(heavy_math_operation, NULL, 1024, 0);
  static uint32_t task_ids[2];
  task_ids[0] = t1;
  task_ids[1] = t2;
  task_create(monitor_task, task_ids, 1024, 0);
  scheduler_start();

  while (1)
    ;
}