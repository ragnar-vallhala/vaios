/**
 * @file test_vfs_fd.c
 * @brief The VFS through the fd API (M5): the mount as a devfs node, so a task
 *        reaches a file with the syscalls it already has, plus the operations
 *        that needed numbers of their own (seek, stat, mkdir, unlink, sync,
 *        directory listing).
 *
 * Runs in the ipcfd binary (DEVFS + SVC on), so the calls trap through
 * v_host_svc into the real v_syscall_dispatch. The filesystem underneath is the
 * recorded-call stub (tests/stubs/v_fs_stub.c), so each test programs what the
 * filesystem answers and asserts what the layer passed down and handed back.
 */
#include "framework.h"
#include "ipc.h" // VA_PASS
#include "stubs/v_fs_stub.h"
#include "task.h"
#include "vfile.h"
#include "vfs.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern int v_test_in_handler; // syscall_stubs.c

static uint32_t task_id;

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

static void setup(void) {
  v_test_in_handler = 1; // privileged setup
  // NOT stub_reset_heap(): vfs_init creates the FatFs mutex on the heap and is
  // idempotent, so wiping the heap would leave every later vfs_lock blocking on
  // a dangling handle. A real system never pulls the heap out from under a live
  // mutex; this suite simply keeps its heap.
  vfs_stub_reset();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  task_count = 0;
  scheduler_init();
  task_id = task_create(dummy_task, NULL, 256, 3);
  current_task = ready_lists[3];

  static int mounted; // the devfs registry is static and additive
  if (!mounted) {
    TEST_ASSERT_EQ(vfs_init(), 0);
    TEST_ASSERT_EQ(v_vfs_mount("/mnt/"), VA_PASS);
    mounted = 1;
  }
  v_test_in_handler = 0; // from here, calls trap like a task's
}

static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id); /* closes its fds -> closes the files */
}

/* A mount claims a subtree, and only a subtree: the prefix must end in '/', the
 * mount point itself is not a file, and a path outside it is still unknown. */
static void test_vfs_mount_validates(void) {
  setup();
  v_test_in_handler = 1;
  TEST_ASSERT_EQ(v_vfs_mount(NULL), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_vfs_mount(""), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_vfs_mount("/mnt"), V_VFS_EINVAL); // no trailing slash
  v_test_in_handler = 0;
  TEST_ASSERT(v_file_open("/mnt/", 0) < 0);        // the mount point itself
  TEST_ASSERT(v_file_open("/nope/file", 0) < 0);   // outside any node
  TEST_ASSERT_EQ(vfs_stub.open_called, 0);         // never reached the fs
  teardown();
}

/* open/read/write/close need no new syscalls: a path under the mount becomes an
 * fd whose reads and writes are the filesystem's, with the path handed down as
 * the tail — the part after the mount prefix. */
static void test_vfs_fd_round_trip(void) {
  setup();
  vfs_stub.open_ret = 7; // the filesystem's own handle
  int fd = v_file_open("/mnt/log.csv", 0);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(vfs_stub.open_called, 1);
  TEST_ASSERT_EQ(strcmp(vfs_stub.open_path, "log.csv"), 0); // tail, not the URL

  vfs_stub.write_ret = 4;
  TEST_ASSERT_EQ(v_file_write(fd, "abcd", 4), 4);
  TEST_ASSERT_EQ(vfs_stub.write_fd, 7); // dispatched to the right file
  TEST_ASSERT_EQ(vfs_stub.write_count, (size_t)4);

  char buf[4];
  vfs_stub.read_ret = 4;
  TEST_ASSERT_EQ(v_file_read(fd, buf, 4), 4);
  TEST_ASSERT_EQ(vfs_stub.read_fd, 7);

  TEST_ASSERT_EQ(v_file_close(fd), 0);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  TEST_ASSERT_EQ(vfs_stub.close_fd, 7);
  /* a closed fd is not usable any more */
  TEST_ASSERT(v_file_write(fd, "x", 1) < 0);
  teardown();
}

/* A filesystem that refuses the open must not consume a mount handle, or a few
 * missing files would exhaust the pool. */
static void test_vfs_fd_failed_open_releases_handle(void) {
  setup();
  vfs_stub.open_ret = -5; // no such file
  for (int i = 0; i < VAIOS_VFS_MAX_OPEN + 2; i++)
    TEST_ASSERT(v_file_open("/mnt/missing", 0) < 0);
  vfs_stub.open_ret = 3; // now one that exists: the pool is still whole
  int fd = v_file_open("/mnt/there", 0);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_file_close(fd), 0);
  teardown();
}

/* The handle pool is bounded, and exiting closes the files a task held. */
static void test_vfs_fd_limits_and_exit_release(void) {
  setup();
  vfs_stub.open_ret = 1;
  int fds[VAIOS_VFS_MAX_OPEN];
  for (int i = 0; i < VAIOS_VFS_MAX_OPEN; i++) {
    fds[i] = v_file_open("/mnt/a", 0);
    TEST_ASSERT(fds[i] >= 0);
  }
  TEST_ASSERT_EQ(v_file_open("/mnt/a", 0), V_VFS_EBUSY); // pool full
  vfs_stub.close_called = 0;
  teardown();                                   /* exit closes them all */
  TEST_ASSERT_EQ(vfs_stub.close_called, VAIOS_VFS_MAX_OPEN);

  setup(); /* and the pool is whole again */
  vfs_stub.open_ret = 1;
  int fd = v_file_open("/mnt/a", 0);
  TEST_ASSERT(fd >= 0);
  teardown();
}

