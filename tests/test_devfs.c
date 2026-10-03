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

static int memmem_simple(const char *h, int n, const char *w) {
  int m = (int)strlen(w);
  for (int i = 0; i + m <= n; i++)
    if (memcmp(h + i, w, (size_t)m) == 0)
      return 1;
  return 0;
}

static TCB g_task;
static int registered;

extern void (*g_stub_console_rx_sink)(const uint8_t *, uint16_t);
extern int g_stub_console_rx_armed;
extern char g_stub_console_out[];
extern int g_stub_console_out_len;

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
 * "/dev/cap" would answer for "/dev/capsule" and hand out the wrong device. */
static void test_open_longer_path_is_not_a_prefix_match(void) {
  reset();
  TEST_ASSERT(v_file_open("/dev/capsule", 0) < 0);
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
  uint8_t in[300], out[300];
  for (int i = 0; i < 300; i++)
    in[i] = (uint8_t)i;
  g_stub_console_rx_sink(in, 300);
  int n = v_file_read(0, out, sizeof out);
  TEST_ASSERT_EQ(n, 255); /* CONSOLE_RX_SIZE 256, one slot kept empty */
  TEST_ASSERT_EQ(memcmp(out, in, (size_t)n), 0);
}

/* Before any writer has run, a console write goes straight to the hardware --
 * the path every example without a writer task keeps. Runs first in the suite:
 * once a writer has stepped, the console stays deferred. */
static void test_console_write_direct_without_writer(void) {
  reset();
  g_stub_console_out_len = 0;
  TEST_ASSERT_EQ(v_file_write(1, "hi", 2), 2);
  TEST_ASSERT_EQ(g_stub_console_out_len, 2);
  TEST_ASSERT_EQ(memcmp(g_stub_console_out, "hi", 2), 0);
}

/* With a writer, a write only queues: nothing reaches the hardware until the
 * writer steps, then all of it does, in order. */
static void test_console_write_deferred_to_writer(void) {
  reset();
  TEST_ASSERT_EQ(v_console_writer_step(0), 0); /* writer up, nothing queued */
  g_stub_console_out_len = 0;
  TEST_ASSERT_EQ(v_file_write(1, "help", 4), 4);
  TEST_ASSERT_EQ(v_file_write(1, "\r\n", 2), 2);
  TEST_ASSERT_EQ(g_stub_console_out_len, 0); /* the syscall did not touch it */
  TEST_ASSERT_EQ(v_console_writer_step(0), 6);
  TEST_ASSERT_EQ(g_stub_console_out_len, 6);
  TEST_ASSERT_EQ(memcmp(g_stub_console_out, "help\r\n", 6), 0);
  TEST_ASSERT_EQ(v_console_writer_step(0), 0); /* drained, signal consumed */
}

/* A write bigger than the queue is taken in part, and the count says so;
 * the caller retries the rest rather than the syscall waiting for space. */
static void test_console_write_full_is_partial(void) {
  reset();
  (void)v_console_writer_step(0);
  char big[300];
  memset(big, 'z', sizeof big);
  TEST_ASSERT_EQ(v_file_write(1, big, sizeof big), 255); /* 256, one kept empty */
  TEST_ASSERT_EQ(v_file_write(1, big, 1), 0);
  g_stub_console_out_len = 0;
  TEST_ASSERT_EQ(v_console_writer_step(0), 255);
  TEST_ASSERT_EQ(v_file_write(1, big, 45), 45); /* space again */
  TEST_ASSERT_EQ(v_console_writer_step(0), 45);
}

/* A lone LF goes out as CRLF (a tty's ONLCR), so a file with LF endings does
 * not staircase across the terminal; an existing CRLF is left alone. */
static void test_console_lf_becomes_crlf(void) {
  reset();
  (void)v_console_writer_step(0);
  g_stub_console_out_len = 0;
  TEST_ASSERT_EQ(v_file_write(1, "a\nb\r\nc\n", 7), 7);
  (void)v_console_writer_step(0);
  TEST_ASSERT_EQ(g_stub_console_out_len, 9);
  TEST_ASSERT_EQ(memcmp(g_stub_console_out, "a\r\nb\r\nc\r\n", 9), 0);
}

/* dup / dup2 copy a plain device's fd, and refuse anything whose close
 * counts (here: /dev/cap, which has a close op), or replacing one. */
static void test_dup(void) {
  reset();
  int d = v_file_dup(1); /* the console: plain */
  TEST_ASSERT(d >= 3);
  TEST_ASSERT_EQ(v_file_write(d, "x", 1), 1);
  int capfd = v_file_open("/dev/capture", 0);
  TEST_ASSERT_EQ(v_file_dup2(capfd, 1), 1);  /* fd 1 now captures */
  TEST_ASSERT_EQ(v_file_write(1, "a\r\nb", 4), 4);
  char buf[8] = {0};
  TEST_ASSERT_EQ(v_file_read(capfd, buf, sizeof buf), 3); /* '\r' dropped */
  TEST_ASSERT_EQ(memcmp(buf, "a\nb", 3), 0);
  TEST_ASSERT_EQ(v_file_dup2(d, 1), 1); /* the console back */
  int c = v_file_open("/dev/cap", 0);   /* has a close op */
  TEST_ASSERT(v_file_dup(c) < 0);
  TEST_ASSERT(v_file_dup2(1, c) < 0); /* would skip its close */
  TEST_ASSERT(v_file_dup2(1, 99) < 0);
}

/* /dev/capture, filled past its 2 KiB without being drained (a command that
 * never asks between chunks): it keeps what fits, says where it was cut, and
 * is empty again once read out. */
static void test_capture_full_is_marked(void) {
  reset();
  int fd = v_file_open("/dev/capture", 0);
  TEST_ASSERT(fd >= 0);
  char x[100];
  memset(x, 'x', sizeof x);
  for (int i = 0; i < 30; i++)
    TEST_ASSERT_EQ(v_file_write(fd, x, sizeof x), (int)sizeof x); /* 3000 */
  static char got[4096];
  int n = 0, r;
  while ((r = v_file_read(fd, got + n, sizeof got - (uint32_t)n)) > 0)
    n += r;
  TEST_ASSERT(n > 1900 && n <= 2048);
  TEST_ASSERT(memmem_simple(got, n, "output cut here"));
  TEST_ASSERT_EQ(v_file_read(fd, got, 16), 0); /* drained: empty again */
  v_file_close(fd);
}

/* A device with no wait op cannot be waited on. */
static void test_wait_unsupported(void) {
  reset();
  int fd = v_file_open("/dev/cap", 0);
  TEST_ASSERT_EQ(v_file_wait(fd, 1), -1);
}

static const test_case_t devfs_cases[] = {
    TEST_CASE(test_console_write_direct_without_writer),
    TEST_CASE(test_console_rx_reaches_fd0),
    TEST_CASE(test_console_wait),
    TEST_CASE(test_console_rx_full_drops),
    TEST_CASE(test_wait_unsupported),
    TEST_CASE(test_dup),
    TEST_CASE(test_capture_full_is_marked),
    TEST_CASE(test_console_write_deferred_to_writer),
    TEST_CASE(test_console_write_full_is_partial),
    TEST_CASE(test_console_lf_becomes_crlf),
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
