/**
 * @file v_fs_stub.h
 * @brief Programmable per-function state for the v_fs/v_open/... stubs.
 *        Tests include this header, reset state, set return values, call
 *        the vfs_* wrappers, and inspect what was recorded.
 */
#ifndef VAIOS_TEST_V_FS_STUB_H
#define VAIOS_TEST_V_FS_STUB_H

#include "utils/v_fs.h"
#include <stddef.h>
#include <stdint.h>

struct vfs_stub_state {
  int init_called;
  int init_ret;

  int open_called;
  const char *open_path;
  int open_flags;
  v_fd_t open_ret;

  int close_called;
  v_fd_t close_fd;
  int close_ret;

  int read_called;
  v_fd_t read_fd;
  void *read_buf;
  size_t read_count;
  int read_ret;

  int write_called;
  v_fd_t write_fd;
  const void *write_buf;
  size_t write_count;
  int write_ret;

  int lseek_called;
  v_fd_t lseek_fd;
  long lseek_offset;
  int lseek_whence;
  long lseek_ret;

  int mkdir_called;
  const char *mkdir_path;
  int mkdir_ret;

  int unlink_called;
  const char *unlink_path;
  int unlink_ret;

  int sync_called;
  v_fd_t sync_fd;
  int sync_ret;

  int preallocate_called;
  const char *preallocate_path;
  uint32_t preallocate_size;
  int preallocate_ret;
};

extern struct vfs_stub_state vfs_stub;
void vfs_stub_reset(void);

#endif /* VAIOS_TEST_V_FS_STUB_H */
