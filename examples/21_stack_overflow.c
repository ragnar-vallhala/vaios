#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

// Deep recursion to consume stack until it hits the MPU stack guard.
//
// Two subtleties defeat the optimizer, which would otherwise stop this from ever
// overflowing at -O2:
//   * `volatile` on the frame array so the 128-byte store isn't a dead-store the
//     compiler elides (an unused local costs no stack).
//   * a use of the array AFTER the recursive call, so the frame must stay live
//     across it — that turns the tail call into a real one (no tail-call/sibling
//     optimization), so each level actually pushes a new frame and the stack
//     grows. Without this, GCC rewrites the tail recursion into a loop that
//     never grows the stack and never trips the guard.
__attribute__((noinline)) void recursive_function(int depth) {
  volatile uint32_t large_array[32]; // consume ~128 B of stack per call
  for (int i = 0; i < 32; i++)
    large_array[i] = (uint32_t)depth;

  v_log(LOG_INFO, "Recursion depth: %d", depth);
  recursive_function(depth + 1);

  // Never reached (we fault first), but referencing the frame here keeps it live
  // across the call above, which is what prevents the tail-call optimization.
  for (int i = 0; i < 32; i++)
    if (large_array[i] != (uint32_t)depth)
      v_log(LOG_ERROR, "frame clobbered at %d", i);
}

void stack_overflow_task(void *args) {
  (void)args;
  v_log(LOG_INFO, "Stack overflow task started");
  v_delay(100);
  recursive_function(1);
}

int main() {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  v_log(LOG_INFO, "Starting Stack Overflow Example");

  // Create a task with enough stack to see some calls, but still overflow
  task_create(stack_overflow_task, NULL, 4096, 1);

  scheduler_start();

  while (1)
    ;
}
