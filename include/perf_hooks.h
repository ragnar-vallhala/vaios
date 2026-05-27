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

void v_perf_on_ipc_take_attempt(void);
void v_perf_on_ipc_take_blocked(void);
void v_perf_on_ipc_give(void);
void v_perf_on_ipc_timeout(void);

void v_perf_on_heap_alloc(uint32_t size, int split);
void v_perf_on_heap_free(int coalesces);
void v_perf_on_heap_oom(void);

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

/* IPC sites — semaphore/mutex give/take. Counters are plain uint32_t and
 * may miss the rare update racing an ISR-context give; the cost of doing
 * proper atomics on the take/give hot paths isn't worth a precise
 * diagnostic counter. */
#define PERF_IPC_TAKE_ATTEMPT() v_perf_on_ipc_take_attempt()
#define PERF_IPC_TAKE_BLOCKED() v_perf_on_ipc_take_blocked()
#define PERF_IPC_GIVE()         v_perf_on_ipc_give()
#define PERF_IPC_TIMEOUT()      v_perf_on_ipc_timeout()

/* Heap sites — fire inside v_malloc/v_free's critical section. The hook
 * reads kernel/memory.c's allocation_size global to keep peak-in-use
 * consistent without passing it explicitly. */
#define PERF_HEAP_ALLOC(size, split) v_perf_on_heap_alloc((size), (split))
#define PERF_HEAP_FREE(coalesces)    v_perf_on_heap_free((coalesces))
#define PERF_HEAP_OOM()              v_perf_on_heap_oom()

#else  /* VAIOS_MODULE_PERF == 0 */

#define PERF_SCHED_SWITCH(prev, next)   ((void)0)
#define PERF_ISR_SYSTICK_BEGIN()        ((void)0)
/* Consume `preempted` so callers that capture wake_up_delayed_tasks_isr's
 * return value don't trip -Wunused-but-set-variable on perf-off builds. */
#define PERF_ISR_SYSTICK_END(preempted) ((void)(preempted))
#define PERF_IPC_TAKE_ATTEMPT()         ((void)0)
#define PERF_IPC_TAKE_BLOCKED()         ((void)0)
#define PERF_IPC_GIVE()                 ((void)0)
#define PERF_IPC_TIMEOUT()              ((void)0)
#define PERF_HEAP_ALLOC(size, split)    ((void)(size), (void)(split))
#define PERF_HEAP_FREE(coalesces)       ((void)(coalesces))
#define PERF_HEAP_OOM()                 ((void)0)

#endif /* VAIOS_MODULE_PERF */

#endif /* VAIOS_PERF_HOOKS_H */
