/**
 * @file bench_ipc.c
 * @brief IPC benchmark suite for VAiOS
 *
 * Sub-benchmarks:
 *  BM_IPC_SEMAPHORE  - Ping-pong semaphore throughput between two tasks
 *  BM_IPC_MUTEX      - Mutex-protected shared counter: two tasks increment
 *                      the same counter; final value must equal total expected
 *                      iterations (races caught as FAIL).
 */

#include "benchmark.h"

/* ----------------------------- tunables --------------------------------- */
#define SEM_PING_ITERS 500u /* round trips per task pair         */
#define MUTEX_ITERS 1000u   /* increments per mutex task         */

/* ======================================================================= */
/* BM_IPC_SEMAPHORE                                                         */
/* Ping-pong: task A gives sem_a, task B waits on sem_a, gives sem_b, etc. */
/* ======================================================================= */

static SemaphoreHandle_t s_sem_a;
static SemaphoreHandle_t s_sem_b;
static volatile uint32_t s_ping_count;
static volatile uint32_t s_pong_count;
static SemaphoreHandle_t s_sem_done;

static void ping_task(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < SEM_PING_ITERS; i++) {
    v_semaphore_give(s_sem_a);
    v_semaphore_take(s_sem_b, 2000);
    s_ping_count++;
  }
  v_semaphore_give(s_sem_done);
}

static void pong_task(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < SEM_PING_ITERS; i++) {
    v_semaphore_take(s_sem_a, 2000);
    s_pong_count++;
    v_semaphore_give(s_sem_b);
  }
  v_semaphore_give(s_sem_done);
}

void bench_ipc_semaphore(void) {
  bench_result_t *r = &g_results[BM_IPC_SEMAPHORE];
  r->name = "IPC: semaphore ping-pong";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  s_ping_count = 0;
  s_pong_count = 0;

  s_sem_a = v_semaphore_create_binary();
  s_sem_b = v_semaphore_create_binary();
  s_sem_done = v_semaphore_create_counting(2, 0);

  if (!s_sem_a || !s_sem_b || !s_sem_done) {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Failed to create semaphores");
    return;
  }

  uint32_t t0 = BENCH_START();
  task_create(ping_task, NULL, 512, 3);
  task_create(pong_task, NULL, 512, 3);

  v_semaphore_take(s_sem_done, 10000);
  v_semaphore_take(s_sem_done, 10000);
  uint32_t dt = BENCH_ELAPSED(t0);

  uint32_t trips = s_ping_count + s_pong_count;
  r->duration_ticks = dt;
  r->ops = trips;
  r->ops_per_sec = BENCH_OPS_PER_SEC(trips, dt);

  if (s_ping_count == SEM_PING_ITERS && s_pong_count == SEM_PING_ITERS) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO, "Semaphore ping-pong: %u trips in %u ms => %u trips/s",
              trips, dt, r->ops_per_sec);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR,
              "Semaphore ping-pong FAIL: ping=%u pong=%u (expected %u each)",
              s_ping_count, s_pong_count, SEM_PING_ITERS);
  }
}

/* ======================================================================= */
/* BM_IPC_MUTEX                                                             */
/* Two tasks increment a shared counter under a mutex.                      */
/* Expected final value = 2 * MUTEX_ITERS.                                  */
/* ======================================================================= */

static MutexHandle_t s_mtx;
static volatile uint32_t s_shared_counter;
static SemaphoreHandle_t s_mtx_done;

static void mutex_worker(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < MUTEX_ITERS; i++) {
    if (v_mutex_lock(s_mtx, 5000) == VA_PASS) {
      uint32_t tmp = s_shared_counter;
      /* Small busy delay to make races more visible if mutex is broken */
      volatile uint32_t spin = 10;
      while (spin--)
        ;
      s_shared_counter = tmp + 1;
      v_mutex_unlock(s_mtx);
    }
    task_yield();
  }
  v_semaphore_give(s_mtx_done);
}

void bench_ipc_mutex(void) {
  bench_result_t *r = &g_results[BM_IPC_MUTEX];
  r->name = "IPC: mutex shared counter";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  s_shared_counter = 0;
  s_mtx = v_mutex_create();
  s_mtx_done = v_semaphore_create_counting(2, 0);

  if (!s_mtx || !s_mtx_done) {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Failed to create mutex/semaphore");
    return;
  }

  uint32_t t0 = BENCH_START();
  task_create(mutex_worker, NULL, 512, 3);
  task_create(mutex_worker, NULL, 512, 3);

  v_semaphore_take(s_mtx_done, 30000);
  v_semaphore_take(s_mtx_done, 30000);
  uint32_t dt = BENCH_ELAPSED(t0);

  uint32_t expected = MUTEX_ITERS * 2u;
  r->duration_ticks = dt;
  r->ops = s_shared_counter;
  r->detail = expected;

  if (s_shared_counter == expected) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO, "Mutex counter: %u == %u (expected) in %u ms — no race",
              s_shared_counter, expected, dt);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Mutex RACE DETECTED: got %u, expected %u",
              s_shared_counter, expected);
  }
}

/* ======================================================================= */
/* Entry point                                                               */
/* ======================================================================= */
void bench_ipc_run(void) {
  BENCH_LOG(LOG_INFO, "========== IPC Benchmarks ==========");
  bench_ipc_semaphore();
  bench_ipc_mutex();
  BENCH_LOG(LOG_INFO, "=====================================");
}
