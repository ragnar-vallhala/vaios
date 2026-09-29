/**
 * @file mem_stress.c
 * @brief Heap allocator stress / A-B profiling example.
 *
 * Drives the heap the way a real workload does: a bounded live-set of
 * allocations churns under a deterministic pseudo-random mix of sizes, with
 * allocations and frees interleaved so the free lists fragment and coalesce
 * continuously. Every single v_malloc / v_free is individually timed with the
 * DWT cycle counter (via v_perf_cycles) and streamed to the UART as one CSV
 * row, so a host script can reconstruct the full per-operation cost profile and
 * plot it.
 *
 * The point is A/B comparison of the two allocator backends selected by
 * VAIOS_HEAP_ALGO (segregated-fit vs. TLSF): build this example once per
 * backend, run each, and overlay the cost distributions. TLSF should show a
 * flat, bounded per-op cost; the segregated-fit heap should show a heavier
 * tail as its first-fit search walks longer free lists.
 *
 * Two cost metrics per operation:
 *   cycles  - v_perf_cycles() delta. Real DWT cycles on hardware; under QEMU it
 *             is the semihosting virtual clock (SYS_ELAPSED) — real time, but
 *             host-jittery unless QEMU runs with -icount.
 *   probes  - free blocks the allocator inspected during the search
 *             (vheap_find_probes). Deterministic and platform-independent: this
 *             is the clean O(n)-vs-O(1) signal. 0 for frees (no search).
 *
 * Output format (one row per operation), embedded in the normal log line:
 *   @MS,<seq>,<A|F|O>,<size>,<cycles>,<probes>,<live>,<bytes_in_use>
 * Framed by:
 *   @MSTART,backend=<name>,heap=<bytes>,ops=<n>,slots=<n>,seed=<hex>
 *   @MSEND,allocs=<n>,frees=<n>,oom=<n>,max_alloc=<cyc>,max_free=<cyc>
 *
 * Build (per backend):
 *   cmake -S . -B build_ms -DNAVHAL=ON -DEXAMPLES=ON \
 *         -DVAIOS_EXAMPLE=MEM_STRESS -DVAIOS_HEAP_ALGO=TLSF
 * Or drive both + plot with tools/mem_stress_ab.sh.
 */
#include "memory.h"
#include "perf.h"  /* v_perf_cycles — DWT-backed 64-bit cycle counter */
#include "utils.h"
#include "vaios.h" /* v_init, v_delay */
#include "vaios_config.h" /* VAIOS_HEAP_ALGO / VAIOS_HEAP_TLSF */
#include <stdint.h>

/* ------------------------------- tunables -------------------------------- */
#ifndef MEM_STRESS_OPS
#define MEM_STRESS_OPS 1200u /* total malloc+free operations to profile */
#endif
#ifndef MEM_STRESS_SLOTS
#define MEM_STRESS_SLOTS 48u /* max simultaneously-live allocations     */
#endif
#ifndef MEM_STRESS_SEED
#define MEM_STRESS_SEED 0x1234abcdu /* fixed => identical workload per backend */
#endif
#define MEM_FLUSH_EVERY 48u /* drain the log buffer every N ops        */

/* Free blocks the last allocator search inspected (kernel/memory backend);
 * the deterministic search-cost metric. Declared here because it is an internal
 * profiling hook, not part of the public heap API. */
extern uint32_t vheap_find_probes;

/* Which backend are we compiled against? (Reported, not selected, here.) */
#if VAIOS_HEAP_ALGO == VAIOS_HEAP_TLSF
#define MS_BACKEND_NAME "TLSF"
#else
#define MS_BACKEND_NAME "SEGLIST"
#endif

/* Live-set, kept in .bss so it never competes with the heap under test. */
static void *g_live[MEM_STRESS_SLOTS];
static uint16_t g_size[MEM_STRESS_SLOTS];

/* Deterministic xorshift32 — same sequence every run, so both backends see the
 * identical workload and the A/B comparison is apples-to-apples. (Math.random
 * / time-seeding would break reproducibility.) */
static uint32_t g_rng = MEM_STRESS_SEED;
static inline uint32_t rnd(void) {
  uint32_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng = x;
  return x;
}

/* A realistic, small-skewed size mix: mostly small objects, some medium, a few
 * large — the shape most embedded workloads actually exhibit. */
