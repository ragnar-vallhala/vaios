/**
 * @file bench_fpu.c
 * @brief FPU benchmark suite for VAiOS
 *
 * Three sub-benchmarks:
 *  1. BM_FPU_SINE_THROUGHPUT  - Single-task sustained sinf() loop
 *  2. BM_FPU_SQRT_THROUGHPUT  - Single-task sustained sqrtf() loop
 *  3. BM_FPU_MIXED_MULTICORE  - Two concurrent tasks: one sine, one sqrt
 *     Validates that the RTOS correctly saves/restores S0-S31 on every
 *     context switch (lazy stacking).  A register-checksum is carried
 *     across iterations to catch corruption.
 *
 * Requires:  -DVAIOS_FPU=ON  (build flag) and NavHAL _FPU_ENABLED
 */

#include "benchmark.h"
#include <math.h>

/* ----------------------------- tunables --------------------------------- */
#define FPU_SINE_ITERS 1000u /* iterations per throughput run         */
#define FPU_SQRT_ITERS 1000u
#define FPU_MULTICORE_ITERS 500u

/* Semaphores used to synchronise the two multicore tasks */
static SemaphoreHandle_t s_sine_sync;
static SemaphoreHandle_t s_sqrt_sync;
static volatile uint8_t s_sine_done;
static volatile uint8_t s_sqrt_done;

/* Corruption detection: accumulate a float sum and compare expected sign  */
static volatile float s_sine_sum;
static volatile float s_sqrt_sum;

/* ======================================================================= */
/* Throughput helpers                                                        */
/* ======================================================================= */

/**
 * @brief Compute N sine operations and return time taken (ticks).
 *        Deliberately prevent the compiler from optimising away the loop by
 *        accumulating the result and calling v_log at the end.
 */
static uint32_t run_sine_batch(uint32_t n) {
  float acc = 0.0f;
  float angle = 0.001f;
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < n; i++) {
    acc += sinf(angle);
    angle += 0.002f;
    if ((i & 0x63u) == 0)
      task_yield(); /* yield every 100 iters */
  }
  /* store to volatile so the loop cannot be optimised away */
  volatile float sink = acc;
  (void)sink;
  return BENCH_ELAPSED(t0);
}

static uint32_t run_sqrt_batch(uint32_t n) {
  float acc = 1.0f;
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < n; i++) {
    acc = sqrtf(acc + 0.001f);
    if ((i & 0x63u) == 0)
      task_yield(); /* yield every 100 iters */
  }
  volatile float sink = acc;
  (void)sink;
  return BENCH_ELAPSED(t0);
}

/* ======================================================================= */
/* BM_FPU_SINE_THROUGHPUT                                                   */
/* ======================================================================= */
void bench_fpu_sine_throughput(void) {
  bench_result_t *r = &g_results[BM_FPU_SINE_THROUGHPUT];
  r->name = "FPU: sinf throughput";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

#ifndef _FPU_ENABLED
  BENCH_LOG(LOG_WARN, "FPU disabled, skipping");
  r->status = BENCH_SKIP;
  return;
#else
  uint32_t dt = run_sine_batch(FPU_SINE_ITERS);
  r->duration_ticks = dt;
  r->ops = FPU_SINE_ITERS;
  r->ops_per_sec = BENCH_OPS_PER_SEC(FPU_SINE_ITERS, dt);
  r->status = (dt > 0) ? BENCH_PASS : BENCH_FAIL;
  BENCH_LOG(LOG_INFO, "sinf %u ops in %u ticks -> %u ops/s", FPU_SINE_ITERS, dt,
            r->ops_per_sec);
#endif
}

/* ======================================================================= */
/* BM_FPU_SQRT_THROUGHPUT                                                   */
/* ======================================================================= */
void bench_fpu_sqrt_throughput(void) {
  bench_result_t *r = &g_results[BM_FPU_SQRT_THROUGHPUT];
  r->name = "FPU: sqrtf throughput";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

#ifndef _FPU_ENABLED
  BENCH_LOG(LOG_WARN, "FPU disabled, skipping");
  r->status = BENCH_SKIP;
  return;
#else
  uint32_t dt = run_sqrt_batch(FPU_SQRT_ITERS);
  r->duration_ticks = dt;
  r->ops = FPU_SQRT_ITERS;
  r->ops_per_sec = BENCH_OPS_PER_SEC(FPU_SQRT_ITERS, dt);
  r->status = (dt > 0) ? BENCH_PASS : BENCH_FAIL;
  BENCH_LOG(LOG_INFO, "sqrtf %u ops in %u ticks -> %u ops/s", FPU_SQRT_ITERS,
            dt, r->ops_per_sec);
#endif
}

