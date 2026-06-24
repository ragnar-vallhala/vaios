/**
 * @file v_fs_stub.c
 * @brief Recorded-call stubs for the NavHAL v_fs / v_open / ... API used by
 *        kernel/vfs.c. Tests program the return values and inspect what
 *        arguments the wrappers passed through.
 *
 * One global `vfs_stub_state` carries all per-function state. Reset it with
 * vfs_stub_reset() at the start of each test.
 */
#include "utils/v_fs.h"
#include <string.h>

#include "v_fs_stub.h"

struct vfs_stub_state vfs_stub = {0};

void vfs_stub_reset(void) { memset(&vfs_stub, 0, sizeof(vfs_stub)); }

int v_fs_init(void) {
  vfs_stub.init_called++;
  return vfs_stub.init_ret;
}

v_fd_t v_open(const char *path, int flags) {
  vfs_stub.open_called++;
  vfs_stub.open_path = path;
  vfs_stub.open_flags = flags;
  return vfs_stub.open_ret;
}

int v_close(v_fd_t fd) {
  vfs_stub.close_called++;
  vfs_stub.close_fd = fd;
  return vfs_stub.close_ret;
}

int v_read(v_fd_t fd, void *buf, size_t count) {
  vfs_stub.read_called++;
  vfs_stub.read_fd = fd;
  vfs_stub.read_buf = buf;
  vfs_stub.read_count = count;
  return vfs_stub.read_ret;
}

int v_write(v_fd_t fd, const void *buf, size_t count) {
  vfs_stub.write_called++;
  vfs_stub.write_fd = fd;
  vfs_stub.write_buf = buf;
  vfs_stub.write_count = count;
  return vfs_stub.write_ret;
}

long v_lseek(v_fd_t fd, long offset, int whence) {
  vfs_stub.lseek_called++;
  vfs_stub.lseek_fd = fd;
  vfs_stub.lseek_offset = offset;
  vfs_stub.lseek_whence = whence;
  return vfs_stub.lseek_ret;
}

int v_mkdir(const char *path) {
  vfs_stub.mkdir_called++;
  vfs_stub.mkdir_path = path;
  return vfs_stub.mkdir_ret;
}

int v_unlink(const char *path) {
  vfs_stub.unlink_called++;
  vfs_stub.unlink_path = path;
  return vfs_stub.unlink_ret;
}

int v_sync(v_fd_t fd) {
  vfs_stub.sync_called++;
  vfs_stub.sync_fd = fd;
  return vfs_stub.sync_ret;
}

int v_preallocate(const char *path, uint32_t size) {
  vfs_stub.preallocate_called++;
  vfs_stub.preallocate_path = path;
  vfs_stub.preallocate_size = size;
  return vfs_stub.preallocate_ret;
}

int v_stat(const char *path, v_stat_t *st) {
  vfs_stub.stat_called++;
  vfs_stub.stat_path = path;
  if (st != NULL)
    *st = vfs_stub.stat_out;
  return vfs_stub.stat_ret;
}

v_dir_t v_opendir(const char *path) {
  vfs_stub.opendir_called++;
  vfs_stub.opendir_path = path;
  return vfs_stub.opendir_ret;
}

int v_readdir(v_dir_t d, v_dirent_t *ent) {
  vfs_stub.readdir_called++;
  vfs_stub.readdir_dir = d;
  if (ent != NULL && vfs_stub.readdir_ret == 1)
    *ent = vfs_stub.readdir_out;
  return vfs_stub.readdir_ret;
}

int v_closedir(v_dir_t d) {
  vfs_stub.closedir_called++;
  vfs_stub.closedir_dir = d;
  return vfs_stub.closedir_ret;
}
