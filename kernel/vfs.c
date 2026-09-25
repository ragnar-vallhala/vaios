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

static vfs_handle_t *vfs_from_fd(int fd) {
  return (vfs_handle_t *)v_fd_obj(fd, &vfs_mount_ops);
}

long v_file_lseek(int fd, long offset, int whence) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return (long)v_svc3(SYS_lseek, (uint32_t)fd, (uintptr_t)offset,
                        (uint32_t)whence);
#endif
  vfs_handle_t *h = vfs_from_fd(fd);
  if (!h || h->file < 0)
    return V_VFS_EINVAL;
  return vfs_lseek(h->file, offset, whence);
}

int v_file_stat(const char *path, vfs_stat_t *st) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_stat, (uintptr_t)path, (uintptr_t)st);
#endif
  if (!path || !st)
    return V_VFS_EINVAL;
  return vfs_stat(vfs_strip_prefix(path), st);
}

int v_file_mkdir(const char *path) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_mkdir, (uintptr_t)path);
#endif
  if (!path)
    return V_VFS_EINVAL;
  return vfs_mkdir(vfs_strip_prefix(path));
}

int v_file_unlink(const char *path) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_unlink, (uintptr_t)path);
#endif
  if (!path)
    return V_VFS_EINVAL;
  return vfs_unlink(vfs_strip_prefix(path));
}

int v_file_sync(int fd) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_sync, (uint32_t)fd);
#endif
  vfs_handle_t *h = vfs_from_fd(fd);
  if (!h || h->file < 0)
    return V_VFS_EINVAL;
  return vfs_sync(h->file);
}

int v_dir_open(const char *path) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_opendir, (uintptr_t)path);
#endif
  if (!path)
    return V_VFS_EINVAL;
  vfs_handle_t *h = vfs_handle_alloc();
  if (!h)
    return V_VFS_EBUSY;
  vfs_dir_t d = vfs_opendir(vfs_strip_prefix(path));
  if (d < 0) {
    h->used = 0;
    return (int)d;
  }
  h->dir = d;
  int fd = v_fd_alloc(&vfs_mount_ops, h);
  if (fd < 0) {
    vfs_fd_close(h); // no descriptor left: don't leak the directory
    return V_VFS_EBUSY;
  }
  return fd;
}

int v_dir_read(int fd, vfs_dirent_t *ent) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_readdir, (uint32_t)fd, (uintptr_t)ent);
#endif
  vfs_handle_t *h = vfs_from_fd(fd);
  if (!h || h->dir < 0 || !ent)
    return V_VFS_EINVAL;
  return vfs_readdir(h->dir, ent);
}
#endif // VAIOS_DEVFS && VAIOS_MODULE_VFS
