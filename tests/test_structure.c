/**
 * @file test_structure.c
 * @brief Unit tests for kernel/structure.c — SPSC FIFO and MPMC queue.
 *
 * structure.c was the largest untested kernel module (~11 KB) before this
 * suite. SPSC is purely lock-free; MPMC layers on top of vaios mutexes and
 * counting semaphores, both of which work on the host stub.
 */
#include "framework.h"
#include "structure.h"
#include <string.h>

extern void stub_reset_heap(void);
extern TCB *current_task;
extern TCB *idle_task;
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern uint32_t task_count;

/* Minimal current_task so MPMC's internal IPC primitives can call
 * get_current_task() during init/take/give. */
static TCB _structure_fake_task;

static void full_reset(void) {
  stub_reset_heap();
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = NULL;
  ready_bitmap = 0;
  task_count = 0;
  memset(&_structure_fake_task, 0, sizeof(_structure_fake_task));
  _structure_fake_task.task_id = 1;
  _structure_fake_task.priority = 3;
  _structure_fake_task.status = TASK_RUNNING;
  current_task = &_structure_fake_task;
  idle_task = &_structure_fake_task;
}

/* -------------------------------------------------------------------------
 * SPSC FIFO
 * ---------------------------------------------------------------------- */

static uint32_t _spsc_buf[16];
static spsc_fifo_t _spsc;

static void spsc_setup(void) {
  full_reset();
  memset(_spsc_buf, 0, sizeof(_spsc_buf));
  spsc_init(&_spsc, _spsc_buf, 16, sizeof(uint32_t));
}

static void test_spsc_init_empty(void) {
  spsc_setup();
  TEST_ASSERT_EQ(spsc_available(&_spsc), 0u);
  TEST_ASSERT(spsc_space(&_spsc) >= 15u); /* capacity-1 for a ring buffer */
}

static void test_spsc_write_read_single(void) {
  spsc_setup();
  uint32_t in = 0xDEADBEEFu, out = 0;
  TEST_ASSERT_EQ(spsc_write(&_spsc, &in, 1), 1u);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 1u);
  TEST_ASSERT_EQ(spsc_read(&_spsc, &out, 1), 1u);
  TEST_ASSERT_EQ(out, 0xDEADBEEFu);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 0u);
}

static void test_spsc_write_read_multi(void) {
  spsc_setup();
  uint32_t in[5] = {1, 2, 3, 4, 5};
  uint32_t out[5] = {0};
  TEST_ASSERT_EQ(spsc_write(&_spsc, in, 5), 5u);
  TEST_ASSERT_EQ(spsc_read(&_spsc, out, 5), 5u);
  for (uint32_t i = 0; i < 5; i++)
    TEST_ASSERT_EQ(out[i], i + 1);
}

static void test_spsc_full_drop_policy(void) {
  spsc_setup();
  spsc_set_policy(&_spsc, SPSC_POLICY_DROP);
  size_t space = spsc_space(&_spsc);
  for (size_t i = 0; i < space; i++) {
    uint32_t v = (uint32_t)i;
    TEST_ASSERT_EQ(spsc_write(&_spsc, &v, 1), 1u);
  }
  /* Queue is now full — DROP policy refuses further writes. */
  uint32_t extra = 99;
  TEST_ASSERT_EQ(spsc_write(&_spsc, &extra, 1), 0u);
  TEST_ASSERT_EQ(spsc_space(&_spsc), 0u);
}

/* Observability: spsc_peak tracks the high-water fill and spsc_drops counts
 * items the full fifo rejected. Both no-ops returning 0 when perf is off. */
static void test_spsc_perf_peak_and_drops(void) {
  spsc_setup();
  TEST_ASSERT_EQ(spsc_peak(&_spsc), 0u);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 0u);

  spsc_set_policy(&_spsc, SPSC_POLICY_DROP);
  size_t space = spsc_space(&_spsc);
  for (size_t i = 0; i < space; i++) {
    uint32_t v = (uint32_t)i;
    TEST_ASSERT_EQ(spsc_write(&_spsc, &v, 1), 1u);
  }
  /* Full: high-water == usable capacity, nothing dropped yet. */
  TEST_ASSERT_EQ(spsc_peak(&_spsc), space);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 0u);

  /* Two writes onto a full DROP fifo -> 2 drops; peak unchanged. */
  uint32_t extra = 99;
  TEST_ASSERT_EQ(spsc_write(&_spsc, &extra, 1), 0u);
  TEST_ASSERT_EQ(spsc_write(&_spsc, &extra, 1), 0u);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 2u);
  TEST_ASSERT_EQ(spsc_peak(&_spsc), space);

  /* Drain to 5 then refill below the mark — high-water is sticky. */
  uint32_t out[16];
  spsc_read(&_spsc, out, space - 5);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 5u);
  uint32_t more[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQ(spsc_write(&_spsc, more, 4), 4u);
  TEST_ASSERT_EQ(spsc_peak(&_spsc), space);
}

