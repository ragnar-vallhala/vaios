#include "vfs.h"
#include "ipc.h"
#include "utils/v_fs.h"

static MutexHandle_t vfs_mutex = NULL;

static void vfs_lock(void) {
  if (vfs_mutex != NULL) {
    v_mutex_lock(vfs_mutex, 0xFFFFFFFF);
  }
}

static void vfs_unlock(void) {
  if (vfs_mutex != NULL) {
    v_mutex_unlock(vfs_mutex);
  }
}

int vfs_init(void) {
  if (vfs_mutex == NULL) {
    vfs_mutex = v_mutex_create();
    if (vfs_mutex == NULL) {
      return -1;
    }
  }

  /* Do not lock here!
   * vfs_init is called during system bootstrap before the scheduler is running.
   * v_mutex_lock blocks indefinitely without a running scheduler.
   */
  return v_fs_init();
}

vfs_fd_t vfs_open(const char *path, int flags) {
  vfs_lock();
  vfs_fd_t fd = (vfs_fd_t)v_open(path, flags);
  vfs_unlock();
  return fd;
}

int vfs_close(vfs_fd_t fd) {
  vfs_lock();
  int res = v_close((v_fd_t)fd);
  vfs_unlock();
  return res;
}

int vfs_read(vfs_fd_t fd, void *buf, size_t count) {
  vfs_lock();
  int res = v_read((v_fd_t)fd, buf, count);
  vfs_unlock();
  return res;
}

int vfs_write(vfs_fd_t fd, const void *buf, size_t count) {
  vfs_lock();
  int res = v_write((v_fd_t)fd, buf, count);
  vfs_unlock();
  return res;
}

long vfs_lseek(vfs_fd_t fd, long offset, int whence) {
  vfs_lock();
  long res = v_lseek((v_fd_t)fd, offset, whence);
  vfs_unlock();
  return res;
}

int vfs_mkdir(const char *path) {
  vfs_lock();
  int res = v_mkdir(path);
  vfs_unlock();
  return res;
}

int vfs_unlink(const char *path) {
  vfs_lock();
  int res = v_unlink(path);
  vfs_unlock();
  return res;
}

int vfs_sync(vfs_fd_t fd) {
  vfs_lock();
  int res = v_sync((v_fd_t)fd);
  vfs_unlock();
  return res;
}

int vfs_preallocate(const char *path, uint32_t size) {
  vfs_lock();
  int res = v_preallocate(path, size);
  vfs_unlock();
  return res;
}

long vfs_size(vfs_fd_t fd) {
  vfs_lock();
  long res = v_lseek((v_fd_t)fd, 0, VFS_SEEK_END);
  vfs_unlock();
  return res;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
  if (st == NULL) {
    return -1;
  }
  v_stat_t vs;
  vfs_lock();
  int res = v_stat(path, &vs);
  vfs_unlock();
  st->exists = vs.exists;
  if (res != 0) {
    return res;
  }
  st->size = vs.size;
  st->mtime = vs.mtime;
  st->is_dir = vs.is_dir;
  return 0;
}

vfs_dir_t vfs_opendir(const char *path) {
  vfs_lock();
  vfs_dir_t d = (vfs_dir_t)v_opendir(path);
  vfs_unlock();
  return d;
}

int vfs_readdir(vfs_dir_t d, vfs_dirent_t *ent) {
  if (ent == NULL) {
    return -1;
  }
  v_dirent_t ve;
  vfs_lock();
  int res = v_readdir((v_dir_t)d, &ve);
  vfs_unlock();
  if (res == 1) {
    for (int i = 0; i < VFS_NAME_MAX; i++) {
      ent->name[i] = ve.name[i];
    }
    ent->size = ve.size;
    ent->is_dir = ve.is_dir;
  }
  return res;
}

int vfs_closedir(vfs_dir_t d) {
  vfs_lock();
  int res = v_closedir((v_dir_t)d);
  vfs_unlock();
  return res;
}

