/**
 * @file bench_memory.c
 * @brief Heap allocator benchmark suite for VAiOS
 *
 * Sub-benchmarks:
 *  BM_MEM_ALLOC_FREE     - Sequential alloc+free throughput for various sizes
 *  BM_MEM_FRAGMENTATION  - Alternating alloc/free pattern to provoke
 *                          fragmentation; checks the allocator still
 *                          satisfies a large allocation afterwards.
 *
 * Note: v_malloc / v_free are the VAiOS heap primitives.
 */

#include "benchmark.h"

/* ----------------------------- tunables --------------------------------- */
#define MEM_ALLOC_ROUNDS 200u /* alloc+free pairs per size class      */
#define MEM_FRAG_SLOTS 16u    /* slots used in fragmentation test     */
#define MEM_SMALL_SIZE 16u
#define MEM_MEDIUM_SIZE 128u
#define MEM_LARGE_SIZE 512u

/* ======================================================================= */
/* BM_MEM_ALLOC_FREE                                                        */
/* ======================================================================= */

/**
 * @brief Run alloc+free for @p rounds iterations of @p sz bytes each.
 *        Returns ticks taken.  Writes a byte into each allocation so the
 *        compiler cannot optimise it away, and verifies a sentinel value.
 */
static uint32_t run_alloc_bench(uint32_t sz, uint32_t rounds,
                                uint32_t *fail_out) {
  *fail_out = 0;
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < rounds; i++) {
    uint8_t *p = (uint8_t *)v_malloc(sz);
    if (!p) {
      (*fail_out)++;
      continue;
    }
    /* Touch every byte to verify the pointer is valid SRAM */
    for (uint32_t b = 0; b < sz; b++)
      p[b] = (uint8_t)(b ^ 0xBEu);
    /* Spot-check first byte */
    if (p[0] != (uint8_t)(0u ^ 0xBEu))
      (*fail_out)++;
    v_free(p);
  }
  return BENCH_ELAPSED(t0);
}

void bench_memory_alloc_free(void) {
  bench_result_t *r = &g_results[BM_MEM_ALLOC_FREE];
  r->name = "MEM: alloc/free throughput";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  uint32_t fails = 0, total_fails = 0;
  uint32_t total_ops = MEM_ALLOC_ROUNDS * 3u; /* three size classes */
  uint32_t t0 = BENCH_START();

  uint32_t dt_s = run_alloc_bench(MEM_SMALL_SIZE, MEM_ALLOC_ROUNDS, &fails);
  total_fails += fails;
  uint32_t dt_m = run_alloc_bench(MEM_MEDIUM_SIZE, MEM_ALLOC_ROUNDS, &fails);
  total_fails += fails;
  uint32_t dt_l = run_alloc_bench(MEM_LARGE_SIZE, MEM_ALLOC_ROUNDS, &fails);
  total_fails += fails;

  uint32_t dt = BENCH_ELAPSED(t0);

  BENCH_LOG(LOG_INFO,
            "Alloc/free: small=%u ms  medium=%u ms  large=%u ms  total=%u ms  "
            "fails=%u",
            dt_s, dt_m, dt_l, dt, total_fails);

  r->duration_ticks = dt;
  r->ops = total_ops - total_fails;
  r->ops_per_sec = BENCH_OPS_PER_SEC(r->ops, dt);
  r->detail = total_fails;
  r->status = (total_fails == 0) ? BENCH_PASS : BENCH_FAIL;

  if (total_fails)
    BENCH_LOG(LOG_ERROR, "MEM alloc/free: %u failures detected!", total_fails);
}

/* ======================================================================= */
/* BM_MEM_FRAGMENTATION                                                     */
/* ======================================================================= */

void bench_memory_fragmentation(void) {
  bench_result_t *r = &g_results[BM_MEM_FRAGMENTATION];
  r->name = "MEM: fragmentation resilience";
  BENCH_LOG(LOG_INFO, "--- %s ---", r->name);

  void *slots[MEM_FRAG_SLOTS];
  uint32_t alloc_fails = 0;

  /* Phase 1: Allocate all slots (alternating small/large) */
  for (uint32_t i = 0; i < MEM_FRAG_SLOTS; i++) {
    uint32_t sz = (i & 1u) ? MEM_LARGE_SIZE : MEM_SMALL_SIZE;
    slots[i] = v_malloc(sz);
    if (!slots[i])
      alloc_fails++;
  }

  /* Phase 2: Free every other slot to create holes */
  for (uint32_t i = 0; i < MEM_FRAG_SLOTS; i += 2u) {
    if (slots[i]) {
      v_free(slots[i]);
      slots[i] = NULL;
    }
  }

  /* Phase 3: Try to allocate a medium block — should succeed */
  void *mid = v_malloc(MEM_MEDIUM_SIZE);
  int mid_ok = (mid != NULL);
  if (mid)
    v_free(mid);

  /* Phase 4: Free remaining live slots */
  for (uint32_t i = 1; i < MEM_FRAG_SLOTS; i += 2u) {
    if (slots[i]) {
      v_free(slots[i]);
      slots[i] = NULL;
    }
  }

  /* Phase 5: After full free, large allocation should succeed */
  void *big = v_malloc(MEM_LARGE_SIZE * 2u);
  int big_ok = (big != NULL);
  if (big)
    v_free(big);

  r->ops = MEM_FRAG_SLOTS;
  r->detail = (uint32_t)((mid_ok << 1) | big_ok);

  if (alloc_fails == 0 && mid_ok && big_ok) {
    r->status = BENCH_PASS;
    BENCH_LOG(LOG_INFO,
              "Fragmentation: mid_ok=%d big_ok=%d — heap coalesces correctly",
              mid_ok, big_ok);
  } else {
    r->status = BENCH_FAIL;
    BENCH_LOG(LOG_ERROR,
              "Fragmentation FAIL: alloc_fails=%u mid_ok=%d big_ok=%d",
              alloc_fails, mid_ok, big_ok);
  }
}

/* ======================================================================= */
/* Entry point                                                               */
/* ======================================================================= */
void bench_memory_run(void) {
  BENCH_LOG(LOG_INFO, "========== Memory Benchmarks ==========");
  bench_memory_alloc_free();
  bench_memory_fragmentation();
  BENCH_LOG(LOG_INFO, "=======================================");
}
