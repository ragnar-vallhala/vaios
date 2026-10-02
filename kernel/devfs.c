/**
 * @file devfs.c
 * @brief Device filesystem + per-task fd table + file syscalls (Phase 3, Stage 2).
 *
 * A tiny object model: devices register /dev nodes carrying a v_file_ops table;
 * v_file_open looks a node up and allocates a descriptor in the calling task's
 * fd table; read/write/close dispatch through the op-table. The public entry
 * points use the same trap-once pattern as the Stage-1 syscalls (v_in_thread_mode):
 * a task traps via `svc`, and the same function reached from the dispatch runs
 * its body. Every task starts with fd 0/1/2 pre-opened to /dev/console.
 */

#include "port.h" // v_port_hw_console_* — the console device's hardware side
#include "vfile.h"

#if VAIOS_DEVFS

#include "ipc.h" // the console's RX wake (a binary semaphore)
#include "syscall.h"
#include "task.h"
#include "utils.h" // v_kmsg_read (/dev/kmsg backing ring)

// --- device registry --------------------------------------------------------
#define MAX_DEV_NODES 8
typedef struct {
  const char *name;
  const v_file_ops *ops;
  void *priv;
} dev_node_t;
static dev_node_t dev_nodes[MAX_DEV_NODES];
static int dev_node_count;

extern TCB *current_task;


int v_devfs_register(const char *name, const v_file_ops *ops, void *priv) {
  if (dev_node_count >= MAX_DEV_NODES)
    return -1;
  dev_nodes[dev_node_count].name = name;
  dev_nodes[dev_node_count].ops = ops;
  dev_nodes[dev_node_count].priv = priv;
  dev_node_count++;
  return 0;
}

// Exact match, or — for a node whose name ends in '/' — a prefix match, which
// is what makes a MOUNT: "/mnt/" claims every path under it. *tail is set to the
// rest of the path for a prefix match, and to "" for an exact one.
static dev_node_t *dev_find(const char *path, const char **tail) {
  for (int i = 0; i < dev_node_count; i++) {
    const char *n = dev_nodes[i].name, *p = path;
    while (*n && *n == *p) {
      n++;
      p++;
    }
    if (!*n && !*p) { // exact
      if (tail)
        *tail = "";
      return &dev_nodes[i];
    }
    // the whole node name matched and it is a mount prefix: the rest is the
    // path within the mount, and it must not be empty.
    if (!*n && *p && n != dev_nodes[i].name && n[-1] == '/') {
      if (tail)
        *tail = p;
      return &dev_nodes[i];
    }
  }
  return NULL;
}

// --- fd table ---------------------------------------------------------------
void v_fd_table_init(TCB *t) {
  for (int i = 0; i < VAIOS_MAX_FDS; i++) {
    t->fds[i].ops = NULL;
    t->fds[i].priv = NULL;
  }
  // Pre-open stdin/stdout/stderr -> /dev/console (registered at boot).
  //
  // The `fd < VAIOS_MAX_FDS` half looks always-true, and CodeQL says so, because
  // Kconfig now gives VAIOS_MAX_FDS `range 3 64` — three is the floor precisely
  // so this loop always fits. It stays anyway: the host test binaries set the
  // symbol with -DVAIOS_MAX_FDS=8 in tests/CMakeLists.txt, which is outside
  // Kconfig and therefore outside that range, so the bound is not provably dead
  // on every build path. It is one comparison against writing past t->fds[].
  dev_node_t *con = dev_find("/dev/console", NULL);
  if (con)
    for (int fd = 0; fd < 3 && fd < VAIOS_MAX_FDS; fd++) {
      t->fds[fd].ops = con->ops;
      t->fds[fd].priv = con->priv;
    }
}

static v_fd_entry *fd_lookup(int fd) {
  if (fd < 0 || fd >= VAIOS_MAX_FDS)
    return NULL;
  v_fd_entry *e = &current_task->fds[fd];
  return e->ops ? e : NULL;
}

