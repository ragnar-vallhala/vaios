/**
 * @file test_vfs_fd.c
 * @brief Files for a task, through the VFS I/O worker (todo 67).
 *
 * The first version of this layer ran the filesystem inline in the syscall. That
 * deadlocks on hardware — SVCall is priority 0, so the handler masks the very
 * SDIO completion it waits for — so the transfers moved into a worker task and a
 * task's file call became submit / wait / finish (docs/user-space.md).
 *
 * Runs in the ipcfd binary (DEVFS + SVC on). The filesystem underneath is the
 * recorded-call stub, so each test programs what it answers and asserts what the
 * layer passed down and handed back. There are no interrupts here — which is
 * exactly why the inline version passed these tests and failed on silicon. What
 * these cover is the plumbing, ownership and teardown, not the timing.
 */
#include "framework.h"
#include "ipc.h" // VA_PASS
#include "stubs/v_fs_stub.h"
#include "syscall.h" // V_SYSCALL_BLOCKED
#include "task.h"
#include "vfile.h"
#include "vfs.h"
#include <string.h>

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

/* A task to be the caller, and the VFS mounted once. No stub_reset_heap here:
 * vfs_init puts the FatFs mutex on the heap and is idempotent, so wiping the heap
 * would leave every later lock on a dangling handle. */
static void setup(void) {
  v_test_in_handler = 1; // privileged setup
  vfs_stub_reset();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  task_count = 0;
  scheduler_init();
  task_id = task_create(dummy_task, NULL, 256, 3);
  current_task = ready_lists[3];

  static int mounted;
  if (!mounted) {
    TEST_ASSERT_EQ(vfs_init(), 0);
    TEST_ASSERT_EQ(v_vfs_mount("/mnt/"), VA_PASS);
    mounted = 1;
  }
  v_test_in_handler = 0; // from here, calls trap like a task's
}

static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id);
}

/* Drive the worker the way an application's own task would: privileged, thread
 * mode — which on target is the whole point, because interrupts are enabled. */
/* NOTE: never call this inside TEST_ASSERT_EQ — the macro evaluates its
 * arguments twice, and this one services a request. */
static int pump(void) {
  v_test_in_handler = 1;
  int n = v_vfs_worker_step(0);
  v_test_in_handler = 0;
  return n;
}

/* Work left queued by a previous test's teardown (which now closes a dying
 * task's files through the worker) is not this test's business: drain it. */
static void drain(void) {
  while (pump())
    ;
}

/* Nothing is serviced until a worker runs: a file call with no worker fails
 * loudly rather than blocking on nobody for ever. */
static void test_vfs_needs_a_worker(void) {
  setup();
  static int first = 1;
  if (first) { /* before any pump() in this binary */
    TEST_ASSERT_EQ(v_vfs_open("/mnt/x", 0, 10), V_VFS_EAGAIN);
    first = 0;
  }
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 0); } /* and the worker waits for work rather than spins */
  teardown();
}

/* A mount claims a subtree, and only a subtree. */
static void test_vfs_mount_validates(void) {
  setup();
  v_test_in_handler = 1;
  TEST_ASSERT_EQ(v_vfs_mount(NULL), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_vfs_mount(""), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_vfs_mount("/mnt"), V_VFS_EINVAL); // no trailing slash
  v_test_in_handler = 0;
  teardown();
}

/* open -> write -> read -> close. Each step is submitted, serviced by the worker,
 * then collected — and crucially the filesystem is NOT touched by the syscall. */
