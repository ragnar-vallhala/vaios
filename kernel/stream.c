// Byte streams as device nodes -- see include/stream.h.
#include "stream.h"

#if VAIOS_DEVFS
#include "port.h" // ENTER/EXIT_CRITICAL_FROM_ISR, v_port_trigger_pendsv
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

#endif // VAIOS_DEVFS
