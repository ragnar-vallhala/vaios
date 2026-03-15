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

#ifdef __cplusplus
}
#endif

#endif // VAIOS_VFS_H
