/**
 * @file bench_bus.c
 * @brief Bus IPC benchmark (plan B8): per-operation cycle cost, and what that
 *        cost does under load — which is the number a control loop cares about.
 *
 * Sub-benchmarks:
 *  BM_BUS_COPY      publish + pop of a single-block message (the copying path)
 *  BM_BUS_ZEROCOPY  reserve/commit + peek/release (no payload copy)
 *  BM_BUS_PIPE      the same round trip on a pipe topic (lock-free SPSC ring)
 *  BM_BUS_LOADED    the copying path again while the pool is saturated and a
 *                   second reader lags, so every publish has to evict first
 *
 * Each reports min / mean / MAX cycles per round trip. The max is the point:
 * an average tells you throughput, the worst case tells you whether a 1 kHz
 * loop will make its deadline.
 *
 * Cycles come from the DWT counter, so this example must call v_perf_init()
 * before measuring — v_init() alone does not arm it.
 */

#include "benchmark.h"
#include "bus.h"
#include "perf.h"

#define ITERS 200u
#define BLOCKS 32u
#define BS 32u

typedef struct {
  uint32_t min, max;
  uint64_t total;
  uint32_t n;
} stat_t;

static void stat_add(stat_t *s, uint32_t c) {
  if (!s->n || c < s->min)
    s->min = c;
  if (c > s->max)
    s->max = c;
  s->total += c;
  s->n++;
}
static uint32_t stat_mean(const stat_t *s) {
  return s->n ? (uint32_t)(s->total / s->n) : 0u;
}

V_BUS_POOL(bench_pool, BS, BLOCKS);
static v_bus_t bb;
static v_bus_topic_t t_copy, t_pipe;
static v_bus_sub_t s_copy, s_pipe, s_lag;
static const v_bus_topic_cfg_t PIPE_CFG = {.pipe = 1, .reserve = 8};
static const v_bus_topic_cfg_t OVERWRITE = {.overflow = V_BUS_OVERWRITE};

/* Record a result the way the rest of the suite does, with the worst case in
 * `detail` because that is the figure a realtime budget is built from. */
static void record(int id, const char *name, const stat_t *s, uint32_t ticks) {
  g_results[id].name = name;
  g_results[id].status = s->n == ITERS ? BENCH_PASS : BENCH_FAIL;
  g_results[id].duration_ticks = ticks;
  g_results[id].ops = s->n;
  g_results[id].ops_per_sec = BENCH_OPS_PER_SEC(s->n, ticks);
  g_results[id].detail = s->max;
  BENCH_LOG(LOG_INFO, "%s: min=%u mean=%u max=%u cycles (%u ops)", name,
            s->min, stat_mean(s), s->max, s->n);
}

/* One publish + one pop, copying. */
static void bench_copy(void) {
  v_bus_init(&bb, bench_pool_blocks, bench_pool_desc, BS, BLOCKS);
  v_bus_topic_declare(&bb, &t_copy, "bench.copy", NULL);
  v_bus_subscribe(&t_copy, &s_copy);

  stat_t st = {0};
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < ITERS; i++) {
    uint32_t payload = i, out = 0;
    uint16_t len = 0;
    uint32_t missed = 0;
    uint64_t c0 = v_perf_cycles();
    v_bus_publish(&t_copy, &payload, sizeof payload);
    v_bus_pop(&s_copy, &out, sizeof out, &len, &missed);
    uint64_t c1 = v_perf_cycles();
    stat_add(&st, (uint32_t)(c1 - c0));
  }
  record(BM_BUS_COPY, "BUS copy publish+pop", &st, BENCH_ELAPSED(t0));
  v_bus_unsubscribe(&s_copy);
}

