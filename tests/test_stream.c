/**
 * @file test_stream.c
 * @brief Byte streams as device nodes (kernel/stream.c): queued writes, the
 *        writer, the drop-while-down policy, one reader, the RX ring.
 *
 * A fake backend records what the writer sends; the test plays the ISR side
 * with v_stream_rx_push_isr. File calls trap through the dispatch like a
 * task's; the writer runs "privileged" (handler mode).
 */
#include "framework.h"
#include "stream.h"
#include "task.h"
#include "vfile.h"
#include "vfs.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern int v_test_in_handler;

static v_stream_t s;
static uint8_t sent[2 * V_STREAM_TX];
static uint32_t nsent, calls;
static int link_up = 1;

static int fake_tx(v_stream_t *st, const uint8_t *p, uint32_t n) {
  (void)st;
  memcpy(sent + nsent, p, n);
  nsent += n;
  calls++;
  return 0;
}
static int fake_up(v_stream_t *st) {
  (void)st;
  return link_up;
}

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

static uint32_t task_id;

// The stream (and its devfs node) is global: register it once, then reset
// its queues per test.
static void setup(void) {
  static int registered;
  stub_reset_heap();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  ready_bitmap = 0;
  task_count = 0;
  scheduler_init();
  task_id = task_create(dummy_task, NULL, 256, 3);
  current_task = ready_lists[3];
  if (!registered) {
    s.tx = fake_tx;
    s.up = fake_up;
    registered = v_stream_register(&s, "/dev/link0") == 0;
  }
  s.tx_head = s.tx_tail = s.rx_head = s.rx_tail = 0;
  s.dropped = s.tx_errors = s.rx_dropped = 0;
  s.reader = 0;
  nsent = calls = 0;
  link_up = 1;
  v_test_in_handler = 0;
}

static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id);
}

static void writer(void) {
  v_test_in_handler = 1;
  v_stream_writer_step(&s, 0);
  v_test_in_handler = 0;
}

static void test_stream_write_then_writer_sends(void) {
  setup();
  int fd = v_file_open("/dev/link0", VFS_O_WRONLY);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_file_write(fd, "hello", 5), 5);
  TEST_ASSERT_EQ(nsent, 0); // only queued
  writer();
  TEST_ASSERT_EQ(nsent, 5);
  TEST_ASSERT(memcmp(sent, "hello", 5) == 0);
  teardown();
}

// A run that crosses the end of the queue goes out as two tx() calls, in order.
static void test_stream_wrap_sends_in_order(void) {
  setup();
  int fd = v_file_open("/dev/link0", VFS_O_WRONLY);
  s.tx_head = s.tx_tail = V_STREAM_TX - 3;
  TEST_ASSERT_EQ(v_file_write(fd, "abcdef", 6), 6);
  writer();
  writer();
  TEST_ASSERT_EQ(calls, 2);
  TEST_ASSERT_EQ(nsent, 6);
  TEST_ASSERT(memcmp(sent, "abcdef", 6) == 0);
  teardown();
}

// A full queue takes what fits; the caller retries the rest.
static void test_stream_full_queue_takes_what_fits(void) {
  setup();
  int fd = v_file_open("/dev/link0", VFS_O_WRONLY);
  static uint8_t big[V_STREAM_TX + 10];
  TEST_ASSERT_EQ(v_file_write(fd, big, sizeof big), V_STREAM_TX - 1);
  TEST_ASSERT_EQ(v_file_write(fd, big, 1), 0);
  teardown();
}

// Far end gone: writes are taken and dropped, never block, and are counted.
static void test_stream_down_drops(void) {
  setup();
  int fd = v_file_open("/dev/link0", VFS_O_WRONLY);
  link_up = 0;
  TEST_ASSERT_EQ(v_file_write(fd, "lost", 4), 4);
  TEST_ASSERT_EQ(s.dropped, 4);
  writer();
  TEST_ASSERT_EQ(nsent, 0);
  teardown();
}

static void test_stream_one_reader(void) {
  setup();
  int r = v_file_open("/dev/link0", VFS_O_RDONLY);
  TEST_ASSERT(r >= 0);
  TEST_ASSERT_EQ(v_file_open("/dev/link0", VFS_O_RDWR), V_STREAM_EBUSY);
  int w = v_file_open("/dev/link0", VFS_O_WRONLY); // writers: any number
  TEST_ASSERT(w >= 0);
  TEST_ASSERT_EQ(v_file_read(w, sent, 1), -1); // a writer can't read
  TEST_ASSERT_EQ(v_file_close(r), 0);
  r = v_file_open("/dev/link0", VFS_O_RDWR);
  TEST_ASSERT(r >= 0); // reader slot free again
  teardown();
  TEST_ASSERT_EQ(s.reader, 0); // the task's exit released it
}

static void test_stream_rx_ring(void) {
  setup();
  int r = v_file_open("/dev/link0", VFS_O_RDONLY);
  TEST_ASSERT_EQ(v_file_wait(r, 0), -1); // nothing yet
  v_stream_rx_push_isr(&s, (const uint8_t *)"ping", 4);
  TEST_ASSERT_EQ(v_file_wait(r, 0), 0);
  uint8_t buf[8];
  TEST_ASSERT_EQ(v_file_read(r, buf, sizeof buf), 4);
  TEST_ASSERT(memcmp(buf, "ping", 4) == 0);
  TEST_ASSERT_EQ(v_file_read(r, buf, sizeof buf), 0);
  static uint8_t flood[V_STREAM_RX + 5];
  v_stream_rx_push_isr(&s, flood, sizeof flood);
  TEST_ASSERT_EQ(s.rx_dropped, 6); // the ring holds V_STREAM_RX - 1
  teardown();
}

static const test_case_t stream_cases[] = {
    TEST_CASE(test_stream_write_then_writer_sends),
    TEST_CASE(test_stream_wrap_sends_in_order),
    TEST_CASE(test_stream_full_queue_takes_what_fits),
    TEST_CASE(test_stream_down_drops),
    TEST_CASE(test_stream_one_reader),
    TEST_CASE(test_stream_rx_ring),
};

const test_suite_t stream_suite = {
    .name = "byte streams (/dev nodes)",
    .cases = stream_cases,
    .count = TEST_COUNT(stream_cases),
};
