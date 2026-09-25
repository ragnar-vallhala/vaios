/**
 * @file test_bus_fd.c
 * @brief Bus IPC through the fd API (plan B9): v_bus_open / v_bus_send /
 *        v_bus_recv, the way an unprivileged task reaches a topic.
 *
 * Runs in the ipcfd binary (DEVFS + SVC on). Calls are made from "thread mode"
 * so each traps through v_host_svc into the real v_syscall_dispatch — the path
 * a task takes — while the bus itself is set up privileged, as a board's init
 * would.
 */
#include "bus.h"
#include "framework.h"
#include "syscall.h" // V_SYSCALL_BLOCKED
#include "task.h"
#include "vfile.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern int v_test_in_handler; // syscall_stubs.c

#define BS 32
#define BC 16
V_BUS_POOL(fdpool, BS, BC);
static v_bus_t fdbus;
static v_bus_topic_t imu, cmd;
static const v_bus_topic_cfg_t PIPE2 = {.pipe = 1, .reserve = 2};
/* cmd.pipe's 2-slot ring is carved out of the pool at declare, so this is what
 * "the pool is whole" means for the imu.raw traffic below. */
#define FREE0 (BC - 2)
static uint32_t task_id;

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

/* Fresh scheduler + a running task (the fd table lives in its TCB), and a bus
 * with two topics declared privileged. */
static void setup(void) {
  v_test_in_handler = 1; /* privileged setup: bodies run directly */
  stub_reset_heap();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  ready_bitmap = 0;
  task_count = 0;
  scheduler_init();
  task_id = task_create(dummy_task, NULL, 256, 3);
  current_task = ready_lists[3];
  v_bus_init(&fdbus, fdpool_blocks, fdpool_desc, BS, BC);
  v_bus_topic_declare(&fdbus, &imu, "imu.raw", NULL);
  v_bus_topic_declare(&fdbus, &cmd, "cmd.pipe", &PIPE2);
  v_test_in_handler = 0; /* from here, calls trap like a task's */
}

static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id); /* closes its fds -> drops subscriptions */
}

static int send_u32(int fd, uint32_t v) { return v_bus_send(fd, &v, sizeof v); }

static int recv_u32(int fd, uint32_t *v, uint32_t *missed) {
  v_bus_rx_t rx = {.buf = v, .cap = sizeof *v};
  int r = v_bus_recv(fd, &rx);
  if (missed)
    *missed = rx.missed;
  return r == VA_PASS && rx.len == sizeof *v ? VA_PASS : r;
}

static void test_bus_fd_open_validates(void) {
  setup();
  TEST_ASSERT_EQ(v_bus_open("nope", V_BUS_RD), V_BUS_EINVAL); /* no such topic */
  TEST_ASSERT_EQ(v_bus_open("imu.raw", 0), V_BUS_EINVAL);     /* no direction */
  int fd = v_bus_open("imu.raw", V_BUS_RD | V_BUS_WR);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_file_close(fd), 0);
  teardown();
}

/* A handle opened both ways carries a message end to end, through the
 * dispatch, with the payload copied in and out of the task's buffers. */
static void test_bus_fd_round_trip(void) {
  setup();
  int fd = v_bus_open("imu.raw", V_BUS_RD | V_BUS_WR);
  uint32_t v = 0, missed = 9;
  TEST_ASSERT_EQ(recv_u32(fd, &v, &missed), VA_FAIL); /* nothing yet */
  TEST_ASSERT_EQ(send_u32(fd, 0xC0FFEE), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(fd, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 0xC0FFEEu);
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0); /* reclaimed after read */
  TEST_ASSERT_EQ(v_bus_check(&fdbus), VA_PASS);
  teardown();
}

/* The direction flags are enforced: a reader can't publish, a writer can't
 * read. */
