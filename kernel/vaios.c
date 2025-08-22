#include "vaios.h"
#include "config.h"
#include "utils.h"
#ifdef NAVHAL
#include "navhal.h"
#endif /* ifdef NAVHAL */

#ifndef NAVHAL
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)   // Control and status
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)   // Reload value
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)   // Current value
#define SYST_CALIB (*(volatile uint32_t *)0xE000E01C) // Calibration

#define SYST_CSR_ENABLE (1 << 0)    // Counter enable
#define SYST_CSR_TICKINT (1 << 1)   // Interrupt enable
#define SYST_CSR_CLKSOURCE (1 << 2) // Clock source (CPU clock)
void _qemu_systick_init() {
  SYST_RVR = 10000 - 1;
  SYST_CVR = 0; // Clear current value
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

#endif /* ifndef NAVHAL */
void v_init(void) {
#ifdef NAVHAL
#ifdef CORTEX_M4
  systick_init(SYSTICK_PERIOD);
#if UART_LOGGING_ENABLE == 1
  uart2_init(UART_BAUDRATE);
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
  v_log(LOG_INFO, "[VAIOS INIT] UART started with baudrate %d bps",
        UART_BAUDRATE);
#endif
#else
#error "CPU NOT RECOGNIZED"
#endif
#else
  _qemu_systick_init();
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
#endif
}
