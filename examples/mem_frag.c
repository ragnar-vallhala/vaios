/**
 * @file mem_frag.c
 * @brief Adversarial fragmentation workload — the O(n)-vs-O(1) search cost,
 *        stretched on purpose.
 *
 * The mem_stress example holds a bounded live-set, so segregated-fit's first-fit
 * walk never grows past a handful of probes. This example does the opposite: it
 * deliberately piles up many free blocks in ONE size class, all just too small
 * to satisfy the request under test, and measures how the search cost grows.
 *
 * Construction: allocate MF_MAX_DEPTH (victim, spacer) pairs, then free the
 * victims one at a time. Each victim is a 72-byte block — size class (64,128] —
 * and each is fenced by an allocated 8-byte spacer so freeing it cannot coalesce
 * it away. After freeing k victims, that class's free list holds k blocks, none
 * of which can satisfy a 120-byte request (72 < 120). So malloc(120):
 *
 *   - segregated fit  walks all k victims (first-fit within the class) before
 *                     falling through to a larger class — probes ≈ k. O(n).
 *   - TLSF            maps 120 to a second-level slot the 72-byte blocks are
 *                     not in, and never inspects them — probes = 2. O(1).
 *
 * One row per depth, embedded in the log line:
 *   @MF,<depth>,<probes>,<min_cycles>
 * framed by @MFSTART,... / @MFEND,... . Select the backend with VAIOS_HEAP_ALGO
 * and drive both with tools/mem_frag_hw.sh; plot with tools/plot_mem_stress.py.
 */
#include "memory.h"
#include "perf.h" /* v_perf_cycles, v_perf_init */
#include "utils.h"
#include "vaios.h" /* v_init, v_delay */
#include "vaios_config.h"
#include <stdint.h>

#ifndef MF_MAX_DEPTH
#define MF_MAX_DEPTH 256u /* how deep to grow the same-class free list */
#endif
#define MF_VICTIM 72u /* class (64,128]; too small for a 120 B request */
#define MF_SPACER 8u  /* allocated fence, keeps victims from coalescing */
#define MF_PROBE 120u /* the request that must walk past every victim   */
#define MF_TRIALS 3u  /* per-depth cycle trials; report the min (dodge IRQ) */
#define MF_FLUSH_EVERY 32u

#if VAIOS_HEAP_ALGO == VAIOS_HEAP_TLSF
#define MF_BACKEND "TLSF"
#else
#define MF_BACKEND "SEGLIST"
#endif

/* Free blocks the last allocator search inspected (kernel/memory backend). */
extern uint32_t vheap_find_probes;

static void *g_victim[MF_MAX_DEPTH];
static void *g_spacer[MF_MAX_DEPTH];

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_perf_init(); /* arm the DWT cycle counter (v_init does not) */
  v_heap_memory_init();

  v_log(LOG_INFO,
        "@MFSTART,backend=%s,heap=%u,maxdepth=%u,victim=%u,probe=%u",
        MF_BACKEND, (unsigned)v_get_heap_size(), (unsigned)MF_MAX_DEPTH,
        (unsigned)MF_VICTIM, (unsigned)MF_PROBE);

  /* Phase 1: allocate all (victim, spacer) pairs. Nothing is freed yet, so no
   * victim is ever reused during this phase — each is a distinct block, and the
   * interleaved spacers guarantee no two victims are physically adjacent. */
  uint32_t n = 0;
  for (; n < MF_MAX_DEPTH; n++) {
    g_victim[n] = v_malloc(MF_VICTIM);
    g_spacer[n] = v_malloc(MF_SPACER);
    if (!g_victim[n] || !g_spacer[n]) { /* heap exhausted — stop growing */
      if (g_spacer[n])
        v_free(g_spacer[n]);
      if (g_victim[n])
        v_free(g_victim[n]);
      break;
    }
  }

  /* Phase 2: free victims one at a time; after each, time a malloc(120) that
   * must walk past the accumulated free 72-byte blocks. The probe request is
   * never satisfiable from a victim (72 < 120), so it neither consumes them nor
   * changes the depth — it is served from the wilderness and freed straight
   * back, leaving the measurement stable across trials. */
  for (uint32_t k = 0; k < n; k++) {
    v_free(g_victim[k]);
    uint32_t depth = k + 1;
    uint32_t probes = 0, best = 0xffffffffu;
    for (uint32_t t = 0; t < MF_TRIALS; t++) {
      uint32_t c0 = (uint32_t)v_perf_cycles();
      void *p = v_malloc(MF_PROBE);
      uint32_t dc = (uint32_t)v_perf_cycles() - c0;
      probes = vheap_find_probes; /* deterministic; same every trial */
      if (p)
        v_free(p);
      if (dc < best)
        best = dc;
    }
    v_log(LOG_INFO, "@MF,%u,%u,%u", (unsigned)depth, (unsigned)probes,
          (unsigned)best);
    if ((k % MF_FLUSH_EVERY) == 0u) {
      v_log_flush();
      v_delay(1);
    }
  }

  v_log(LOG_INFO, "@MFEND,depth=%u", (unsigned)n);
  v_log_flush();

  while (1) {
  }
}
