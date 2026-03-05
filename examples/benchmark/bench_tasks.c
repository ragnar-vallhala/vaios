/**
 * @file bench_tasks.c
 * @brief Task / scheduler benchmark suite for VAiOS
 *
 * Sub-benchmarks:
 *  BM_TASK_CTX_SWITCH     - Context-switch rate (yielding round-robin)
 *  BM_TASK_PRIORITY       - Priority preemption: high-priority task must
 *                           complete before low-priority task records progress
 *  BM_TASK_DELAY_ACCURACY - v_delay(N) measured against v_get_ticks():
 *                           checks the delay does not deviate by more than 5 %
 */

#include "benchmark.h"

/* ----------------------------- tunables --------------------------------- */
#define CTX_SWITCH_WINDOW_MS 200u /* measure switches for 200 ms        */
#define CTX_SWITCH_TASKS 4u       /* number of round-robin tasks        */
#define DELAY_TEST_MS 100u        /* delay amount to test accuracy      */
#define DELAY_TOLERANCE_PCT 5u    /* acceptable deviation in %          */

/* ======================================================================= */
/* BM_TASK_CTX_SWITCH                                                       */
/* ======================================================================= */

static volatile uint32_t s_switch_count;
static volatile uint8_t s_switch_stop;

static void ctx_switch_task(void *arg) {
  (void)arg;
  while (!s_switch_stop) {
    s_switch_count++;
    task_yield();
  }
}

void bench_task_ctx_switch(void) {
  bench_result_t *r = &g_results[BM_TASK_CTX_SWITCH];
  r->name = "TASK: context switch rate";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  s_switch_count = 0;
  s_switch_stop = 0;

  /* Create N equal-priority tasks that yield in a tight loop */
  for (uint32_t i = 0; i < CTX_SWITCH_TASKS; i++) {
    /* Create at priority 0 so they don't starve the benchmark runner task */
    task_create(ctx_switch_task, NULL, 512, 0);
  }

  uint32_t t0 = BENCH_START();
  /* Let them run for the measurement window */
  v_delay(CTX_SWITCH_WINDOW_MS);
  s_switch_stop = 1;
  /* One more delay to let tasks see the flag and exit cleanly */
  v_delay(10);
  uint32_t dt = BENCH_ELAPSED(t0);

  r->duration_ticks = dt;
  r->ops = s_switch_count;
  r->ops_per_sec = BENCH_OPS_PER_SEC(s_switch_count, dt);
  r->status = (s_switch_count > 0) ? BENCH_PASS : BENCH_FAIL;

  BENCH_LOG(LOG_INFO, "Context switches: %u in %u ms => %u sw/s",
            s_switch_count, dt, r->ops_per_sec);
}

/* ======================================================================= */
/* BM_TASK_PRIORITY                                                         */
/* ======================================================================= */

/*
 * Design:
 *  - LOW task counts s_low_iters in a loop with task_yield()
 *  - HIGH task increments s_high_done once, then waits on a semaphore
 *  - After HIGH finishes, check that s_low_iters is still 0 or very small
 *    (demonstrating pre-emption by the higher-priority task)
 */
static volatile uint32_t s_low_iters;
static volatile uint8_t s_high_done;
static SemaphoreHandle_t s_prio_sem;

static void prio_low_task(void *arg) {
  (void)arg;
  while (!s_high_done) {
    s_low_iters++;
    task_yield();
  }
  v_semaphore_give(s_prio_sem);
}

static void prio_high_task(void *arg) {
  (void)arg;
  /* Burn some CPU intentionally — low task should be starved */
  volatile uint32_t busy = 0;
  for (uint32_t i = 0; i < 200000u; i++)
    busy++;
  s_high_done = 1;
  v_semaphore_give(s_prio_sem);
}

void bench_task_priority(void) {
  bench_result_t *r = &g_results[BM_TASK_PRIORITY];
  r->name = "TASK: priority preemption";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  s_low_iters = 0;
  s_high_done = 0;
  s_prio_sem = v_semaphore_create_counting(2, 0);

  if (!s_prio_sem) {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Failed to allocate semaphore");
    return;
  }

  uint32_t t0 = BENCH_START();

  /* LOW priority = 1, HIGH priority = 5 */
  task_create(prio_low_task, NULL, 512, 1);
  task_create(prio_high_task, NULL, 512, 5);

  /* Wait for both to finish */
  v_semaphore_take(s_prio_sem, 5000);
  v_semaphore_take(s_prio_sem, 5000);
  uint32_t dt = BENCH_ELAPSED(t0);

  r->duration_ticks = dt;
  r->ops = s_low_iters;
  r->detail = s_high_done;

  /*
   * PASS criterion: the low-priority task should have run very little
   * while the high-priority task was burning CPU.
   * We accept < 100 low-task iterations as evidence of correct preemption.
   */
  if (s_high_done && s_low_iters < 100u) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO,
              "Priority preemption OK: low_iters=%u high_done=%d in %u ticks",
              s_low_iters, s_high_done, dt);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Priority preemption FAIL: low_iters=%u high_done=%d",
              s_low_iters, s_high_done);
  }
}

/* ======================================================================= */
/* BM_TASK_DELAY_ACCURACY                                                   */
/* ======================================================================= */

void bench_task_delay_accuracy(void) {
  bench_result_t *r = &g_results[BM_TASK_DELAY_ACCURACY];
  r->name = "TASK: delay accuracy";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  uint32_t t0 = BENCH_START();
  v_delay(DELAY_TEST_MS);
  uint32_t dt = BENCH_ELAPSED(t0);

  /* Compute deviation in percent */
  uint32_t expected = DELAY_TEST_MS;
  uint32_t diff = (dt > expected) ? (dt - expected) : (expected - dt);
  uint32_t pct = (diff * 100u) / expected;

  r->duration_ticks = dt;
  r->ops = 1;
  r->detail = pct; /* deviation % */

  if (pct <= DELAY_TOLERANCE_PCT) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO, "Delay accuracy: requested %u ms, got %u ms (err=%u%%)",
              expected, dt, pct);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR,
              "Delay accuracy FAIL: requested %u ms, got %u ms (err=%u%%)",
              expected, dt, pct);
  }
}

/* ======================================================================= */
/* Entry point                                                               */
/* ======================================================================= */
void bench_tasks_run(void) {
  BENCH_LOG(LOG_INFO, "========== Task Benchmarks ==========");
  bench_task_ctx_switch();
  bench_task_priority();
  bench_task_delay_accuracy();
  BENCH_LOG(LOG_INFO, "=====================================");
}
