/**
 * @file main.c
 * @brief Benchmark entry point for VAiOS + NavHAL
 *
 * Sequence:
 *  1. Init VAIOS (v_init, v_heap_memory_init, scheduler_init)
 *  2. With NavHAL: clock init, FPU enable
 *  3. Spawn a single top-level benchmark runner task
 *  4. Start scheduler
 *
 * The runner task sequentially calls each benchmark module.
 * All output goes through v_log() over UART via DMA (buffered logging).
 *
 * Build flags expected:
 *   -DVAIOS_FPU=ON   (enables FPU hard-float + _FPU_ENABLED define)
 *   -DNAVHAL=ON      (links NavHAL; enables DMA, clock, fpu_enable)
 *   -D_DMA_ENABLED   (set by NavHAL CMakeLists when DMA driver is included)
 */

#include "benchmark.h"
#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"

#ifdef NAVHAL
#ifndef CORTEX_M4
#define CORTEX_M4
#endif
#include "common/hal_gpio.h"
#include "common/hal_pwm.h"
#include "navhal.h" /* hal_fpu_enable, GPIO, PWM */
#endif

/* -----------------------------------------------------------------------
 * Global result table (defined here, declared extern in benchmark.h)
 * --------------------------------------------------------------------- */
bench_result_t g_results[BM_COUNT];

/* -----------------------------------------------------------------------
 * Summary printer (called after all benchmarks finish)
 * --------------------------------------------------------------------- */
static const char *status_str(bench_status_t s) {
  switch (s) {
  case BENCH_PASS:
    return "PASS   ";
  case BENCH_FAIL:
    return "FAIL   ";
  case BENCH_TIMEOUT:
    return "TIMEOUT";
  case BENCH_SKIP:
    return "SKIP   ";
  default:
    return "UNKNOWN";
  }
}

void bench_print_summary(void) {
  uint32_t pass = 0, fail = 0, skip = 0, timeout = 0;
  v_log(LOG_INFO, "");
  v_log(LOG_INFO, "==================================================");
  v_log(LOG_INFO, "           VAIOS BENCHMARK SUMMARY");
  v_log(LOG_INFO, "==================================================");
  for (uint32_t i = 0; i < BM_COUNT; i++) {
    bench_result_t *r = &g_results[i];
    if (!r->name)
      continue;
    v_log(LOG_INFO, "[%s] %s  (%u ticks, %u ops/s)", status_str(r->status),
          r->name, r->duration_ticks, r->ops_per_sec);
    switch (r->status) {
    case BENCH_PASS:
      pass++;
      break;
    case BENCH_FAIL:
      fail++;
      break;
    case BENCH_TIMEOUT:
      timeout++;
      break;
    case BENCH_SKIP:
      skip++;
      break;
    }
  }
  v_log(LOG_INFO, "--------------------------------------------------");
  v_log(LOG_INFO, "TOTAL: %u PASS  %u FAIL  %u TIMEOUT  %u SKIP", pass, fail,
        timeout, skip);
  v_log(LOG_INFO, "==================================================");

  if (fail == 0 && timeout == 0) {
    v_log(LOG_INFO, "*** ALL BENCHMARKS PASSED ***");
  } else {
    v_log(LOG_ERROR, "*** %u BENCHMARK(S) FAILED ***", fail + timeout);
  }
}

/* -----------------------------------------------------------------------
 * Heartbeat task  —  PB10, 1 Hz blink
 *
 * Toggles PB10 every 500 ms (1 Hz).  No timer or PWM hardware needed.
 * Connect a scope or LED+resistor to PB10 — if it blinks the RTOS
 * scheduler is alive even when UART is quiet during long computations.
 * ----------------------------------------------------------------------- */
#ifdef NAVHAL
static void heartbeat_task(void *arg) {
  (void)arg;

  hal_gpio_setmode(GPIO_PB10, GPIO_OUTPUT, GPIO_PUPD_NONE);

  while (1) {
    hal_gpio_digitalwrite(GPIO_PB10, GPIO_HIGH);
    v_delay(500);
    hal_gpio_digitalwrite(GPIO_PB10, GPIO_LOW);
    v_delay(500);
  }
}
#endif