/* OVERWRITE policy: items the full fifo overwrites are counted as drops — the
 * branch the DROP-policy test doesn't exercise. */
static void test_spsc_overwrite_counts_drops(void) {
  spsc_setup();
  spsc_set_policy(&_spsc, SPSC_POLICY_OVERWRITE);
  size_t space = spsc_space(&_spsc);
  for (uint32_t i = 1; i <= space; i++)
    spsc_write(&_spsc, &i, 1);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 0u); /* exactly full -> none overwritten */
  TEST_ASSERT_EQ(spsc_peak(&_spsc), space);

  uint32_t more[3] = {100, 101, 102}; /* 3 onto a full fifo -> 3 overwritten */
  spsc_write(&_spsc, more, 3);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 3u);
  TEST_ASSERT_EQ(spsc_peak(&_spsc), space);

  uint32_t more2[2] = {200, 201}; /* accumulates -> 5 */
  spsc_write(&_spsc, more2, 2);
  TEST_ASSERT_EQ(spsc_drops(&_spsc), 5u);
}

static void test_spsc_full_overwrite_policy(void) {
  spsc_setup();
  size_t space = spsc_space(&_spsc);
  /* Fill perfectly, then enable OVERWRITE and push one more. */
  for (uint32_t i = 1; i <= space; i++)
    spsc_write(&_spsc, &i, 1);
  spsc_set_policy(&_spsc, SPSC_POLICY_OVERWRITE);
  uint32_t extra = 100;
  spsc_write(&_spsc, &extra, 1);
  /* The oldest element (1) was dropped; first read returns 2. */
  uint32_t first = 0;
  TEST_ASSERT_EQ(spsc_read(&_spsc, &first, 1), 1u);
  TEST_ASSERT_EQ(first, 2u);
}

static void test_spsc_wrap_around(void) {
  spsc_setup();
  /* Fill, drain, refill — exercises the wrap. */
  size_t space = spsc_space(&_spsc);
  for (uint32_t i = 0; i < space; i++)
    spsc_write(&_spsc, &i, 1);
  uint32_t drain;
  for (uint32_t i = 0; i < space; i++)
    spsc_read(&_spsc, &drain, 1);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 0u);

  uint32_t in[3] = {100, 101, 102}, out[3] = {0};
  TEST_ASSERT_EQ(spsc_write(&_spsc, in, 3), 3u);
  TEST_ASSERT_EQ(spsc_read(&_spsc, out, 3), 3u);
  TEST_ASSERT_EQ(out[0], 100u);
  TEST_ASSERT_EQ(out[1], 101u);
  TEST_ASSERT_EQ(out[2], 102u);
}

static void test_spsc_peek_does_not_consume(void) {
  spsc_setup();
  uint32_t in[3] = {7, 8, 9};
  spsc_write(&_spsc, in, 3);
  uint32_t peek[3] = {0};
  TEST_ASSERT_EQ(spsc_peek(&_spsc, peek, 3), 3u);
  TEST_ASSERT_EQ(peek[0], 7u);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 3u); /* still all there */
}

static void test_spsc_skip_consumes_without_copying(void) {
  spsc_setup();
  uint32_t in[3] = {7, 8, 9};
  spsc_write(&_spsc, in, 3);
  TEST_ASSERT_EQ(spsc_skip(&_spsc, 2), 2u);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 1u);
  uint32_t out = 0;
  spsc_read(&_spsc, &out, 1);
  TEST_ASSERT_EQ(out, 9u);
}

static void test_spsc_reset(void) {
  spsc_setup();
  uint32_t v = 42;
  spsc_write(&_spsc, &v, 1);
  spsc_reset(&_spsc);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 0u);
}

/* -------------------------------------------------------------------------
 * MPMC Queue
 * ---------------------------------------------------------------------- */

static uint32_t _mpmc_buf[8];
static mpmc_queue_t _mpmc;

