/*
 * Host demo — runs the vaios scheduler natively on the host (VAIOS_PORT=host).
 * Build & run:
 *   cmake -S . -B build_host -DVAIOS_PORT=host -DNAVHAL=OFF \
 *         -DEXAMPLES=ON -DVAIOS_EXAMPLE=HOST_DEMO
 *   cmake --build build_host && ./build_host/examples/main
 *
 * Two equal-priority tasks each do a burst of CPU work (only the SIGALRM tick can
 * preempt that) and then sleep with v_delay (the tick wakes them). Their output
 * interleaves — proof of real preemptive context switching on the host.
 */
#include "port.h" // ENTER_CRITICAL / EXIT_CRITICAL
#include "task.h"
#include "vaios.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// printf is not reentrant, and under ucontext preemption every task shares one
// OS thread, so a tick landing mid-printf could let the other task re-enter it.
// A critical section masks the tick for the print — the RTOS way to guard a
// shared, non-reentrant resource. burn_cpu() stays outside it, so preemption is
// still on show.
#define ATOMIC_PRINT(...)                                                      \
  do {                                                                         \
    ENTER_CRITICAL();                                                          \
    printf(__VA_ARGS__);                                                       \
    fflush(stdout);                                                            \
    EXIT_CRITICAL();                                                           \
  } while (0)

static volatile int g_done;

static void burn_cpu(unsigned long n) {
  volatile unsigned long x = 0;
  while (n--)
    x += n;
}

static void worker(void *arg) {
  long id = (long)arg;
  for (int i = 0; i < 6; i++) {
    ATOMIC_PRINT("task %ld: iter %d  (tick=%u)\n", id, i, v_get_ticks());
    burn_cpu(30000000UL); // CPU-bound: preemptible only by the tick
    v_delay(30);          // sleep: the tick wakes us, the other task runs
  }
  ATOMIC_PRINT("task %ld: DONE (tick=%u)\n", id, v_get_ticks());
  if (++g_done == 2) {
    ATOMIC_PRINT("both tasks finished — exiting\n");
    exit(0);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();
  task_create(worker, (void *)0, 64 * 1024, 1);
  task_create(worker, (void *)1, 64 * 1024, 1);
  printf("starting scheduler...\n");
  fflush(stdout);
  scheduler_start();
  return 0;
}