static uint32_t pick_size(void) {
  uint32_t r = rnd() % 100u;
  if (r < 60u)
    return 8u + (rnd() % 56u); /* 8..63    small  */
  if (r < 90u)
    return 64u + (rnd() % 192u); /* 64..255  medium */
  return 256u + (rnd() % 768u);  /* 256..1023 large */
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);            /* clocks, SysTick, console */
  v_perf_init();           /* arm the DWT cycle counter. v_init() does NOT do
                              this — only the full v_system_init() path (heap +
                              scheduler + perf) does, and this bare-main example
                              skips it. Without this, DWT CYCCNT stays disabled
                              and every v_perf_cycles() reads 0 on hardware. */
  v_heap_memory_init();

  for (uint32_t i = 0; i < MEM_STRESS_SLOTS; i++) {
    g_live[i] = NULL;
    g_size[i] = 0;
  }
  uint32_t live = 0;
  uint32_t allocs = 0, frees = 0, oom = 0;
  uint32_t max_alloc = 0, max_free = 0;

  v_log(LOG_INFO, "@MSTART,backend=%s,heap=%u,ops=%u,slots=%u,seed=%x",
        MS_BACKEND_NAME, (unsigned)v_get_heap_size(), (unsigned)MEM_STRESS_OPS,
        (unsigned)MEM_STRESS_SLOTS, (unsigned)MEM_STRESS_SEED);

  for (uint32_t seq = 0; seq < MEM_STRESS_OPS; seq++) {
    /* Decide alloc vs. free: forced at the extremes, mild alloc bias between
     * so the heap tends to run fairly full (maximising fragmentation). */
    int do_alloc;
    if (live == 0u)
      do_alloc = 1;
    else if (live >= MEM_STRESS_SLOTS)
      do_alloc = 0;
    else
      do_alloc = ((rnd() % 100u) < 55u);

    if (do_alloc) {
      uint32_t sz = pick_size();
      uint32_t c0 = (uint32_t)v_perf_cycles();
      void *p = v_malloc(sz);
      uint32_t dc = (uint32_t)v_perf_cycles() - c0;
      uint32_t probes = vheap_find_probes; /* search cost of THIS v_malloc */

      if (p) {
        g_live[live] = p;
        g_size[live] = (uint16_t)sz;
        live++;
        allocs++;
        if (dc > max_alloc)
          max_alloc = dc;
        /* Touch the block so the allocation can't be optimised to a no-op. */
        *(volatile uint8_t *)p = (uint8_t)seq;
        v_log(LOG_INFO, "@MS,%u,A,%u,%u,%u,%u,%u", (unsigned)seq, (unsigned)sz,
              (unsigned)dc, (unsigned)probes, (unsigned)live,
              (unsigned)v_get_heap_allocation_size());
      } else {
        oom++;
        v_log(LOG_INFO, "@MS,%u,O,%u,%u,%u,%u,%u", (unsigned)seq, (unsigned)sz,
              (unsigned)dc, (unsigned)probes, (unsigned)live,
              (unsigned)v_get_heap_allocation_size());
      }
    } else {
      uint32_t idx = rnd() % live; /* free a random live block */
      void *p = g_live[idx];
      uint32_t sz = g_size[idx];

      uint32_t c0 = (uint32_t)v_perf_cycles();
      v_free(p);
      uint32_t dc = (uint32_t)v_perf_cycles() - c0;

      /* swap-and-pop the freed slot out of the live set */
      live--;
      g_live[idx] = g_live[live];
      g_size[idx] = g_size[live];
      g_live[live] = NULL;
      g_size[live] = 0;

      frees++;
      if (dc > max_free)
        max_free = dc;
      /* Frees do no search, so probes = 0. */
      v_log(LOG_INFO, "@MS,%u,F,%u,%u,0,%u,%u", (unsigned)seq, (unsigned)sz,
            (unsigned)dc, (unsigned)live,
            (unsigned)v_get_heap_allocation_size());
    }

    /* Periodically drain the buffered-log DMA so a fast op stream can't
     * overflow the ring and drop rows. This runs OUTSIDE every timed region,
     * so it never perturbs a measurement. */
    if ((seq % MEM_FLUSH_EVERY) == 0u) {
      v_log_flush();
      v_delay(1);
    }
  }

  v_log(LOG_INFO, "@MSEND,allocs=%u,frees=%u,oom=%u,max_alloc=%u,max_free=%u",
        (unsigned)allocs, (unsigned)frees, (unsigned)oom, (unsigned)max_alloc,
        (unsigned)max_free);
  v_log_flush();

  while (1) {
  }
}
