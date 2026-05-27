# Plan — Optional `perf` module for vaios

Date: 2026-05-27

The perf module tracks kernel performance metrics (context switches, ISR
latency, scheduler decisions, IPC throughput, heap allocator stats, idle
time, etc.) and is excludable from the build the same way VFS is.

---

## 1. Build-time opt-out (mirror VFS exactly)

The pattern is established and tight — three touch-points:

- **`CMakeLists.txt:58-61`** — add `option(VAIOS_MODULE_PERF "Build kernel performance counters" ON)` next to the existing module options.
- **`include/vaios_config_default.h:83-84`** — add `#ifndef VAIOS_MODULE_PERF / #define VAIOS_MODULE_PERF 1 / #endif`.
- **`kernel/CMakeLists.txt`** — at line 13-15 add `list(FILTER VAIOS_SOURCES EXCLUDE REGEX "perf\\.c$")` under `if(NOT VAIOS_MODULE_PERF)`; at line 29 add `VAIOS_MODULE_PERF=$<BOOL:${VAIOS_MODULE_PERF}>` to `target_compile_definitions`.

Outcome: a single line in `config.cmake` (`-DVAIOS_MODULE_PERF=OFF`) drops the module entirely.

## 2. Files & layout

- **`include/perf.h`** — public API + opaque snapshot struct + `PERF_EVENT_*` macros.
- **`kernel/perf.c`** — counters, snapshot logic, dump, reset, time-base.
- **`include/perf_hooks.h`** *(internal)* — the macro definitions used at instrumentation sites; resolves to no-ops when `VAIOS_MODULE_PERF==0`. Keeps `perf.h` clean of macros that callers shouldn't invoke.

## 3. Instrumentation strategy — **macro hooks**