/* Reserve/commit + peek/release: the payload never moves. */
static void bench_zerocopy(void) {
  v_bus_init(&bb, bench_pool_blocks, bench_pool_desc, BS, BLOCKS);
  v_bus_topic_declare(&bb, &t_copy, "bench.zc", NULL);
  v_bus_subscribe(&t_copy, &s_copy);

  stat_t st = {0};
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < ITERS; i++) {
    uint64_t c0 = v_perf_cycles();
    uint16_t ticket = 0;
    void *slot = v_bus_reserve(&t_copy, sizeof(uint32_t), &ticket);
    if (slot) {
      *(uint32_t *)slot = i;
      v_bus_commit(&t_copy, ticket, sizeof(uint32_t));
      const void *view = 0;
      uint16_t len = 0;
      uint32_t missed = 0;
      if (v_bus_peek(&s_copy, &view, &len, &missed) == VA_PASS)
        v_bus_release(&s_copy);
    }
    uint64_t c1 = v_perf_cycles();
    stat_add(&st, (uint32_t)(c1 - c0));
  }
  record(BM_BUS_ZEROCOPY, "BUS zero-copy reserve+peek", &st, BENCH_ELAPSED(t0));
  v_bus_unsubscribe(&s_copy);
}

/* A pipe topic: the lock-free ring, one producer and one consumer by contract. */
static void bench_pipe(void) {
  v_bus_init(&bb, bench_pool_blocks, bench_pool_desc, BS, BLOCKS);
  v_bus_topic_declare(&bb, &t_pipe, "bench.pipe", &PIPE_CFG);
  v_bus_subscribe(&t_pipe, &s_pipe);

  stat_t st = {0};
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < ITERS; i++) {
    uint32_t payload = i, out = 0;
    uint16_t len = 0;
    uint32_t missed = 0;
    uint64_t c0 = v_perf_cycles();
    v_bus_publish(&t_pipe, &payload, sizeof payload);
    v_bus_pop(&s_pipe, &out, sizeof out, &len, &missed);
    uint64_t c1 = v_perf_cycles();
    stat_add(&st, (uint32_t)(c1 - c0));
  }
  record(BM_BUS_PIPE, "BUS pipe publish+pop", &st, BENCH_ELAPSED(t0));
  v_bus_unsubscribe(&s_pipe);
}

/* Under load: an overwrite topic with a reader that never reads, so the pool
 * stays full and every publish must evict before it can allocate. This is the
 * jitter case — the max here is what a deadline has to survive. */
static void bench_loaded(void) {
  v_bus_init(&bb, bench_pool_blocks, bench_pool_desc, BS, BLOCKS);
  v_bus_topic_declare(&bb, &t_copy, "bench.load", &OVERWRITE);
  v_bus_subscribe(&t_copy, &s_copy);
  v_bus_subscribe(&t_copy, &s_lag); /* subscribes and then never reads */

  /* Saturate: fill the pool so allocation has to reclaim from here on. */
  for (uint32_t i = 0; i < BLOCKS + 4u; i++) {
    uint32_t payload = i;
    v_bus_publish(&t_copy, &payload, sizeof payload);
  }

  stat_t st = {0};
  uint32_t t0 = BENCH_START();
  for (uint32_t i = 0; i < ITERS; i++) {
    uint32_t payload = i, out = 0;
    uint16_t len = 0;
    uint32_t missed = 0;
    uint64_t c0 = v_perf_cycles();
    v_bus_publish(&t_copy, &payload, sizeof payload); /* evicts to make room */
    v_bus_pop(&s_copy, &out, sizeof out, &len, &missed);
    uint64_t c1 = v_perf_cycles();
    stat_add(&st, (uint32_t)(c1 - c0));
  }
  record(BM_BUS_LOADED, "BUS copy under a full pool", &st, BENCH_ELAPSED(t0));

  v_bus_topic_stats_t ts;
  if (v_bus_topic_stats(&t_copy, &ts) == VA_PASS)
    BENCH_LOG(LOG_INFO, "BUS load: published=%u evicted=%u dropped=%u",
              ts.published, ts.evicted, ts.dropped);
  v_bus_unsubscribe(&s_copy);
  v_bus_unsubscribe(&s_lag);
}

void bench_bus_run(void) {
  BENCH_LOG(LOG_INFO, "=== Bus IPC (B8) ===");
  v_perf_init(); // arm the DWT: v_init() does not
  bench_copy();
  bench_zerocopy();
  bench_pipe();
  bench_loaded();
}
