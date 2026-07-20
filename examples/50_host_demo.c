/*
 * Host demo — runs the vaios scheduler natively on the host (VAIOS_PORT=host).
 * Build & run:
 *   tools/run_host.sh            (or VAIOS_EXAMPLE=HOST_DEMO through CMake)
 *
 * Two equal-priority tasks each do a burst of CPU work (only the SIGALRM tick can
 * preempt that) and then sleep with v_delay (the tick wakes them). Their output
 * interleaves — proof of real preemptive context switching on the host.
 *
 * Output goes through vaios's own logger (v_log), which routes to the port's
 * console (v_port_hw_console_* -> stdio here) and serializes with a critical
 * section — so it is preemption-safe, unlike a raw printf from a task.
 */
#include "memory.h" // v_heap_memory_init
#include "task.h"
#include "utils.h"   // v_log
#include "vaios.h"
#include <stdint.h>
#include <stdlib.h> // exit

static volatile int g_done;

static void burn_cpu(unsigned long n) {
  volatile unsigned long x = 0;
  while (n--)
    x += n;
}

static void worker(void *arg) {
  int id = (int)(long)arg;
  for (int i = 0; i < 6; i++) {
    v_log(LOG_INFO, "task %d: iter %d (tick=%u)", id, i, v_get_ticks());
    burn_cpu(30000000UL); // CPU-bound: preemptible only by the tick
    v_delay(30);          // sleep: the tick wakes us, the other task runs
  }
  v_log(LOG_INFO, "task %d: DONE (tick=%u)", id, v_get_ticks());
  if (++g_done == 2) {
    v_log(LOG_INFO, "both tasks finished - exiting");
    v_log_flush(); // drain the log buffer before we tear the process down
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
  v_log(LOG_INFO, "starting scheduler...");
  scheduler_start();
  return 0;
}
