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
#include "utils.h" /* print_fmt, print_fmt_buf, v_get_ticks */

#include <stddef.h>
#include <stdint.h>

#if VAIOS_MODULE_VFS
#include "vfs.h"
#endif

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
 * Per-task getter. v_perf_task_t is the same struct embedded in TCB; this
 * copies under a critical section so readers see a consistent record even
 * if a context switch races with the read.
 * -------------------------------------------------------------------------- */

void v_perf_task_stats(struct Task_Control_Block *t, v_perf_task_t *out) {
  if (!out) return;
  if (!t) { v_perf_task_t z = {0}; *out = z; return; }
  ENTER_CRITICAL();
  *out = t->perf;
  EXIT_CRITICAL();
}

/* --------------------------------------------------------------------------
 * Composite snapshot. Each sub-getter is internally consistent (its own
 * critical section); the cycle stamp and uptime tag the read for ordering
 * but the sub-domains may advance independently between getters. Good
 * enough for diagnostic display.
 * -------------------------------------------------------------------------- */

void v_perf_snapshot(v_perf_snapshot_t *out) {
  if (!out) return;
  ENTER_CRITICAL();
  out->cycles         = v_perf_cycles();
  out->uptime_ticks   = v_get_ticks();
  out->sched_switches = _perf_sched_switches;
  out->idle_cycles    = _perf_idle_cycles;
  EXIT_CRITICAL();
  v_perf_isr_stats(&out->isr);
  v_perf_ipc_stats(&out->ipc);
  v_perf_heap_stats(&out->heap);
}

/* --------------------------------------------------------------------------
 * Dump. Pretty-prints the snapshot via print_fmt — one logical line per
 * counter group. Caller pays the UART cost; use v_perf_dump_to_file
 * (Phase 6b) for long-running capture to avoid baud-stretch.
 * -------------------------------------------------------------------------- */

void v_perf_dump(void) {
  v_perf_snapshot_t s;
  v_perf_snapshot(&s);

  /* Drain any pending v_log entries first. v_log is buffered (see
   * BUFFERED_LOGGING in vaios_config_default.h) and we write straight
   * to UART via print_fmt below — without this, any v_log("snapshot
   * incoming...") line a caller issued just before us would arrive
   * AFTER the snapshot on the wire. */
  v_log_flush();

  print_fmt("=== vaios perf snapshot ===\r\n");
  print_fmt("uptime:     %u ticks\r\n", (unsigned)s.uptime_ticks);
  /* 64-bit cycles printed as two %x halves — print_fmt is integer-only. */
  print_fmt("cycles:     0x%x%x\r\n",
            (unsigned)(s.cycles >> 32), (unsigned)(s.cycles & 0xFFFFFFFFu));

  print_fmt("[sched]\r\n");
  print_fmt("  switches:     %u\r\n", (unsigned)s.sched_switches);
  print_fmt("  idle cycles:  0x%x%x\r\n",
            (unsigned)(s.idle_cycles >> 32),
            (unsigned)(s.idle_cycles & 0xFFFFFFFFu));

  print_fmt("[isr/systick]\r\n");
  print_fmt("  count:        %u\r\n", (unsigned)s.isr.systick_count);
  print_fmt("  last:         %u cyc\r\n", (unsigned)s.isr.systick_last_cyc);
  print_fmt("  min/max:      %u / %u cyc\r\n",
            (unsigned)s.isr.systick_min_cyc, (unsigned)s.isr.systick_max_cyc);
  print_fmt("  preemptions:  %u\r\n", (unsigned)s.isr.systick_preemptions);

  print_fmt("[ipc]\r\n");
  print_fmt("  takes:        %u (blocked=%u, timeout=%u)\r\n",
            (unsigned)s.ipc.takes, (unsigned)s.ipc.takes_blocked,
            (unsigned)s.ipc.timeouts);
  print_fmt("  gives:        %u\r\n", (unsigned)s.ipc.gives);

  print_fmt("[heap]\r\n");
  print_fmt("  allocs/frees: %u / %u (oom=%u)\r\n",
            (unsigned)s.heap.allocs, (unsigned)s.heap.frees,
            (unsigned)s.heap.oom);
  print_fmt("  splits:       %u\r\n", (unsigned)s.heap.splits);
  print_fmt("  coalesces:    %u\r\n", (unsigned)s.heap.coalesces);
  print_fmt("  peak bytes:   %u\r\n", (unsigned)s.heap.peak_bytes_in_use);
  print_fmt("  by class:     ");
  for (int i = 0; i < V_PERF_HEAP_NUM_CLASSES; i++) {
    print_fmt("c%d=%u ", i, (unsigned)s.heap.per_class_allocs[i]);
  }
  print_fmt("\r\n=== end ===\r\n");
}

/* --------------------------------------------------------------------------
 * Init / reset. v_perf_reset zeros every counter and re-primes the cycle
 * counter shadow + current_task->perf.last_scheduled_cyc so accounting
 * resumes from a clean baseline. v_perf_init also brings up the hardware
 * cycle counter on the target — only needed once at boot.
 * -------------------------------------------------------------------------- */

extern TCB *current_task; /* defined in kernel/task.c */

static void _perf_zero_counters(void) {
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
}

