/**
 * @file test_pbus_fd.c
 * @brief User access to the peripheral-bus arbiter: v_pbus_open + the
 *        submit / wait / finish transfer syscalls (kernel/periph_bus.c).
 *
 * Runs in the ipcfd binary (DEVFS on, SVC on). Unlike the IPC tests there,
 * calls are made from "thread mode" so each one traps through v_host_svc into
 * the real v_syscall_dispatch — the path a user task takes. The board hook is a
 * fake: it records the kernel-side descriptor, and the test plays the DMA-
 * complete IRQ with v_pbus_done_isr().
 */
#include "framework.h"
#include "periph_bus.h"
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

static v_pbus_t bus;
static const v_pbus_xfer_t *seen; // kernel descriptor the hook was handed
static int starts;
static int sync_complete; // hook completes inside start (a polled HAL)

static int fake_start(const v_pbus_xfer_t *x) {
  seen = x;
  starts++;
  if (sync_complete) {
    memset(x->rx, 0xAB, x->rx_len);
    v_pbus_done_isr(&bus, (int)x->rx_len);
  }
  return 0;
}

// Never completes on its own: a kernel job that occupies the bus.
static int blocker_start(v_pbus_job_t *j) {
  (void)j;
  return 0;
}

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

static uint32_t task_id;

// Fresh scheduler + one running task (the fd table lives in its TCB). The bus
// registration is global and survives tests, so register once.
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
  memset(&bus, 0, sizeof(bus));
  seen = NULL;
  starts = 0;
  sync_complete = 0;
  if (!registered)
    registered = v_pbus_register("i2c1", &bus, fake_start) == 0;
  v_test_in_handler = 0; // calls trap through the dispatch, like a task's
}

// Exit the task (closes its fds -> frees the handles) back in handler mode.
static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id);
}

static void test_pbus_open_unknown_fails(void) {
  setup();
  TEST_ASSERT(v_pbus_open("spi9") < 0);
  TEST_ASSERT(v_pbus_open("i2c1") >= 0);
  teardown();
}

/* Full transfer via the one-call wrapper: tx reaches the hook through a kernel
 * copy (not the task's buffer), rx comes back, rc is the hook's. */
static void test_pbus_xfer_roundtrip(void) {
  setup();
  sync_complete = 1;
  int fd = v_pbus_open("i2c1");
  uint8_t reg = 0x3B, rx[6] = {0};
  v_pbus_xfer_t x = {.addr = 0x68, .tx = &reg, .tx_len = 1, .rx = rx,
                     .rx_len = sizeof rx};
  TEST_ASSERT_EQ(v_pbus_xfer(fd, &x, 2, 10), 6);
  TEST_ASSERT_EQ(seen->addr, 0x68);
  TEST_ASSERT(seen->tx != (const void *)&reg); /* bounced */
  TEST_ASSERT_EQ(((const uint8_t *)seen->tx)[0], 0x3B);
  TEST_ASSERT_EQ(rx[0], 0xAB);
  TEST_ASSERT_EQ(rx[5], 0xAB);
  teardown();
}

/* Asynchronous completion, step by step. */
static void test_pbus_xfer_async_steps(void) {
  setup();
  int fd = v_pbus_open("i2c1");
  uint8_t rx[4] = {0};
  v_pbus_xfer_t x = {.rx = rx, .rx_len = sizeof rx};
  TEST_ASSERT_EQ(v_pbus_xfer_submit(fd, &x, 1), 0);
  TEST_ASSERT_EQ(starts, 1);
  TEST_ASSERT_EQ(v_pbus_xfer_submit(fd, &x, 1), V_PBUS_EBUSY); /* one at a time */
  memset(seen->rx, 0x5A, 4);
  v_pbus_done_isr(&bus, 0);
  TEST_ASSERT_EQ(v_pbus_xfer_wait(fd, 0), VA_PASS);
  TEST_ASSERT_EQ(v_pbus_xfer_finish(fd, rx, sizeof rx), 0);
  TEST_ASSERT_EQ(rx[3], 0x5A);
  teardown();
}