static void test_vfs_round_trip(void) {
  setup();
  drain();

  vfs_stub.open_ret = 7;
  v_vfs_desc_t d = {.op = 1 /*OPEN*/, .path = "/mnt/log.csv", .len = 0};
  int slot = v_vfs_submit(&d);
  TEST_ASSERT(slot >= 0);
  TEST_ASSERT_EQ(vfs_stub.open_called, 0); // not in the syscall
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT_EQ(vfs_stub.open_called, 1); // in the worker
  TEST_ASSERT_EQ(strcmp(vfs_stub.open_path, "log.csv"), 0); // prefix stripped
  int fd = v_vfs_finish(slot, 0, 0);
  TEST_ASSERT(fd >= 0);

  vfs_stub.write_ret = 4;
  const char *payload = "abcd";
  v_vfs_desc_t w = {.op = 4 /*WRITE*/, .fd = fd, .len = 4, .data = payload};
  slot = v_vfs_submit(&w);
  TEST_ASSERT(slot >= 0);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT_EQ(v_vfs_finish(slot, 0, 0), 4);
  TEST_ASSERT_EQ(vfs_stub.write_fd, 7);
  TEST_ASSERT_EQ(vfs_stub.write_count, (size_t)4);

  vfs_stub.read_ret = 4;
  char buf[8];
  memset(buf, 0, sizeof buf);
  v_vfs_desc_t r = {.op = 3 /*READ*/, .fd = fd, .len = 4};
  slot = v_vfs_submit(&r);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT_EQ(v_vfs_finish(slot, buf, sizeof buf), 4);
  TEST_ASSERT_EQ(vfs_stub.read_fd, 7);

  v_vfs_desc_t c = {.op = 2 /*CLOSE*/, .fd = fd};
  slot = v_vfs_submit(&c);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT_EQ(v_vfs_finish(slot, 0, 0), 0);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  teardown();
}

/* What crosses is bounded: a request larger than the bounce buffer is refused
 * (the wrapper chunks instead), and a path longer than a slot is refused rather
 * than truncated into the wrong file. */
static void test_vfs_requests_are_bounded(void) {
  setup();
  drain();
  v_vfs_desc_t big = {.op = 3, .fd = 0, .len = VAIOS_VFS_IO_BUF + 1u};
  TEST_ASSERT_EQ(v_vfs_submit(&big), V_VFS_EINVAL);

  static char longpath[80];
  memset(longpath, 'a', sizeof longpath - 1);
  longpath[sizeof longpath - 1] = 0;
  v_vfs_desc_t lp = {.op = 1, .path = longpath};
  TEST_ASSERT_EQ(v_vfs_submit(&lp), V_VFS_EINVAL);
  teardown();
}

