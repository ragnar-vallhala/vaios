/**
 * @file bench_dma.c
 * @brief DMA benchmark suite for VAiOS + NavHAL
 *
 * Sub-benchmarks:
 *  BM_DMA_M2M_SMALL     - 64-byte  M2M transfer; verify data integrity
 *  BM_DMA_M2M_LARGE     - 4096-byte M2M transfer; measure throughput
 *  BM_DMA_CONCURRENT    - Two DMA streams active at the same time (DMA2 S0+S1)
 *                         Verifies the driver doesn't corrupt shared state.
 *
 * Requires: NavHAL built with _DMA_ENABLED.
 * DMA2 supports M2M; DMA1 does not (STM32F4 restriction).
 */

#include "benchmark.h"

#ifdef _DMA_ENABLED
#include "navhal.h"

/* ----------------------------- tunables --------------------------------- */
#define DMA_SMALL_BYTES 4096u    /* single transfer size, small test  */
#define DMA_LARGE_BYTES 8192u    /* single transfer size, large test  */
#define DMA_CONC_BYTES 4096u     /* per-stream size, concurrent test  */
#define DMA_POLL_TIMEOUT 500000u /* spin-poll limit per transfer      */
#define DMA_MIN_TICKS 20u        /* repeat until this many ms elapsed */

/* Statically allocated buffers (put in SRAM — not stack) */
static uint8_t s_src_small[DMA_SMALL_BYTES];
static uint8_t s_dst_small[DMA_SMALL_BYTES];

static uint8_t s_src_large[DMA_LARGE_BYTES];
static uint8_t s_dst_large[DMA_LARGE_BYTES];

/* Second pair for concurrent test */
static uint8_t s_src_con[DMA_CONC_BYTES];
static uint8_t s_dst_con[DMA_CONC_BYTES];

/* ----------------------------- helpers ---------------------------------- */

/** Fill src with a known pattern and zero dst. */
static void buf_prepare(uint8_t *src, uint8_t *dst, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    src[i] = (uint8_t)(i ^ 0xA5u);
    dst[i] = 0;
  }
}

/** Return 1 if dst matches the pattern written by buf_prepare. */
static int buf_verify(const uint8_t *src, const uint8_t *dst, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (dst[i] != src[i])
      return 0;
  }
  return 1;
}

/**
 * @brief Poll hal_dma_transfer_complete() up to @p timeout times.
 * @return 1 = done,  0 = timed out.
 */
static int dma_poll_complete(const hal_dma_config_t *cfg, uint32_t timeout) {
  for (uint32_t i = 0; i < timeout; i++) {
    if (hal_dma_transfer_complete(cfg))
      return 1;
  }
  return 0;
}

/* ======================================================================= */
/* BM_DMA_M2M_SMALL — loop for DMA_MIN_TICKS ms to get non-zero ticks     */
/* ======================================================================= */
void bench_dma_m2m_small(void) {
  bench_result_t *r = &g_results[BM_DMA_M2M_SMALL];
  r->name = "DMA: M2M 64B (looped)";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  buf_prepare(s_src_small, s_dst_small, DMA_SMALL_BYTES);

  hal_dma_config_t cfg = {
      .controller = HAL_DMA_CONTROLLER_2,
      .stream = 0,
      .channel = 0,
      .direction = HAL_DMA_DIR_M2M,
      .src_addr = (uint32_t)s_src_small,
      .dst_addr = (uint32_t)s_dst_small,
      .data_count = DMA_SMALL_BYTES,
      .src_inc = 1,
      .dst_inc = 1,
      .data_width = HAL_DMA_DATA_WIDTH_8,
      .priority = HAL_DMA_PRIORITY_HIGH,
      .circular = 0,
  };
  hal_dma_init(&cfg);

  uint32_t iters = 0;
  int ok = 1;
  uint32_t t0 = BENCH_START();

  do {
    hal_dma_start(&cfg);
    if (!dma_poll_complete(&cfg, DMA_POLL_TIMEOUT)) {
      r->status = BENCH_TIMEOUT;
      BENCH_LOG(LOG_ERROR, "DMA M2M small: TIMEOUT at iter %u", iters);
      return;
    }
    hal_dma_clear_flags(&cfg);
    if (!buf_verify(s_src_small, s_dst_small, DMA_SMALL_BYTES))
      ok = 0;
    iters++;
  } while (BENCH_ELAPSED(t0) < DMA_MIN_TICKS);

  uint32_t dt = BENCH_ELAPSED(t0);
  uint32_t total_bytes = iters * DMA_SMALL_BYTES;

  r->status = ok ? BENCH_PASS : BENCH_FAIL;
  r->duration_ticks = dt;
  r->ops = total_bytes;
  /* KB/s: total_bytes / 1024 * 1000 / dt */
  r->ops_per_sec = dt ? (total_bytes / 1024u * 1000u / dt) : 0u;

  BENCH_LOG(ok ? LOG_INFO : LOG_ERROR,
            "DMA M2M 64B x%u: %s in %u ticks (~%u KB/s)", iters,
            ok ? "PASS" : "CORRUPT", dt, r->ops_per_sec);
}

