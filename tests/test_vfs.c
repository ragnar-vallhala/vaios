/**
 * @file test_vfs.c
 * @brief Unit tests for kernel/vfs.c — the mutex-serialized thin wrapper
 *        over NavHAL's v_fs / v_open / ... API. The v_* functions are
 *        recorded-call stubs in tests/stubs/v_fs_stub.c, so each test
 *        programs the stub return, calls the vfs_* wrapper, and asserts
 *        the wrapper passed the right args and returned the right value.
 */
#include "framework.h"
#include "stubs/v_fs_stub.h"
#include "task.h"
#include "vfs.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *current_task;
extern TCB *idle_task;
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern uint32_t task_count;

/* Minimal current_task — vfs_lock -> v_mutex_lock writes rm->owner. */
static TCB _vfs_fake_task;

static void full_reset(void) {
  stub_reset_heap();
  vfs_stub_reset();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = NULL;
  ready_bitmap = 0;
  task_count = 0;
  memset(&_vfs_fake_task, 0, sizeof(_vfs_fake_task));
  _vfs_fake_task.task_id = 1;
  _vfs_fake_task.priority = 3;
  _vfs_fake_task.status = TASK_RUNNING;
  current_task = &_vfs_fake_task;
  idle_task = &_vfs_fake_task;
}

/* vfs_init creates the mutex on first call and propagates the underlying
 * v_fs_init() return value. Subsequent calls re-init v_fs (idempotent
 * mutex creation, NOT idempotent v_fs_init). */
static void test_vfs_init_calls_v_fs_init(void) {
  full_reset();
  vfs_stub.init_ret = 0;
  TEST_ASSERT_EQ(vfs_init(), 0);
  TEST_ASSERT_EQ(vfs_stub.init_called, 1);

  /* A different return value plumbs through. */
  vfs_stub.init_ret = -7;
  TEST_ASSERT_EQ(vfs_init(), -7);
  TEST_ASSERT_EQ(vfs_stub.init_called, 2);
}

static void test_vfs_open_passes_args_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.open_ret = 42;
  vfs_fd_t fd = vfs_open("/path/foo.txt", VFS_O_RDONLY);
  TEST_ASSERT_EQ(fd, 42);
  TEST_ASSERT_EQ(vfs_stub.open_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.open_path, "/path/foo.txt") == 0);
  TEST_ASSERT_EQ(vfs_stub.open_flags, VFS_O_RDONLY);
}

static void test_vfs_close_passes_fd_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.close_ret = 0;
  TEST_ASSERT_EQ(vfs_close(7), 0);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  TEST_ASSERT_EQ(vfs_stub.close_fd, 7);
}

static void test_vfs_read_passes_args_and_returns(void) {
  full_reset();
  vfs_init();
  uint8_t buf[16];
  vfs_stub.read_ret = 12;
  TEST_ASSERT_EQ(vfs_read(3, buf, sizeof(buf)), 12);
  TEST_ASSERT_EQ(vfs_stub.read_called, 1);
  TEST_ASSERT_EQ(vfs_stub.read_fd, 3);
  TEST_ASSERT_EQ(vfs_stub.read_buf, buf);
  TEST_ASSERT_EQ(vfs_stub.read_count, sizeof(buf));
}

static void test_vfs_write_passes_args_and_returns(void) {
  full_reset();
  vfs_init();
  const char *data = "hello";
  vfs_stub.write_ret = 5;
  TEST_ASSERT_EQ(vfs_write(4, data, 5), 5);
  TEST_ASSERT_EQ(vfs_stub.write_called, 1);
  TEST_ASSERT_EQ(vfs_stub.write_fd, 4);
  TEST_ASSERT_EQ((const char *)vfs_stub.write_buf, data);
  TEST_ASSERT_EQ(vfs_stub.write_count, 5u);
}

static void test_vfs_lseek_passes_args_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.lseek_ret = 123;
  TEST_ASSERT_EQ(vfs_lseek(2, 100, VFS_SEEK_SET), 123);
  TEST_ASSERT_EQ(vfs_stub.lseek_called, 1);
  TEST_ASSERT_EQ(vfs_stub.lseek_fd, 2);
  TEST_ASSERT_EQ(vfs_stub.lseek_offset, 100);
  TEST_ASSERT_EQ(vfs_stub.lseek_whence, VFS_SEEK_SET);
}

/* vfs_size is vfs_lseek(fd, 0, VFS_SEEK_END) under the hood. */
static void test_vfs_size_uses_lseek_end(void) {
  full_reset();
  vfs_init();
  vfs_stub.lseek_ret = 4096;
  TEST_ASSERT_EQ(vfs_size(5), 4096);
  TEST_ASSERT_EQ(vfs_stub.lseek_called, 1);
  TEST_ASSERT_EQ(vfs_stub.lseek_fd, 5);
  TEST_ASSERT_EQ(vfs_stub.lseek_offset, 0);
  TEST_ASSERT_EQ(vfs_stub.lseek_whence, VFS_SEEK_END);
}

static void test_vfs_mkdir_passes_path_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.mkdir_ret = 0;
  TEST_ASSERT_EQ(vfs_mkdir("/logs"), 0);
  TEST_ASSERT_EQ(vfs_stub.mkdir_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.mkdir_path, "/logs") == 0);
}

