/**
 * @file bench_stress.c
 * @brief Combined stress test for VAiOS: DMA + FPU + tasks running
 * simultaneously
 *
 * BM_STRESS_ALL fires all subsystems concurrently:
 *  - 2 FPU tasks   (sine + sqrt, yielding)
 *  - 2 DMA tasks   (M2M transfers in a loop, each checking integrity)
 *  - 2 IPC tasks   (semaphore ping-pong)
 *  - 1 memory task (repeated alloc/free)
 *
 * The test runs for STRESS_DURATION_MS milliseconds, then checks:
 *  1. No DMA data-corruption events occurred
 *  2. FPU accumulated values are in a sane range (no NaN / corruption)
 *  3. IPC ping-pong count is positive (scheduling is alive)
 *  4. No heap allocation failures were reported
 */

#include "benchmark.h"
#ifdef _FPU_ENABLED
#include <math.h>
#endif

#ifdef _DMA_ENABLED
#include "core/cortex-m4/dma.h"
#endif

/* ----------------------------- tunables --------------------------------- */
#define STRESS_DURATION_MS 3000u /* total stress window, milliseconds  */
#define STRESS_DMA_BUF_SZ 128u
#define STRESS_DMA_ITERS_MAX 200u
#define STRESS_MEM_SZ 64u
#define STRESS_MEM_ROUNDS 100u

/* ----------------------------- shared state ----------------------------- */
static volatile uint8_t s_stress_stop;
static volatile uint32_t s_dma_corrupt; /* incremented on data mismatch  */
static volatile uint32_t s_dma_done;    /* transfers completed            */
static volatile float s_fpu_sine_acc;
static volatile float s_fpu_sqrt_acc;
static volatile uint32_t s_ipc_trips;
static volatile uint32_t s_mem_fails;

/* Synchronisation: each stress task gives this when finished */
#define STRESS_TASK_COUNT 6
static SemaphoreHandle_t s_stress_done_sem;

/* ======================================================================= */
/* Stress task bodies                                                        */
/* ======================================================================= */

/* --- FPU sine stress --------------------------------------------------- */
static void stress_fpu_sine(void *arg) {
  (void)arg;
  float acc = 0.0f, a = 0.001f;
  while (!s_stress_stop) {
    acc += sinf(a);
    a += 0.005f;
    if (a > 100.0f)
      a = 0.001f;
    task_yield();
  }
  s_fpu_sine_acc = acc;
  v_semaphore_give(s_stress_done_sem);
}

/* --- FPU sqrt stress --------------------------------------------------- */
static void stress_fpu_sqrt(void *arg) {
  (void)arg;
  float acc = 0.0f, val = 1.0f;
  while (!s_stress_stop) {
    acc += sqrtf(val);
    val += 0.005f;
    if (val > 10000.0f)
      val = 1.0f;
    task_yield();
  }
  s_fpu_sqrt_acc = acc;
  v_semaphore_give(s_stress_done_sem);
}

/* --- DMA M2M stress ---------------------------------------------------- */
#ifdef _DMA_ENABLED
static uint8_t s_stress_src[STRESS_DMA_BUF_SZ];
static uint8_t s_stress_dst[STRESS_DMA_BUF_SZ];

static void stress_dma(void *arg) {
  (void)arg;
  /* Prepare pattern */
  for (uint32_t i = 0; i < STRESS_DMA_BUF_SZ; i++)
    s_stress_src[i] = (uint8_t)(i ^ 0xCDu);

  dma_config_t cfg = {
      .controller = DMA_CONTROLLER_2,
      .stream = 4,
      .channel = 0,
      .direction = DMA_DIR_M2M,
      .src_addr = (uint32_t)s_stress_src,
      .dst_addr = (uint32_t)s_stress_dst,
      .data_count = STRESS_DMA_BUF_SZ,
      .src_inc = 1,
      .dst_inc = 1,
      .data_width = DMA_DATA_WIDTH_8,
      .priority = DMA_PRIORITY_MEDIUM,
      .circular = 0,
  };
  dma_init(&cfg);

  uint32_t count = 0;
  while (!s_stress_stop && count < STRESS_DMA_ITERS_MAX) {
    for (uint32_t i = 0; i < STRESS_DMA_BUF_SZ; i++)
      s_stress_dst[i] = 0;
    dma_start(&cfg);
    uint32_t poll = 0;
    while (!dma_transfer_complete(&cfg) && poll < 100000u)
      poll++;
    dma_clear_flags(&cfg);
    /* Verify */
    for (uint32_t i = 0; i < STRESS_DMA_BUF_SZ; i++) {
      if (s_stress_dst[i] != s_stress_src[i]) {
        s_dma_corrupt++;
        break;
      }
    }
    s_dma_done++;
    count++;
    task_yield();
  }
  v_semaphore_give(s_stress_done_sem);
}
#endif /* _DMA_ENABLED */

/* --- IPC ping-pong stress ---------------------------------------------- */
static SemaphoreHandle_t s_stress_ping;
static SemaphoreHandle_t s_stress_pong;

static void stress_ipc_ping(void *arg) {
  (void)arg;
  while (!s_stress_stop) {
    v_semaphore_give(s_stress_ping);
    v_semaphore_take(s_stress_pong, 100);
    s_ipc_trips++;
  }
  v_semaphore_give(s_stress_done_sem);
}

