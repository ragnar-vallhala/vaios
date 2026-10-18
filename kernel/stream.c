// Byte streams as device nodes -- see include/stream.h.
#include "stream.h"

#if VAIOS_DEVFS
#include "port.h" // ENTER/EXIT_CRITICAL_FROM_ISR, v_port_trigger_pendsv
#include "periph_bus.h" // v_dma_claim
#include "vfile.h"
#include "vfs.h" // VFS_O_RDONLY

static v_stream_t *streams[V_STREAM_MAX];

static void give_isr(SemaphoreHandle_t sem) {
  int woken = 0;
  v_semaphore_give_from_isr(sem, &woken);
  if (woken)
    v_port_trigger_pendsv();
}

static int is_up(v_stream_t *s) { return !s->up || s->up(s); }

// The reader's priv is the stream with its low bit set (streams are aligned),
// so close can tell the reader's fd from a writer's.
#define READER(s) ((void *)((uintptr_t)(s) | 1u))
static v_stream_t *untag(void *priv) {
  return (v_stream_t *)((uintptr_t)priv & ~(uintptr_t)1u);
}

static int name_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int stream_open(const char *path, int flags, void **priv_out) {
  v_stream_t *s = NULL;
  for (int i = 0; i < V_STREAM_MAX && !s; i++)
    if (streams[i] && name_eq(streams[i]->name, path))
      s = streams[i];
  if (!s)
    return V_STREAM_EINVAL;
  if (!(flags & VFS_O_RDONLY)) { // write-only
    *priv_out = s;
    return 0;
  }
  uint32_t c = ENTER_CRITICAL_FROM_ISR();
  int busy = s->reader;
  s->reader = 1;
  EXIT_CRITICAL_FROM_ISR(c);
  if (busy)
    return V_STREAM_EBUSY;
  *priv_out = READER(s);
  return 0;
}

static int stream_close(void *priv) {
  if ((uintptr_t)priv & 1u)
    untag(priv)->reader = 0;
  return 0;
}

// Takes what fits and returns that; the caller retries the rest. While the
// far end is down everything is taken and dropped.
static int stream_write(void *priv, const void *buf, uint32_t len) {
  v_stream_t *s = untag(priv);
  const uint8_t *p = (const uint8_t *)buf;
  if (!is_up(s)) {
    s->dropped += len;
    return (int)len;
  }
  uint32_t c = ENTER_CRITICAL_FROM_ISR(); // writers share the queue
  uint32_t head = s->tx_head;
  uint32_t room = (s->tx_tail + V_STREAM_TX - head - 1u) % V_STREAM_TX;
  uint32_t n = len < room ? len : room;
  for (uint32_t i = 0; i < n; i++)
    s->txq[(head + i) % V_STREAM_TX] = p[i];
  s->tx_head = (uint16_t)((head + n) % V_STREAM_TX);
  EXIT_CRITICAL_FROM_ISR(c);
  if (n)
    give_isr(s->kick);
  return (int)n;
}

static int stream_read(void *priv, void *buf, uint32_t len) {
  if (!((uintptr_t)priv & 1u))
    return -1; // opened write-only
  v_stream_t *s = untag(priv);
  if (s->rx)
    return (int)s->rx(s, (uint8_t *)buf, len);
  uint8_t *p = (uint8_t *)buf;
  uint32_t n = 0;
  uint32_t tail = s->rx_tail;
  while (n < len && tail != s->rx_head) {
    p[n++] = s->rxq[tail];
    tail = (tail + 1u) % V_STREAM_RX;
  }
  s->rx_tail = (uint16_t)tail;
  return (int)n;
}

static int stream_wait(void *priv, uint32_t ticks) {
  if (!((uintptr_t)priv & 1u))
    return -1;
  v_stream_t *s = untag(priv);
  if (!s->rx && s->rx_tail != s->rx_head)
    return 0;
  return v_semaphore_take(s->rx_ready, ticks) == VA_PASS ? 0 : -1;
}

static const v_file_ops stream_ops = {.read = stream_read,
                                      .write = stream_write,
                                      .close = stream_close,
                                      .open = stream_open,
                                      .wait = stream_wait};

int v_stream_register(v_stream_t *s, const char *name) {
  if (!s || !name || !s->tx)
    return V_STREAM_EINVAL;
  int slot = -1;
  for (int i = 0; i < V_STREAM_MAX; i++) {
    if (streams[i] == s)
      return V_STREAM_EINVAL; // registered twice
    if (!streams[i] && slot < 0)
      slot = i;
  }
  if (slot < 0)
    return V_STREAM_EBUSY;
  s->name = name;
  s->kick = v_semaphore_create_binary_static(&s->kick_buf);
  s->rx_ready = v_semaphore_create_binary_static(&s->rx_buf);
  s->tx_done = v_semaphore_create_binary_static(&s->done_buf);
  int rc = v_devfs_register(name, &stream_ops, s);
  if (rc < 0)
    return rc;
  streams[slot] = s;
  return 0;
}

void v_stream_writer_step(v_stream_t *s, uint32_t ticks) {
  if (s->tx_head == s->tx_tail &&
      v_semaphore_take(s->kick, ticks) != VA_PASS)
    return;
  // One contiguous run per tx(): the queue's wrap point ends it. The bytes stay
  // in place, and writers only ever add at the head, until tx() returns.
  uint32_t tail = s->tx_tail, head = s->tx_head;
  uint32_t n = (head >= tail ? head : V_STREAM_TX) - tail;
  if (!n)
    return;
  if (!is_up(s))
    s->dropped += n;
  else if (s->tx(s, &s->txq[tail], n) < 0)
    s->tx_errors++;
  s->tx_tail = (uint16_t)((tail + n) % V_STREAM_TX);
}

void v_stream_rx_push_isr(v_stream_t *s, const uint8_t *p, uint32_t n) {
  uint32_t head = s->rx_head;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t next = (head + 1u) % V_STREAM_RX;
    if (next == s->rx_tail) {
      s->rx_dropped += n - i;
      break;
    }
    s->rxq[head] = p[i];
    head = next;
  }
  s->rx_head = (uint16_t)head;
  give_isr(s->rx_ready);
}

void v_stream_rx_signal_isr(v_stream_t *s) { give_isr(s->rx_ready); }

void v_stream_tx_done_isr(v_stream_t *s) { give_isr(s->tx_done); }

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

#endif // VAIOS_DEVFS
