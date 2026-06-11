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

/* System-wide IPC counters. Per-primitive stats would require either
 * registering each handle (runtime cost) or growing sema_t past the
 * STATIC_SEMAPHORE_SIZE budget — deferred. The aggregate counters here
 * are enough to spot contention, timeout storms, and give/take imbalance. */
typedef struct {
  uint32_t takes;         /* successful + blocking + non-blocking attempts */
  uint32_t takes_blocked; /* subset of `takes` that had to wait            */
  uint32_t gives;         /* total v_semaphore_give / give_from_isr calls  */
  uint32_t timeouts;      /* takes that returned VA_FAIL via timeout       */
} v_perf_ipc_t;

/* Heap allocator counters. per_class_allocs bins by the same size classes
 * as kernel/memory.c (8/16/32/64/128/256/512/larger). */
#define V_PERF_HEAP_NUM_CLASSES 8

typedef struct {
  uint32_t allocs;             /* successful v_malloc calls                */
  uint32_t frees;              /* v_free calls (excludes NULL/error paths) */
  uint32_t oom;                /* allocations that returned NULL           */
  uint32_t splits;             /* malloc-time block splits                 */
  uint32_t coalesces;          /* free-time merges (fwd + bwd combined)    */
  uint32_t peak_bytes_in_use;  /* peak payload bytes outstanding           */
  uint32_t per_class_allocs[V_PERF_HEAP_NUM_CLASSES];
} v_perf_heap_t;

/* Per-task perf counters. Embedded in TCB (only when VAIOS_MODULE_PERF=1)
 * but the type is always defined so the v_perf_task_stats signature stays
 * stable across ON/OFF builds. */
typedef struct {
  uint64_t cycles_run;        /* total cycles this task held the CPU      */
  uint64_t last_scheduled_cyc;/* cycles@last switch-in (transient)        */
  uint32_t max_burst_cyc;     /* longest single uninterrupted run         */
  uint32_t switches_in;       /* times this task was switched ONTO CPU    */
  /* Stack high-water. Filled on demand by v_perf_task_stats: the unused
   * stack still holds the V_PERF_STACK_FILL sentinel painted at create, so
   * the deepest the stack ever grew is derivable. Lets you right-size
   * over-allocated stacks and reclaim SRAM. 0 in the in-TCB instance. */
  uint32_t stack_size;        /* total stack bytes allocated for the task */
  uint32_t stack_peak;        /* peak bytes ever used (high-water)        */
} v_perf_task_t;

/* Sentinel painted over a task's whole stack at create time. Unused words
 * retain it; the lowest untouched run measures free headroom. 0xC5 pattern
 * is unlikely to occur naturally in a frame and is non-zero/non-0xFF. */
#define V_PERF_STACK_FILL 0xC5C5C5C5u

/* Composite snapshot returned by v_perf_snapshot. Bundles the system-wide
 * scheduler stats with the per-subsystem sub-structs so callers can take
 * a single coherent reading. The cycles+uptime pair stamps the snapshot. */
typedef struct {
  uint64_t cycles;              /* v_perf_cycles() at snapshot time       */
  uint32_t uptime_ticks;        /* v_get_ticks() at snapshot time         */
  uint32_t sched_switches;
  uint64_t idle_cycles;
  v_perf_isr_t  isr;
  v_perf_ipc_t  ipc;
  v_perf_heap_t heap;
} v_perf_snapshot_t;

/* Forward decl so v_perf_task_stats's signature doesn't drag in task.h. */
struct Task_Control_Block;

#if VAIOS_MODULE_PERF

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

/* IPC getter — atomic snapshot. */
void v_perf_ipc_stats(v_perf_ipc_t *out);

/* Heap getter — atomic snapshot. */
void v_perf_heap_stats(v_perf_heap_t *out);

/* Per-task getter — copies the in-TCB counters. */
void v_perf_task_stats(struct Task_Control_Block *t, v_perf_task_t *out);

/* Composite snapshot — calls each *_stats getter and stamps cycles/uptime.
 * Not strictly atomic across sub-domains (each sub-struct is internally
 * consistent, but counters in domain A may advance while B is being read).
 * Good enough for diagnostic display; not for cross-domain math. */
void v_perf_snapshot(v_perf_snapshot_t *out);

/* Pretty-print the snapshot via print_fmt — multi-line, UART-bound. Slow:
 * use v_perf_dump_to_file (Phase 6b) for long-run capture to avoid
 * stretching the serial console. */
void v_perf_dump(void);

/* Zero all counters. The cycle counter and per-task last_scheduled_cyc
 * are re-primed to the current cycle so accounting resumes cleanly. */
void v_perf_reset(void);

#if VAIOS_MODULE_VFS
/* CSV dump to a VFS-backed file. Returns 0 on success, -1 on open
 * failure (path invalid, VFS not mounted, disk full, etc). Format is
 * documented in docs/perf/IMPLEMENTATION_PLAN.md §9a — one section per
 * subsystem, greppable, importable without a parser. Cycles are split
 * into _high / _low uint32 fields because print_fmt is 32-bit only. */
int v_perf_dump_to_file(const char *path);
#endif

#else  /* VAIOS_MODULE_PERF == 0 */

static inline void     v_perf_init(void)              {}
static inline uint64_t v_perf_cycles(void)            { return 0; }
static inline uint32_t v_perf_sched_switches(void)    { return 0; }
static inline uint64_t v_perf_idle_cycles(void)       { return 0; }
static inline void v_perf_isr_stats(v_perf_isr_t *out) {
  if (out) { v_perf_isr_t z = {0}; *out = z; }
}
static inline void v_perf_ipc_stats(v_perf_ipc_t *out) {
  if (out) { v_perf_ipc_t z = {0}; *out = z; }
}
static inline void v_perf_heap_stats(v_perf_heap_t *out) {
  if (out) { v_perf_heap_t z = {0}; *out = z; }
}
static inline void v_perf_task_stats(struct Task_Control_Block *t,
                                     v_perf_task_t *out) {
  (void)t;
  if (out) { v_perf_task_t z = {0}; *out = z; }
}
static inline void v_perf_snapshot(v_perf_snapshot_t *out) {
  if (out) { v_perf_snapshot_t z = {0}; *out = z; }
}
static inline void v_perf_dump(void) {}
static inline void v_perf_reset(void) {}
#if VAIOS_MODULE_VFS
static inline int v_perf_dump_to_file(const char *path) {
  (void)path; return -1;
}
#endif

#endif /* VAIOS_MODULE_PERF */

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_PERF_H */