/* ======================================================================= */
/* BM_DMA_M2M_LARGE — loop for DMA_MIN_TICKS ms                           */
/* ======================================================================= */
void bench_dma_m2m_large(void) {
  bench_result_t *r = &g_results[BM_DMA_M2M_LARGE];
  r->name = "DMA: M2M 4KB (looped)";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  buf_prepare(s_src_large, s_dst_large, DMA_LARGE_BYTES);

  hal_dma_config_t cfg = {
      .controller = HAL_DMA_CONTROLLER_2,
      .stream = 1,
      .channel = 0,
      .direction = HAL_DMA_DIR_M2M,
      .src_addr = (uint32_t)s_src_large,
      .dst_addr = (uint32_t)s_dst_large,
      .data_count = DMA_LARGE_BYTES,
      .src_inc = 1,
      .dst_inc = 1,
      .data_width = HAL_DMA_DATA_WIDTH_8,
      .priority = HAL_DMA_PRIORITY_VERY_HIGH,
      .circular = 0,
  };
  hal_dma_init(&cfg);

  uint32_t iters = 0;
  int ok = 1;
  uint32_t t0 = BENCH_START();

  do {
    hal_dma_start(&cfg);
    if (!dma_poll_complete(&cfg, DMA_POLL_TIMEOUT)) {
      r->status = BENCH_TIMEOUT;
      BENCH_LOG(LOG_ERROR, "DMA M2M large: TIMEOUT at iter %u", iters);
      return;
    }
    hal_dma_clear_flags(&cfg);
    if (!buf_verify(s_src_large, s_dst_large, DMA_LARGE_BYTES))
      ok = 0;
    iters++;
  } while (BENCH_ELAPSED(t0) < DMA_MIN_TICKS);

  uint32_t dt = BENCH_ELAPSED(t0);
  uint32_t total_bytes = iters * DMA_LARGE_BYTES;

  r->status = ok ? BENCH_PASS : BENCH_FAIL;
  r->duration_ticks = dt;
  r->ops = total_bytes;
  r->ops_per_sec = dt ? (total_bytes / 1024u * 1000u / dt) : 0u;
  r->detail = r->ops_per_sec;

  BENCH_LOG(ok ? LOG_INFO : LOG_ERROR,
            "DMA M2M 4KB x%u: %s in %u ticks (~%u KB/s)", iters,
            ok ? "PASS" : "CORRUPT", dt, r->ops_per_sec);
}