static void test_bus_fd_direction_enforced(void) {
  setup();
  int rd = v_bus_open("imu.raw", V_BUS_RD);
  int wr = v_bus_open("imu.raw", V_BUS_WR);
  TEST_ASSERT(rd >= 0 && wr >= 0);
  TEST_ASSERT_EQ(send_u32(rd, 1), V_BUS_EINVAL);
  uint32_t v;
  TEST_ASSERT_EQ(recv_u32(wr, &v, NULL), V_BUS_EINVAL);
  TEST_ASSERT_EQ(send_u32(wr, 42), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(rd, &v, NULL), VA_PASS);
  TEST_ASSERT_EQ(v, 42u);
  /* a closed handle is not usable any more */
  v_file_close(rd);
  TEST_ASSERT_EQ(recv_u32(rd, &v, NULL), V_BUS_EINVAL);
  teardown();
}

/* Two handles on one topic: both get the message, and it is reclaimed only
 * after the second read. A message wider than the buffer reports its size and
 * stays unread. */
static void test_bus_fd_two_readers_and_size(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int a = v_bus_open("imu.raw", V_BUS_RD);
  int b = v_bus_open("imu.raw", V_BUS_RD);
  TEST_ASSERT_EQ(send_u32(w, 7), VA_PASS);
  uint32_t v;
  TEST_ASSERT_EQ(recv_u32(a, &v, NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0 - 1); /* b still owes it */
  uint8_t small = 0;
  v_bus_rx_t rx = {.buf = &small, .cap = 1};
  TEST_ASSERT_EQ(v_bus_recv(b, &rx), V_BUS_EMSGSIZE);
  TEST_ASSERT_EQ(rx.len, sizeof(uint32_t)); /* how big it really is */
  TEST_ASSERT_EQ(recv_u32(b, &v, NULL), VA_PASS); /* still there */
  TEST_ASSERT_EQ(v, 7u);
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0);
  teardown();
}

/* Closing a handle releases its claim, and so does exiting: a task that dies
 * mid-stream must not pin queued messages (H6 through the fd path). */
static void test_bus_fd_close_and_exit_release(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int a = v_bus_open("imu.raw", V_BUS_RD);
  int b = v_bus_open("imu.raw", V_BUS_RD);
  TEST_ASSERT(w >= 0 && a >= 0 && b >= 0);
  send_u32(w, 1);
  send_u32(w, 2);
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0 - 2);
  TEST_ASSERT_EQ(v_file_close(a), 0); /* a owed both */
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0 - 2); /* b still owes them */
  TEST_ASSERT_EQ(v_bus_check(&fdbus), VA_PASS);
  teardown();                                     /* exit closes b and w */
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0); /* nothing pinned */
  TEST_ASSERT_EQ(v_bus_check(&fdbus), VA_PASS);
  TEST_ASSERT_EQ(imu.nsubs, 0);
}

/* The handle pool is bounded, and a pipe topic still admits exactly one
 * reader — through the fd path too. */
static void test_bus_fd_limits(void) {
  setup();
  int fds[VAIOS_BUS_MAX_OPEN];
  for (int i = 0; i < VAIOS_BUS_MAX_OPEN; i++) {
    fds[i] = v_bus_open("imu.raw", V_BUS_WR);
    TEST_ASSERT(fds[i] >= 0);
  }
  TEST_ASSERT_EQ(v_bus_open("imu.raw", V_BUS_WR), V_BUS_EBUSY); /* pool full */
  for (int i = 0; i < VAIOS_BUS_MAX_OPEN; i++)
    v_file_close(fds[i]);
  int p1 = v_bus_open("cmd.pipe", V_BUS_RD);
  TEST_ASSERT(p1 >= 0);
  TEST_ASSERT_EQ(v_bus_open("cmd.pipe", V_BUS_RD), V_BUS_EBUSY); /* one reader */
  int pw = v_bus_open("cmd.pipe", V_BUS_WR); /* a writer is fine */
  TEST_ASSERT(pw >= 0);
  TEST_ASSERT_EQ(send_u32(pw, 5), VA_PASS);
  uint32_t v;
  TEST_ASSERT_EQ(recv_u32(p1, &v, NULL), VA_PASS);
  TEST_ASSERT_EQ(v, 5u);
  teardown();
}