void v_perf_reset(void) {
  ENTER_CRITICAL();
  _perf_zero_counters();
  /* Re-prime the running task so its next switch-out delta is sane. */
  if (current_task) {
    current_task->perf.cycles_run = 0;
    current_task->perf.max_burst_cyc = 0;
    current_task->perf.switches_in = 0;
    current_task->perf.last_scheduled_cyc = v_perf_cycles();
  }
  EXIT_CRITICAL();
}

void v_perf_init(void) {
#ifdef NAVHAL
  hal_cycle_counter_init();
#endif
  _perf_zero_counters();
  /* scheduler_init() runs before v_perf_init() and points current_task at
   * the idle task. Prime its last_scheduled_cyc so the first real switch
   * accumulates a sensible delta instead of (now - 0). */
  if (current_task) {
    current_task->perf.last_scheduled_cyc = v_perf_cycles();
  }
}

/* --------------------------------------------------------------------------
 * VFS-backed CSV dump (Phase 6b).
 *
 * The terminal `perf show` path stretches the UART (a full snapshot is a
 * few KB at 115200 baud ≈ 11 KB/s — measurable seconds of console wait
 * during which other output competes). Saving to SD via the VFS layer
 * drops the dump to a single buffered write at FatFs throughput.
 *
 * Format per docs/perf/IMPLEMENTATION_PLAN.md §9a — one section per
 * subsystem, one key/value pair per line, blank line between sections.
 * 64-bit cycle counters are split into _high / _low uint32 fields because
 * print_fmt_buf is 32-bit-only.
 *
 * No runtime mount probe: vfs_open is the probe. If FatFs isn't mounted
 * (or the path is invalid) the open fails and we return -1 — the terminal
 * surfaces a clear error.
 * -------------------------------------------------------------------------- */

#if VAIOS_MODULE_VFS

static int _perf_write_line(vfs_fd_t fd, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vaprint_fmt_buf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return 0;
  return vfs_write(fd, buf, (size_t)n);
}

int v_perf_dump_to_file(const char *path) {
  if (!path) return -1;

  vfs_fd_t fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
  if (fd < 0) return -1;

  v_perf_snapshot_t s;
  v_perf_snapshot(&s);

  _perf_write_line(fd, "# vaios perf snapshot\n");
  _perf_write_line(fd, "uptime_ticks,%u\n", (unsigned)s.uptime_ticks);
  _perf_write_line(fd, "cycles_high,%u\n", (unsigned)(s.cycles >> 32));
  _perf_write_line(fd, "cycles_low,%u\n",
                   (unsigned)(s.cycles & 0xFFFFFFFFu));

  _perf_write_line(fd, "\n# sched\n");
  _perf_write_line(fd, "total_switches,%u\n", (unsigned)s.sched_switches);
  _perf_write_line(fd, "idle_cycles_high,%u\n",
                   (unsigned)(s.idle_cycles >> 32));
  _perf_write_line(fd, "idle_cycles_low,%u\n",
                   (unsigned)(s.idle_cycles & 0xFFFFFFFFu));

  _perf_write_line(fd, "\n# isr\n");
  _perf_write_line(fd, "systick_count,%u\n", (unsigned)s.isr.systick_count);
  _perf_write_line(fd, "systick_last_cyc,%u\n",
                   (unsigned)s.isr.systick_last_cyc);
  _perf_write_line(fd, "systick_min_cyc,%u\n",
                   (unsigned)s.isr.systick_min_cyc);
  _perf_write_line(fd, "systick_max_cyc,%u\n",
                   (unsigned)s.isr.systick_max_cyc);
  _perf_write_line(fd, "systick_preemptions,%u\n",
                   (unsigned)s.isr.systick_preemptions);

  _perf_write_line(fd, "\n# ipc\n");
  _perf_write_line(fd, "takes,%u\n", (unsigned)s.ipc.takes);
  _perf_write_line(fd, "takes_blocked,%u\n", (unsigned)s.ipc.takes_blocked);
  _perf_write_line(fd, "gives,%u\n", (unsigned)s.ipc.gives);
  _perf_write_line(fd, "timeouts,%u\n", (unsigned)s.ipc.timeouts);

  _perf_write_line(fd, "\n# heap\n");
  _perf_write_line(fd, "allocs,%u\n", (unsigned)s.heap.allocs);
  _perf_write_line(fd, "frees,%u\n", (unsigned)s.heap.frees);
  _perf_write_line(fd, "oom,%u\n", (unsigned)s.heap.oom);
  _perf_write_line(fd, "splits,%u\n", (unsigned)s.heap.splits);
  _perf_write_line(fd, "coalesces,%u\n", (unsigned)s.heap.coalesces);
  _perf_write_line(fd, "peak_bytes_in_use,%u\n",
                   (unsigned)s.heap.peak_bytes_in_use);

  _perf_write_line(fd, "\n# heap_by_class\n");
  _perf_write_line(fd, "class,allocs\n");
  for (int i = 0; i < V_PERF_HEAP_NUM_CLASSES; i++) {
    _perf_write_line(fd, "%d,%u\n", i, (unsigned)s.heap.per_class_allocs[i]);
  }

  vfs_close(fd);
  return 0;
}

#endif /* VAIOS_MODULE_VFS */

#endif /* VAIOS_MODULE_PERF */
