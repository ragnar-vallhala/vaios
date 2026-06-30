/**
 * @file port_hw.c
 * @brief Cortex-M4 port hardware facade.
 *
 * This is the ONLY translation unit in the target/QEMU build that is allowed
 * to touch hardware (NavHAL, raw MMIO, semihosting). The kernel sources call
 * the v_port_hw_* wrappers declared in port.h and never
 * names a hal_* symbol, a uartN_* shim, an IRQn enum, or a peripheral
 * register directly. All backend selection (real STM32 HAL vs. the QEMU
 * semihosting model) lives here, behind a single #ifdef NAVHAL.
 *
 * Host unit-test builds replace these wrappers with the stubs in
 * tests/stubs/stubs.c (they shadow this file via the stub port.h).
 */

#include "port.h"
#include "vaios_config.h"
#include <stdint.h>

#ifdef NAVHAL
#include "navhal.h"
#else
#include "qemu_irq.h"
#include "semihosting.h"

/* QEMU SysTick model: program the ARMv7-M system timer registers directly.
 * The netduino/STM32F4 Renode model exposes the standard SysTick block. */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define SYST_CSR_ENABLE (1 << 0)
#define SYST_CSR_TICKINT (1 << 1)
#define SYST_CSR_CLKSOURCE (1 << 2)
#define CPU_CLOCK_HZ 16000000UL
#endif /* NAVHAL */

/* ---------------------------------------------------------------------------
 * Clock / FPU bring-up
 * ------------------------------------------------------------------------- */

void v_port_hw_clock_init(uint8_t internal_clock_setup) {
#ifdef NAVHAL
  if (internal_clock_setup == 1) {
    hal_pll_config_t pll_cfg = {.input_src = HAL_CLOCK_SOURCE_HSI,
                                .pll_m = 16,
                                .pll_n = 336,
                                .pll_p = 4,
                                .pll_q = 7};
    hal_clock_config_t clk_cfg = {.source = HAL_CLOCK_SOURCE_PLL};
    hal_clock_init(&clk_cfg, &pll_cfg);
  }
#else
  (void)internal_clock_setup; /* QEMU boots with a usable clock already. */
#endif
}

void v_port_hw_fpu_enable(void) {
#if defined(NAVHAL) && defined(_FPU_ENABLED)
  hal_fpu_enable();
#endif
}

/* ---------------------------------------------------------------------------
 * System tick + scheduler interrupt priorities
 * ------------------------------------------------------------------------- */

void v_port_hw_systick_init(uint32_t period_us) {
#ifdef NAVHAL
  hal_timebase_init(period_us);
#else
  uint32_t period_ms = period_us / 1000u;
  uint32_t reload = (CPU_CLOCK_HZ / 1000u) * period_ms - 1u;
  if (reload > 0xFFFFFFu) {
    reload = 0xFFFFFFu; /* SysTick is 24-bit. */
  }
  SYST_RVR = reload;
  SYST_CVR = 0;
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
#endif
}

void v_port_hw_sched_irq_init(void) {
  /* SysTick below PendSV so a tick can pend a context switch that runs only
   * once all higher-priority IRQs have drained. PendSV is the lowest. */
#ifdef NAVHAL
  hal_interrupt_set_priority(SysTick_IRQn, 14);
  hal_interrupt_set_priority(PendSV_IRQn, 15);
#else
  set_systick_interrupt_priority(14);
  set_pendsv_interrupt_priority(15);
#endif
}

/* ---------------------------------------------------------------------------
 * Console (log/terminal UART, or semihosting under QEMU)
 * ------------------------------------------------------------------------- */

void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void)) {
#ifdef NAVHAL
  hal_uart_config_t uart_cfg = {.baudrate = baudrate};
  hal_uart_init(HAL_UART_2, &uart_cfg);
#if defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA) &&                     \
    (BUFFERED_LOGGING == 1)
  if (dma_tx_done_cb) {
    hal_interrupt_attach_callback(DMA1_Stream6_IRQn, dma_tx_done_cb);
  }
#else
  (void)dma_tx_done_cb;
#endif
#else
  (void)baudrate;      /* Semihosting needs no init. */
  (void)dma_tx_done_cb;
#endif
}

void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len) {
#if defined(NAVHAL) && defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA)
  hal_uart_write_dma(HAL_UART_2, bytes, len);
#else
  (void)bytes;
  (void)len;
#endif
}

void v_port_hw_console_write_string(const char *str) {
#ifdef NAVHAL
  hal_uart_write_string(HAL_UART_2, str);
#else
  sh_write0(str);
#endif
}

char v_port_hw_console_read_char(void) {
#ifdef NAVHAL
  return hal_uart_read_char(HAL_UART_2);
#else
  return sh_readc();
#endif
}

void v_port_hw_console_rx_irq_init(void (*rx_cb)(void)) {
#ifdef NAVHAL
  hal_interrupt_attach_callback(USART2_IRQn, rx_cb);
  hal_interrupt_enable(USART2_IRQn);
#else
  /* QEMU semihosting has no async RX IRQ; the terminal polls instead. */
  (void)rx_cb;
#endif
}

/* ---------------------------------------------------------------------------
 * SD/MMC (SDIO) — used by the VFS init path
 * ------------------------------------------------------------------------- */

int v_port_hw_sdio_init(void) {
#ifdef NAVHAL
  /* clock_div is auto-calculated from the system clock. */
  hal_sdio_config_t sd_config = {.clock_div = 118, .bus_width = 1};
  return (hal_sdio_init(&sd_config) == HAL_SDIO_OK) ? 0 : -1;
#else
  return -1; /* No SDIO model under QEMU. */
#endif
}

int v_port_hw_sdio_card_init(void) {
#ifdef NAVHAL
  return (hal_sdio_card_init() == HAL_SDIO_OK) ? 0 : -1;
#else
  return -1;
#endif
}

/* ---------------------------------------------------------------------------
 * Cycle counter (DWT CYCCNT) — perf module backend
 * ------------------------------------------------------------------------- */

void v_port_hw_cycle_counter_init(void) {
#ifdef NAVHAL
  hal_cycle_counter_init();
#endif
}

uint32_t v_port_hw_cycle_counter_read(void) {
#ifdef NAVHAL
  return hal_cycle_counter_get();
#else
  /* QEMU has no DWT we rely on here; monotonic stub keeps perf accounting
   * sane without a real cycle source. */
  static uint32_t fake;
  fake += 100;
  return fake;
#endif
}
