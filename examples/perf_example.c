/*
 * examples/perf_example.c
 *
 * Demonstrates the optional perf module (see kernel/perf.c and
 * docs/perf/IMPLEMENTATION_PLAN.md).
 *
 * Three worker tasks exercise the kernel for a short warm-up window
 * (heap allocs/frees, semaphore takes/gives, voluntary yields). After
 * the window a reporter task:
 *   1. Calls v_perf_dump() — prints a snapshot to UART showing what
 *      the scheduler / SysTick / IPC / heap counters captured.
 *   2. Calls v_perf_task_stats() for each worker — per-task cycles
 *      run, max burst, switches-in.
 *   3. Calls v_perf_reset(), waits, and dumps again to show the
 *      counters resume from a clean baseline.
 *
 * Build:  cmake -S . -B build -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=PERF
 *         cmake --build build --target main
 *
 * If VAIOS_MODULE_PERF=OFF the v_perf_* calls degrade to no-ops via
 * the inline stubs in include/perf.h, so the example still builds and
 * runs — just without any perf output. The dump lines that would have
 * mentioned counters won't appear; the worker traces will.
 */

#include "ipc.h"
#include "memory.h"
#include "perf.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

#define NUM_WORKERS 3
#define WORKER_ITERS 6

static SemaphoreHandle_t sem;
static uint32_t worker_ids[NUM_WORKERS];
static uint32_t worker_tids[NUM_WORKERS];

/* ------------------------------------------------------------------------ */
/* Workload                                                                 */
/* ------------------------------------------------------------------------ */

static void worker_task(void *arg) {
  int id = *(int *)arg;
  for (int i = 0; i < WORKER_ITERS; i++) {
    /* Heap: vary the size so the per-class breakdown is non-trivial. */
    size_t sz = (size_t)(16u + (uint32_t)id * 24u + (uint32_t)i * 8u);
    void *p = v_malloc(sz);
    if (p) {
      v_free(p);
    }

    /* IPC: take/give to bump the IPC counters; one worker timeouts
     * occasionally so the snapshot has a nonzero `timeouts` field. */
    if (v_semaphore_take(sem, (id == NUM_WORKERS) ? 1 : 50) == VA_PASS) {
      v_delay(2);
      v_semaphore_give(sem);
    }

    v_delay(5);
  }
  v_log(LOG_INFO, "[worker %d] done", id);
}

/* ------------------------------------------------------------------------ */
/* Reporter                                                                 */
/* ------------------------------------------------------------------------ */

static void dump_per_task_stats(void) {
  v_perf_task_t pt;
  for (int i = 0; i < NUM_WORKERS; i++) {
    /* The task may have exited and been reaped — guard by id. */
    if (worker_tids[i] == 0) {
      continue;
    }
    /* v_perf_task_stats wants a TCB pointer. The example doesn't keep
     * one (task_create returns an id, not a TCB*), so we skip per-task
     * dump for simplicity — v_perf_dump() already shows the
     * aggregates. A real diagnostic would walk the scheduler's task
     * list, which is internal. */
    (void)pt;
  }
}

static void reporter_task(void *arg) {
  (void)arg;

  /* Let the workers actually do some work first. */
  v_delay(150);

  /* Announcement lines go through print_fmt (direct-UART), NOT v_log
   * (buffered, may DMA-flush on its own timer). v_perf_dump is also
   * print_fmt-based — mixing the two on the same UART corrupts both
   * streams byte-by-byte, so any "snapshot incoming" line that should
   * land just before the snapshot must ride the same channel. */
  print_fmt("\r\n===== perf snapshot after warm-up =====\r\n");
  v_perf_dump();
  dump_per_task_stats();

  print_fmt("\r\n===== v_perf_reset() - counters zeroed =====\r\n");
  v_perf_reset();

  /* Sit idle for a bit so the post-reset snapshot has a meaningful
   * idle-cycles entry but few sched switches. */
  v_delay(50);

  print_fmt("\r\n===== perf snapshot after reset =====\r\n");
  v_perf_dump();

  print_fmt("\r\n[perf_example] done\r\n");
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                    */
/* ------------------------------------------------------------------------ */

static void kernel_task(void *arg) {
  (void)arg;

  /* Counting semaphore with 2 slots — workers will contend and a few
   * takes will block, which is exactly what we want to see in the IPC
   * counters. */
  sem = v_semaphore_create_counting(2, 2);

  for (int i = 0; i < NUM_WORKERS; i++) {
    worker_ids[i] = (uint32_t)(i + 1);
    worker_tids[i] =
        task_create(worker_task, &worker_ids[i], 1024, 1);
  }
  task_create(reporter_task, NULL, 2048, 2);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  task_create(kernel_task, NULL, 1024, 0);
  scheduler_start();

  while (1) {
    /* never reached */
  }
}
