#include "vaios.h"
#include "config.h"
#include "utils.h"
#ifdef NAVHAL
#include "navhal.h"
#endif /* ifdef NAVHAL */

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
#error "NAVHAL not attached"
#endif
}