// ---------------------------------------------------------------------------
// The VFS on the fd table (M5). It mounts itself as a devfs node, so a task
// reaches a file through the file syscalls it already has: v_file_open on a path
// under the mount prefix returns an fd whose reads and writes land here. Only
// the operations with no fd equivalent (seek, stat, mkdir, unlink, sync,
// directory listing) needed syscalls of their own.
//
// Every vfs_* call below runs privileged, from the syscall dispatch, so the
// FatFs mutex is taken by the kernel on the caller's behalf and no raw handle
// ever reaches a task.
// ---------------------------------------------------------------------------
#if VAIOS_DEVFS && VAIOS_MODULE_VFS

#include "port.h" // ENTER_CRITICAL_FROM_ISR
#include "syscall.h"
#include "vfile.h"

typedef struct {
  vfs_fd_t file; // >= 0 when this handle is an open file
  vfs_dir_t dir; // >= 0 when it is an open directory
  uint8_t used;
  // For a task's handle (the worker path): who owns it, so a task that dies with
  // a file open gets it CLOSED — and therefore flushed — rather than leaked. A
  // handle opened by privileged code through the devfs mount has no owner.
  TCB *owner;
} vfs_handle_t;

static vfs_handle_t vfs_handles[VAIOS_VFS_MAX_OPEN];
static const char *vfs_mount_prefix; // NULL until v_vfs_mount

static vfs_handle_t *vfs_handle_alloc(void) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_VFS_MAX_OPEN; i++)
    if (!vfs_handles[i].used) {
      vfs_handles[i].used = 1;
      vfs_handles[i].file = -1;
      vfs_handles[i].dir = -1;
      vfs_handles[i].owner = 0;
      EXIT_CRITICAL_FROM_ISR(s);
      return &vfs_handles[i];
    }
  EXIT_CRITICAL_FROM_ISR(s);
  return 0;
}

static int vfs_fd_read(void *priv, void *buf, uint32_t len) {
  vfs_handle_t *h = (vfs_handle_t *)priv;
  if (!h || h->file < 0)
    return V_VFS_EINVAL; // a directory handle has nothing to read this way
  return vfs_read(h->file, buf, len);
}

static int vfs_fd_write(void *priv, const void *buf, uint32_t len) {
  vfs_handle_t *h = (vfs_handle_t *)priv;
  if (!h || h->file < 0)
    return V_VFS_EINVAL;
  return vfs_write(h->file, buf, len);
}

static int vfs_fd_close(void *priv) {
  vfs_handle_t *h = (vfs_handle_t *)priv;
  if (!h)
    return V_VFS_EINVAL;
  int r = 0;
  if (h->file >= 0)
    r = vfs_close(h->file);
  else if (h->dir >= 0)
    r = vfs_closedir(h->dir);
  h->file = h->dir = -1;
  h->owner = 0;
  h->used = 0;
  return r;
}

static int vfs_fd_open(const char *tail, int flags, void **priv_out) {
  if (!tail || !*tail)
    return V_VFS_EINVAL; // the mount point itself is not a file
  vfs_handle_t *h = vfs_handle_alloc();
  if (!h)
    return V_VFS_EBUSY;
  vfs_fd_t f = vfs_open(tail, flags);
  if (f < 0) {
    h->used = 0;
    return (int)f; // the filesystem's own error, not ours
  }
  h->file = f;
  *priv_out = h;
  return 0;
}

static const v_file_ops vfs_mount_ops = {.read = vfs_fd_read,
                                         .write = vfs_fd_write,
                                         .close = vfs_fd_close,
                                         .open = vfs_fd_open};

int v_vfs_mount(const char *prefix) {
  if (!prefix || !*prefix)
    return V_VFS_EINVAL;
  const char *p = prefix;
  while (*p)
    p++;
  if (p[-1] != '/') // a mount claims a subtree, so it must end in '/'
    return V_VFS_EINVAL;
  if (v_devfs_register(prefix, &vfs_mount_ops, NULL) != 0)
    return V_VFS_EINVAL;
  vfs_mount_prefix = prefix;
  return VA_PASS;
}