static void mpmc_setup(void) {
  full_reset();
  memset(_mpmc_buf, 0, sizeof(_mpmc_buf));
  mpmc_init(&_mpmc, _mpmc_buf, 8, sizeof(uint32_t));
}

static void test_mpmc_init_empty(void) {
  mpmc_setup();
  TEST_ASSERT(mpmc_is_empty(&_mpmc));
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 0u);
  TEST_ASSERT_EQ(mpmc_capacity(&_mpmc), 8u);
}

static void test_mpmc_try_push_pop_single(void) {
  mpmc_setup();
  uint32_t in = 0xCAFEBABEu, out = 0;
  TEST_ASSERT(mpmc_try_push(&_mpmc, &in));
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 1u);
  TEST_ASSERT(mpmc_try_pop(&_mpmc, &out));
  TEST_ASSERT_EQ(out, 0xCAFEBABEu);
  TEST_ASSERT(mpmc_is_empty(&_mpmc));
}

static void test_mpmc_try_pop_empty(void) {
  mpmc_setup();
  uint32_t out = 0;
  TEST_ASSERT(!mpmc_try_pop(&_mpmc, &out));
}

static void test_mpmc_bulk(void) {
  mpmc_setup();
  uint32_t in[4] = {10, 20, 30, 40};
  uint32_t out[4] = {0};
  TEST_ASSERT_EQ(mpmc_push_bulk(&_mpmc, in, 4), 4u);
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 4u);
  TEST_ASSERT_EQ(mpmc_pop_bulk(&_mpmc, out, 4), 4u);
  TEST_ASSERT_EQ(out[0], 10u);
  TEST_ASSERT_EQ(out[3], 40u);
  TEST_ASSERT(mpmc_is_empty(&_mpmc));
}

static void test_mpmc_full_try_push_fails(void) {
  mpmc_setup();
  for (uint32_t i = 0; i < 8; i++)
    TEST_ASSERT(mpmc_try_push(&_mpmc, &i));
  TEST_ASSERT(mpmc_is_full(&_mpmc));
  uint32_t extra = 99;
  TEST_ASSERT(!mpmc_try_push(&_mpmc, &extra));
}

/* -------------------------------------------------------------------------
 * Blocking mpmc_push/mpmc_pop on a non-full / non-empty queue return
 * immediately (they never reach the scheduler), so the happy path is
 * host-testable.
 * ---------------------------------------------------------------------- */
static void test_mpmc_blocking_push_pop_immediate(void) {
  mpmc_setup();
  uint32_t in = 0xABCDu, out = 0;
  TEST_ASSERT(mpmc_push(&_mpmc, &in)); /* space available -> immediate */
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 1u);
  TEST_ASSERT(mpmc_pop(&_mpmc, &out)); /* data available -> immediate */
  TEST_ASSERT_EQ(out, 0xABCDu);
  TEST_ASSERT(mpmc_is_empty(&_mpmc));
}

/* mpmc_peek returns the oldest element without consuming it. */
static void test_mpmc_peek_does_not_consume(void) {
  mpmc_setup();
  uint32_t a = 11u, b = 22u, seen = 0;
  mpmc_try_push(&_mpmc, &a);
  mpmc_try_push(&_mpmc, &b);
  TEST_ASSERT(mpmc_peek(&_mpmc, &seen));
  TEST_ASSERT_EQ(seen, 11u);             /* oldest (FIFO) */
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 2u); /* still all there */
}

/* mpmc_peek on an empty queue reports failure. */
static void test_mpmc_peek_empty_fails(void) {
  mpmc_setup();
  uint32_t seen = 0;
  TEST_ASSERT(!mpmc_peek(&_mpmc, &seen));
}

/* mpmc_reset empties the queue and restores capacity. */
static void test_mpmc_reset_empties(void) {
  mpmc_setup();
  uint32_t v = 7u;
  for (int i = 0; i < 3; i++)
    mpmc_try_push(&_mpmc, &v);
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 3u);
  mpmc_reset(&_mpmc);
  TEST_ASSERT(mpmc_is_empty(&_mpmc));
  TEST_ASSERT_EQ(mpmc_size(&_mpmc), 0u);
  /* not_full was restored to capacity, so a full re-fill still works. */
  for (uint32_t i = 0; i < 8u; i++)
    TEST_ASSERT(mpmc_try_push(&_mpmc, &i));
  TEST_ASSERT(mpmc_is_full(&_mpmc));
}

/* Under MPMC_POLICY_OVERWRITE a push into a full queue overwrites the oldest
 * item instead of failing (the overwrite path is non-blocking). */
