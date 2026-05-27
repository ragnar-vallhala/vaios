/*
 * vaios perf module — kernel performance counters.
 *
 * Phase 0: skeleton only. See docs/perf/IMPLEMENTATION_PLAN.md for the
 * full phased plan. Counters, snapshot/dump API, and VFS-backed CSV
 * dump are added in later phases.
 */

#include "perf.h"

#if VAIOS_MODULE_PERF

void v_perf_init(void) {
  /* Phase 0: nothing to do. Later phases initialize the cycle-counter
   * shadow, zero per-subsystem counters, etc. */
}

#endif /* VAIOS_MODULE_PERF */
