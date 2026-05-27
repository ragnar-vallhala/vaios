#ifndef VAIOS_PERF_HOOKS_H
#define VAIOS_PERF_HOOKS_H

/*
 * Internal perf instrumentation hooks. Kernel sites #include this and
 * sprinkle PERF_* macros at instrumentation points. When the perf
 * module is compiled out (VAIOS_MODULE_PERF=0) every macro expands to
 * ((void)0), so call sites cost nothing.
 *
 * This header is internal — kernel code only. Public callers use perf.h.
 */

#include "vaios_config.h"

#if VAIOS_MODULE_PERF

#include "perf.h" /* v_perf_cycles for the inline ISR hook */

/* Forward decl: avoid pulling task.h here, since task.h pulls perf.h
 * (for the v_perf_task_t TCB field) and we don't want a cycle. */
struct Task_Control_Block;

void v_perf_on_sched_switch(struct Task_Control_Block *prev,
                            struct Task_Control_Block *next);

void v_perf_on_isr_systick_exit(uint32_t duration_cyc, int preempted);

#define PERF_SCHED_SWITCH(prev, next) v_perf_on_sched_switch((prev), (next))

/* SysTick wrappers: BEGIN declares a local timestamp; END reads the cycle
 * counter again and forwards the delta plus the preemption flag (nonzero
 * when wake_up_delayed_tasks_isr unblocked a higher-priority task) to the
 * accumulator. Wrap-extension is unnecessary at this granularity — a
 * single SysTick body that exceeds 2^32 cycles (>50 s) is a hung system. */
#define PERF_ISR_SYSTICK_BEGIN()                                               \
  uint32_t _perf_isr_systick_start = (uint32_t)v_perf_cycles()
#define PERF_ISR_SYSTICK_END(preempted)                                        \
  v_perf_on_isr_systick_exit(                                                  \
      (uint32_t)v_perf_cycles() - _perf_isr_systick_start, (preempted))

#else  /* VAIOS_MODULE_PERF == 0 */

#define PERF_SCHED_SWITCH(prev, next)   ((void)0)
#define PERF_ISR_SYSTICK_BEGIN()        ((void)0)
/* Consume `preempted` so callers that capture wake_up_delayed_tasks_isr's
 * return value don't trip -Wunused-but-set-variable on perf-off builds. */
#define PERF_ISR_SYSTICK_END(preempted) ((void)(preempted))

#endif /* VAIOS_MODULE_PERF */

#endif /* VAIOS_PERF_HOOKS_H */
