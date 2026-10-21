// NavHAL backends for byte streams -- see stream_hw.h and include/stream.h.
#include "stream_hw.h"

#if VAIOS_DEVFS && defined(NAVHAL)
#include "periph_bus.h" // v_dma_claim
#include "port.h"

// --- USB CDC ----------------------------------------------------------------
#if defined(NAVHAL) && NAVHAL_CONFIG_DRV_USB_CDC &&                            \
    !NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
static v_stream_t *cdc_stream; // NavHAL's RX callback takes no context

static void cdc_rx_isr(const uint8_t *data, uint16_t len) {
  if (cdc_stream)
    v_stream_rx_push_isr(cdc_stream, data, len);
}

// Waits for the host to take the data, HAL_ERR_TIMEOUT if it stops reading.
static int cdc_tx(v_stream_t *s, const uint8_t *p, uint32_t n) {
  (void)s;
  return hal_usb_cdc_write(p, (uint16_t)n) == HAL_OK ? 0 : -1;
}

static int cdc_up(v_stream_t *s) {
  (void)s;
  return hal_usb_cdc_connected();
}

int v_stream_cdc_attach(v_stream_t *s, const char *name) {
  if (!s || cdc_stream)
    return V_STREAM_EINVAL; // one USB port
  s->tx = cdc_tx;
  s->rx = NULL;
  s->up = cdc_up;
  int rc = v_stream_register(s, name);
  if (rc < 0)
    return rc;
  cdc_stream = s;
  if (hal_usb_cdc_init() != HAL_OK)
    return V_STREAM_EINVAL;
  hal_usb_cdc_set_rx_callback(cdc_rx_isr);
  return 0;
}
#endif

// --- UART over DMA ----------------------------------------------------------
#if defined(NAVHAL) && NAVHAL_CONFIG_DRV_UART_DMA
// ponytail: up to two UART streams (NavHAL's DMA callbacks take no context, so
// each needs its own trampoline); add one per extra UART.
static v_stream_t *uart_streams[2];
static void uart0_tx_done(void) { v_stream_tx_done_isr(uart_streams[0]); }
static void uart1_tx_done(void) { v_stream_tx_done_isr(uart_streams[1]); }
static void uart0_rx(void) { v_stream_rx_signal_isr(uart_streams[0]); }
static void uart1_rx(void) { v_stream_rx_signal_isr(uart_streams[1]); }
static void (*const uart_tx_cb[2])(void) = {uart0_tx_done, uart1_tx_done};
static void (*const uart_rx_cb[2])(void) = {uart0_rx, uart1_rx};

typedef struct {
  hal_uart_t uart;
  uint16_t rd; // next byte of the DMA ring to hand out
} uart_hw_t;
static uart_hw_t uart_hw[2];

static int uart_tx(v_stream_t *s, const uint8_t *p, uint32_t n) {
  uart_hw_t *u = (uart_hw_t *)s->hw;
  if (hal_uart_write_dma(u->uart, p, (uint16_t)n) != HAL_OK)
    return -1;
  // 100 ms covers 512 B at 57600 baud; past that the DMA IRQ is lost.
  return v_semaphore_take(s->tx_done, 100) == VA_PASS ? 0 : -1;
}

// The RX ring is the stream's rxq, filled by circular DMA; the DMA's write
// index is the producer. ponytail: an overrun (the DMA lapping the reader) is
// not detected; size V_STREAM_RX for the longest gap between reads.
static uint32_t uart_rx(v_stream_t *s, uint8_t *p, uint32_t n) {
  uart_hw_t *u = (uart_hw_t *)s->hw;
  uint16_t wr;
  if (hal_uart_dma_rx_index(u->uart, &wr) != HAL_OK)
    return 0;
  uint32_t got = 0;
  while (got < n && u->rd != wr) {
    uint16_t end = wr > u->rd ? wr : V_STREAM_RX; // up to the wrap
    uint32_t run = end - u->rd;
    if (run > n - got)
      run = n - got;
    hal_uart_dma_rx_sync(u->uart, u->rd, (uint16_t)run); // F7: drop stale lines
    for (uint32_t i = 0; i < run; i++)
      p[got + i] = s->rxq[u->rd + i];
    got += run;
    u->rd = (uint16_t)((u->rd + run) % V_STREAM_RX);
  }
  return got;
}

int v_stream_uart_attach(v_stream_t *s, const char *name, hal_uart_t uart) {
  int i = !uart_streams[0] ? 0 : !uart_streams[1] ? 1 : -1;
  hal_dma_binding_t tx, rx;
  if (!s || i < 0 || hal_uart_dma_get_binding(uart, true, &tx) != HAL_OK ||
      hal_uart_dma_get_binding(uart, false, &rx) != HAL_OK)
    return V_STREAM_EINVAL;
  if (v_dma_claim(tx.controller == HAL_DMA_CONTROLLER_1 ? 1 : 2,
                  (uint8_t)tx.stream, s) != VA_PASS ||
      v_dma_claim(rx.controller == HAL_DMA_CONTROLLER_1 ? 1 : 2,
                  (uint8_t)rx.stream, s) != VA_PASS)
    return V_STREAM_EBUSY; // another bus, or SDIO, has the stream
  uart_hw[i] = (uart_hw_t){.uart = uart};
  s->hw = &uart_hw[i];
  s->tx = uart_tx;
  s->rx = uart_rx;
  s->up = NULL;
  int rc = v_stream_register(s, name);
  if (rc < 0)
    return rc;
  uart_streams[i] = s;
  hal_interrupt_attach_callback(tx.irq, uart_tx_cb[i]);
  hal_uart_init_dma_rx(uart, s->rxq, V_STREAM_RX);
  hal_uart_dma_rx_attach_callback(uart, uart_rx_cb[i]);
  hal_uart_attach_idle_callback(uart, uart_rx_cb[i]);
  return 0;
}
#endif

#endif // VAIOS_DEVFS && NAVHAL