/* The slot pool is bounded, and a slot belongs to whoever submitted it. */
static void test_vfs_slots_are_owned_and_bounded(void) {
  setup();
  drain();
  vfs_stub.open_ret = 3;

  int slots[VAIOS_VFS_IO_SLOTS];
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS; i++) {
    v_vfs_desc_t d = {.op = 1, .path = "/mnt/a"};
    slots[i] = v_vfs_submit(&d);
    TEST_ASSERT(slots[i] >= 0);
  }
  v_vfs_desc_t extra = {.op = 1, .path = "/mnt/b"};
  TEST_ASSERT_EQ(v_vfs_submit(&extra), V_VFS_EBUSY); // pool full

  TCB *owner = current_task;
  static TCB other;
  other = *owner;
  other.task_id = owner->task_id + 100u;
  current_task = &other;
  TEST_ASSERT_EQ(v_vfs_wait(slots[0], 0), V_VFS_EINVAL);   // not yours
  TEST_ASSERT_EQ(v_vfs_finish(slots[0], 0, 0), V_VFS_EINVAL);
  current_task = owner;

  TEST_ASSERT_EQ(v_vfs_wait(-1, 0), V_VFS_EINVAL);
  TEST_ASSERT_EQ(v_vfs_finish(VAIOS_VFS_IO_SLOTS, 0, 0), V_VFS_EINVAL);

  int handles[VAIOS_VFS_IO_SLOTS];
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS; i++) {
    { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
    handles[i] = v_vfs_finish(slots[i], 0, 0);
    TEST_ASSERT(handles[i] >= 0);
  }
  int s2 = v_vfs_submit(&extra); // released again
  TEST_ASSERT(s2 >= 0);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  int h2 = v_vfs_finish(s2, 0, 0);

  /* Close what we opened: handles are a bounded pool too, and nothing pumps the
   * closes teardown would queue after this test ends. */
  for (int i = 0; i < VAIOS_VFS_IO_SLOTS; i++) {
    v_vfs_desc_t c = {.op = 2, .fd = handles[i]};
    int cs = v_vfs_submit(&c);
    { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
    v_vfs_finish(cs, 0, 0);
  }
  if (h2 >= 0) {
    v_vfs_desc_t c = {.op = 2, .fd = h2};
    int cs = v_vfs_submit(&c);
    { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
    v_vfs_finish(cs, 0, 0);
  }
  teardown();
}

/* A task that dies in flight must not leave the worker publishing into a slot
 * nobody owns; a task that dies with a file open must have it CLOSED, so the
 * data is flushed rather than lost. */
static void test_vfs_teardown_abandons_and_closes(void) {
  setup();
  drain();

  vfs_stub.open_ret = 5;
  v_vfs_desc_t d = {.op = 1, .path = "/mnt/dies"};
  int slot = v_vfs_submit(&d);
  TEST_ASSERT(slot >= 0);
  v_test_in_handler = 1;
  v_vfs_task_teardown(current_task);
  v_test_in_handler = 0;
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }    // runs it, drops the result, frees the slot
  int again = v_vfs_submit(&d); // so the slot is available again
  TEST_ASSERT(again >= 0);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT(v_vfs_finish(again, 0, 0) >= 0);

  vfs_stub.open_ret = 6;
  v_vfs_desc_t o = {.op = 1, .path = "/mnt/open"};
  int s = v_vfs_submit(&o);
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  int fd = v_vfs_finish(s, 0, 0);
  TEST_ASSERT(fd >= 0);

  vfs_stub.close_called = 0;
  v_test_in_handler = 1;
  v_vfs_task_teardown(current_task);
  v_test_in_handler = 0;
  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); } // services the close teardown queued
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  teardown();
}

/* Waiting parks the caller through the scheduler — the deferred-result path, not
 * a spin — and the worker finishing it wakes them. */
static void test_vfs_wait_blocks_the_caller(void) {
  setup();
  drain();
  vfs_stub.open_ret = 2;
  v_vfs_desc_t d = {.op = 1, .path = "/mnt/slow"};
  int slot = v_vfs_submit(&d);
  TEST_ASSERT(slot >= 0);

  TCB *me = current_task;
  int r = v_vfs_wait(slot, 50); /* once: TEST_ASSERT_EQ evaluates twice */
  TEST_ASSERT_EQ(r, V_SYSCALL_BLOCKED);
  TEST_ASSERT_EQ(me->status, TASK_BLOCKED);

  { int p_ = pump(); TEST_ASSERT_EQ(p_, 1); }
  TEST_ASSERT(me->status != TASK_BLOCKED);
  TEST_ASSERT(v_vfs_finish(slot, 0, 0) >= 0);
  teardown();
}

static const test_case_t vfs_fd_cases[] = {
    TEST_CASE(test_vfs_needs_a_worker),
    TEST_CASE(test_vfs_mount_validates),
    TEST_CASE(test_vfs_round_trip),
    TEST_CASE(test_vfs_requests_are_bounded),
    TEST_CASE(test_vfs_slots_are_owned_and_bounded),
    TEST_CASE(test_vfs_teardown_abandons_and_closes),
    TEST_CASE(test_vfs_wait_blocks_the_caller),
};

const test_suite_t vfs_fd_suite = {
    .name = "Files through the VFS I/O worker (todo 67)",
    .cases = vfs_fd_cases,
    .count = TEST_COUNT(vfs_fd_cases),
};
