/**
 * @file 98_coverage_sched.c
 * @brief Scheduler-exercising on-target coverage runner (-DVAIOS_GCOV=ON).
 *
 * The no-scheduler unit runner (99_unit_tests.c) covers port_hw.c bring-up but
 * leaves port.c at 0% — all of port.c is context-switch / SVCall / PendSV /
 * fault machinery that only runs under a live scheduler, and syscall.c's
 * dispatch is never reached without SVC syscalls. This runner fills that gap: it
 * starts the scheduler with worker tasks that drive real kernel entry through
 * SVC (yield, delay, semaphore, mutex, task exit), so PendSV_Handler,
 * SVCall_Handler, v_syscall_dispatch, init_task_stack and scheduler_start all
 * execute and register coverage. The last worker to finish dumps every TU's
 * .gcda over the UART (see portable/cortex-m4/gcov_dump.c).
 *
 * Built under VAIOS_MPU_ENABLE + VAIOS_SYSCALL_SVC (privileged tasks — the
 * unprivileged-flip enforcement is exercised separately by mpu_user_demo /
 * tools/renode_isolation.sh, which faults by design and so cannot dump).
 *
 * RUN VEHICLE: intended for REAL HARDWARE. On silicon the instrumented image runs
 * at full core speed and the UART .gcda stream completes in milliseconds. Under
 * Renode it is impractically slow — Renode advances virtual time in lock-step
 * with executed instructions, and gcov instrumentation plus context-switch
 * traffic multiplies the instruction count enough that reaching the dump takes
 * many minutes of wall clock (which is why the Renode/CI coverage path is the
 * lean no-scheduler UNIT_TESTS runner; this one is the on-hardware complement
 * that reaches port.c's scheduler/SVC machinery).
 *
 * Build:  cmake -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=COVERAGE_SCHED -DVAIOS_GCOV=ON ..
 * Flash, capture the UART, then reconstruct with tools/gcov_uart_decode.py.
 */
#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>
#if defined(VAIOS_GCOV)
#include "gcov_dump.h"
#endif

#include "memory.h"

/* Kept minimal on purpose: coverage is binary (a path is hit or not), and an
 * instrumented image runs slowly enough under Renode that every extra iteration
 * and every tick delay costs real wall-clock. Two workers give worker<->worker
 * context switches; one iteration hits each syscall path once. */
#define NUM_WORKERS 2
#define ITERS 1

static SemaphoreHandle_t g_sem;
static MutexHandle_t g_mtx;
static volatile int g_workers_done;
static int g_counter; /* guarded by g_mtx */

/* The last worker to finish dumps coverage and reports the result. */
static void dump_and_report(void);

/* Each worker drives the SVC syscall surface: counting semaphore take/give,
 * mutex lock/unlock (the guarded increment), a heap round-trip, a cooperative
 * yield, and a short delay (block/wake through SYS_delay) — then exits through
 * the SYS_exit syscall.
 *
 * There is deliberately NO busy-waiting coordinator: a task spinning on
 * task_yield keeps the CPU 100% busy, and Renode advances virtual time in
 * lock-step with executed instructions, so an instrumented spin makes seconds of
 * virtual time cost minutes of wall clock. Instead workers *block* (v_delay /
 * semaphore) — a blocked run queue lets the idle task WFI, which advances virtual
 * time cheaply — and the last worker to complete triggers the dump. */
static void worker(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; ++i) {
    if (v_semaphore_take(g_sem, V_WAIT_FOREVER) == VA_PASS) {
      v_mutex_lock(g_mtx, V_WAIT_FOREVER); /* SVC */
      g_counter++;                         /* protected critical section */
      v_mutex_unlock(g_mtx);               /* SVC */
      v_semaphore_give(g_sem);             /* SVC */
    }
    void *p = malloc(48);
    if (p)
      free(p);
    task_yield(); /* SVC -> PendSV context switch */
  }
  /* One SYS_delay to register task_delay's block/wake path (a single tick — the
   * idle task busy-loops, so blocked ticks are costly wall-clock here). */
  v_delay(1);

  v_mutex_lock(g_mtx, V_WAIT_FOREVER);
  int done = ++g_workers_done;
  v_mutex_unlock(g_mtx);

  if (done == NUM_WORKERS) {
    dump_and_report(); /* last one out: dump before exiting the run queue */
    for (;;) {
    }
  }
  task_exit(); /* SVC SYS_exit; never returns */
}

static void dump_and_report(void) {
  int ok = (g_counter == NUM_WORKERS * ITERS);
#if defined(VAIOS_GCOV)
  vaios_gcov_dump();
#endif
  print_fmt("\r\n==== COVERAGE RUN: counter=%d expected=%d ====\r\n", g_counter,
            NUM_WORKERS * ITERS);
  print_fmt(ok ? "ON-TARGET RESULT: ALL PASS\r\n"
               : "ON-TARGET RESULT: FAIL\r\n");
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();

  print_fmt("\r\n==== vaios ON-TARGET coverage (scheduler) ====\r\n");

  g_sem = v_semaphore_create_counting(NUM_WORKERS, NUM_WORKERS);
  g_mtx = v_mutex_create();

  /* 4 KB stacks: any worker may be the last one out and run the .gcda dump,
   * which needs headroom for __gcov_info_to_gcda's serialisation. */
  for (int i = 0; i < NUM_WORKERS; ++i)
    task_create(worker, NULL, 4096, 1);

  scheduler_start(); /* never returns */
  return 0;
}
