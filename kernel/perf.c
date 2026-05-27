/*
 * vaios perf module — kernel performance counters.
 *
 * See docs/perf/IMPLEMENTATION_PLAN.md for the full phased plan.
 *
 * Phase 0: skeleton + build opt-out.
 * Phase 1: 64-bit cycle counter + scheduler accounting.
 */

#include "perf.h"

#if VAIOS_MODULE_PERF

#include "task.h"

#include <stdint.h>

#ifdef NAVHAL
#include "common/hal_dwt.h"
#endif

/* --------------------------------------------------------------------------
 * Cycle-counter backend.
 *
 * On the Cortex-M4 target the source is the DWT CYCCNT register (32-bit at
 * SYSCLK, 84 MHz → wraps every ~51 s). Wrap is extended in software inside
 * v_perf_cycles: every call samples the hardware, detects wrap via the
 * unsigned-subtraction trick, and accumulates into a 64-bit shadow.
 *
 * On host builds we have no DWT. A simple monotonic stub keeps the rest
 * of the module testable: each read returns the previous value + 1. That
 * is enough for the unit tests (monotonicity, snapshot atomicity) — real
 * wall-clock cycles are not the point of the host suite.
 * -------------------------------------------------------------------------- */

static volatile uint64_t _perf_cyc_high;  /* upper bits of extended counter */
static volatile uint32_t _perf_cyc_last;  /* last raw 32-bit sample         */

static inline uint32_t _perf_cyc_raw(void) {
#ifdef NAVHAL
  return hal_cycle_counter_get();
#else
  /* Host stub: monotonic uint32 counter. Strictly increasing — perfect for
   * the host tests, useless for measuring real time. */
  static uint32_t fake;
  fake += 100; /* stride > 0 so deltas show up in per-task accounting */
  return fake;
#endif
}

/* Single-writer convention: v_perf_cycles is called from kernel paths
 * (PendSV/SysTick/task context). PendSV holds BASEPRI, so the only races
 * are between PendSV and SysTick — both of which are at priority 15 and
 * therefore cannot preempt each other on Cortex-M4. So the seq below is
 * race-free in the live kernel. */
uint64_t v_perf_cycles(void) {
  uint32_t now = _perf_cyc_raw();
  uint32_t last = _perf_cyc_last;
  if (now < last) {
    _perf_cyc_high += (1ULL << 32);
  }
  _perf_cyc_last = now;
  return _perf_cyc_high + (uint64_t)now;
}

/* --------------------------------------------------------------------------
 * Scheduler accounting. Hooked from kernel/task.c:get_next_task via
 * PERF_SCHED_SWITCH (see perf_hooks.h).
 * -------------------------------------------------------------------------- */

extern TCB *idle_task; /* defined in kernel/task.c */

static uint32_t _perf_sched_switches; /* perf's own switch counter         */
static uint64_t _perf_idle_cycles;    /* time spent in the idle task       */

void v_perf_on_sched_switch(TCB *prev, TCB *next) {
  uint64_t now = v_perf_cycles();

  if (prev) {
    uint64_t delta = now - prev->perf.last_scheduled_cyc;
    prev->perf.cycles_run += delta;
    if (delta > prev->perf.max_burst_cyc) {
      /* Saturate at uint32 max — a single run >2^32 cycles (50+ s) means
       * the system is hung anyway; clamping rather than aliasing is the
       * safer surprise. */
      prev->perf.max_burst_cyc =
          (delta > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)delta;
    }
    if (prev == idle_task) {
      _perf_idle_cycles += delta;
    }
  }

  if (next) {
    next->perf.last_scheduled_cyc = now;
    next->perf.switches_in++;
  }

  _perf_sched_switches++;
}

uint32_t v_perf_sched_switches(void) { return _perf_sched_switches; }
uint64_t v_perf_idle_cycles(void)    { return _perf_idle_cycles; }

/* --------------------------------------------------------------------------
 * Init. Brings up the cycle counter and zeros internal state. Safe to call
 * before scheduler_start.
 * -------------------------------------------------------------------------- */

extern TCB *current_task; /* defined in kernel/task.c */

void v_perf_init(void) {
#ifdef NAVHAL
  hal_cycle_counter_init();
#endif
  _perf_cyc_high = 0;
  _perf_cyc_last = 0;
  _perf_sched_switches = 0;
  _perf_idle_cycles = 0;
  /* scheduler_init() runs before v_perf_init() and points current_task at
   * the idle task. Prime its last_scheduled_cyc so the first real switch
   * accumulates a sensible delta instead of (now - 0). */
  if (current_task) {
    current_task->perf.last_scheduled_cyc = v_perf_cycles();
  }
}

#endif /* VAIOS_MODULE_PERF */