int v_fd_alloc(const v_file_ops *ops, void *priv) {
  for (int fd = 0; fd < VAIOS_MAX_FDS; fd++) {
    if (current_task->fds[fd].ops == NULL) {
      current_task->fds[fd].ops = ops;
      current_task->fds[fd].priv = priv;
      return fd;
    }
  }
  return -1; // no free descriptor
}

void *v_fd_obj(int fd, const v_file_ops *ops) {
  v_fd_entry *e = fd_lookup(fd);
  if (!e || e->ops != ops) // type check: must be exactly this kind of object
    return NULL;
  return e->priv;
}

void v_fd_close_all(TCB *t) {
  for (int fd = 0; fd < VAIOS_MAX_FDS; fd++) {
    v_fd_entry *e = &t->fds[fd];
    if (!e->ops)
      continue;
    if (e->ops->close)
      e->ops->close(e->priv); // e.g. ipc_sem_close -> drop named-object refcount
    e->ops = NULL;
    e->priv = NULL;
  }
}

// --- file API (trap-once: task traps via svc; dispatch runs the body) -------
int v_file_open(const char *path, int flags) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_open, (uintptr_t)path, (uint32_t)flags);
#endif
  const char *tail = "";
  dev_node_t *node = dev_find(path, &tail);
  if (!node)
    return -1; // no such device
  if (!node->ops->open) {
    (void)flags;
    return v_fd_alloc(node->ops, node->priv); // a device: one shared priv
  }
  void *priv = NULL; // a mount: per-open state, from the node itself
  int r = node->ops->open(tail, flags, &priv);
  if (r < 0)
    return r;
  int fd = v_fd_alloc(node->ops, priv);
  if (fd < 0 && node->ops->close)
    node->ops->close(priv); // no descriptor left: don't leak the open file
  return fd;
}

int v_file_write(int fd, const void *buf, uint32_t len) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_write, (uint32_t)fd, (uintptr_t)buf, len);
#endif
  v_fd_entry *e = fd_lookup(fd);
  if (!e || !e->ops->write)
    return -1;
  return e->ops->write(e->priv, buf, len);
}

int v_file_read(int fd, void *buf, uint32_t len) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_read, (uint32_t)fd, (uintptr_t)buf, len);
#endif
  v_fd_entry *e = fd_lookup(fd);
  if (!e || !e->ops->read)
    return -1;
  return e->ops->read(e->priv, buf, len);
}

int v_file_wait(int fd, uint32_t ticks) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_file_wait, (uint32_t)fd, ticks);
#endif
  v_fd_entry *e = fd_lookup(fd);
  if (!e || !e->ops->wait)
    return -1;
  return e->ops->wait(e->priv, ticks);
}

int v_file_close(int fd) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_close, (uint32_t)fd);
#endif
  v_fd_entry *e = fd_lookup(fd);
  if (!e)
    return -1;
  int r = e->ops->close ? e->ops->close(e->priv) : 0;
  e->ops = NULL;
  e->priv = NULL;
  return r;
}

// --- /dev/console output ----------------------------------------------------
// Inside a syscall the console hardware cannot be driven: SVCall runs at the
// ceiling and masks the USB interrupt that completes a CDC transfer, so
// hal_usb_cdc_write times out with bytes unsent ("help" echoed as "hl", todo
// 163). So once a writer task exists, a write only queues here and wakes it;
// v_console_writer_step does the hardware write in thread mode. Same split as
// the VFS worker, and like it the application runs the writer, at a priority
// above the tasks that print -- then the wake preempts on syscall return and
// output comes out in the order it was written.
//
// Until a writer has run, writes go straight to the hardware as they always
// did: before the scheduler, in examples that never start one, and on routes
// where a direct write is fine.
#ifndef CONSOLE_TX_SIZE
#define CONSOLE_TX_SIZE 256
#endif
static char con_tx[CONSOLE_TX_SIZE];
static volatile uint16_t con_tx_head, con_tx_tail;
static StaticSemaphore_t con_tx_sem_store;
static SemaphoreHandle_t con_tx_sem;
static volatile uint8_t con_writer_seen;

