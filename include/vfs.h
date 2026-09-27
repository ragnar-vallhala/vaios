#ifndef VAIOS_VFS_H
#define VAIOS_VFS_H

#include <stddef.h>
#include <stdint.h>

/* File access modes (matching NavHAL) */
#define VFS_O_RDONLY 0x01
#define VFS_O_WRONLY 0x02
#define VFS_O_RDWR 0x03
#define VFS_O_CREAT 0x04
#define VFS_O_TRUNC 0x08
#define VFS_O_APPEND 0x10

/* Seek origins */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

typedef int vfs_fd_t;
typedef int vfs_dir_t;

/* FatFS short (8.3) names: 12 chars + NUL (FF_USE_LFN = 0). */
#define VFS_NAME_MAX 13

/* One directory entry from vfs_readdir(). */
typedef struct {
  char name[VFS_NAME_MAX];
  uint32_t size; /* 0 for directories */
  uint8_t is_dir;
} vfs_dirent_t;

/* File/directory status from vfs_stat(). */
typedef struct {
  uint32_t size;
  uint32_t mtime; /* FatFS packed: (fdate << 16) | ftime */
  uint8_t is_dir;
  uint8_t exists; /* 0 distinguishes "absent" from "error" */
} vfs_stat_t;

#ifdef __cplusplus
extern "C" {
#endif

int vfs_init(void);
vfs_fd_t vfs_open(const char *path, int flags);
int vfs_close(vfs_fd_t fd);
int vfs_read(vfs_fd_t fd, void *buf, size_t count);
int vfs_write(vfs_fd_t fd, const void *buf, size_t count);
long vfs_lseek(vfs_fd_t fd, long offset, int whence);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_sync(vfs_fd_t fd);
int vfs_preallocate(const char *path, uint32_t size);
long vfs_size(vfs_fd_t fd);

/* Directory iteration + status. All take the same vfs_mutex as the rest of the
 * VFS, so SD access stays single-transaction. vfs_stat returns 0 if the path
 * exists (st->exists=1), <0 otherwise (st->exists=0 — lets callers report
 * "path does not exist" distinctly). vfs_readdir returns 1 per entry, 0 at end
 * of directory, <0 on error. */
int vfs_stat(const char *path, vfs_stat_t *st);
vfs_dir_t vfs_opendir(const char *path);
int vfs_readdir(vfs_dir_t d, vfs_dirent_t *ent);
int vfs_closedir(vfs_dir_t d);

/* --------------------------------------------------------------------------
 * Files for an unprivileged task: the I/O worker (roadmap M5 + todo 67).
 *
 * The first cut of this mounted the VFS as a devfs node so SYS_open/read/write
 * worked unchanged. That is wrong on hardware, and the reason is structural:
 * SVCall runs at priority 0, so a syscall body masks SysTick and every
 * peripheral IRQ. A filesystem call made inside the handler therefore waits
 * forever for an SDIO completion that cannot be delivered, and its millisecond
 * timeout cannot expire either — the whole system stops. Measured on an F401;
 * see docs/user-space.md.
 *
 * So the transfers happen in THREAD MODE, in a worker task the application owns,
 * with interrupts enabled. A task's file call is three syscalls, exactly the
 * shape v_pbus_xfer already uses for the same reason:
 *
 *   submit   validate the arguments, copy them (and any write payload) into a
 *            kernel slot, hand it to the worker           -> slot id
 *   wait     block the CALLER on that slot's semaphore, under the scheduler
 *   finish   copy any result out of the slot's bounce buffer, release the slot
 *
 * The public v_vfs_* calls below compose all three, so callers never see it. No
 * pointer of the caller's is held while it sleeps, and nothing but whole
 * elements crosses the boundary — the same two properties the bus and queue
 * blocking paths are built on.
 *
 * Reads and writes are bounded by VAIOS_VFS_IO_BUF (one sector by default): the
 * wrappers loop in chunks, so kernel memory stays fixed no matter what size a
 * task asks for.
 *
 * The devfs mount (v_vfs_mount) still exists and still works — but only for
 * PRIVILEGED callers in thread mode, such as the bus snapshotter, where inline
 * I/O is safe because interrupts are enabled. It is not a path for tasks.
 * -------------------------------------------------------------------------- */
#if VAIOS_DEVFS && VAIOS_MODULE_VFS
#define V_VFS_EINVAL (-22)  /* bad path, handle or descriptor */
#define V_VFS_EBUSY (-16)   /* no free handle or request slot */
#define V_VFS_EAGAIN (-11)  /* no worker is running to service the request */
#define V_VFS_ETIMEDOUT (-110)

/* Privileged, at init: publish the VFS under a devfs path prefix (which must end
 * in '/', e.g. "/mnt/"). VA_PASS or V_VFS_EINVAL. */
int v_vfs_mount(const char *prefix);

/* One file operation, as a task hands it to the kernel. It lives in the
 * CALLER's memory and is validated there; everything the worker needs is copied
 * into a kernel slot at submit, so none of the caller's buffers are touched
 * again while it waits. The v_vfs_* calls below fill this in for you. */
typedef struct {
  uint8_t op;       /* private to the implementation */
  int fd;           /* handle, for the operations that take one */
  uint32_t len;     /* bytes to read or write (also open flags) */
  long offset;      /* seek */
  int whence;       /* seek */
  const char *path; /* path operations */
  const void *data; /* write payload */
} v_vfs_desc_t;

/* The three steps. Callers normally use the v_vfs_* helpers further down, which
 * compose them; these are exposed because the syscall dispatch names them. */
int v_vfs_submit(const v_vfs_desc_t *d);
int v_vfs_wait(int slot, uint32_t ticks);
int v_vfs_finish(int slot, void *out, uint32_t cap);

/* --- the worker -------------------------------------------------------------
 * Run this from a task of your own, at a priority you choose: recording and
 * logging must not outrank control, and that is a flight decision, not the
 * kernel's. Services at most one request per call.
 *
 *   static void vfs_io_task(void *arg) {
 *     for (;;) v_vfs_worker_step(100);
 *   }
 *   task_create_named(vfs_io_task, NULL, 2048, 1, "vfsio");
 *
 * Returns 1 if it serviced a request, 0 if it waited `ticks` and none came.
 * Until a worker has run at least once, task file calls fail with V_VFS_EAGAIN
 * rather than blocking forever on nobody. */
int v_vfs_worker_step(uint32_t ticks);

/* --- the task-facing file API -----------------------------------------------
 * Same shape as the privileged vfs_* calls, but worker-backed. `ticks` bounds
 * how long the CALLER waits for the worker, not the transfer itself. */
int v_vfs_open(const char *path, int flags, uint32_t ticks);
int v_vfs_close(int fd, uint32_t ticks);
int v_vfs_read(int fd, void *buf, uint32_t len, uint32_t ticks);
int v_vfs_write(int fd, const void *buf, uint32_t len, uint32_t ticks);
long v_vfs_seek(int fd, long offset, int whence, uint32_t ticks);
int v_vfs_flush(int fd, uint32_t ticks);
int v_vfs_info(const char *path, vfs_stat_t *st, uint32_t ticks);
int v_vfs_makedir(const char *path, uint32_t ticks);
int v_vfs_remove(const char *path, uint32_t ticks);
int v_vfs_diropen(const char *path, uint32_t ticks);
int v_vfs_dirnext(int fd, vfs_dirent_t *ent, uint32_t ticks);

/* Release any request slot a dying task still owns. Called by the task teardown
 * path, like v_ipc_task_teardown and v_pbus_task_teardown. */
struct Task_Control_Block;
void v_vfs_task_teardown(struct Task_Control_Block *t);
#endif /* VAIOS_DEVFS && VAIOS_MODULE_VFS */

#ifdef __cplusplus
}
#endif

#endif // VAIOS_VFS_H
