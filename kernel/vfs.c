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