// A path a task passed in is relative to the mount: strip the prefix if it gave
// the full one, so both "/mnt/log.csv" and "log.csv" work the way v_file_open's
// tail does.
static const char *vfs_strip_prefix(const char *path) {
  const char *m = vfs_mount_prefix, *p = path;
  if (!m)
    return path;
  while (*m && *m == *p) {
    m++;
    p++;
  }
  return *m ? path : p;
}


// --- what the worker actually calls -----------------------------------------
// Handle indices, not fd-table descriptors: a task's file handle must not be a
// devfs fd, because devfs close() would run f_close (which writes) inside the
// syscall handler — the very thing this worker exists to avoid.
static int vfs_handle_valid(int idx) {
  return idx >= 0 && idx < VAIOS_VFS_MAX_OPEN && vfs_handles[idx].used;
}

static int vfs_fd_open_internal(const char *path, int flags) {
  vfs_handle_t *h = vfs_handle_alloc();
  if (!h)
    return V_VFS_EBUSY;
  vfs_fd_t f = vfs_open(vfs_strip_prefix(path), flags);
  if (f < 0) {
    h->used = 0;
    return (int)f;
  }
  h->file = f;
  return (int)(h - vfs_handles);
}

static int vfs_fd_close_internal(int idx) {
  if (!vfs_handle_valid(idx))
    return V_VFS_EINVAL;
  return vfs_fd_close(&vfs_handles[idx]); // closes file or dir, frees the slot
}

static int vfs_fd_read_internal(int idx, void *buf, uint32_t len) {
  if (!vfs_handle_valid(idx) || vfs_handles[idx].file < 0)
    return V_VFS_EINVAL;
  return vfs_read(vfs_handles[idx].file, buf, len);
}

static int vfs_fd_write_internal(int idx, const void *buf, uint32_t len) {
  if (!vfs_handle_valid(idx) || vfs_handles[idx].file < 0)
    return V_VFS_EINVAL;
  return vfs_write(vfs_handles[idx].file, buf, len);
}

static long vfs_fd_seek_internal(int idx, long off, int whence) {
  if (!vfs_handle_valid(idx) || vfs_handles[idx].file < 0)
    return V_VFS_EINVAL;
  return vfs_lseek(vfs_handles[idx].file, off, whence);
}

static int vfs_fd_flush_internal(int idx) {
  if (!vfs_handle_valid(idx) || vfs_handles[idx].file < 0)
    return V_VFS_EINVAL;
  return vfs_sync(vfs_handles[idx].file);
}

static int vfs_dir_open_internal(const char *path) {
  vfs_handle_t *h = vfs_handle_alloc();
  if (!h)
    return V_VFS_EBUSY;
  vfs_dir_t d = vfs_opendir(vfs_strip_prefix(path));
  if (d < 0) {
    h->used = 0;
    return (int)d;
  }
  h->dir = d;
  return (int)(h - vfs_handles);
}

static int vfs_dir_next_internal(int idx, vfs_dirent_t *ent) {
  if (!vfs_handle_valid(idx) || vfs_handles[idx].dir < 0)
    return V_VFS_EINVAL;
  return vfs_readdir(vfs_handles[idx].dir, ent);
}

// ---------------------------------------------------------------------------
// The I/O worker (todo 67). Everything above this line runs the filesystem
// inline, which is correct for a PRIVILEGED caller in thread mode — interrupts
// are enabled, so an SDIO completion can be delivered. It is not correct inside
// a syscall: SVCall runs at priority 0 and masks the very interrupt the transfer
// is waiting for, so the call never returns and takes the scheduler with it.
//
// Below, a task's file call becomes submit / wait / finish, with the transfer
// done by a worker task in thread mode. Same split as v_pbus_xfer, same reason.
// ---------------------------------------------------------------------------

