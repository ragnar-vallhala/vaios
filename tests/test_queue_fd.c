/**
 * @file test_queue_fd.c
 * @brief Queues through the fd API (M4): v_queue_register / v_queue_open /
 *        v_queue_send / v_queue_recv, the way an unprivileged task reaches the
 *        MPMC queue and an already-existing SPSC fifo.
 *
 * Runs in the ipcfd binary (DEVFS + SVC on), so the calls trap through
 * v_host_svc into the real v_syscall_dispatch — the path a task takes — while
 * the queues themselves are declared and registered privileged, as a board's
 * init would.
 */
#include "framework.h"
#include "structure.h"
#include "syscall.h" // V_SYSCALL_BLOCKED
#include "task.h"
#include "vfile.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern int v_test_in_handler; // syscall_stubs.c

#define CAP 4
#define ELEM sizeof(uint32_t)

static mpmc_queue_t mq;
static uint32_t mq_buf[CAP];
static spsc_fifo_t sq;
static uint32_t sq_buf[CAP];
static uint32_t task_id;

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

/* Fresh scheduler + a running task (the fd table lives in its TCB), and the two
 * queues registered privileged. The registry is static and additive, so the
 * names are registered once for the whole binary. */
static void setup(void) {
  v_test_in_handler = 1; // privileged setup: bodies run directly
  stub_reset_heap();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  task_count = 0;
  scheduler_init();
  task_id = task_create(dummy_task, NULL, 256, 3);
  current_task = ready_lists[3];

  static int registered;
  mpmc_init(&mq, mq_buf, CAP, ELEM);
  spsc_init(&sq, sq_buf, CAP, ELEM);
  if (!registered) {
    v_queue_register("/q/telemetry", &mq);
    v_queue_register_spsc("/q/hot", &sq);
    registered = 1;
  }
  v_test_in_handler = 0; // from here, calls trap like a task's
}

static void teardown(void) {
  v_test_in_handler = 1;
  task_exit_request(task_id); /* closes its fds -> releases handles */
}

static int send_u32(int fd, uint32_t v, uint32_t ticks) {
  return v_queue_send(fd, &v, ticks);
}
static int recv_u32(int fd, uint32_t *v, uint32_t ticks) {
  return v_queue_recv(fd, v, ticks);
}

/* Registration is by name, and a name means one queue for the whole system. */
static void test_queue_register_validates(void) {
  setup();
  v_test_in_handler = 1;
  TEST_ASSERT_EQ(v_queue_register(NULL, &mq), V_Q_EINVAL);
  TEST_ASSERT_EQ(v_queue_register("", &mq), V_Q_EINVAL);
  TEST_ASSERT_EQ(v_queue_register("/q/telemetry", &mq), V_Q_EINVAL); // taken
  TEST_ASSERT_EQ(v_queue_register("/q/other", NULL), V_Q_EINVAL);
  v_test_in_handler = 0;
  TEST_ASSERT_EQ(v_queue_open("/q/nope", V_Q_RD), V_Q_EINVAL); // no such queue
  TEST_ASSERT_EQ(v_queue_open("/q/telemetry", 0), V_Q_EINVAL); // no direction
  teardown();
}

/* An MPMC handle opened both ways carries an element end to end, through the
 * dispatch, with the payload copied in and out of the task's own buffers. */
static void test_queue_fd_round_trip(void) {
  setup();
  int fd = v_queue_open("/q/telemetry", V_Q_RD | V_Q_WR);
  TEST_ASSERT(fd >= 0);
  uint32_t v = 0;
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_FAIL); // empty
  TEST_ASSERT_EQ(send_u32(fd, 0xABCDEF, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(v, 0xABCDEFu);
  TEST_ASSERT_EQ(v_file_close(fd), 0);
  /* a closed handle is not usable any more */
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), V_Q_EINVAL);
  teardown();
}

