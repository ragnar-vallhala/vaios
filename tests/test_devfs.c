/*
 * test_devfs — the everything-is-a-file device layer (kernel/devfs.c): the /dev
 * registry, the per-task fd table, and the v_file_* API. This is the
 * unprivileged I/O path under Stage 5, but devfs is #if VAIOS_DEVFS and tied to
 * the fd table in the TCB, so it runs in its own binary built with DEVFS=1 /
 * SVC=0 (the trap-once shims compile out; the bodies run directly).
 */
#include "framework.h"
#include "task.h"
#include "vfile.h"
#include <string.h>

extern TCB *current_task;

/* A capture device: write appends to cap_buf, read serves from cap_src. */
static char cap_buf[64];
static int cap_len;
static const char *cap_src;
static int cap_src_pos;
static int cap_close_calls;

static int cap_write(void *priv, const void *buf, uint32_t len) {
  (void)priv;
  for (uint32_t i = 0; i < len && cap_len < (int)sizeof cap_buf; i++)
    cap_buf[cap_len++] = ((const char *)buf)[i];
  return (int)len;
}
static int cap_read(void *priv, void *buf, uint32_t len) {
  (void)priv;
  uint32_t n = 0;
  while (n < len && cap_src && cap_src[cap_src_pos])
    ((char *)buf)[n++] = cap_src[cap_src_pos++];
  return (int)n;
}
static int cap_close(void *priv) {
  (void)priv;
  cap_close_calls++;
  return 0;
}
static const v_file_ops cap_ops = {cap_read, cap_write, cap_close, NULL, NULL};
static const v_file_ops other_ops = {0, 0, 0, 0, 0};

static TCB g_task;
static int registered;

extern void (*g_stub_console_rx_sink)(const uint8_t *, uint16_t);
extern int g_stub_console_rx_armed;

static void reset(void) {
  if (!registered) {
    g_stub_console_rx_armed = 1; /* this binary tests the interrupt path */
    v_devfs_init();                          /* /dev/console, /dev/kmsg */
    v_devfs_register("/dev/cap", &cap_ops, (void *)0x1234);
    registered = 1;
  }
  memset(&g_task, 0, sizeof g_task);
  current_task = &g_task;
  v_fd_table_init(&g_task); /* clears fds, pre-opens 0/1/2 -> /dev/console */
  { /* drain the console and any banked wake a previous case left behind */
    char d[256];
    while (v_file_read(0, d, sizeof d) > 0)
      ;
    (void)v_file_wait(0, 0);
  }
  cap_len = 0;
  cap_src = 0;
  cap_src_pos = 0;
  cap_close_calls = 0;
}

static void test_fd_table_preopens_console(void) {
  reset();
  TEST_ASSERT_NOT_NULL(g_task.fds[0].ops);
  TEST_ASSERT_NOT_NULL(g_task.fds[1].ops);
  TEST_ASSERT_NOT_NULL(g_task.fds[2].ops);
}

static void test_open_known_device(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  TEST_ASSERT_EQ(fd, 3); /* 0/1/2 are the pre-opened console */
}

/* A node name is matched whole: only a name ending in '/' claims the paths under
 * it (that is how a mount works — see v_vfs_mount). Without that rule
 * "/dev/cap" would answer for "/dev/capture" and hand out the wrong device. */
static void test_open_longer_path_is_not_a_prefix_match(void) {
  reset();
  TEST_ASSERT(v_file_open("/dev/capture", 0) < 0);
  TEST_ASSERT(v_file_open("/dev/cap/more", 0) < 0);
  TEST_ASSERT(v_file_open("/dev/ca", 0) < 0);
  TEST_ASSERT(v_file_open("/dev/cap", 0) >= 0); /* the node itself still opens */
}

static void test_open_unknown_returns_error(void) {
  reset();
  TEST_ASSERT_EQ(v_file_open("/dev/nope", 0), -1);
}

static void test_write_reaches_device(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  TEST_ASSERT_EQ(v_file_write(fd, "hi", 2), 2);
  TEST_ASSERT_EQ(cap_len, 2);
  TEST_ASSERT(cap_buf[0] == 'h' && cap_buf[1] == 'i');
}

static void test_read_from_device(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  cap_src = "XY";
  char buf[4] = {0};
  TEST_ASSERT_EQ(v_file_read(fd, buf, 2), 2);
  TEST_ASSERT(buf[0] == 'X' && buf[1] == 'Y');
}

static void test_close_frees_fd_and_calls_op(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  TEST_ASSERT_EQ(v_file_close(fd), 0);
  TEST_ASSERT_EQ(cap_close_calls, 1);
  /* the slot is now free: writing to the closed fd fails */
  TEST_ASSERT_EQ(v_file_write(fd, "x", 1), -1);
}

