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

#include "atomic.h" /* ENTER_CRITICAL / EXIT_CRITICAL via port.h */
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
 * ISR accounting. Hooked from SysTick_Handler via PERF_ISR_SYSTICK_BEGIN /
 * PERF_ISR_SYSTICK_END (see perf_hooks.h).
 *
 * Stats are bare 32-bit globals; the single writer is SysTick itself,
 * which runs at the lowest exception priority (15) and cannot preempt
 * itself. A reader from task context can race but at worst sees stale
 * counts — that's why v_perf_isr_stats copies under a brief critical
 * section instead of returning fields piecewise.
 * -------------------------------------------------------------------------- */

static uint32_t _perf_isr_systick_count;
static uint32_t _perf_isr_systick_last_cyc;
static uint32_t _perf_isr_systick_min_cyc;
static uint32_t _perf_isr_systick_max_cyc;
static uint32_t _perf_isr_systick_preemptions;

void v_perf_on_isr_systick_exit(uint32_t duration_cyc, int preempted) {
  _perf_isr_systick_count++;
  _perf_isr_systick_last_cyc = duration_cyc;
  if (_perf_isr_systick_min_cyc == 0 || duration_cyc < _perf_isr_systick_min_cyc) {
    _perf_isr_systick_min_cyc = duration_cyc;
  }
  if (duration_cyc > _perf_isr_systick_max_cyc) {
    _perf_isr_systick_max_cyc = duration_cyc;
  }
  if (preempted) {
    _perf_isr_systick_preemptions++;
  }
}

void v_perf_isr_stats(v_perf_isr_t *out) {
  if (!out) return;
  ENTER_CRITICAL();
  out->systick_count       = _perf_isr_systick_count;
  out->systick_last_cyc    = _perf_isr_systick_last_cyc;
  out->systick_min_cyc     = _perf_isr_systick_min_cyc;
  out->systick_max_cyc     = _perf_isr_systick_max_cyc;
  out->systick_preemptions = _perf_isr_systick_preemptions;
  EXIT_CRITICAL();
}

/* --------------------------------------------------------------------------
 * IPC accounting. Hooked from kernel/ipc.c's semaphore_take_common,
 * semaphore_give_common, and the timeout branch.
 *
 * Counters are plain uint32_t — give_from_isr races with task-context
 * giver/taker hooks, but a missed increment on a perf counter beats the
 * cost of atomic ops on the IPC hot path. v_perf_ipc_stats copies the
 * snapshot under a critical section so callers don't see torn fields.
 * -------------------------------------------------------------------------- */

static uint32_t _perf_ipc_takes;
static uint32_t _perf_ipc_takes_blocked;
static uint32_t _perf_ipc_gives;
static uint32_t _perf_ipc_timeouts;

void v_perf_on_ipc_take_attempt(void) { _perf_ipc_takes++; }
void v_perf_on_ipc_take_blocked(void) { _perf_ipc_takes_blocked++; }
void v_perf_on_ipc_give(void)         { _perf_ipc_gives++; }
void v_perf_on_ipc_timeout(void)      { _perf_ipc_timeouts++; }

void v_perf_ipc_stats(v_perf_ipc_t *out) {
  if (!out) return;
  ENTER_CRITICAL();
  out->takes         = _perf_ipc_takes;
  out->takes_blocked = _perf_ipc_takes_blocked;
  out->gives         = _perf_ipc_gives;
  out->timeouts      = _perf_ipc_timeouts;
  EXIT_CRITICAL();
}

/* --------------------------------------------------------------------------
 * Heap accounting. Hooked from kernel/memory.c's v_malloc/v_free inside
 * their critical sections, so the increment of allocation_size in memory.c
 * is visible to the peak-tracking read here.
 *
 * The size-class binning here must match kernel/memory.c's size_class()
 * — kept in sync by hand. V_PERF_HEAP_NUM_CLASSES is exposed in perf.h
 * so the array dimension is the single source of truth on the public
 * side; if memory.c ever changes its NUM_SIZE_CLASSES, both must move.
 * -------------------------------------------------------------------------- */

extern uint32_t allocation_size; /* kernel/memory.c */

static uint32_t _perf_heap_allocs;
static uint32_t _perf_heap_frees;
static uint32_t _perf_heap_oom;
static uint32_t _perf_heap_splits;
static uint32_t _perf_heap_coalesces;
static uint32_t _perf_heap_peak;
static uint32_t _perf_heap_per_class[V_PERF_HEAP_NUM_CLASSES];

static inline int _perf_heap_size_class(uint32_t size) {
  if (size <= 8)   return 0;
  if (size <= 16)  return 1;
  if (size <= 32)  return 2;
  if (size <= 64)  return 3;
  if (size <= 128) return 4;
  if (size <= 256) return 5;
  if (size <= 512) return 6;
  return 7;
}

void v_perf_on_heap_alloc(uint32_t size, int split) {
  _perf_heap_allocs++;
  if (split) _perf_heap_splits++;
  _perf_heap_per_class[_perf_heap_size_class(size)]++;
  if (allocation_size > _perf_heap_peak) {
    _perf_heap_peak = allocation_size;
  }
}

void v_perf_on_heap_free(int coalesces) {
  _perf_heap_frees++;
  _perf_heap_coalesces += (uint32_t)coalesces;
}

void v_perf_on_heap_oom(void) { _perf_heap_oom++; }

void v_perf_heap_stats(v_perf_heap_t *out) {
  if (!out) return;
  ENTER_CRITICAL();
  out->allocs            = _perf_heap_allocs;
  out->frees             = _perf_heap_frees;
  out->oom               = _perf_heap_oom;
  out->splits            = _perf_heap_splits;
  out->coalesces         = _perf_heap_coalesces;
  out->peak_bytes_in_use = _perf_heap_peak;
  for (int i = 0; i < V_PERF_HEAP_NUM_CLASSES; i++) {
    out->per_class_allocs[i] = _perf_heap_per_class[i];
  }
  EXIT_CRITICAL();
}

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
  _perf_isr_systick_count = 0;
  _perf_isr_systick_last_cyc = 0;
  _perf_isr_systick_min_cyc = 0;
  _perf_isr_systick_max_cyc = 0;
  _perf_isr_systick_preemptions = 0;
  _perf_ipc_takes = 0;
  _perf_ipc_takes_blocked = 0;
  _perf_ipc_gives = 0;
  _perf_ipc_timeouts = 0;
  _perf_heap_allocs = 0;
  _perf_heap_frees = 0;
  _perf_heap_oom = 0;
  _perf_heap_splits = 0;
  _perf_heap_coalesces = 0;
  _perf_heap_peak = 0;
  for (int i = 0; i < V_PERF_HEAP_NUM_CLASSES; i++) {
    _perf_heap_per_class[i] = 0;
  }
  /* scheduler_init() runs before v_perf_init() and points current_task at
   * the idle task. Prime its last_scheduled_cyc so the first real switch
   * accumulates a sensible delta instead of (now - 0). */
  if (current_task) {
    current_task->perf.last_scheduled_cyc = v_perf_cycles();
  }
}

#endif /* VAIOS_MODULE_PERF */