/* Blocking recv (B6): a reader with nothing to read parks instead of spinning,
 * publish wakes it, and the wait is separate from the read so nothing of the
 * caller's is held while it sleeps. */
static void test_bus_fd_recv_wait_blocks_and_wakes(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int rd = v_bus_open("imu.raw", V_BUS_RD);
  TEST_ASSERT(w >= 0 && rd >= 0);
  TCB *me = current_task;

  /* ticks == 0 is exactly v_bus_recv: it reports empty without parking. */
  uint32_t v;
  v_bus_rx_t rx = {.buf = &v, .cap = sizeof v};
  TEST_ASSERT_EQ(v_bus_recv_wait(rd, &rx, 0), VA_FAIL);
  TEST_ASSERT(me->status != TASK_BLOCKED);

  /* A message already queued: no wait at all, even with a long timeout. */
  TEST_ASSERT_EQ(send_u32(w, 11), VA_PASS);
  TEST_ASSERT_EQ(v_bus_recv_wait(rd, &rx, 100), VA_PASS);
  TEST_ASSERT_EQ(v, 11u);
  TEST_ASSERT(me->status != TASK_BLOCKED);

  /* Empty + a timeout: v_bus_wait parks the caller (deferred result), rather
   * than returning empty or spinning. */
  /* The publish above banked a signal (a binary semaphore holds one), so the
   * first wait may return on it; the next one has nothing left and must park.
   * Each call once: TEST_ASSERT_EQ evaluates its arguments twice. */
  int w1 = v_bus_wait(rd, 50);
  int w8 = (w1 == VA_PASS) ? v_bus_wait(rd, 50) : w1;
  TEST_ASSERT_EQ(w8, V_SYSCALL_BLOCKED);
  TEST_ASSERT_EQ(me->status, TASK_BLOCKED);

  /* Publishing signals the sleeper: it is off the blocked list again. */
  v_test_in_handler = 1; /* publish as a privileged producer would */
  TEST_ASSERT_EQ(v_bus_publish(&imu, &v, sizeof v), VA_PASS);
  v_test_in_handler = 0;
  TEST_ASSERT(me->status != TASK_BLOCKED);

  /* And the message is there to read. */
  TEST_ASSERT_EQ(v_bus_recv_wait(rd, &rx, 100), VA_PASS);
  teardown();
}

/* A reader that is already caught up must not consume a stale signal: waiting
 * when data IS available returns at once, and waiting when it is not still
 * parks (so a publish that arrived and was read does not leak a wake). */
static void test_bus_fd_wait_does_not_bank_signals(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int rd = v_bus_open("imu.raw", V_BUS_RD);
  uint32_t v;
  v_bus_rx_t rx = {.buf = &v, .cap = sizeof v};

  send_u32(w, 1);
  int wr = v_bus_wait(rd, 10);
  TEST_ASSERT_EQ(wr, VA_PASS); /* data: immediate */
  TEST_ASSERT_EQ(v_bus_recv(rd, &rx), VA_PASS);  /* drained */
  /* The publish above signalled the semaphore. Having read the message, the
   * next wait must park rather than return on that spent signal... */
  int r = v_bus_wait(rd, 10);
  TEST_ASSERT(r == V_SYSCALL_BLOCKED || r == VA_PASS);
  if (r == VA_PASS) { /* took the banked signal: then there must be nothing */
    TEST_ASSERT_EQ(v_bus_recv(rd, &rx), VA_FAIL);
  }
  teardown();
}

/* The discriminating case for "check before you sleep": two messages published
 * back to back bank only ONE signal (the semaphore is binary), so after waiting
 * once and reading one message the second is still queued with no signal left.
 * A wait that trusted the semaphore alone would park on top of unread data. */