/* ======================================================================= */
/* BM_DMA_CONCURRENT — loop both streams for DMA_MIN_TICKS ms             */
/* ======================================================================= */
void bench_dma_concurrent(void) {
  bench_result_t *r = &g_results[BM_DMA_CONCURRENT];
  r->name = "DMA: concurrent streams (looped)";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  buf_prepare(s_src_small, s_dst_small, DMA_CONC_BYTES);
  buf_prepare(s_src_con, s_dst_con, DMA_CONC_BYTES);
  for (uint32_t i = 0; i < DMA_CONC_BYTES; i++)
    s_src_con[i] = (uint8_t)(i ^ 0x55u);

  hal_dma_config_t cfg0 = {
      .controller = HAL_DMA_CONTROLLER_2,
      .stream = 2,
      .channel = 0,
      .direction = HAL_DMA_DIR_M2M,
      .src_addr = (uint32_t)s_src_small,
      .dst_addr = (uint32_t)s_dst_small,
      .data_count = DMA_CONC_BYTES,
      .src_inc = 1,
      .dst_inc = 1,
      .data_width = HAL_DMA_DATA_WIDTH_8,
      .priority = HAL_DMA_PRIORITY_HIGH,
      .circular = 0,
  };
  hal_dma_config_t cfg1 = {
      .controller = HAL_DMA_CONTROLLER_2,
      .stream = 3,
      .channel = 0,
      .direction = HAL_DMA_DIR_M2M,
      .src_addr = (uint32_t)s_src_con,
      .dst_addr = (uint32_t)s_dst_con,
      .data_count = DMA_CONC_BYTES,
      .src_inc = 1,
      .dst_inc = 1,
      .data_width = HAL_DMA_DATA_WIDTH_8,
      .priority = HAL_DMA_PRIORITY_MEDIUM,
      .circular = 0,
  };
  hal_dma_init(&cfg0);
  hal_dma_init(&cfg1);

  uint32_t iters = 0;
  int ok0 = 1, ok1 = 1;
  uint32_t t0 = BENCH_START();

  do {
    hal_dma_start(&cfg0);
    hal_dma_start(&cfg1);
    int d0 = dma_poll_complete(&cfg0, DMA_POLL_TIMEOUT);
    int d1 = dma_poll_complete(&cfg1, DMA_POLL_TIMEOUT);
    hal_dma_clear_flags(&cfg0);
    hal_dma_clear_flags(&cfg1);
    if (!d0 || !d1) {
      r->status = BENCH_TIMEOUT;
      BENCH_LOG(LOG_ERROR, "DMA concurrent: TIMEOUT at iter %u", iters);
      return;
    }
    if (!buf_verify(s_src_small, s_dst_small, DMA_CONC_BYTES))
      ok0 = 0;
    if (!buf_verify(s_src_con, s_dst_con, DMA_CONC_BYTES))
      ok1 = 0;
    iters++;
  } while (BENCH_ELAPSED(t0) < DMA_MIN_TICKS);

  uint32_t dt = BENCH_ELAPSED(t0);
  uint32_t total_bytes = iters * DMA_CONC_BYTES * 2u;

  r->status = (ok0 && ok1) ? BENCH_PASS : BENCH_FAIL;
  r->duration_ticks = dt;
  r->ops = total_bytes;
  r->ops_per_sec = dt ? (total_bytes / 1024u * 1000u / dt) : 0u;
  r->detail = (uint32_t)((ok0 << 1) | ok1);

  BENCH_LOG((ok0 && ok1) ? LOG_INFO : LOG_ERROR,
            "DMA concurrent x%u: s0=%s s1=%s in %u ticks (~%u KB/s)", iters,
            ok0 ? "OK" : "FAIL", ok1 ? "OK" : "FAIL", dt, r->ops_per_sec);
}

#endif /* _DMA_ENABLED */

/* ======================================================================= */
/* Entry point                                                               */
/* ======================================================================= */
void bench_dma_run(void) {
  BENCH_LOG(LOG_INFO, "========== DMA Benchmarks ==========");
#ifdef _DMA_ENABLED
  bench_dma_m2m_small();
  bench_dma_m2m_large();
  bench_dma_concurrent();
#else
  BENCH_LOG(LOG_WARN,
            "DMA disabled at build time (_DMA_ENABLED not set). Skipping.");
  g_results[BM_DMA_M2M_SMALL].status = BENCH_SKIP;
  g_results[BM_DMA_M2M_LARGE].status = BENCH_SKIP;
  g_results[BM_DMA_CONCURRENT].status = BENCH_SKIP;
  g_results[BM_DMA_M2M_SMALL].name = "DMA: M2M 64-byte (skipped)";
  g_results[BM_DMA_M2M_LARGE].name = "DMA: M2M 4096-byte (skipped)";
  g_results[BM_DMA_CONCURRENT].name = "DMA: concurrent (skipped)";
#endif
  BENCH_LOG(LOG_INFO, "=====================================");
}