Considered three approaches: inline `#if`, macro hooks, or weak-symbol/function-pointer dispatch. Pick **macros** — zero overhead when compiled out, one-line disruption per hot path, no indirect-call cost on the critical scheduler path (matters because BASEPRI already constrains ISR work; we don't want to add cycles there).

Examples:
```c
PERF_SCHED_SWITCH(prev, next, cyc);
PERF_HEAP_ALLOC(size, class_idx, latency_cyc);
PERF_IPC_TAKE(sem, blocked /*0|1*/, wait_cyc);
PERF_ISR_ENTER(systick); PERF_ISR_EXIT(systick);
```
Each expands to a counter update + cycle-stamp; defined to `((void)0)` when off.

## 4. Metrics tracked (by subsystem)

**Scheduler** *(extend the existing `context_switch_count` at `kernel/task.c:18,247`)*
- per-task: `cycles_run`, `switches_in`, `max_burst_cyc`, `last_scheduled_cyc`
- system: total switches (exists), idle cycles, scheduler-decision cycles (cost of `get_next_task`)
- preemption count (tick-driven vs voluntary `v_yield`)

**ISR**
- `SysTick_Handler` duration (cycles, min/max/last) at `kernel/utils.c:910-920`
- wake-induced preemption count (`wake_up_delayed_tasks_isr` returning 1)

**IPC** *(hook `kernel/ipc.c` give/take entries)*
- per-primitive (when handle is registered): take/give count, blocked-take count, timeout count, max wait-queue depth, peak wait-cycles

**Heap** *(hook `kernel/memory.c:125,184`)*
- per-size-class fast-path hits / splits / coalesces, OOM count, peak bytes-in-use, alloc/free latency histogram (small fixed buckets)

**System**
- 64-bit extended cycle counter (snapshot in SysTick to handle DWT 32-bit wrap @ 84 MHz ≈ 51.1 s)
- uptime ticks (already via `systick_count`)

## 5. Public API (header sketch)

```c
void v_perf_init(void);              /* called from v_system_init when enabled */
void v_perf_reset(void);             /* zero counters for a benchmark window  */
uint64_t v_perf_cycles(void);        /* 64-bit extended cycle counter         */

/* Snapshot model: atomic copy under brief critical section. */
typedef struct { /* sched, isr, ipc, heap sub-structs */ } v_perf_snapshot_t;
void v_perf_snapshot(v_perf_snapshot_t *out);

/* Per-subsystem getters for the cheap, single-value queries. */
uint32_t v_perf_sched_switches(void);
uint32_t v_perf_sched_preemptions(void);
uint64_t v_perf_idle_cycles(void);
uint32_t v_perf_isr_systick_last_cyc(void);
void     v_perf_task_stats(TCB *t, v_perf_task_stats_t *out);
void     v_perf_heap_stats(v_perf_heap_stats_t *out);
/* IPC: caller passes the handle. */
void     v_perf_sema_stats(SemaphoreHandle_t s, v_perf_ipc_stats_t *out);

void v_perf_dump(void);              /* pretty-print via existing utils printf */

#if VAIOS_MODULE_VFS
int v_perf_dump_to_file(const char *path);   /* CSV dump; -1 if VFS unmounted */
#endif
```

Naming follows existing convention (`v_` prefix on functions, no prefix on typedefs — matches `TCB`, `sema_t`).

## 6. Time-base

Use NavHAL's `hal_cycle_counter_init/read` (`extern/NavHAL/include/common/hal_dwt.h`). Wrap-extension done in SysTick: each tick samples DWT and accumulates into a 64-bit shadow guarded by SysTick's natural single-writer invariant. Host build: replace with a monotonic-ms stub keyed off `v_get_ticks()`.

## 7. TCB extension

Add a `#if VAIOS_MODULE_PERF` block to the `TCB` definition with a small `perf_task_t` sub-struct (~24-32 bytes). Guarded so it costs nothing when the module is off — important because TCB count scales with tasks.

## 8. Concurrency & snapshot atomicity

- Single-writer counters (the kernel path owning the data) → plain 32-bit increments are atomic on M4; readers tolerate skew.
- `v_perf_snapshot()` brackets the multi-field copy with `vPortEnterCritical/Exit` (or equivalent) to get a consistent picture. Documented as O(struct size); not for hot loops.
- 64-bit cycle counter: read low/high/low pattern (seqlock-lite) so readers don't need to mask interrupts.

## 9. Integration points

- **`v_system_init`** (`kernel/task.c:101`): call `v_perf_init()` under `#if VAIOS_MODULE_PERF`, mirroring the VFS pattern at `kernel/vaios.c:120-127`.
- **Terminal**: register `perf` command (when `VAIOS_MODULE_TERMINAL && VAIOS_MODULE_PERF`) at `kernel/terminal.c`. Sub-commands:
    - `perf show` — calls `v_perf_dump()` (UART pretty-print, always available).
    - `perf reset` — calls `v_perf_reset()`.
    - `perf save <path>` — **only when `VAIOS_MODULE_VFS` is compiled in AND a filesystem is currently mounted.** Calls `v_perf_dump_to_file(path)`; prints a clear error if VFS isn't mounted. Avoids the **UART baud-stretch** — at 115200 baud a full snapshot streams for seconds and partly stalls the kernel; the SD path drops that to a single buffered write at FatFs throughput. Example: `perf save /sd/perf_<ts>.csv`.
- **Bench harness** (`examples/benchmark/`): make `bench_result_t` optionally pull from `v_perf_snapshot` so existing benchmarks gain richer reporting for free. Long bench runs should prefer `v_perf_dump_to_file` over UART streaming for the same baud-stretch reason.

### 9a. CSV dump schema (proposed)

One file, one section per subsystem, each section preceded by a `# <name>` header. Keeps it greppable and importable without a parser. Sketch:

```
# vaios perf snapshot
uptime_ticks,5421
cycles,455364000000

# sched
total_switches,184221
preemptions,12044
idle_cycles,2210045008

# task
id,name,cycles_run,switches_in,max_burst_cyc
0,idle,2210045008,90011,123440
1,main,18445221,5012,210330
...

# heap
class,size,allocs,frees,fast_hits,splits,coalesces,peak_bytes,oom
0,8,1204,1190,1100,18,12,0,0
...

# ipc
handle,kind,take,give,blocked,timeout,peak_depth
0x20003040,sema,5022,5022,18,0,3
...

# isr
isr,calls,last_cyc,min_cyc,max_cyc
systick,5421,82,71,140
```

## 10a. VFS-mount detection

`v_perf_dump_to_file` must distinguish three states: (a) `VAIOS_MODULE_VFS=0` at compile time → function not present, terminal sub-command is hidden; (b) VFS compiled in but no mount → return `-1` with a defined errno-like code; (c) mount present → write CSV. The mount check uses the existing VFS API (`vfs.h`); no new probing is added.

## 10. Phased implementation

Each phase is a self-contained commit, mirroring the recently-completed Phase 1-6 test campaign style:

| Phase | Deliverable | Verifies |
|---|---|---|
| **0** | Skeleton: option, macro, empty `perf.c`/`perf.h`, gated build, no-op `v_perf_init`. Build green with `=ON` and `=OFF`. | Opt-out works before any logic exists. |
| **1** | 64-bit cycle counter + scheduler metrics (switches, idle cycles, per-task cycles). Hook `get_next_task` at `kernel/task.c:219,247`. | Core time-base + the most-asked metric. |
| **2** | ISR metrics: SysTick duration, preemption count. | Validates ISR-safe critical sections. |
| **3** | IPC metrics: take/give counters, wait-queue peak depth, timeouts. Hooks in `kernel/ipc.c`. | Highest-value diagnostic for drone IPC paths. |
| **4** | Heap metrics: size-class hit rate, peak in-use, OOM. Hooks in `kernel/memory.c`. Ties back to the prior A1 malloc regression / O2 build work — gives a permanent home for those numbers. | Allocator regressions become visible. |
| **5** | `v_perf_snapshot()` + `v_perf_dump()` + reset. | Public API complete. |
| **6** | Terminal `perf` command (`show` / `reset`); bench harness pulls from snapshot. | Live introspection from running firmware. |
| **6b** | `v_perf_dump_to_file` + `perf save <path>` sub-command, gated on `VAIOS_MODULE_VFS` and runtime mount check. CSV schema per §9a. | Decouples dumps from UART; enables long-run capture to SD. |
| **7** | Host-test suite (`tests/test_perf.c`): counter monotonicity, reset semantics, snapshot atomicity, opt-out compile test, CSV-dump round-trip via a memory-backed VFS stub. | Lock in behavior, matches existing Phase-1..6c test discipline. |

## 11. Risks to flag now

- **DWT wrap** — must be handled (Phase 1), not deferred. 51 s is shorter than typical bench runs.
- **TCB growth** — measure before/after; document the per-task cost in `vaios_config_default.h` comment.
- **Hook placement in `get_next_task`** — recent perf commits (`07f9294`, `eec6144`, `bb22dbf`) carefully leaned this function out. Adding macros here must be benchmarked; if it costs >20-30 cycles when on, gate the heaviest hooks behind a finer `VAIOS_PERF_DETAILED` sub-option.
- **Critical-section discipline** — perf hooks must never call into anything that can block or reorder priority; pure counter updates only.
