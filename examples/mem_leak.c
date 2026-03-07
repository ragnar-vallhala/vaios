#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

void leaking_task(void *args) {
  v_log(LOG_INFO, "Leaking task started");
  uint32_t count = 0;
  while (1) {
    void *ptr = v_malloc(1024); // Allocate 1KB
    // if (ptr == NULL) {
    //   v_log(LOG_ERROR, "Allocation failed at count %d", count);
    //   // Now try to create a new task to trigger the PANIC in task_create
    //   v_log(LOG_INFO, "Attempting to create a task to trigger PANIC...");
    //   task_create(leaking_task, NULL, 512, 1);

    //   // If we reach here, something is wrong
    //   while (1)
    //     ;
    // }
    count++;
    if (count % 8 == 0) {
      v_log(LOG_INFO, "Allocated %d KB", count);
    }
    v_delay(50);
  }
}

int main() {
  v_init();
  v_heap_memory_init();
  scheduler_init();

  v_log(LOG_INFO, "Starting Memory Leak Example");
  v_log(LOG_INFO, "Heap Size: %u bytes", v_get_heap_size());

  task_create(leaking_task, NULL, 1024, 1);

  scheduler_start();

  while (1)
    ;
}
