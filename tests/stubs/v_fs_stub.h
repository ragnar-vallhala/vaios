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
  char open_path[64]; /* a copy: the VFS resolves into its own stack */
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
  char mkdir_path[64]; /* a copy: the VFS resolves into its own stack */
  int mkdir_ret;

  int unlink_called;
  char unlink_path[64]; /* a copy: the VFS resolves into its own stack */
  int unlink_ret;

  int rename_called;
  char rename_from[64], rename_to[64]; /* copies: the worker's slot is reused */
  int rename_ret;

  int getfree_called;
  char getfree_drive[8];
  uint32_t getfree_total, getfree_free;
  int getfree_ret;

  int sync_called;
  v_fd_t sync_fd;
  int sync_ret;

  int preallocate_called;
  char preallocate_path[64]; /* a copy: the VFS resolves into its own stack */
  uint32_t preallocate_size;
  int preallocate_ret;

  int stat_called;
  char stat_path[64]; /* a copy: the VFS resolves into its own stack */
  int stat_ret;
  v_stat_t stat_out; /* copied into the caller's *st */

  int opendir_called;
  char opendir_path[64]; /* a copy: the VFS resolves into its own stack */
  v_dir_t opendir_ret;

  int readdir_called;
  v_dir_t readdir_dir;
  int readdir_ret;
  v_dirent_t readdir_out; /* copied into the caller's *ent when ret == 1 */

  int closedir_called;
  v_dir_t closedir_dir;
  int closedir_ret;
};

extern struct vfs_stub_state vfs_stub;
void vfs_stub_reset(void);

#endif /* VAIOS_TEST_V_FS_STUB_H */
