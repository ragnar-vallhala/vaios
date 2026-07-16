#ifndef VAIOS_VFILE_H
#define VAIOS_VFILE_H

/**
 * @file vfile.h
 * @brief Everything-is-a-file object model + per-task fd table (Phase 3, Stage 2).
 *
 * A file descriptor is a small non-negative integer indexing the calling task's
 * fd table (in the TCB). Each open fd points at a driver op-table (v_file_ops)
 * plus opaque private data. Devices register /dev nodes; open() looks a node up
 * and allocates an fd; read/write/close dispatch through the op-table. This is
 * the substrate the file syscalls (SYS_open/read/write/close) run on.
 *
 * Gated by VAIOS_DEVFS. The fd table lives in the TCB (task.h includes this for
 * the v_fd_entry type); functions taking a TCB forward-declare it to avoid a
 * circular include.
 */

#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>

// Driver op-table. Handlers receive the fd's private data; return >= 0 on
// success (byte count / value), < 0 on error. Any may be NULL (unsupported).
typedef struct v_file_ops {
  int (*read)(void *priv, void *buf, uint32_t len);
  int (*write)(void *priv, const void *buf, uint32_t len);
  int (*close)(void *priv);
} v_file_ops;

// One fd-table slot. ops == NULL means the slot is free.
typedef struct {
  const v_file_ops *ops;
  void *priv;
} v_fd_entry;

#if VAIOS_DEVFS

struct Task_Control_Block;

// Register a device node, e.g. "/dev/console". Returns 0 or a negative error.
int v_devfs_register(const char *name, const v_file_ops *ops, void *priv);

// Kernel-side file API (called from the syscall dispatch, privileged). Each
// operates on the current task's fd table. Return an fd / byte count, or a
// negative error.
int v_file_open(const char *path, int flags);
int v_file_read(int fd, void *buf, uint32_t len);
int v_file_write(int fd, const void *buf, uint32_t len);
int v_file_close(int fd);

// Allocate an fd in the current task's table for (ops, priv). Returns the fd or
// a negative error. Used by open() and by fd-typed IPC (v_sem_open).
int v_fd_alloc(const v_file_ops *ops, void *priv);

// Return the private data of `fd` if its op-table is exactly `ops` (a type
// check, so a sem fd can't be used as a console fd), else NULL.
void *v_fd_obj(int fd, const v_file_ops *ops);

// Initialise a task's fd table (all slots free). Called from task creation.
void v_fd_table_init(struct Task_Control_Block *t);

// Close every open descriptor a task holds (calling each op-table's close, so
// fd-typed IPC objects drop their refcount). Called from the task exit paths so
// a terminating task doesn't leak named-object slots. Operates on `t`, not the
// current task.
void v_fd_close_all(struct Task_Control_Block *t);

// Register the built-in devices (/dev/console). Called once at boot.
void v_devfs_init(void);

#endif // VAIOS_DEVFS

#endif // !VAIOS_VFILE_H