static void test_mpmc_overwrite_policy(void) {
  mpmc_setup();
  mpmc_set_policy(&_mpmc, MPMC_POLICY_OVERWRITE);
  for (uint32_t i = 0; i < 8u; i++) /* fill 0..7 */
    TEST_ASSERT(mpmc_try_push(&_mpmc, &i));
  TEST_ASSERT(mpmc_is_full(&_mpmc));

  uint32_t extra = 99u;
  TEST_ASSERT(mpmc_try_push(&_mpmc, &extra)); /* overwrites oldest (0) */
  TEST_ASSERT(mpmc_is_full(&_mpmc));          /* stays full */

  uint32_t out = 0;
  mpmc_try_pop(&_mpmc, &out);
  TEST_ASSERT_EQ(out, 1u); /* 0 was overwritten; head is now 1 */
}

/* -------------------------------------------------------------------------
 * SPSC zero-copy (in-place) API: write_ptr/commit_write, read_ptr/commit_read.
 * ---------------------------------------------------------------------- */
static void test_spsc_zero_copy_write_read(void) {
  spsc_setup();
  size_t wmax = 0;
  uint32_t *wp = (uint32_t *)spsc_write_ptr(&_spsc, &wmax);
  TEST_ASSERT_NOT_NULL(wp);
  TEST_ASSERT(wmax >= 3u);
  wp[0] = 100u;
  wp[1] = 200u;
  wp[2] = 300u;
  spsc_commit_write(&_spsc, 3);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 3u);

  size_t rmax = 0;
  uint32_t *rp = (uint32_t *)spsc_read_ptr(&_spsc, &rmax);
  TEST_ASSERT_NOT_NULL(rp);
  TEST_ASSERT(rmax >= 3u);
  TEST_ASSERT_EQ(rp[0], 100u);
  TEST_ASSERT_EQ(rp[2], 300u);
  spsc_commit_read(&_spsc, 3);
  TEST_ASSERT_EQ(spsc_available(&_spsc), 0u);
}

/* -------------------------------------------------------------------------
 * Request/response handoff contract (mirrors fs_owner's fs_sync_call).
 *
 * fs_owner routes cross-task vfs ops through a rendezvous built from the mpmc
 * primitive: a 1-token lock serialiser + a cap-1 request lane + a cap-1
 * response lane. The hazard the SDIO file-transfer debugging surfaced is a
 * request/response DESYNC: if a requester abandons its wait (a timeout), its
 * late response lingers in the cap-1 response lane and is mis-delivered to the
 * NEXT requester. The contract that prevents it: while holding the lock token,
 * DRAIN any stale response before issuing a request. These pin that contract on
 * the primitive — host-deterministic, since the hazard lives in the handoff
 * protocol, not in real preemption timing.
 * ---------------------------------------------------------------------- */
static mpmc_queue_t hs_lock, hs_req, hs_res;
static uint8_t hs_lock_buf[1];
static uint32_t hs_req_buf[1];
static uint32_t hs_res_buf[1];

#define HS_ANSWER(id) ((uint32_t)((id) ^ 0xA5A5u))

static void handoff_setup(void) {
  mpmc_init(&hs_lock, hs_lock_buf, 1, sizeof(uint8_t));
  mpmc_init(&hs_req, hs_req_buf, 1, sizeof(uint32_t));
  mpmc_init(&hs_res, hs_res_buf, 1, sizeof(uint32_t));
  uint8_t tok = 1u;
  mpmc_try_push(&hs_lock, &tok); /* token starts available */
}

/* The FS-task side: answer one pending request (res = req ^ MARK). The cap-1
 * response lane uses the default DROP policy, so a parked stale answer makes a
 * fresh answer drop — exactly the on-target behaviour. */
static int handoff_server_step(void) {
  uint32_t r;
  if (!mpmc_try_pop(&hs_req, &r))
    return 0;
  uint32_t ans = HS_ANSWER(r);
  mpmc_try_push(&hs_res, &ans);
  return 1;
}

/* A correct requester: take the token, DRAIN any stale response, request, let
 * the server answer, take the response, release the token. */
static uint32_t handoff_call(uint32_t id) {
  uint8_t tok;
  TEST_ASSERT(mpmc_try_pop(&hs_lock, &tok));
  uint32_t junk;
  while (mpmc_try_pop(&hs_res, &junk)) { /* drain stale (the contract) */
  }
  TEST_ASSERT(mpmc_try_push(&hs_req, &id));
  handoff_server_step();
  uint32_t res = 0;
  TEST_ASSERT(mpmc_try_pop(&hs_res, &res));
  mpmc_try_push(&hs_lock, &tok);
  return res;
}