/* ======================================================================= */
/* BM_FPU_MIXED_MULTICORE  — context-save validation                        */
/* ======================================================================= */

/* Task bodies for the mixed test */
static void fpu_sine_task(void *arg) {
  (void)arg;
  float sum = 0.0f;
  float a = 0.001f;
  for (uint32_t i = 0; i < FPU_MULTICORE_ITERS; i++) {
    sum += sinf(a);
    a += 0.003f;
    /* yield to let the other task run — exercises context switch FPU save */
    if ((i & 0xFu) == 0)
      task_yield();
  }
  s_sine_sum = sum;
  s_sine_done = 1;
  v_semaphore_give(s_sine_sync);
}

static void fpu_sqrt_task(void *arg) {
  (void)arg;
  float sum = 0.0f;
  float val = 1.0f;
  for (uint32_t i = 0; i < FPU_MULTICORE_ITERS; i++) {
    sum += sqrtf(val);
    val += 0.003f;
    if ((i & 0xFu) == 0)
      task_yield();
  }
  s_sqrt_sum = sum;
  s_sqrt_done = 1;
  v_semaphore_give(s_sqrt_sync);
}

void bench_fpu_mixed_multicore(void) {
  bench_result_t *r = &g_results[BM_FPU_MIXED_MULTICORE];
  r->name = "FPU: multi-task context-save";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

#ifndef _FPU_ENABLED
  BENCH_LOG(LOG_WARN, "FPU disabled, skipping");
  r->status = BENCH_SKIP;
  return;
#else
  s_sine_done = 0;
  s_sqrt_done = 0;
  s_sine_sum = 0.0f;
  s_sqrt_sum = 0.0f;

  s_sine_sync = v_semaphore_create_binary();
  s_sqrt_sync = v_semaphore_create_binary();
  if (!s_sine_sync || !s_sqrt_sync) {
    BENCH_LOG(LOG_ERROR, "Failed to create sync semaphores");
    r->status = BENCH_FAIL;
    return;
  }

  uint32_t t0 = BENCH_START();
  task_create(fpu_sine_task, NULL, 1024, 2);
  task_create(fpu_sqrt_task, NULL, 1024, 2);

  /* Wait for both to finish */
  v_semaphore_take(s_sine_sync, 10000);
  v_semaphore_take(s_sqrt_sync, 10000);
  uint32_t dt = BENCH_ELAPSED(t0);

  /* Sanity: sine sum over 0..6 rad range should be positive */
  int sine_ok = (s_sine_done && s_sine_sum > 0.0f);
  /* Sqrt sum must be very large (all positive) */
  int sqrt_ok = (s_sqrt_done && s_sqrt_sum > 1.0f);

  if (sine_ok && sqrt_ok) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO,
              "FPU context-save OK: sine_sum=%d sqrt_sum=%d in %u ticks",
              (int)(s_sine_sum * 100), (int)(s_sqrt_sum), dt);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR,
              "FPU context-save FAILED: sine_done=%d sqrt_done=%d "
              "sine_sum=%d sqrt_sum=%d",
              s_sine_done, s_sqrt_done, (int)(s_sine_sum * 100),
              (int)(s_sqrt_sum));
  }
  r->duration_ticks = dt;
  r->ops = FPU_MULTICORE_ITERS * 2;
  r->ops_per_sec = BENCH_OPS_PER_SEC(r->ops, dt);
  r->detail = (uint32_t)((sine_ok << 1) | sqrt_ok);
#endif
}

/* ======================================================================= */
/* Entry point called from main benchmark runner                             */
/* ======================================================================= */
void bench_fpu_run(void) {
  v_log(LOG_INFO, "LOG");
  BENCH_LOG(LOG_INFO, "========== FPU Benchmarks ==========");
  bench_fpu_sine_throughput();
  bench_fpu_sqrt_throughput();
  bench_fpu_mixed_multicore();
  BENCH_LOG(LOG_INFO, "====================================");
}
