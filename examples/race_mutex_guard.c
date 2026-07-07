/*
 * race_mutex_guard.c — multitask race regression demo (QEMU, real preemption).
 *
 * Companion to main_race_cond.c, which shows the BUG: two equal-priority tasks
 * doing an unguarded `count++` lose updates whenever the preemptive scheduler
 * switches between the load and the store — the classic read-modify-write race.
 * It is the same shape as the races found in the SDIO file-transfer stack: a
 * non-atomic sequence interrupted by an asynchronous event (a task preemption,
 * or an IRQ) corrupting shared state.
 *
 * This example shows the FIX and self-checks it under real preemption: the two
 * workers each increment a shared counter ITERS times UNDER A MUTEX. The mutex
 * must serialize every read-modify-write, so the final count is EXACTLY
 * WORKERS*ITERS. If the mutex or scheduler regresses (e.g. a leaked critical
 * section on recursive unlock — a real bug this kernel has fixed before), updates
 * are lost and the count comes up short -> FAIL.
 *
 * vaios CI runs only the host suite, so this lives as a QEMU example rather than
 * a CI gate. Run:
 *     VAIOS_EXAMPLE=RACE_MUTEX_GUARD bash tools/qemu_no_hal.sh
 * and look for "[race_guard] PASS".
 */
#include "ipc.h"
#include "memory.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>
#include <task.h>

#define ITERS 50000u
#define WORKERS 2u

static volatile uint32_t counter;
static volatile uint32_t done[WORKERS];
static MutexHandle_t mtx;

static void worker(void *arg) {
  uint32_t id = (uint32_t)(uintptr_t)arg;
  for (uint32_t i = 0; i < ITERS; i++) {
    v_mutex_lock(mtx, V_WAIT_FOREVER);
    uint32_t t = counter; /* read-modify-write — the lock makes it atomic versus */
    counter = t + 1u;     /* the other worker even if preemption lands in between */
    v_mutex_unlock(mtx);
  }
  done[id] = 1u;
  for (;;)
    v_delay(1000); /* finished: idle so task lifecycle stays trivial */
}

static void kernel_task(void *arg) {
  (void)arg;
  mtx = v_mutex_create();
  counter = 0u;
  for (uint32_t i = 0; i < WORKERS; i++)
    done[i] = 0u;

  task_create(worker, (void *)(uintptr_t)0u, 2048, 1);
  task_create(worker, (void *)(uintptr_t)1u, 2048, 1);

  for (;;) { /* wait for both workers, yielding so they (prio 1) can run */
    uint32_t all = 1u;
    for (uint32_t i = 0; i < WORKERS; i++)
      all &= done[i];
    if (all)
      break;
    v_delay(5);
  }

  uint32_t expect = WORKERS * ITERS;
  if (counter == expect)
    v_log(LOG_INFO,
          "[race_guard] PASS: %u workers x %u increments under mutex, count=%u",
          WORKERS, ITERS, counter);
  else
    v_log(LOG_ERROR,
          "[race_guard] FAIL: lost updates under mutex! count=%u expected=%u",
          counter, expect);
  v_log_flush();

  for (;;)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();
  task_create(kernel_task, NULL, 2048, 0);
  scheduler_start();
  while (1)
    ;
}