// Write to the hardware, in NUL-terminated chunks via the port string facade --
// works regardless of the DMA config. (Console output is text; embedded NULs,
// rare here, would truncate a chunk.)
static void console_emit(const char *p, uint32_t len) {
  char tmp[128];
  uint32_t done = 0;
  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > sizeof(tmp) - 1)
      chunk = sizeof(tmp) - 1;
    for (uint32_t i = 0; i < chunk; i++)
      tmp[i] = p[done + i];
    tmp[chunk] = '\0';
    v_port_hw_console_write_string(tmp);
#if VAIOS_CONSOLE_TO_KMSG
    // Mirror into the kernel log ring as well, so console output is recoverable
    // from RAM: readable through /dev/kmsg, and — the reason this exists — over
    // SWD after the fact on a board whose debug probe has no serial port, or
    // post-mortem after a fault took the UART with it. Costs one memcpy into a
    // ring; off by default.
    v_kmsg_append(tmp, chunk);
#endif
    done += chunk;
  }
}

// Newlines: a lone '\n' goes out as "\r\n", as a Unix tty does (ONLCR) --
// otherwise a file with LF endings printed by `cat` walks right across the
// screen, each line starting under the end of the last. A '\n' that already
// follows a '\r' passes as it is. con_last carries that across writes.
static char con_last;

// Returns how much of `buf` was taken, which is less than `len` when the queue
// is full; the caller retries the rest (v_printf does) rather than the syscall
// waiting for space. A '\n' is taken only with room for its '\r' too.
static int console_write(void *priv, const void *buf, uint32_t len) {
  (void)priv;
  const char *p = (const char *)buf;
  if (!con_writer_seen) {
    char tmp[64];
    uint32_t t = 0;
    for (uint32_t i = 0; i < len; i++) {
      if (p[i] == '\n' && con_last != '\r')
        tmp[t++] = '\r';
      tmp[t++] = p[i];
      con_last = p[i];
      if (t >= sizeof tmp - 1) {
        console_emit(tmp, t);
        t = 0;
      }
    }
    if (t)
      console_emit(tmp, t);
    return (int)len;
  }
  uint32_t n = 0;
  for (; n < len; n++) {
    uint32_t room = (uint32_t)((con_tx_tail + CONSOLE_TX_SIZE - con_tx_head - 1u) %
                               CONSOLE_TX_SIZE);
    int crlf = p[n] == '\n' && con_last != '\r';
    if (room < (crlf ? 2u : 1u))
      break;
    if (crlf) {
      con_tx[con_tx_head] = '\r';
      con_tx_head = (uint16_t)((con_tx_head + 1u) % CONSOLE_TX_SIZE);
    }
    con_tx[con_tx_head] = p[n];
    con_tx_head = (uint16_t)((con_tx_head + 1u) % CONSOLE_TX_SIZE);
    con_last = p[n];
  }
  if (n) {
    int woke = 0;
    (void)v_semaphore_give_from_isr(con_tx_sem, &woke);
    if (woke) // the writer outranks us: let it drain on syscall return
      v_port_trigger_pendsv();
  }
  return (int)n;
}

int v_console_writer_step(uint32_t ticks) {
  con_writer_seen = 1;
  if (con_tx_tail == con_tx_head &&
      v_semaphore_take(con_tx_sem, ticks) != VA_PASS)
    return 0;
  int total = 0;
  while (con_tx_tail != con_tx_head) {
    char tmp[64];
    uint32_t n = 0;
    uint16_t t = con_tx_tail;
    while (n < sizeof tmp && t != con_tx_head) {
      tmp[n++] = con_tx[t];
      t = (uint16_t)((t + 1u) % CONSOLE_TX_SIZE);
    }
    console_emit(tmp, n);
    con_tx_tail = t; // free the space only once it is on the wire
    total += (int)n;
  }
  return total;
}