static void test_vfs_unlink_passes_path_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.unlink_ret = 0;
  TEST_ASSERT_EQ(vfs_unlink("/stale.tmp"), 0);
  TEST_ASSERT_EQ(vfs_stub.unlink_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.unlink_path, "/stale.tmp") == 0);
}

static void test_vfs_sync_passes_fd_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.sync_ret = 0;
  TEST_ASSERT_EQ(vfs_sync(9), 0);
  TEST_ASSERT_EQ(vfs_stub.sync_called, 1);
  TEST_ASSERT_EQ(vfs_stub.sync_fd, 9);
}

static void test_vfs_preallocate_passes_args_and_returns(void) {
  full_reset();
  vfs_init();
  vfs_stub.preallocate_ret = 0;
  TEST_ASSERT_EQ(vfs_preallocate("/data.bin", 8192u), 0);
  TEST_ASSERT_EQ(vfs_stub.preallocate_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.preallocate_path, "/data.bin") == 0);
  TEST_ASSERT_EQ(vfs_stub.preallocate_size, 8192u);
}

/* vfs_stat copies v_stat fields through and propagates exists/ret, so a caller
 * can tell "absent" (ret<0, exists=0) from a real entry. */
static void test_vfs_stat_passes_and_translates(void) {
  full_reset();
  vfs_init();
  vfs_stub.stat_ret = 0;
  vfs_stub.stat_out.size = 1234u;
  vfs_stub.stat_out.mtime = 0xABCD1234u;
  vfs_stub.stat_out.is_dir = 1;
  vfs_stub.stat_out.exists = 1;
  vfs_stat_t st;
  memset(&st, 0, sizeof st);
  TEST_ASSERT_EQ(vfs_stat("/logs", &st), 0);
  TEST_ASSERT_EQ(vfs_stub.stat_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.stat_path, "/logs") == 0);
  TEST_ASSERT_EQ(st.size, 1234u);
  TEST_ASSERT_EQ(st.mtime, 0xABCD1234u);
  TEST_ASSERT_EQ(st.is_dir, 1);
  TEST_ASSERT_EQ(st.exists, 1);

  /* Missing path: ret<0 and exists=0 reach the caller. */
  vfs_stub.stat_ret = -4; /* FR_NO_FILE */
  vfs_stub.stat_out.exists = 0;
  TEST_ASSERT(vfs_stat("/nope", &st) < 0);
  TEST_ASSERT_EQ(st.exists, 0);
}

/* vfs_opendir/readdir/closedir plumb the handle + translate each entry; a
 * readdir returning 1 yields an entry, 0 ends the walk. */
static void test_vfs_dir_iteration(void) {
  full_reset();
  vfs_init();
  vfs_stub.opendir_ret = 1;
  TEST_ASSERT_EQ(vfs_opendir("0:"), 1);
  TEST_ASSERT_EQ(vfs_stub.opendir_called, 1);
  TEST_ASSERT(strcmp(vfs_stub.opendir_path, "0:") == 0);

  vfs_stub.readdir_ret = 1;
  vfs_stub.readdir_out.size = 99u;
  vfs_stub.readdir_out.is_dir = 0;
  memcpy(vfs_stub.readdir_out.name, "LOG.BIN", 8);
  vfs_dirent_t ent;
  memset(&ent, 0, sizeof ent);
  TEST_ASSERT_EQ(vfs_readdir(1, &ent), 1);
  TEST_ASSERT_EQ(vfs_stub.readdir_dir, 1);
  TEST_ASSERT(strcmp(ent.name, "LOG.BIN") == 0);
  TEST_ASSERT_EQ(ent.size, 99u);
  TEST_ASSERT_EQ(ent.is_dir, 0);

  /* End of directory. */
  vfs_stub.readdir_ret = 0;
  TEST_ASSERT_EQ(vfs_readdir(1, &ent), 0);

  vfs_stub.closedir_ret = 0;
  TEST_ASSERT_EQ(vfs_closedir(1), 0);
  TEST_ASSERT_EQ(vfs_stub.closedir_dir, 1);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
void run_vfs_tests(void) {
  TEST_SUITE_BEGIN("VFS (mutex-serialized wrapper)");
  TEST_RUN(test_vfs_init_calls_v_fs_init);
  TEST_RUN(test_vfs_open_passes_args_and_returns);
  TEST_RUN(test_vfs_close_passes_fd_and_returns);
  TEST_RUN(test_vfs_read_passes_args_and_returns);
  TEST_RUN(test_vfs_write_passes_args_and_returns);
  TEST_RUN(test_vfs_lseek_passes_args_and_returns);
  TEST_RUN(test_vfs_size_uses_lseek_end);
  TEST_RUN(test_vfs_mkdir_passes_path_and_returns);
  TEST_RUN(test_vfs_unlink_passes_path_and_returns);
  TEST_RUN(test_vfs_sync_passes_fd_and_returns);
  TEST_RUN(test_vfs_preallocate_passes_args_and_returns);
  TEST_RUN(test_vfs_stat_passes_and_translates);
  TEST_RUN(test_vfs_dir_iteration);
  TEST_SUITE_END();
}