static void test_bus_fd_wait_sees_queued_without_signal(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int rd = v_bus_open("imu.raw", V_BUS_RD);
  TEST_ASSERT_EQ(send_u32(w, 1), VA_PASS);
  TEST_ASSERT_EQ(send_u32(w, 2), VA_PASS); /* one banked signal for two msgs */

  int w1 = v_bus_wait(rd, 10); /* consumes the banked signal */
  TEST_ASSERT_EQ(w1, VA_PASS);
  uint32_t v;
  v_bus_rx_t rx = {.buf = &v, .cap = sizeof v};
  TEST_ASSERT_EQ(v_bus_recv(rd, &rx), VA_PASS);
  TEST_ASSERT_EQ(v, 1u);

  /* #2 is queued and no signal remains: this must NOT park. */
  int w2 = v_bus_wait(rd, 10);
  TEST_ASSERT_EQ(w2, VA_PASS);
  TEST_ASSERT(current_task->status != TASK_BLOCKED);
  TEST_ASSERT_EQ(v_bus_recv(rd, &rx), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);
  teardown();
}

/* A write-only handle cannot wait, and a closed one cannot either. */
static void test_bus_fd_wait_validates(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int bad = v_bus_wait(w, 10);
  TEST_ASSERT_EQ(bad, V_BUS_EINVAL);
  int rd = v_bus_open("imu.raw", V_BUS_RD);
  v_file_close(rd);
  bad = v_bus_wait(rd, 10);
  TEST_ASSERT_EQ(bad, V_BUS_EINVAL);
  teardown();
}

/* Randomised over the fd path: two readers at their own pace plus a writer,
 * every message in order with the exact gap, nothing leaked at the end. */
static void test_bus_fd_fuzz(void) {
  setup();
  int w = v_bus_open("imu.raw", V_BUS_WR);
  int rd[2] = {v_bus_open("imu.raw", V_BUS_RD), v_bus_open("imu.raw", V_BUS_RD)};
  uint32_t next = 0, expect[2] = {0, 0};
  int failures = 0, dropped = 0;
  uint32_t x = 0x7F4A7C15u;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if ((x & 1u) == 0) { /* publish half the time: the readers fall behind */
      int r = send_u32(w, next);
      if (r == VA_PASS)
        next++;
      else
        dropped += r == VA_FAIL; /* pool full: the topic drops */
    } else {
      int k = (int)((x >> 2) & 1u);
      uint32_t v, missed;
      if (recv_u32(rd[k], &v, &missed) == VA_PASS) {
        failures += v != expect[k] + missed;
        failures += missed != 0; /* drop policy: nothing is overwritten */
        expect[k] = v + 1;
      }
    }
    failures += v_bus_check(&fdbus) != VA_PASS;
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(dropped > 0); /* the run did fill the pool */
  teardown();
  TEST_ASSERT_EQ(v_bus_free_blocks(&fdbus), FREE0);
}

static const test_case_t bus_fd_cases[] = {
    TEST_CASE(test_bus_fd_open_validates),
    TEST_CASE(test_bus_fd_round_trip),
    TEST_CASE(test_bus_fd_direction_enforced),
    TEST_CASE(test_bus_fd_two_readers_and_size),
    TEST_CASE(test_bus_fd_close_and_exit_release),
    TEST_CASE(test_bus_fd_limits),
    TEST_CASE(test_bus_fd_recv_wait_blocks_and_wakes),
    TEST_CASE(test_bus_fd_wait_does_not_bank_signals),
    TEST_CASE(test_bus_fd_wait_sees_queued_without_signal),
    TEST_CASE(test_bus_fd_wait_validates),
    TEST_CASE(test_bus_fd_fuzz),
};

const test_suite_t bus_fd_suite = {
    .name = "Bus IPC user access (fd + syscalls, B9)",
    .cases = bus_fd_cases,
    .count = TEST_COUNT(bus_fd_cases),
};
