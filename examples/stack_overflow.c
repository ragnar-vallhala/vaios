#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

// Deep recursion to consume stack
void recursive_function(int depth) {
  uint32_t large_array[32]; // Consume some stack in each call
  for (int i = 0; i < 32; i++)
    large_array[i] = (uint32_t)depth;

  v_log(LOG_INFO, "Recursion depth: %d", depth);
  recursive_function(depth + 1);
}

void stack_overflow_task(void *args) {
  (void)args;
  v_log(LOG_INFO, "Stack overflow task started");
  v_delay(100);
  recursive_function(1);
}

int main() {
  v_init();
  v_heap_memory_init();
  scheduler_init();

  v_log(LOG_INFO, "Starting Stack Overflow Example");

  // Create a task with enough stack to see some calls, but still overflow
  task_create(stack_overflow_task, NULL, 4096, 1);

  scheduler_start();

  while (1)
    ;
}