typedef enum {
  VFS_OP_OPEN = 1,
  VFS_OP_CLOSE,
  VFS_OP_READ,
  VFS_OP_WRITE,
  VFS_OP_SEEK,
  VFS_OP_FLUSH,
  VFS_OP_STAT,
  VFS_OP_MKDIR,
  VFS_OP_REMOVE,
  VFS_OP_DIROPEN,
  VFS_OP_DIRNEXT,
  VFS_OP_SIZE,
  VFS_OP_PREALLOC,
} vfs_op_t;

typedef struct {
  uint8_t used;      // slot allocated
  uint8_t queued;    // waiting for the worker to pick it up
  uint8_t op;
  int fd;
  uint32_t len;
  long offset;
  int whence;
  char path[64];
  uint8_t buf[VAIOS_VFS_IO_BUF];
  int32_t result;             // what the operation returned
  uint32_t out_len;           // bytes in buf for the caller to collect
  TCB *owner;                 // who is waiting (NULL once abandoned)
  SemaphoreHandle_t done;
  StaticSemaphore_t done_store;
} vfs_req_t;

static vfs_req_t vfs_reqs[VAIOS_VFS_IO_SLOTS];
static SemaphoreHandle_t vfs_work_sem; // counts queued requests
static StaticSemaphore_t vfs_work_store;
static uint8_t vfs_worker_seen; // a worker has run at least once

static void vfs_worker_init_once(void) {
  if (!vfs_work_sem)
    vfs_work_sem = v_semaphore_create_counting_static(
        VAIOS_VFS_IO_SLOTS, 0, &vfs_work_store);
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS; i++)
    if (!vfs_reqs[i].done)
      vfs_reqs[i].done =
          v_semaphore_create_binary_static(&vfs_reqs[i].done_store);
}

// Copy a NUL-terminated path into the slot. The dispatch has already checked it
// is the caller's to read; this bounds it.
static int vfs_copy_path(vfs_req_t *r, const char *path) {
  if (!path)
    return V_VFS_EINVAL;
  uint32_t i = 0;
  for (; i + 1u < sizeof r->path && path[i]; i++)
    r->path[i] = path[i];
  if (path[i]) // longer than a slot can hold: refuse rather than truncate
    return V_VFS_EINVAL;
  r->path[i] = 0;
  return VA_PASS;
}

int v_vfs_submit(const v_vfs_desc_t *d) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_vfs_submit, (uintptr_t)d);
#endif
  if (!d || !d->op)
    return V_VFS_EINVAL;
  if (!vfs_worker_seen)
    return V_VFS_EAGAIN; // nobody would ever service it
  if (d->len > VAIOS_VFS_IO_BUF)
    return V_VFS_EINVAL; // the caller's wrapper is meant to chunk

  vfs_worker_init_once();
  vfs_req_t *r = 0;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS && !r; i++)
    if (!vfs_reqs[i].used) {
      r = &vfs_reqs[i];
      r->used = 1;
    }
  EXIT_CRITICAL_FROM_ISR(s);
  if (!r)
    return V_VFS_EBUSY;

  r->op = d->op;
  r->fd = d->fd;
  r->len = d->len;
  r->offset = d->offset;
  r->whence = d->whence;
  r->result = V_VFS_EINVAL;
  r->out_len = 0;
  r->owner = get_current_task();
  r->path[0] = 0;

  int need_path = d->op == VFS_OP_OPEN || d->op == VFS_OP_STAT ||
                  d->op == VFS_OP_MKDIR || d->op == VFS_OP_REMOVE ||
                  d->op == VFS_OP_DIROPEN || d->op == VFS_OP_PREALLOC;
  if (need_path && vfs_copy_path(r, d->path) != VA_PASS) {
    r->used = 0;
    return V_VFS_EINVAL;
  }
  if (d->op == VFS_OP_WRITE) { // the payload crosses now, not later
    if (!d->data) {
      r->used = 0;
      return V_VFS_EINVAL;
    }
    for (uint32_t i = 0; i < d->len; i++)
      r->buf[i] = ((const uint8_t *)d->data)[i];
  }

  // Drain any signal left on this slot by a previous request: a binary semaphore
  // banks one, and a stale give would let wait() return before the worker had
  // done anything.
  while (v_semaphore_take(r->done, 0) == VA_PASS)
    ;
  r->queued = 1;
  v_semaphore_give(vfs_work_sem); // wake the worker
  return (int)(r - vfs_reqs);     // slot id
}