/* Direction flags are enforced per handle, both queue kinds. */
static void test_queue_fd_direction_enforced(void) {
  setup();
  int rd = v_queue_open("/q/telemetry", V_Q_RD);
  int wr = v_queue_open("/q/telemetry", V_Q_WR);
  TEST_ASSERT(rd >= 0 && wr >= 0);
  uint32_t v;
  TEST_ASSERT_EQ(send_u32(rd, 1, 0), V_Q_EINVAL);
  TEST_ASSERT_EQ(recv_u32(wr, &v, 0), V_Q_EINVAL);
  TEST_ASSERT_EQ(send_u32(wr, 9, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(rd, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(v, 9u);
  teardown();
}

/* The queue's capacity is the queue's business: a full one refuses rather than
 * overwriting (the default policy), and an empty one reports empty. */
static void test_queue_fd_bounds(void) {
  setup();
  int fd = v_queue_open("/q/telemetry", V_Q_RD | V_Q_WR);
  for (uint32_t i = 0; i < CAP; i++)
    TEST_ASSERT_EQ(send_u32(fd, i, 0), VA_PASS);
  TEST_ASSERT_EQ(send_u32(fd, 99, 0), VA_FAIL); // full, drop policy
  uint32_t v;
  for (uint32_t i = 0; i < CAP; i++) {
    TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
    TEST_ASSERT_EQ(v, i); // FIFO order survives the fd layer
  }
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_FAIL); // empty again
  teardown();
}

/* An SPSC queue admits one reader and one writer — the lock-free property is the
 * caller's promise, so the kernel holds them to it — and closing gives the side
 * back. It has nothing to sleep on, so a timeout does not make it block. */
static void test_queue_fd_spsc_exclusive(void) {
  setup();
  int w = v_queue_open("/q/hot", V_Q_WR);
  int r = v_queue_open("/q/hot", V_Q_RD);
  TEST_ASSERT(w >= 0 && r >= 0);
  TEST_ASSERT_EQ(v_queue_open("/q/hot", V_Q_WR), V_Q_EBUSY);
  TEST_ASSERT_EQ(v_queue_open("/q/hot", V_Q_RD), V_Q_EBUSY);

  uint32_t v;
  TEST_ASSERT_EQ(send_u32(w, 5, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(r, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(v, 5u);
  /* Empty with a timeout: no semaphore, so it reports empty instead of parking */
  TEST_ASSERT_EQ(recv_u32(r, &v, 100), VA_FAIL);
  TEST_ASSERT(current_task->status != TASK_BLOCKED);

  /* Closing releases that side for someone else. */
  TEST_ASSERT_EQ(v_file_close(w), 0);
  int w2 = v_queue_open("/q/hot", V_Q_WR);
  TEST_ASSERT(w2 >= 0);
  teardown();
}

/* The handle pool is bounded, and exiting releases every handle a task held —
 * including the SPSC sides, which would otherwise stay taken forever. */
static void test_queue_fd_limits_and_exit_release(void) {
  setup();
  int fds[VAIOS_QUEUE_MAX_OPEN];
  for (int i = 0; i < VAIOS_QUEUE_MAX_OPEN; i++) {
    fds[i] = v_queue_open("/q/telemetry", V_Q_WR);
    TEST_ASSERT(fds[i] >= 0);
  }
  TEST_ASSERT_EQ(v_queue_open("/q/telemetry", V_Q_WR), V_Q_EBUSY); // pool full
  for (int i = 0; i < VAIOS_QUEUE_MAX_OPEN; i++)
    TEST_ASSERT_EQ(v_file_close(fds[i]), 0);

  /* Take an SPSC side and die holding it. */
  int r = v_queue_open("/q/hot", V_Q_RD);
  TEST_ASSERT(r >= 0);
  teardown(); /* task exit closes its fds */

  /* The side is free again, and so is the handle pool. */
  setup();
  int r2 = v_queue_open("/q/hot", V_Q_RD);
  TEST_ASSERT(r2 >= 0);
  teardown();
}

/* Blocking receive on the MPMC queue: an empty queue with a timeout parks the
 * caller (deferred result) rather than spinning or reporting empty, and the wake
 * hint is given by WHOEVER pushes — including privileged code that never goes
 * near the fd layer, which is why the hint lives in the queue and not in the
 * registry. */
static void test_queue_fd_recv_blocks_and_wakes(void) {
  setup();
  int fd = v_queue_open("/q/telemetry", V_Q_RD | V_Q_WR);
  TEST_ASSERT(fd >= 0);
  TCB *me = current_task;
  uint32_t v;

  /* ticks == 0 is a plain try: it reports empty without parking. */
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_FAIL);
  TEST_ASSERT(me->status != TASK_BLOCKED);

  /* An element already there: no wait, even with a long timeout. */
  TEST_ASSERT_EQ(send_u32(fd, 4, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 100), VA_PASS);
  TEST_ASSERT_EQ(v, 4u);
  TEST_ASSERT(me->status != TASK_BLOCKED);

  /* Empty + a timeout: park. The push above banked a hint, so the first wait may
   * return on it; the next has nothing left. */
  int w1 = v_queue_wait(fd, 50, 0);
  int w2 = (w1 == VA_PASS) ? v_queue_wait(fd, 50, 0) : w1;
  TEST_ASSERT_EQ(w2, V_SYSCALL_BLOCKED);
  TEST_ASSERT_EQ(me->status, TASK_BLOCKED);

  /* A PRIVILEGED producer pushing straight into the queue wakes it. */
  v_test_in_handler = 1;
  uint32_t item = 77;
  TEST_ASSERT(mpmc_try_push(&mq, &item));
  v_test_in_handler = 0;
  TEST_ASSERT(me->status != TASK_BLOCKED);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 100), VA_PASS);
  TEST_ASSERT_EQ(v, 77u);
  teardown();
}

/* A blocked SENDER wakes when a consumer makes room, and the queue's own
 * counters are untouched by the waiting: after the wake the element really goes
 * in, and the queue's size accounting still adds up. */
static void test_queue_fd_send_blocks_until_room(void) {
  setup();
  int fd = v_queue_open("/q/telemetry", V_Q_RD | V_Q_WR);
  for (uint32_t i = 0; i < CAP; i++)
    TEST_ASSERT_EQ(send_u32(fd, i, 0), VA_PASS);
  TEST_ASSERT_EQ(mpmc_size(&mq), (size_t)CAP);

  /* Full: waiting for room parks (after any banked hint is spent). */
  int w1 = v_queue_wait(fd, 50, 1);
  int w2 = (w1 == VA_PASS) ? v_queue_wait(fd, 50, 1) : w1;
  TEST_ASSERT_EQ(w2, V_SYSCALL_BLOCKED);
  TEST_ASSERT_EQ(current_task->status, TASK_BLOCKED);

  /* A privileged consumer makes room and the sender wakes. */
  v_test_in_handler = 1;
  uint32_t out = 0;
  TEST_ASSERT(mpmc_try_pop(&mq, &out));
  v_test_in_handler = 0;
  TEST_ASSERT(current_task->status != TASK_BLOCKED);
  TEST_ASSERT_EQ(out, 0u);

  /* And the counters are consistent: room for exactly one more. */
  TEST_ASSERT_EQ(send_u32(fd, 42, 0), VA_PASS);
  TEST_ASSERT_EQ(mpmc_size(&mq), (size_t)CAP);
  TEST_ASSERT_EQ(send_u32(fd, 43, 0), VA_FAIL);
  teardown();
}

/* Check the queue, not just the hint. Two pushes bank ONE hint (it is binary),
 * so after waiting once and popping once there is still an element with no hint
 * left — a wait that trusted the signal alone would park on top of it. The
 * mirror image holds for a full queue and a sender. */
static void test_queue_fd_wait_sees_state_without_signal(void) {
  setup();
  int fd = v_queue_open("/q/telemetry", V_Q_RD | V_Q_WR);
  TEST_ASSERT_EQ(send_u32(fd, 1, 0), VA_PASS);
  TEST_ASSERT_EQ(send_u32(fd, 2, 0), VA_PASS);

  int w1 = v_queue_wait(fd, 10, 0); /* consumes the banked hint */
  TEST_ASSERT_EQ(w1, VA_PASS);
  uint32_t v;
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(v, 1u);

  /* Element 2 is queued and no hint remains: this must NOT park. */
  int w2 = v_queue_wait(fd, 10, 0);
  TEST_ASSERT_EQ(w2, VA_PASS);
  TEST_ASSERT(current_task->status != TASK_BLOCKED);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);

  /* Same for room: fill it, pop twice (two hints, banked as one), and a wait
   * for room must see the space rather than sleep. */
  for (uint32_t i = 0; i < CAP; i++)
    TEST_ASSERT_EQ(send_u32(fd, i, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
  TEST_ASSERT_EQ(recv_u32(fd, &v, 0), VA_PASS);
  int ww1 = v_queue_wait(fd, 10, 1); /* consumes the banked hint */
  TEST_ASSERT_EQ(ww1, VA_PASS);
  int ww2 = v_queue_wait(fd, 10, 1);
  TEST_ASSERT_EQ(ww2, VA_PASS);
  TEST_ASSERT(current_task->status != TASK_BLOCKED);
  teardown();
}

static const test_case_t queue_fd_cases[] = {
    TEST_CASE(test_queue_register_validates),
    TEST_CASE(test_queue_fd_round_trip),
    TEST_CASE(test_queue_fd_direction_enforced),
    TEST_CASE(test_queue_fd_bounds),
    TEST_CASE(test_queue_fd_spsc_exclusive),
    TEST_CASE(test_queue_fd_limits_and_exit_release),
    TEST_CASE(test_queue_fd_recv_blocks_and_wakes),
    TEST_CASE(test_queue_fd_send_blocks_until_room),
    TEST_CASE(test_queue_fd_wait_sees_state_without_signal),
};

const test_suite_t queue_fd_suite = {
    .name = "Queues on the fd table (M4)",
    .cases = queue_fd_cases,
    .count = TEST_COUNT(queue_fd_cases),
};