/* Timed out while queued: cancelled, never started, gone from the bus queue. */
static void test_pbus_xfer_timeout_queued_cancels(void) {
  setup();
  v_pbus_job_t hog = {.start = blocker_start};
  v_pbus_submit(&bus, &hog);
  int fd = v_pbus_open("i2c1");
  v_pbus_xfer_t x = {.rx_len = 0};
  TEST_ASSERT_EQ(v_pbus_xfer(fd, &x, 1, 0), V_PBUS_ETIMEDOUT);
  TEST_ASSERT_NULL(bus.queue);
  v_pbus_done_isr(&bus, 0); /* hog finishes: nothing else to start */
  TEST_ASSERT_EQ(starts, 0);
  teardown();
}

/* Timed out after it started: EBUSY, the fd stays busy until the bus finishes
 * it, and that late completion must not satisfy the NEXT transfer's wait. */
static void test_pbus_xfer_timeout_active_then_fresh(void) {
  setup();
  int fd = v_pbus_open("i2c1");
  v_pbus_xfer_t x = {.rx_len = 0};
  TEST_ASSERT_EQ(v_pbus_xfer(fd, &x, 1, 0), V_PBUS_EBUSY); /* on the bus */
  TEST_ASSERT_EQ(v_pbus_xfer_submit(fd, &x, 1), V_PBUS_EBUSY);
  v_pbus_done_isr(&bus, 0); /* late completion: gives the done semaphore */

  v_pbus_job_t hog = {.start = blocker_start};
  v_pbus_submit(&bus, &hog);
  TEST_ASSERT_EQ(v_pbus_xfer(fd, &x, 1, 0), V_PBUS_ETIMEDOUT); /* not stale */
  teardown();
}

/* Closing mid-transfer keeps the handle until the DMA lands, then frees it;
 * all handles are reusable afterwards. */
static void test_pbus_close_mid_transfer_frees_later(void) {
  setup();
  int fd = v_pbus_open("i2c1");
  v_pbus_xfer_t x = {.rx_len = 0};
  v_pbus_xfer_submit(fd, &x, 1);
  TEST_ASSERT_EQ(v_file_close(fd), 0);
  v_pbus_done_isr(&bus, 0);
  int fds[VAIOS_PBUS_MAX_OPEN];
  for (int i = 0; i < VAIOS_PBUS_MAX_OPEN; i++) {
    fds[i] = v_pbus_open("i2c1");
    TEST_ASSERT(fds[i] >= 0);
  }
  TEST_ASSERT(v_pbus_open("i2c1") < 0); /* table full */
  teardown();
}

/* Bad input: oversize payload, wrong fd type, finish without submit. */
static void test_pbus_bad_args(void) {
  setup();
  int fd = v_pbus_open("i2c1");
  v_pbus_xfer_t big = {.rx_len = VAIOS_PBUS_XFER_MAX + 1};
  TEST_ASSERT_EQ(v_pbus_xfer_submit(fd, &big, 1), V_PBUS_EINVAL);
  v_pbus_xfer_t x = {.rx_len = 0};
  int sem_fd = v_sem_open("notabus", V_IPC_CREATE);
  TEST_ASSERT(sem_fd >= 0);
  TEST_ASSERT_EQ(v_pbus_xfer_submit(sem_fd, &x, 1), V_PBUS_EINVAL); /* type */
  TEST_ASSERT_EQ(v_pbus_xfer_finish(fd, NULL, 0), V_PBUS_EINVAL);
  teardown();
}

static const test_case_t pbus_fd_cases[] = {
    TEST_CASE(test_pbus_open_unknown_fails),
    TEST_CASE(test_pbus_xfer_roundtrip),
    TEST_CASE(test_pbus_xfer_async_steps),
    TEST_CASE(test_pbus_xfer_timeout_queued_cancels),
    TEST_CASE(test_pbus_xfer_timeout_active_then_fresh),
    TEST_CASE(test_pbus_close_mid_transfer_frees_later),
    TEST_CASE(test_pbus_bad_args),
};

const test_suite_t pbus_fd_suite = {
    .name = "periph_bus user access (fd + transfer syscalls)",
    .cases = pbus_fd_cases,
    .count = TEST_COUNT(pbus_fd_cases),
};