int v_vfs_wait(int slot, uint32_t ticks) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_vfs_wait, (uint32_t)slot, ticks);
#endif
  if (slot < 0 || slot >= VAIOS_VFS_IO_SLOTS || !vfs_reqs[slot].used)
    return V_VFS_EINVAL;
  if (vfs_reqs[slot].owner != get_current_task())
    return V_VFS_EINVAL; // not yours to wait on
  return v_semaphore_take(vfs_reqs[slot].done, ticks);
}

int v_vfs_finish(int slot, void *out, uint32_t cap) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_vfs_finish, (uint32_t)slot, (uintptr_t)out, cap);
#endif
  if (slot < 0 || slot >= VAIOS_VFS_IO_SLOTS || !vfs_reqs[slot].used)
    return V_VFS_EINVAL;
  vfs_req_t *r = &vfs_reqs[slot];
  if (r->owner != get_current_task())
    return V_VFS_EINVAL;
  int32_t res = r->result;
  // An open that succeeded hands this task a handle. Record the owner HERE, in
  // the caller's own context, so teardown knows whose files to close.
  if (res >= 0 && (r->op == VFS_OP_OPEN || r->op == VFS_OP_DIROPEN) &&
      vfs_handle_valid((int)res))
    vfs_handles[res].owner = get_current_task();
  uint32_t n = r->out_len < cap ? r->out_len : cap;
  if (n && out)
    for (uint32_t i = 0; i < n; i++)
      ((uint8_t *)out)[i] = r->buf[i];
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  r->owner = 0;
  r->queued = 0;
  r->used = 0;
  EXIT_CRITICAL_FROM_ISR(s);
  return (int)res;
}