/* --- console input ----------------------------------------------------------
 * The port's RX interrupt (UART RXNE, or the CDC driver's rx callback) hands
 * bytes to console_rx_sink, which queues them here, in KERNEL memory, and wakes
 * any reader. A reader never touches the hardware: read is non-blocking and
 * drains this ring; v_file_wait parks the caller until the sink signals. That
 * is what lets an unprivileged task read the console without polling it.
 *
 * Single producer (the RX ISR) and single consumer (read, inside the syscall).
 * On target the syscall runs at the ceiling and masks the console IRQ anyway,
 * so the two never actually interleave; the volatile indices are for the
 * no-SVC builds where read runs in thread mode and the ISR can cut in.
 *
 * A port with no RX interrupt (semihosting, host, a UART build whose NavHAL has
 * no USART vectors) reports so, and the console falls back to polling the
 * port's try_read: read polls it directly and wait sleeps one poll interval
 * instead of waiting for a signal nobody will send.
 */
#ifndef CONSOLE_RX_SIZE
#define CONSOLE_RX_SIZE 128 /* holds a pasted line at 115200 with room to spare */
#endif
#define CONSOLE_POLL_TICKS MS_TO_TICKS(10)

static uint8_t con_rx[CONSOLE_RX_SIZE];
static volatile uint16_t con_head, con_tail;
static StaticSemaphore_t con_sem_store;
static SemaphoreHandle_t con_rx_sem;
static int con_rx_armed;

/* ISR context. A full ring drops the rest of the burst: losing input is better
   than an interrupt that waits for a reader. */
static void console_rx_sink(const uint8_t *data, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((con_head + 1u) % CONSOLE_RX_SIZE);
    if (next == con_tail)
      break;
    con_rx[con_head] = data[i];
    con_head = next;
  }
  int woke = 0;
  if (con_rx_sem)
    (void)v_semaphore_give_from_isr(con_rx_sem, &woke);
  if (woke) // the reader outranks what we interrupted: switch on return
    v_port_trigger_pendsv();
}

static int console_read(void *priv, void *buf, uint32_t len) {
  (void)priv;
  uint8_t *p = (uint8_t *)buf;
  uint32_t got = 0;
  while (got < len && con_tail != con_head) {
    p[got++] = con_rx[con_tail];
    con_tail = (uint16_t)((con_tail + 1u) % CONSOLE_RX_SIZE);
  }
  if (!con_rx_armed) {
    char c;
    while (got < len && v_port_hw_console_try_read(&c))
      p[got++] = (uint8_t)c;
  }
  return (int)got;
}

static int console_wait(void *priv, uint32_t ticks) {
  (void)priv;
  if (con_tail != con_head)
    return 0; /* already there: don't sleep on a signal already consumed */
  if (!con_rx_sem)
    return -1;
  if (!con_rx_armed && ticks > CONSOLE_POLL_TICKS)
    ticks = CONSOLE_POLL_TICKS; /* nothing will signal: wake up and re-poll */
  return v_semaphore_take(con_rx_sem, ticks) == VA_PASS ? 0 : -1;
}

static const v_file_ops console_ops = {
    .read = console_read,
    .write = console_write,
    .close = NULL,
    .wait = console_wait,
};

// --- /dev/kmsg (read-only kernel log ring) ----------------------------------
static int kmsg_dev_read(void *priv, void *buf, uint32_t len) {
  (void)priv;
  return v_kmsg_read((char *)buf, len); // non-blocking: 0 if nothing new
}

static const v_file_ops kmsg_ops = {
    .read = kmsg_dev_read,
    .write = NULL, // read-only
    .close = NULL,
};

void v_devfs_init(void) {
  con_head = con_tail = 0;
  con_tx_head = con_tx_tail = 0;
  con_rx_sem = v_semaphore_create_binary_static(&con_sem_store);
  con_tx_sem = v_semaphore_create_binary_static(&con_tx_sem_store);
  con_rx_armed = v_port_hw_console_rx_irq_init(console_rx_sink);
  v_devfs_register("/dev/console", &console_ops, NULL);
  v_devfs_register("/dev/kmsg", &kmsg_ops, NULL);
}

#endif // VAIOS_DEVFS