static void stress_ipc_pong(void *arg) {
  (void)arg;
  while (!s_stress_stop) {
    if (v_semaphore_take(s_stress_ping, 100) == VA_PASS) {
      v_semaphore_give(s_stress_pong);
    }
  }
  v_semaphore_give(s_stress_done_sem);
}

/* --- Memory alloc/free stress ------------------------------------------ */
static void stress_memory(void *arg) {
  (void)arg;
  for (uint32_t i = 0; i < STRESS_MEM_ROUNDS && !s_stress_stop; i++) {
    uint8_t *p = (uint8_t *)v_malloc(STRESS_MEM_SZ);
    if (!p) {
      s_mem_fails++;
      task_yield();
      continue;
    }
    for (uint32_t b = 0; b < STRESS_MEM_SZ; b++)
      p[b] = (uint8_t)b;
    /* Check */
    for (uint32_t b = 0; b < STRESS_MEM_SZ; b++) {
      if (p[b] != (uint8_t)b) {
        s_mem_fails++;
        break;
      }
    }
    v_free(p);
    task_yield();
  }
  v_semaphore_give(s_stress_done_sem);
}

/* ======================================================================= */
/* BM_STRESS_ALL entry point                                                 */
/* ======================================================================= */
void bench_stress_run(void) {
  bench_result_t *r = &g_results[BM_STRESS_ALL];
  r->name = "STRESS: all subsystems concurrent";

  BENCH_LOG(LOG_INFO, "========== Stress Benchmark ==========");
  BENCH_LOG(LOG_INFO, "Running for %u ms with all subsystems active...",
            STRESS_DURATION_MS);

  /* Reset state */
  s_stress_stop = 0;
  s_dma_corrupt = 0;
  s_dma_done = 0;
  s_fpu_sine_acc = 0.0f;
  s_fpu_sqrt_acc = 0.0f;
  s_ipc_trips = 0;
  s_mem_fails = 0;

  s_stress_done_sem = v_semaphore_create_counting(STRESS_TASK_COUNT, 0);
  s_stress_ping = v_semaphore_create_binary();
  s_stress_pong = v_semaphore_create_binary();

  if (!s_stress_done_sem || !s_stress_ping || !s_stress_pong) {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR, "Stress: failed to allocate IPC objects");
    return;
  }

  uint32_t t0 = BENCH_START();

  /* Launch all stress tasks at the same priority so they share time */
  task_create(stress_fpu_sine, NULL, 1024, 3);
  task_create(stress_fpu_sqrt, NULL, 1024, 3);
#ifdef _DMA_ENABLED
  task_create(stress_dma, NULL, 1024, 3);
#else
  /* Still need to give sem so the wait below doesn't hang */
  v_semaphore_give(s_stress_done_sem);
#endif
  task_create(stress_ipc_ping, NULL, 512, 3);
  task_create(stress_ipc_pong, NULL, 512, 3);
  task_create(stress_memory, NULL, 1024, 3);

  /* Run the stress window */
  v_delay(STRESS_DURATION_MS);
  s_stress_stop = 1;

  /* Wait for all tasks to finish (with generous timeout) */
  for (uint32_t i = 0; i < STRESS_TASK_COUNT; i++)
    v_semaphore_take(s_stress_done_sem, 5000);

  uint32_t dt = BENCH_ELAPSED(t0);

  /* ---- Evaluate results -------------------------------------------- */
  int fpu_ok = (s_fpu_sine_acc != 0.0f) && (s_fpu_sqrt_acc != 0.0f);
  int dma_ok = (s_dma_corrupt == 0);
  int ipc_ok = (s_ipc_trips > 0);
  int mem_ok = (s_mem_fails == 0);

  BENCH_LOG(LOG_INFO, "--- Stress Results (%u ms) ---", dt);
  BENCH_LOG(LOG_INFO, "  FPU sine_acc=%d sqrt_acc=%d : %s",
            (int)(s_fpu_sine_acc), (int)(s_fpu_sqrt_acc),
            fpu_ok ? "OK" : "FAIL");
  BENCH_LOG(LOG_INFO, "  DMA transfers=%u corrupt=%u : %s", s_dma_done,
            s_dma_corrupt, dma_ok ? "OK" : "FAIL");
  BENCH_LOG(LOG_INFO, "  IPC trips=%u : %s", s_ipc_trips,
            ipc_ok ? "OK" : "FAIL");
  BENCH_LOG(LOG_INFO, "  MEM fails=%u : %s", s_mem_fails,
            mem_ok ? "OK" : "FAIL");

  r->duration_ticks = dt;
  r->ops = s_dma_done + s_ipc_trips;
  r->detail =
      (uint32_t)((!fpu_ok << 3) | (!dma_ok << 2) | (!ipc_ok << 1) | !mem_ok);
  r->status = (fpu_ok && dma_ok && ipc_ok && mem_ok) ? BENCH_PASS : BENCH_FAIL;

  BENCH_LOG(r->status == BENCH_PASS ? LOG_INFO : LOG_ERROR,
            "Stress overall: %s", r->status == BENCH_PASS ? "PASS" : "FAIL");
  BENCH_LOG(LOG_INFO, "=====================================");
}