/* The operations with no fd equivalent. Each is checked for what it passed down
 * (the tail, the right file handle) and what it returned. */
static void test_vfs_fd_extra_ops(void) {
  setup();
  vfs_stub.open_ret = 9;
  int fd = v_file_open("/mnt/data.bin", 0);
  TEST_ASSERT(fd >= 0);

  vfs_stub.lseek_ret = 128;
  TEST_ASSERT_EQ(v_file_lseek(fd, 128, 0), 128);
  TEST_ASSERT_EQ(vfs_stub.lseek_fd, 9);
  TEST_ASSERT_EQ(vfs_stub.lseek_offset, 128);

  vfs_stub.sync_ret = 0;
  TEST_ASSERT_EQ(v_file_sync(fd), 0);
  TEST_ASSERT_EQ(vfs_stub.sync_fd, 9);

  vfs_stub.mkdir_ret = 0;
  TEST_ASSERT_EQ(v_file_mkdir("/mnt/logs"), 0);
  TEST_ASSERT_EQ(strcmp(vfs_stub.mkdir_path, "logs"), 0); // prefix stripped
  /* a path already relative to the mount works too */
  TEST_ASSERT_EQ(v_file_mkdir("logs2"), 0);
  TEST_ASSERT_EQ(strcmp(vfs_stub.mkdir_path, "logs2"), 0);

  vfs_stub.unlink_ret = 0;
  TEST_ASSERT_EQ(v_file_unlink("/mnt/old.csv"), 0);
  TEST_ASSERT_EQ(strcmp(vfs_stub.unlink_path, "old.csv"), 0);

  vfs_stub.stat_ret = 0;
  vfs_stub.stat_out.size = 4096;
  vfs_stat_t st;
  memset(&st, 0, sizeof st);
  TEST_ASSERT_EQ(v_file_stat("/mnt/data.bin", &st), 0);
  TEST_ASSERT_EQ(strcmp(vfs_stub.stat_path, "data.bin"), 0);
  TEST_ASSERT_EQ(st.size, 4096u);

  /* Bad arguments are refused before the filesystem is bothered. */
  TEST_ASSERT_EQ(v_file_stat("/mnt/x", NULL), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_file_lseek(fd + 40, 0, 0), V_VFS_EINVAL); // not a file fd
  TEST_ASSERT_EQ(v_file_sync(fd + 40), V_VFS_EINVAL);
  v_file_close(fd);
  teardown();
}

/* Directory listing: opendir is an fd like any other, closed with close, and a
 * directory handle is not a file — reading it as one is refused rather than
 * dispatched to vfs_read. */
static void test_vfs_dir_listing(void) {
  setup();
  vfs_stub.opendir_ret = 2;
  int dir = v_dir_open("/mnt/logs");
  TEST_ASSERT(dir >= 0);
  TEST_ASSERT_EQ(strcmp(vfs_stub.opendir_path, "logs"), 0);

  vfs_stub.readdir_ret = 1;
  memcpy(vfs_stub.readdir_out.name, "a.csv", 6);
  vfs_dirent_t ent;
  memset(&ent, 0, sizeof ent);
  TEST_ASSERT_EQ(v_dir_read(dir, &ent), 1);
  TEST_ASSERT_EQ(vfs_stub.readdir_dir, 2);
  TEST_ASSERT_EQ(strcmp(ent.name, "a.csv"), 0);

  /* Not a file: reading or seeking it is refused, not forwarded. */
  char buf[4];
  vfs_stub.read_called = 0;
  TEST_ASSERT(v_file_read(dir, buf, 4) < 0);
  TEST_ASSERT_EQ(vfs_stub.read_called, 0);
  TEST_ASSERT_EQ(v_file_lseek(dir, 0, 0), V_VFS_EINVAL);

  /* And a file is not a directory. */
  vfs_stub.open_ret = 5;
  int fd = v_file_open("/mnt/f", 0);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_dir_read(fd, &ent), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_dir_read(dir, NULL), V_VFS_EINVAL);

  TEST_ASSERT_EQ(v_file_close(dir), 0);
  TEST_ASSERT_EQ(vfs_stub.closedir_called, 1);
  TEST_ASSERT_EQ(vfs_stub.closedir_dir, 2);
  v_file_close(fd);
  teardown();
}

static const test_case_t vfs_fd_cases[] = {
    TEST_CASE(test_vfs_mount_validates),
    TEST_CASE(test_vfs_fd_round_trip),
    TEST_CASE(test_vfs_fd_failed_open_releases_handle),
    TEST_CASE(test_vfs_fd_limits_and_exit_release),
    TEST_CASE(test_vfs_fd_extra_ops),
    TEST_CASE(test_vfs_dir_listing),
};

const test_suite_t vfs_fd_suite = {
    .name = "VFS on the fd table (M5)",
    .cases = vfs_fd_cases,
    .count = TEST_COUNT(vfs_fd_cases),
};
