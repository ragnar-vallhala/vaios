#ifndef VAIOS_STREAM_HW_H
#define VAIOS_STREAM_HW_H

// NavHAL backends for v_stream_t (include/stream.h): USB CDC and UART over DMA.

#include "stream.h"

#if VAIOS_DEVFS
#if defined(NAVHAL)
#include "navhal.h"
#if NAVHAL_CONFIG_DRV_USB_CDC && !NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
// The board's USB device port, as a CDC ACM serial port. Not available when
// the console is routed to CDC: that is the same port.
int v_stream_cdc_attach(v_stream_t *s, const char *name);
#endif
#if NAVHAL_CONFIG_DRV_UART_DMA
// A UART other than the console's: TX and circular RX both over DMA. The UART
// must already be initialised (hal_uart_init) by the board.
int v_stream_uart_attach(v_stream_t *s, const char *name, hal_uart_t uart);
#endif
#endif

#endif // VAIOS_DEVFS
#endif
