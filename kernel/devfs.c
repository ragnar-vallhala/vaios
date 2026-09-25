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

#include "vfile.h"

#if VAIOS_DEVFS

#include "port.h"
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

// --- /dev/console -----------------------------------------------------------
static int console_write(void *priv, const void *buf, uint32_t len) {
  (void)priv;
  // Blocking write in NUL-terminated chunks via the port string facade — works
  // regardless of the DMA config. (Console output is text; embedded NULs, rare
  // here, would truncate a chunk.)
  const char *p = (const char *)buf;
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
    done += chunk;
  }
  return (int)len;
}

static int console_read(void *priv, void *buf, uint32_t len) {
  (void)priv;
  char *p = (char *)buf;
  for (uint32_t i = 0; i < len; i++)
    p[i] = v_port_hw_console_read_char(); // blocking per char
  return (int)len;
}

static const v_file_ops console_ops = {
    .read = console_read,
    .write = console_write,
    .close = NULL,
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
  v_devfs_register("/dev/console", &console_ops, NULL);
  v_devfs_register("/dev/kmsg", &kmsg_ops, NULL);
}

#endif // VAIOS_DEVFS
