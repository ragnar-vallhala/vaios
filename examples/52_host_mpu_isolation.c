/*
 * Host per-task region isolation demo (VAIOS_PORT=host, VAIOS_MPU_USER_SEPARATION).
 *
 * The software MPU gives each task an RW region over only its own stack: while a
 * task runs, every OTHER task's stack is PROT_NONE. Task B publishes the address
 * of one of its own stack variables; task A then reaches in and writes it. Because
 * B is suspended (its stack closed) when A runs, A's write is a cross-task
 * violation and faults — exactly as the MPU would trap it on target. The run ends
 * with a kernel panic ("MPU fault in task N | memory access"), which is the
 * PASS condition. Build & run with tools/run_host.sh HOST_MPU_ISOLATION.
 */
#include "memory.h" // v_heap_memory_init
#include "task.h"
#include "utils.h" // v_log
#include "vaios.h"
#include <stdint.h>
#include <stdlib.h> // exit

static volatile uintptr_t g_b_stack_var; // address of a variable on B's stack

static void task_b(void *arg) {
  (void)arg;
  volatile int secret = 0x1234;
  g_b_stack_var = (uintptr_t)&secret;
  v_log(LOG_INFO, "B: my stack variable is at 0x%x", (unsigned)g_b_stack_var);
  for (;;)
    v_delay(20); // suspend, so A runs while B's stack is PROT_NONE
}

static void task_a(void *arg) {
  (void)arg;
  while (!g_b_stack_var)
    v_delay(2); // wait until B has published
  v_delay(10);  // let B settle into its delay (suspended, its stack closed)
  v_log(LOG_INFO, "A: writing into B's stack at 0x%x (expect MPU fault)",
        (unsigned)g_b_stack_var);
  *(volatile int *)g_b_stack_var = 0xDEAD; // cross-task write -> region violation
  v_log(LOG_ERROR, "A: NO fault -- isolation FAILED");
  exit(2);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();
  task_create(task_a, NULL, 4096, 2);
  task_create(task_b, NULL, 4096, 2);
  v_log(LOG_INFO, "MPU isolation demo: A will try to reach into B's stack");
  scheduler_start();
  return 0;
}
