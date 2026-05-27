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

/* Forward decl: avoid pulling task.h here, since task.h pulls perf.h
 * (for the v_perf_task_t TCB field) and we don't want a cycle. */
struct Task_Control_Block;

void v_perf_on_sched_switch(struct Task_Control_Block *prev,
                            struct Task_Control_Block *next);

#define PERF_SCHED_SWITCH(prev, next) v_perf_on_sched_switch((prev), (next))

#else  /* VAIOS_MODULE_PERF == 0 */

#define PERF_SCHED_SWITCH(prev, next) ((void)0)

#endif /* VAIOS_MODULE_PERF */

#endif /* VAIOS_PERF_HOOKS_H */
