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

#ifdef __cplusplus
}
#endif

#endif // VAIOS_VFS_H