/* -----------------------------------------------------------------------
 * Top-level benchmark runner task
 * --------------------------------------------------------------------- */
static void benchmark_runner(void *arg) {
  (void)arg;
  v_log(LOG_INFO, "=== VAiOS Benchmark Suite Starting ===");
  v_log(LOG_INFO, "Ticks at start: %u", v_get_ticks());

#ifdef _FPU_ENABLED
  v_log(LOG_INFO, "FPU: ENABLED (hard-float ABI)");
#else
  v_log(LOG_WARN, "FPU: DISABLED (soft-float build)");
#endif

#ifdef _DMA_ENABLED
  v_log(LOG_INFO, "DMA: ENABLED");
#else
  v_log(LOG_WARN, "DMA: DISABLED");
#endif

  /* Small delay so UART has time to flush startup messages */
  v_delay(50);

  /* ---- Run all benchmark categories -------------------------------- */
  bench_fpu_run();
  v_delay(20);

  bench_dma_run();
  v_delay(20);

  bench_tasks_run();
  v_delay(20);

  bench_ipc_run();
  v_delay(20);

  bench_memory_run();
  v_delay(20);

  bench_stress_run();
  v_delay(20);

  /* ---- Print final summary ----------------------------------------- */
  bench_print_summary();

  v_log(LOG_INFO, "Benchmark runner finished. System idle.");

  /* Drain the log buffer synchronously so the DMA pipe is empty
   * before we enter the idle loop.  Without this the last DMA TX
   * may never get its TC interrupt acknowledged, leaving the
   * log_buffer_storage_read_lock stuck at 1 and wedging the RTOS. */
  v_log_flush();
  v_delay(50); /* give UART time to clock out the last bytes at 115200 baud */

  /* Loop forever — idle task keeps the watchdog fed */
  while (1) {
    v_delay(1000);
  }
}

/* -----------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------- */
int main(void) {
#ifdef NAVHAL
  /* Switch to PLL @ 84 MHz using HSI (no external crystal required).
   * SYSCLK = (HSI / PLLM) * PLLN / PLLP = (16 / 16) * 336 / 4 = 84 MHz
   * PLLQ = 7  →  USB/SDIO clock = 336 / 7 = 48 MHz
   *
   * Must be called BEFORE v_init() so that systick_init() and uart2_init()
   * derive their reload / BRR divisors from the live APB clocks. */
  hal_pll_config_t pll_cfg = {
      .input_src = HAL_CLOCK_SOURCE_HSI,
      .pll_m = 16,
      .pll_n = 336,
      .pll_p = 4,
      .pll_q = 7,
  };
  hal_clock_config_t clk_cfg = {.source = HAL_CLOCK_SOURCE_PLL};
  hal_clock_init(&clk_cfg, &pll_cfg);
#endif

  /* 1. Core VAIOS init (uses live APB clocks for UART + SysTick) */
  v_init();
  v_heap_memory_init();
  scheduler_init();

#ifdef NAVHAL
  /* Enable FPU coprocessors (CP10 + CP11, lazy stacking) */
  hal_fpu_enable();
#endif

  /* 4. Initialise result table to zero */
  for (uint32_t i = 0; i < BM_COUNT; i++) {
    g_results[i].status = BENCH_SKIP;
    g_results[i].name = "(not run)";
  }

  /* 5. Spawn runner at high priority so it always gets cpu first */
  task_create(benchmark_runner, NULL, 6144, MAX_PRIORITY);

#ifdef NAVHAL
  /* PWM heartbeat on PB10: breathes 0→100% duty cycle every ~4 s.
   * Lowest priority — purely a visual "system alive" indicator.
   * Watch on an oscilloscope or connect to an LED via a resistor. */
  task_create(heartbeat_task, NULL, 512, MAX_PRIORITY);
#endif

  /* 6. Hand control to scheduler */
  scheduler_start();

  /* Never reached */
  while (1)
    ;
  return 0;
}