/* Normal operation: every request gets its own response, repeatedly. */
static void test_handoff_pairs_request_with_response(void) {
  handoff_setup();
  for (uint32_t id = 1u; id <= 32u; id++)
    TEST_ASSERT_EQ(handoff_call(id), HS_ANSWER(id));
}

/* After a requester abandons its wait (a stale response left parked), the next
 * drained call still gets ITS OWN response — the contract holds. */
static void test_handoff_drain_recovers_after_abandon(void) {
  handoff_setup();
  uint8_t tok;
  uint32_t a_id = 7u;
  TEST_ASSERT(mpmc_try_pop(&hs_lock, &tok)); /* requester A */
  TEST_ASSERT(mpmc_try_push(&hs_req, &a_id));
  handoff_server_step();                /* server answers A */
  mpmc_try_push(&hs_lock, &tok);        /* A releases WITHOUT popping its res */
  TEST_ASSERT(!mpmc_is_empty(&hs_res)); /* a stale response is parked */

  TEST_ASSERT_EQ(handoff_call(11u), HS_ANSWER(11u)); /* B gets B's answer */
}

/* Documents WHY the drain is required: a naive requester that skips it reads the
 * previous requester's stale response — the exact desync the contract prevents.
 * If this ever stops holding, the handoff's cap-1/DROP assumptions changed. */
static void test_handoff_without_drain_desyncs(void) {
  handoff_setup();
  uint8_t tok;
  uint32_t a_id = 7u, b_id = 11u, got = 0u;
  /* A abandons after requesting -> res(A) parked in the cap-1 lane. */
  mpmc_try_pop(&hs_lock, &tok);
  mpmc_try_push(&hs_req, &a_id);
  handoff_server_step();
  mpmc_try_push(&hs_lock, &tok);
  /* B skips the drain: the cap-1 lane is full of A's answer, so B's answer is
   * dropped and B reads A's stale one. */
  mpmc_try_pop(&hs_lock, &tok);
  mpmc_try_push(&hs_req, &b_id);
  handoff_server_step();
  mpmc_try_pop(&hs_res, &got);
  mpmc_try_push(&hs_lock, &tok);
  TEST_ASSERT_EQ(got, HS_ANSWER(a_id)); /* A's stale answer, not B's */
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
static const test_case_t structure_cases[] = {
    /* SPSC */
    TEST_CASE(test_spsc_init_empty),
    TEST_CASE(test_spsc_write_read_single),
    TEST_CASE(test_spsc_write_read_multi),
    TEST_CASE(test_spsc_full_drop_policy),
    TEST_CASE(test_spsc_perf_peak_and_drops),
    TEST_CASE(test_spsc_overwrite_counts_drops),
    TEST_CASE(test_spsc_full_overwrite_policy),
    TEST_CASE(test_spsc_wrap_around),
    TEST_CASE(test_spsc_peek_does_not_consume),
    TEST_CASE(test_spsc_skip_consumes_without_copying),
    TEST_CASE(test_spsc_reset),
    TEST_CASE(test_spsc_zero_copy_write_read),
    /* MPMC */
    TEST_CASE(test_mpmc_init_empty),
    TEST_CASE(test_mpmc_try_push_pop_single),
    TEST_CASE(test_mpmc_try_pop_empty),
    TEST_CASE(test_mpmc_bulk),
    TEST_CASE(test_mpmc_full_try_push_fails),
    TEST_CASE(test_mpmc_blocking_push_pop_immediate),
    TEST_CASE(test_mpmc_peek_does_not_consume),
    TEST_CASE(test_mpmc_peek_empty_fails),
    TEST_CASE(test_mpmc_reset_empties),
    TEST_CASE(test_mpmc_overwrite_policy),
    /* Request/response handoff contract (fs_owner rendezvous) */
    TEST_CASE(test_handoff_pairs_request_with_response),
    TEST_CASE(test_handoff_drain_recovers_after_abandon),
    TEST_CASE(test_handoff_without_drain_desyncs),
};
const test_suite_t structure_suite = {
    .name = "Structure (SPSC FIFO & MPMC Queue)",
    .cases = structure_cases,
    .count = TEST_COUNT(structure_cases),
};