// The worker: thread mode, privileged, interrupts enabled — which is the whole
// point. One request per call, at whatever priority the application gave it.
int v_vfs_worker_step(uint32_t ticks) {
  vfs_worker_init_once();
  vfs_worker_seen = 1;
  if (v_semaphore_take(vfs_work_sem, ticks) != VA_PASS)
    return 0; // nothing queued

  vfs_req_t *r = 0;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS && !r; i++)
    if (vfs_reqs[i].used && vfs_reqs[i].queued) {
      r = &vfs_reqs[i];
      r->queued = 0; // ours now
    }
  EXIT_CRITICAL_FROM_ISR(s);
  if (!r)
    return 0; // the semaphore counted a request that has since gone away

  int32_t res = V_VFS_EINVAL;
  uint32_t out = 0;
  switch (r->op) {
  case VFS_OP_OPEN:
    res = vfs_fd_open_internal(r->path, (int)r->len);
    break;
  case VFS_OP_CLOSE:
    res = vfs_fd_close_internal(r->fd);
    break;
  case VFS_OP_READ: {
    int n = vfs_fd_read_internal(r->fd, r->buf, r->len);
    res = n;
    out = n > 0 ? (uint32_t)n : 0u;
    break;
  }
  case VFS_OP_WRITE:
    res = vfs_fd_write_internal(r->fd, r->buf, r->len);
    break;
  case VFS_OP_SEEK:
    res = (int32_t)vfs_fd_seek_internal(r->fd, r->offset, r->whence);
    break;
  case VFS_OP_FLUSH:
    res = vfs_fd_flush_internal(r->fd);
    break;
  case VFS_OP_SIZE: {
    /* Note this does NOT leave the cursor at the end, unlike the privileged
       vfs_size (which is lseek-to-END and whose test pins that). A task asking
       how big a file is has not asked to move its own read position, and
       finding out that it had would be a bad afternoon. Save, measure,
       restore. */
    long cur = vfs_fd_seek_internal(r->fd, 0, VFS_SEEK_CUR);
    if (cur < 0) {
      res = (int32_t)cur;
      break;
    }
    long end = vfs_fd_seek_internal(r->fd, 0, VFS_SEEK_END);
    if (end < 0) {
      res = (int32_t)end;
      break;
    }
    long back = vfs_fd_seek_internal(r->fd, cur, VFS_SEEK_SET);
    res = back < 0 ? (int32_t)back : (int32_t)end;
    break;
  }
  case VFS_OP_PREALLOC:
    res = vfs_preallocate(vfs_strip_prefix(r->path), r->len);
    break;
  case VFS_OP_STAT: {
    vfs_stat_t st;
    res = vfs_stat(vfs_strip_prefix(r->path), &st);
    if (res == 0 && sizeof st <= sizeof r->buf) {
      for (uint32_t i = 0; i < sizeof st; i++)
        r->buf[i] = ((const uint8_t *)&st)[i];
      out = (uint32_t)sizeof st;
    }
    break;
  }
  case VFS_OP_MKDIR:
    res = vfs_mkdir(vfs_strip_prefix(r->path));
    break;
  case VFS_OP_REMOVE:
    res = vfs_unlink(vfs_strip_prefix(r->path));
    break;
  case VFS_OP_DIROPEN:
    res = vfs_dir_open_internal(r->path);
    break;
  case VFS_OP_DIRNEXT: {
    vfs_dirent_t ent;
    res = vfs_dir_next_internal(r->fd, &ent);
    if (res == 1 && sizeof ent <= sizeof r->buf) {
      for (uint32_t i = 0; i < sizeof ent; i++)
        r->buf[i] = ((const uint8_t *)&ent)[i];
      out = (uint32_t)sizeof ent;
    }
    break;
  }
  default:
    break;
  }

  uint32_t s2 = ENTER_CRITICAL_FROM_ISR();
  if (!r->owner) { // died while we worked: drop the result, release the slot
    r->used = 0;
    EXIT_CRITICAL_FROM_ISR(s2);
    return 1;
  }
  r->result = res;
  r->out_len = out;
  EXIT_CRITICAL_FROM_ISR(s2);
  v_semaphore_give(r->done);
  return 1;
}

void v_vfs_task_teardown(TCB *t) {
  if (!t)
    return;
  // Its open files: hand each to the worker as an ownerless close, so the data
  // is flushed rather than lost. Nothing waits for these.
  for (int i = 0; i < VAIOS_VFS_MAX_OPEN; i++) {
    uint32_t hs = ENTER_CRITICAL_FROM_ISR();
    int mine = vfs_handles[i].used && vfs_handles[i].owner == t;
    if (mine)
      vfs_handles[i].owner = 0; // claim it before releasing the section
    EXIT_CRITICAL_FROM_ISR(hs);
    if (!mine)
      continue;
    vfs_req_t *r = 0;
    uint32_t rs = ENTER_CRITICAL_FROM_ISR();
    for (int k = 0; k < VAIOS_VFS_IO_SLOTS && !r; k++)
      if (!vfs_reqs[k].used) {
        r = &vfs_reqs[k];
        r->used = 1;
      }
    EXIT_CRITICAL_FROM_ISR(rs);
    if (!r) { // no slot to queue a close: release the handle, losing the flush
      vfs_fd_close(&vfs_handles[i]);
      continue;
    }
    r->op = VFS_OP_CLOSE;
    r->fd = i;
    r->len = r->out_len = 0;
    r->result = 0;
    r->owner = 0; // ownerless: the worker frees the slot when it is done
    r->queued = 1;
    if (vfs_work_sem)
      v_semaphore_give(vfs_work_sem);
  }

  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS; i++)
    if (vfs_reqs[i].used && vfs_reqs[i].owner == t) {
      // Abandon it. If the worker has it in hand, it sees owner == NULL and
      // releases the slot itself; if it has not started, the slot is free now.
      vfs_reqs[i].owner = 0;
      if (vfs_reqs[i].out_len || !vfs_work_sem)
        vfs_reqs[i].used = 0;
    }
  EXIT_CRITICAL_FROM_ISR(s);
}

