/**
 * @file utils/v_fs.h (host test stub)
 * @brief Minimal declarations of the NavHAL v_fs / v_open / ... API so
 *        kernel/vfs.c can compile and link on the host.
 *
 * The real header lives in NavHAL (extern/NavHAL/include/utils/v_fs.h) and
 * pulls in FatFs/SDIO; the host test build doesn't want any of that. The
 * implementations are recorded-call stubs in tests/stubs/v_fs_stub.c.
 */
#ifndef VAIOS_TEST_STUB_V_FS_H
#define VAIOS_TEST_STUB_V_FS_H

#include <stddef.h>
#include <stdint.h>

typedef int v_fd_t;

int v_fs_init(void);
v_fd_t v_open(const char *path, int flags);
int v_close(v_fd_t fd);
int v_read(v_fd_t fd, void *buf, size_t count);
int v_write(v_fd_t fd, const void *buf, size_t count);
long v_lseek(v_fd_t fd, long offset, int whence);
int v_mkdir(const char *path);
int v_unlink(const char *path);
int v_sync(v_fd_t fd);
int v_preallocate(const char *path, uint32_t size);

#endif /* VAIOS_TEST_STUB_V_FS_H */
