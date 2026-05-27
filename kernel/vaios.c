#include "vaios.h"
#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios_config.h"
#if VAIOS_MODULE_VFS
#include "vfs.h"
#endif
#include "perf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef NAVHAL
#include "navhal.h"
#else
#include "semihosting.h"
#endif /* ifdef NAVHAL */

#ifndef NAVHAL
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define SYST_CALIB (*(volatile uint32_t *)0xE000E01C)

#define SYST_CSR_ENABLE (1 << 0)
#define SYST_CSR_TICKINT (1 << 1)
#define SYST_CSR_CLKSOURCE (1 << 2)

/* Define CPU clock for QEMU (adjust if needed) */
#define CPU_CLOCK_HZ 16000000UL

void _qemu_systick_init_ms(uint32_t period_ms) {
  uint32_t reload = (CPU_CLOCK_HZ / 1000) * period_ms - 1;

  if (reload > 0xFFFFFF) {
    // SysTick is only 24-bit, so max reload is 0xFFFFFF
    reload = 0xFFFFFF;
  }

  SYST_RVR = reload;
  SYST_CVR = 0; // Clear current value
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

#endif /* ifndef NAVHAL */

extern uint32_t systick_count;

void v_init(vaios_init_config_t *cfg) {
  // resetting global variables
  systick_count = 0;
#ifdef NAVHAL
#ifdef CORTEX_M4
  /* Setup clocks */
  if (cfg->internal_clock_setup == 1) {
    hal_pll_config_t pll_cfg = {.input_src = HAL_CLOCK_SOURCE_HSI,
                                .pll_m = 16,
                                .pll_n = 336,
                                .pll_p = 4,
                                .pll_q = 7};
    hal_clock_config_t clk_cfg = {.source = HAL_CLOCK_SOURCE_PLL};
    hal_clock_init(&clk_cfg, &pll_cfg);
  }

#ifdef _FPU_ENABLED
  hal_fpu_enable();
#endif
  systick_init(SYSTICK_PERIOD);
  hal_set_interrupt_priority(SysTick_IRQn, 14);
  hal_set_interrupt_priority(PendSV_IRQn, 15);

#if UART_LOGGING_ENABLE == 1
uart2_init(UART_BAUDRATE);
#if defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA) &&                     \
    (BUFFERED_LOGGING == 1)
  hal_interrupt_attach_callback(DMA1_Stream6_IRQn, dma_tx_complete_callback);
#endif
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
  v_log(LOG_INFO, "[VAIOS INIT] UART started with baudrate %d bps",
        UART_BAUDRATE);
#endif
#else
#error "CPU NOT RECOGNIZED"
#endif
#else
  _qemu_systick_init_ms(SYSTICK_PERIOD / 1000);
  set_systick_interrupt_priority(14);
  set_pendsv_interrupt_priority(15);
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
#endif
}
// extern void start_scheduler(void);

void v_system_init(vaios_init_config_t *cfg) {
#ifdef NAVHAL
  /* 1. Core VAIOS Init (Clocks, SysTick, UART, etc.) */
  v_init(cfg);

  /* 2. Memory & Scheduler Init */
  v_heap_memory_init();
  scheduler_init();
  v_perf_init();
  if (cfg->internal_sd_card_setup) {

    /* 3. SDIO Init: clock_div is auto-calculated from the system clock */
    hal_sdio_config_t sd_config = {.clock_div = 118, .bus_width = 1};
    if (sdio_init(&sd_config) != HAL_SDIO_OK) {
      v_log(LOG_ERROR, "SDIO Peripheral Init Failed!");
      while (1)
        ;
    }

    /* 4. SD Card Handshake */
    if (sdio_card_init() != HAL_SDIO_OK) {
      v_log(LOG_ERROR, "SD Card Handshake Failed!");
      while (1)
        ;
    }

#if VAIOS_MODULE_VFS
    /* 5. Mount FatFS via the thread-safe VFS layer */
    if (vfs_init() != 0) {
      v_log(LOG_ERROR, "Failed to initialize VFS.");
      while (1)
        ;
    }
#endif
  }
#else
  v_init(cfg);
  v_heap_memory_init();
  scheduler_init();
  v_perf_init();
#endif
}

void v_start(void) {
  // start_scheduler();
  // scheduler_state = SCHEDULER_RUNNING;
}
extern uint8_t scheduler_running;
void v_delay(uint32_t ms) {
  uint32_t delay_ticks = (ms * 1000) / SYSTICK_PERIOD;
  if (scheduler_running) {
    task_delay(delay_ticks);
  } else {
    // Busy wait if scheduler is not running (e.g., during sensor init)
    uint32_t initial_ticks = systick_count;
    while (systick_count < initial_ticks + delay_ticks) {
      __asm volatile("nop");
    }
  }
}

// void v_stop(void) { scheduler_state = SCHEDULER_STOPPED; }
