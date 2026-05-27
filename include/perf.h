#ifndef VAIOS_PERF_H
#define VAIOS_PERF_H

/*
 * vaios perf module — kernel performance counters.
 *
 * Excluded from the build when -DVAIOS_MODULE_PERF=OFF (see top-level
 * CMakeLists.txt). When excluded, callers in kernel/ are still safe to
 * include this header — the public API becomes empty inline stubs so
 * call sites compile to nothing. See docs/perf/IMPLEMENTATION_PLAN.md.
 *
 * Time-base: 64-bit cycles. On Cortex-M4 the backing source is the DWT
 * CYCCNT register (32-bit, wraps at ~51s @ 84 MHz); wrap is extended
 * inside v_perf_cycles by detecting backward steps. On host builds the
 * counter is a monotonic stub useful for unit tests, not real timing.
 */

#include "vaios_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISR-level perf counters (Phase 2: SysTick only). Always defined so the
 * v_perf_isr_stats prototype is type-stable across ON/OFF builds. */
typedef struct {
  uint32_t systick_count;       /* total SysTick invocations              */
  uint32_t systick_last_cyc;    /* duration of most recent SysTick (cyc)  */
  uint32_t systick_min_cyc;     /* fastest observed SysTick               */
  uint32_t systick_max_cyc;     /* slowest observed SysTick               */
  uint32_t systick_preemptions; /* SysTicks that woke a higher-prio task  */
} v_perf_isr_t;

#if VAIOS_MODULE_PERF

/* Per-task perf counters. Embedded in TCB; sized so the on-target growth
 * is well under one cache line. */
typedef struct {
  uint64_t cycles_run;        /* total cycles this task held the CPU      */
  uint64_t last_scheduled_cyc;/* cycles@last switch-in (transient)        */
  uint32_t max_burst_cyc;     /* longest single uninterrupted run         */
  uint32_t switches_in;       /* times this task was switched ONTO CPU    */
} v_perf_task_t;

void     v_perf_init(void);

/* 64-bit cycle counter, wrap-extended above the 32-bit DWT register. Safe
 * to call from kernel paths (PendSV / SysTick / task context). Reading
 * from outside the kernel is technically racy but tolerable — readers
 * may see counter skew up to one DWT period (~51 s on the 84 MHz target). */
uint64_t v_perf_cycles(void);

/* Scheduler getters — system-wide. */
uint32_t v_perf_sched_switches(void);
uint64_t v_perf_idle_cycles(void);

/* ISR getter — atomic snapshot. */
void v_perf_isr_stats(v_perf_isr_t *out);

#else  /* VAIOS_MODULE_PERF == 0 */

static inline void     v_perf_init(void)              {}
static inline uint64_t v_perf_cycles(void)            { return 0; }
static inline uint32_t v_perf_sched_switches(void)    { return 0; }
static inline uint64_t v_perf_idle_cycles(void)       { return 0; }
static inline void v_perf_isr_stats(v_perf_isr_t *out) {
  if (out) { v_perf_isr_t z = {0}; *out = z; }
}

#endif /* VAIOS_MODULE_PERF */

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_PERF_H */