static void test_bad_fd_rejected(void) {
  reset();
  TEST_ASSERT_EQ(v_file_write(-1, "x", 1), -1);
  TEST_ASSERT_EQ(v_file_write(9999, "x", 1), -1);
  TEST_ASSERT_EQ(v_file_read(-1, cap_buf, 1), -1);
  TEST_ASSERT_EQ(v_file_close(9999), -1);
}

static void test_fd_alloc_exhausts(void) {
  reset(); /* fds 0/1/2 taken by console; 3..7 free (VAIOS_MAX_FDS=8) */
  int got = 0;
  for (int i = 0; i < 16; i++)
    if (v_fd_alloc(&cap_ops, 0) >= 0)
      got++;
  TEST_ASSERT_EQ(got, 5); /* exactly the 5 remaining slots */
  TEST_ASSERT_EQ(v_fd_alloc(&cap_ops, 0), -1);
}

static void test_fd_obj_typecheck(void) {
  reset();
  int fd = v_fd_alloc(&cap_ops, (void *)0x1234);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_fd_obj(fd, &cap_ops), (void *)0x1234);
  TEST_ASSERT_NULL(v_fd_obj(fd, &other_ops)); /* wrong ops -> NULL */
  TEST_ASSERT_NULL(v_fd_obj(9999, &cap_ops)); /* bad fd -> NULL */
}

/* What the console RX interrupt delivers is what fd 0 reads, in order, across
 * calls -- the bytes sit in kernel memory between the ISR and the reader. */
static void test_console_rx_reaches_fd0(void) {
  reset();
  char buf[8] = {0};
  TEST_ASSERT_EQ(v_file_read(0, buf, sizeof buf), 0); /* nothing yet */
  g_stub_console_rx_sink((const uint8_t *)"ab", 2);
  g_stub_console_rx_sink((const uint8_t *)"c", 1);
  TEST_ASSERT_EQ(v_file_read(0, buf, 2), 2);
  TEST_ASSERT_EQ(memcmp(buf, "ab", 2), 0);
  TEST_ASSERT_EQ(v_file_read(0, buf, sizeof buf), 1);
  TEST_ASSERT_EQ(buf[0], 'c');
}

/* A wait returns at once when bytes are already queued, is woken by the ISR's
 * signal otherwise, and times out when nothing came. */
static void test_console_wait(void) {
  reset();
  char buf[4];
  TEST_ASSERT_EQ(v_file_wait(0, 5), -1); /* empty, no signal: timed out */
  g_stub_console_rx_sink((const uint8_t *)"x", 1);
  TEST_ASSERT_EQ(v_file_wait(0, 5), 0); /* data queued */
  TEST_ASSERT_EQ(v_file_read(0, buf, sizeof buf), 1);
  TEST_ASSERT_EQ(v_file_wait(0, 5), 0);  /* the signal is banked... */
  TEST_ASSERT_EQ(v_file_wait(0, 5), -1); /* ...once */
}

/* A burst bigger than the ring keeps what fits and drops the rest, never
 * overwriting what the reader has not taken yet. */
static void test_console_rx_full_drops(void) {
  reset();
  uint8_t in[200], out[200];
  for (int i = 0; i < 200; i++)
    in[i] = (uint8_t)i;
  g_stub_console_rx_sink(in, 200);
  int n = v_file_read(0, out, sizeof out);
  TEST_ASSERT_EQ(n, 127); /* CONSOLE_RX_SIZE 128, one slot kept empty */
  TEST_ASSERT_EQ(memcmp(out, in, (size_t)n), 0);
}

/* A device with no wait op cannot be waited on. */
static void test_wait_unsupported(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  TEST_ASSERT_EQ(v_file_wait(fd, 1), -1);
}

static const test_case_t devfs_cases[] = {
    TEST_CASE(test_console_rx_reaches_fd0),
    TEST_CASE(test_console_wait),
    TEST_CASE(test_console_rx_full_drops),
    TEST_CASE(test_wait_unsupported),
    TEST_CASE(test_fd_table_preopens_console),
    TEST_CASE(test_open_known_device),
    TEST_CASE(test_open_unknown_returns_error),
    TEST_CASE(test_open_longer_path_is_not_a_prefix_match),
    TEST_CASE(test_write_reaches_device),
    TEST_CASE(test_read_from_device),
    TEST_CASE(test_close_frees_fd_and_calls_op),
    TEST_CASE(test_bad_fd_rejected),
    TEST_CASE(test_fd_alloc_exhausts),
    TEST_CASE(test_fd_obj_typecheck),
};
const test_suite_t devfs_suite = {
    .name = "devfs (fd table + /dev)",
    .cases = devfs_cases,
    .count = TEST_COUNT(devfs_cases),
};