// --- the task-facing calls: submit + wait + finish, composed ---------------
static int vfs_do(v_vfs_desc_t *d, void *out, uint32_t cap, uint32_t ticks) {
  int slot = v_vfs_submit(d);
  if (slot < 0)
    return slot;
  if (v_vfs_wait(slot, ticks) != VA_PASS) {
    // Timed out. The slot stays the caller's until finish collects it, so ask
    // for it now: the worker either already finished (we take the result) or
    // never will within our deadline (we take the timeout).
    int r = v_vfs_finish(slot, out, cap);
    return r < 0 ? V_VFS_ETIMEDOUT : r;
  }
  return v_vfs_finish(slot, out, cap);
}

int v_vfs_open(const char *path, int flags, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_OPEN, .path = path, .len = (uint32_t)flags};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_close(int fd, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_CLOSE, .fd = fd};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_read(int fd, void *buf, uint32_t len, uint32_t ticks) {
  // Chunked, so a task may ask for any size while kernel memory stays fixed.
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (want > VAIOS_VFS_IO_BUF)
      want = VAIOS_VFS_IO_BUF;
    v_vfs_desc_t d = {.op = VFS_OP_READ, .fd = fd, .len = want};
    int n = vfs_do(&d, (uint8_t *)buf + done, want, ticks);
    if (n < 0)
      return done ? (int)done : n; // report what did arrive
    done += (uint32_t)n;
    if ((uint32_t)n < want)
      break; // short read: end of file
  }
  return (int)done;
}
int v_vfs_write(int fd, const void *buf, uint32_t len, uint32_t ticks) {
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (want > VAIOS_VFS_IO_BUF)
      want = VAIOS_VFS_IO_BUF;
    v_vfs_desc_t d = {.op = VFS_OP_WRITE, .fd = fd, .len = want,
                      .data = (const uint8_t *)buf + done};
    int n = vfs_do(&d, 0, 0, ticks);
    if (n < 0)
      return done ? (int)done : n;
    done += (uint32_t)n;
    if ((uint32_t)n < want)
      break; // short write: the medium is full
  }
  return (int)done;
}
long v_vfs_seek(int fd, long offset, int whence, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_SEEK, .fd = fd, .offset = offset,
                    .whence = whence};
  return (long)vfs_do(&d, 0, 0, ticks);
}
int v_vfs_flush(int fd, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_FLUSH, .fd = fd};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_info(const char *path, vfs_stat_t *st, uint32_t ticks) {
  if (!st)
    return V_VFS_EINVAL;
  v_vfs_desc_t d = {.op = VFS_OP_STAT, .path = path};
  return vfs_do(&d, st, (uint32_t)sizeof *st, ticks);
}
int v_vfs_makedir(const char *path, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_MKDIR, .path = path};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_remove(const char *path, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_REMOVE, .path = path};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_diropen(const char *path, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_DIROPEN, .path = path};
  return vfs_do(&d, 0, 0, ticks);
}
long v_vfs_size(int fd, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_SIZE, .fd = fd};
  return (long)vfs_do(&d, 0, 0, ticks);
}
int v_vfs_preallocate(const char *path, uint32_t size, uint32_t ticks) {
  v_vfs_desc_t d = {.op = VFS_OP_PREALLOC, .path = path, .len = size};
  return vfs_do(&d, 0, 0, ticks);
}
int v_vfs_dirnext(int fd, vfs_dirent_t *ent, uint32_t ticks) {
  if (!ent)
    return V_VFS_EINVAL;
  v_vfs_desc_t d = {.op = VFS_OP_DIRNEXT, .fd = fd};
  return vfs_do(&d, ent, (uint32_t)sizeof *ent, ticks);
}

#endif // VAIOS_DEVFS && VAIOS_MODULE_VFS
