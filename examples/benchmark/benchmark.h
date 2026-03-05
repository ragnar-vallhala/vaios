/**
 * @file benchmark.h
 * @brief Comprehensive benchmark suite for VAiOS + NavHAL
 *
 * Tests DMA M2M transfers, FPU math throughput, task context switching,
 * IPC semaphore/mutex throughput, and heap allocator performance.
 * All results are reported via v_log and collected into bench_result_t.
 */
#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "ipc.h"    /* semaphore / mutex */
#include "memory.h" /* v_malloc, v_free */
#include "task.h"   /* task_create, task_delay, v_delay */
#include "utils.h"  /* v_log, v_get_ticks */
#include "vaios.h"  /* v_init, v_delay */
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Benchmark module identifiers
 * ---------------------------------------------------------------------- */
#define BM_FPU_SINE_THROUGHPUT 0
#define BM_FPU_SQRT_THROUGHPUT 1
#define BM_FPU_MIXED_MULTICORE 2
#define BM_DMA_M2M_SMALL 3
#define BM_DMA_M2M_LARGE 4
#define BM_DMA_CONCURRENT 5
#define BM_TASK_CTX_SWITCH 6
#define BM_TASK_PRIORITY 7
#define BM_TASK_DELAY_ACCURACY 8
#define BM_IPC_SEMAPHORE 9
#define BM_IPC_MUTEX 10
#define BM_MEM_ALLOC_FREE 11
#define BM_MEM_FRAGMENTATION 12
#define BM_STRESS_ALL 13
#define BM_COUNT 14

/* -------------------------------------------------------------------------
 * Benchmark result record
 * ---------------------------------------------------------------------- */
typedef enum {
  BENCH_PASS = 0,
  BENCH_FAIL,
  BENCH_TIMEOUT,
  BENCH_SKIP,
} bench_status_t;

typedef struct {
  const char *name;
  bench_status_t status;
  uint32_t duration_ticks; /* wall-clock ticks taken */
  uint32_t ops;            /* operations counted     */
  uint32_t ops_per_sec;    /* throughput estimate    */
  uint32_t detail;         /* benchmark-specific extra data */
} bench_result_t;

extern bench_result_t g_results[BM_COUNT];

/* -------------------------------------------------------------------------
 * Helper macros
 * ---------------------------------------------------------------------- */
#define BENCH_LOG(lvl, fmt, ...) v_log(lvl, "[BENCH] " fmt, ##__VA_ARGS__)
#define BENCH_START() (v_get_ticks())
#define BENCH_ELAPSED(t0) (v_get_ticks() - (t0))

/* Ops-per-second estimate (ops * 1000 / ticks, since 1 tick = 1 ms) */
#define BENCH_OPS_PER_SEC(ops, ticks) ((ticks) ? ((ops) * 1000u / (ticks)) : 0u)

/* -------------------------------------------------------------------------
 * Public run functions (implemented in bench_*.c)
 * ---------------------------------------------------------------------- */
void bench_fpu_run(void);
void bench_dma_run(void);
void bench_tasks_run(void);
void bench_ipc_run(void);
void bench_memory_run(void);
void bench_stress_run(void);
void bench_print_summary(void);

#endif /* BENCHMARK_H */
