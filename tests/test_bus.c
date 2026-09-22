/**
 * @file test_bus.c
 * @brief Bus IPC subsystem (kernel/bus.c), per docs/plans/bus-subsystem.md:
 *        B1 the block-pool allocator (gate: invariant H1 holds under fuzz),
 *        B2 topics, publish and polling pop (gate: ordering, ref-count reclaim),
 *        B3 unsubscribe, overwrite, slow subscribers (gate: H4/H5/H6),
 *        the zero-copy path (reserve/commit, peek/release, pinning), and
 *        soft reservations (per-topic stash, loans, reclaim).
 */
#include "bus.h"
#include "framework.h"
#include <string.h>

#define BS 16
#define BC 8
V_BUS_POOL(pool, BS, BC);
static v_bus_t bus;

static void reset(void) {
  memset(pool_blocks, 0, sizeof pool_blocks);
  v_bus_init(&bus, pool_blocks, pool_desc, BS, BC);
}

static void test_bus_init_validates(void) {
  static v_bus_t b;
  TEST_ASSERT_EQ(v_bus_init(NULL, pool_blocks, pool_desc, BS, BC), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, NULL, pool_desc, BS, BC), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, NULL, BS, BC), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, 0, BC), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, 8, BC), VA_FAIL);  /* < hdr */
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, 18, BC), VA_FAIL); /* %4 */
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, BS, 0), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, BS, V_BUS_NIL),
                 VA_FAIL);
  TEST_ASSERT_EQ(v_bus_init(&b, pool_blocks, pool_desc, BS, BC), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&b), BC);
  TEST_ASSERT_EQ(v_bus_check(&b), VA_PASS);
}

/* H1: a message gets all the blocks it needs or none — never a partial chain.
 * 8 blocks of 16 bytes: the first carries the header + 4 payload bytes, each
 * further one 16. */
static v_bus_topic_t pt;
static v_bus_sub_t ps;
static uint8_t payload[4 + 16 * (BC - 1)];
#define LEN_FOR(blocks) ((uint16_t)(4 + 16 * ((blocks) - 1)))

static void test_bus_alloc_all_or_nothing(void) {
  reset();
  v_bus_topic_declare(&bus, &pt, "p", NULL);
  v_bus_subscribe(&pt, &ps);
  TEST_ASSERT_EQ(v_bus_publish(&pt, payload, LEN_FOR(5)), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), 3);
  TEST_ASSERT_EQ(v_bus_publish(&pt, payload, LEN_FOR(4)), VA_FAIL); /* 3 left */
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), 3);                       /* untouched */
  TEST_ASSERT_EQ(v_bus_publish(&pt, payload, LEN_FOR(3)), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), 0);
  TEST_ASSERT_EQ(v_bus_publish(&pt, NULL, 0), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_check(&bus), VA_PASS);
  /* reading them hands every block back */
  static uint8_t back[sizeof payload];
  uint16_t len;
  TEST_ASSERT_EQ(v_bus_pop(&ps, back, sizeof back, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(len, LEN_FOR(5));
  TEST_ASSERT_EQ(v_bus_pop(&ps, back, sizeof back, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), BC);
  TEST_ASSERT_EQ(v_bus_check(&bus), VA_PASS);
  /* a message wider than the whole pool is refused outright */
  static uint8_t huge[LEN_FOR(BC) + 1];
  TEST_ASSERT_EQ(v_bus_publish(&pt, huge, sizeof huge), V_BUS_EINVAL);
}

/* ---- B2: topics, publish, polling pop --------------------------------------
 * Pool of 16 x 32-byte blocks: a message's first block carries the 12-byte
 * header + 20 payload bytes, each further block 32. */
#define MS 32
#define MC 16
#define FIRST (MS - V_BUS_HDR_SIZE)
V_BUS_POOL(mp, MS, MC);
static v_bus_t mb;
static v_bus_topic_t tp;
static v_bus_sub_t sa, sb, sc;

static void mreset(void) {
  v_bus_init(&mb, mp_blocks, mp_desc, MS, MC);
  v_bus_topic_declare(&mb, &tp, "imu.raw", NULL);
}

static int pop_u32(v_bus_sub_t *s, uint32_t *v) {
  uint16_t len = 0;
  int r = v_bus_pop(s, v, sizeof *v, &len, NULL);
  return r == VA_PASS && len == sizeof *v ? VA_PASS : r;
}

static void test_bus_topic_declare(void) {
  static v_bus_topic_t dup, other;
  mreset();
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &dup, "imu.raw", NULL), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &tp, "again", NULL), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &other, "baro", NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_topic_declare(NULL, &other, "x", NULL), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_subscribe(&tp, &sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_subscribe(&tp, &sa), V_BUS_EINVAL); /* no re-link */
}

/* Gate: one subscriber reads messages in publish order, then nothing; the
 * pool is whole again once everything is read. */
static void test_bus_publish_pop_order(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  for (uint32_t i = 1; i <= 5; i++)
    TEST_ASSERT_EQ(v_bus_publish(&tp, &i, sizeof i), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 5);
  for (uint32_t i = 1; i <= 5; i++) {
    uint32_t v = 0;
    TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
    TEST_ASSERT_EQ(v, i);
  }
  uint32_t v;
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Gate: a message's blocks stay until the LAST subscriber has read it. */
static void test_bus_refcount_reclaim(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  v_bus_subscribe(&tp, &sb);
  uint32_t v = 7;
  v_bus_publish(&tp, &v, sizeof v);
  v_bus_publish(&tp, &v, sizeof v);
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 2); /* B still owes both */
  TEST_ASSERT_EQ(pop_u32(&sb, &v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 1);
  TEST_ASSERT_EQ(pop_u32(&sb, &v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* No subscriber: nothing is kept. A late subscriber sees only what comes
 * after it, and earlier messages don't wait for it. */
static void test_bus_late_subscriber(void) {
  mreset();
  uint32_t v = 1;
  TEST_ASSERT_EQ(v_bus_publish(&tp, &v, sizeof v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC); /* nobody to deliver to */
  v_bus_subscribe(&tp, &sa);
  v = 2;
  v_bus_publish(&tp, &v, sizeof v);
  v_bus_subscribe(&tp, &sb); /* joins after message 2 */
  v = 3;
  v_bus_publish(&tp, &v, sizeof v);
  TEST_ASSERT_EQ(pop_u32(&sb, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 3u);
  TEST_ASSERT_EQ(pop_u32(&sb, &v), VA_FAIL);
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 1); /* only 3 left, owed by A */
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 3u);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

/* A payload spanning several blocks round-trips byte for byte, using exactly
 * the blocks it needs; a zero-length message is a valid event. */
static void test_bus_multiblock_and_empty(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  static uint8_t big[FIRST + 2 * MS + 5], back[sizeof big];
  for (unsigned i = 0; i < sizeof big; i++)
    big[i] = (uint8_t)(i * 7 + 1);
  TEST_ASSERT_EQ(v_bus_publish(&tp, big, sizeof big), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 4); /* hdr+20, 32, 32, 5 */
  TEST_ASSERT_EQ(v_bus_publish(&tp, NULL, 0), VA_PASS);
  uint16_t len = 0;
  TEST_ASSERT_EQ(v_bus_pop(&sa, back, sizeof back, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(len, sizeof big);
  TEST_ASSERT_EQ(memcmp(big, back, sizeof big), 0);
  TEST_ASSERT_EQ(v_bus_pop(&sa, back, sizeof back, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(len, 0);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

/* A too-small buffer reports the size and leaves the message unread. */
static void test_bus_pop_too_small(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  uint32_t two[2] = {11, 22}, got[2] = {0};
  v_bus_publish(&tp, two, sizeof two);
  uint16_t len = 0;
  TEST_ASSERT_EQ(v_bus_pop(&sa, got, 4, &len, NULL), V_BUS_EMSGSIZE);
  TEST_ASSERT_EQ(len, sizeof two);
  TEST_ASSERT_EQ(v_bus_pop(&sa, got, sizeof got, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(got[1], 22u);
}

/* A full pool drops the new message (and leaks nothing); a message the pool
 * could never hold is refused outright. */
static void test_bus_publish_full_pool(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  uint32_t v = 0;
  for (int i = 0; i < MC; i++)
    TEST_ASSERT_EQ(v_bus_publish(&tp, &v, sizeof v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_publish(&tp, &v, sizeof v), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  static uint8_t huge[FIRST + MC * MS];
  TEST_ASSERT_EQ(v_bus_publish(&tp, huge, sizeof huge), V_BUS_EINVAL);
  while (pop_u32(&sa, &v) == VA_PASS)
    ;
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

/* Randomised: 3 subscribers each popping at their own pace see a gap-free,
 * in-order stream of every message published while the pool had room, the
 * pool invariant holds throughout, and draining returns every block. */
static void test_bus_fuzz_three_readers(void) {
  mreset();
  v_bus_sub_t *subs[3] = {&sa, &sb, &sc};
  uint32_t expect[3] = {0, 0, 0}, next = 0, dropped_any = 0;
  for (int i = 0; i < 3; i++)
    v_bus_subscribe(&tp, subs[i]);
  int failures = 0;
  uint32_t x = 0x9E3779B9u;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if ((x & 3u) == 0) {
      int r = v_bus_publish(&tp, &next, sizeof next);
      if (r == VA_PASS)
        next++;
      else
        dropped_any |= r == VA_FAIL; /* pool full: dropped, not queued */
    } else {
      int k = (int)((x >> 2) % 3u);
      uint32_t v;
      if (pop_u32(subs[k], &v) == VA_PASS)
        failures += v != expect[k]++; /* in order, no gaps */
    }
    failures += v_bus_check(&mb) != VA_PASS;
  }
  for (int k = 0; k < 3; k++) {
    uint32_t v;
    while (pop_u32(subs[k], &v) == VA_PASS)
      failures += v != expect[k]++;
    failures += expect[k] != next; /* each saw every queued message */
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(dropped_any); /* the run did hit a full pool */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

/* ---- B3: unsubscribe, overwrite, slow subscribers ------------------------ */
static const v_bus_topic_cfg_t OVERWRITE = {.overflow = V_BUS_OVERWRITE};
static v_bus_topic_t tw; /* an overwrite topic */

static void wreset(void) {
  v_bus_init(&mb, mp_blocks, mp_desc, MS, MC);
  v_bus_topic_declare(&mb, &tw, "att.est", &OVERWRITE);
}

static int pop_m(v_bus_sub_t *s, uint32_t *v, uint32_t *missed) {
  uint16_t len = 0;
  return v_bus_pop(s, v, sizeof *v, &len, missed);
}

/* H6: unsubscribing releases exactly what the leaving subscriber owed. */
static void test_bus_unsubscribe_releases(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  v_bus_subscribe(&tp, &sb);
  for (uint32_t i = 0; i < 3; i++)
    v_bus_publish(&tp, &i, sizeof i);
  uint32_t v;
  pop_u32(&sa, &v); /* A read #0; B owes all three */
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sb), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 2); /* #0 freed, A owes #1 #2 */
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sb), V_BUS_EINVAL); /* already gone */
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sa), VA_PASS);       /* last reader */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* A detached subscription can join again and sees only new messages. */
  TEST_ASSERT_EQ(v_bus_subscribe(&tp, &sa), VA_PASS);
  v = 9;
  v_bus_publish(&tp, &v, sizeof v);
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 9u);
}

/* Overwrite: a full pool evicts the topic's oldest; the slow reader skips
 * ahead and learns how many it lost. A drop topic refuses instead. */
static void test_bus_overwrite_reports_missed(void) {
  wreset();
  v_bus_subscribe(&tw, &sa);
  for (uint32_t i = 0; i < MC + 4; i++)
    TEST_ASSERT_EQ(v_bus_publish(&tw, &i, sizeof i), VA_PASS);
  uint32_t v, missed = 99;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 4u);
  TEST_ASSERT_EQ(missed, 4u);
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 5u);
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* the default policy drops instead */
  mreset();
  v_bus_subscribe(&tp, &sb);
  for (uint32_t i = 0; i < MC; i++)
    v_bus_publish(&tp, &i, sizeof i);
  TEST_ASSERT_EQ(v_bus_publish(&tp, &v, sizeof v), VA_FAIL);
}

/* A reader that keeps up loses nothing while a slow one on the same topic
 * gets overwritten. */
static void test_bus_overwrite_fast_reader_unaffected(void) {
  wreset();
  v_bus_subscribe(&tw, &sa); /* fast */
  v_bus_subscribe(&tw, &sb); /* never reads until the end */
  uint32_t v, missed;
  for (uint32_t i = 0; i < 3 * MC; i++) {
    v_bus_publish(&tw, &i, sizeof i);
    TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
    TEST_ASSERT_EQ(v, i);
    TEST_ASSERT_EQ(missed, 0u);
  }
  TEST_ASSERT_EQ(pop_m(&sb, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 2u * MC);   /* the oldest MC survived */
  TEST_ASSERT_EQ(missed, 2u * MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Overwrite only evicts when that can actually make room: another topic
 * holding the pool isn't this topic's to evict. */
static void test_bus_overwrite_no_futile_eviction(void) {
  static v_bus_topic_t other;
  wreset();
  v_bus_topic_declare(&mb, &other, "log", NULL);
  v_bus_subscribe(&tw, &sa);
  v_bus_subscribe(&other, &sb);
  uint32_t v = 1;
  v_bus_publish(&tw, &v, sizeof v); /* tw holds 1 block */
  for (int i = 0; i < MC - 1; i++)
    v_bus_publish(&other, &v, sizeof v); /* other holds the rest */
  static uint8_t two_blocks[FIRST + 1];
  TEST_ASSERT_EQ(v_bus_publish(&tw, two_blocks, sizeof two_blocks), VA_FAIL);
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS); /* its message wasn't evicted */
  TEST_ASSERT_EQ(v, 1u);
}

/* H4/H5: a message evicted WHILE a pop copies it (an ISR publish preempting
 * the reader) must not be returned torn: pop sees the block's epoch change,
 * drops the copy and reads the next message, reporting the gap. */
extern void (*v_bus_test_mid_pop)(void);
static void evict_during_pop(void) {
  v_bus_test_mid_pop = NULL; /* once */
  uint32_t v = 1000;
  v_bus_publish(&tw, &v, sizeof v); /* pool full: evicts the head */
}
static void test_bus_evicted_mid_pop_rereads(void) {
  wreset();
  v_bus_subscribe(&tw, &sa);
  for (uint32_t i = 0; i < MC; i++)
    v_bus_publish(&tw, &i, sizeof i); /* pool exactly full */
  v_bus_test_mid_pop = evict_during_pop;
  uint32_t v = 0, missed = 0;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_NULL(v_bus_test_mid_pop); /* the eviction did run mid-pop */
  TEST_ASSERT_EQ(v, 1u);      /* not #0, which was freed under the copy */
  TEST_ASSERT_EQ(missed, 1u); /* #0 is reported lost */
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Randomised: overwrite topic, one reader that stays plus two that come and
 * go. Every pop is in order with missed == the exact gap; the invariant holds
 * after every step; once everyone leaves, the pool is whole. */
static void test_bus_fuzz_churn_overwrite(void) {
  wreset();
  v_bus_sub_t *subs[3] = {&sa, &sb, &sc};
  int on[3] = {1, 1, 1};
  uint32_t expect[3] = {0, 0, 0}, next = 0;
  for (int k = 0; k < 3; k++)
    v_bus_subscribe(&tw, subs[k]);
  int failures = 0, gaps = 0;
  uint32_t x = 0x1B873593u;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    int k = (int)((x >> 3) % 3u);
    switch (x & 7u) {
    case 0: case 1: case 2: /* publish (always someone subscribed: sa stays) */
      failures += v_bus_publish(&tw, &next, sizeof next) != VA_PASS;
      next++;
      break;
    case 3: /* churn one of the two that may leave */
      if (k == 0)
        break;
      if (on[k]) {
        failures += v_bus_unsubscribe(subs[k]) != VA_PASS;
      } else {
        failures += v_bus_subscribe(&tw, subs[k]) != VA_PASS;
        expect[k] = next; /* sees only what comes next */
      }
      on[k] = !on[k];
      break;
    default: { /* pop */
      uint32_t v, missed;
      if (on[k] && pop_m(subs[k], &v, &missed) == VA_PASS) {
        failures += v != expect[k] + missed; /* in order, gap exact */
        gaps += missed != 0;
        expect[k] = v + 1;
      }
    }
    }
    failures += v_bus_check(&mb) != VA_PASS;
  }
  for (int k = 0; k < 3; k++)
    if (on[k])
      failures += v_bus_unsubscribe(subs[k]) != VA_PASS;
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(gaps > 0); /* overwrite did happen */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* ---- Zero copy: reserve/commit, peek/release ------------------------------ */
static void test_bus_zc_reserve_commit(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  uint16_t t, len;
  uint32_t *w = v_bus_reserve(&tp, sizeof(uint32_t), &t);
  TEST_ASSERT(w != NULL);
  TEST_ASSERT((uint8_t *)w > mp_blocks && (uint8_t *)w < mp_blocks + sizeof mp_blocks);
  *w = 0xC0FFEEu; /* written in place */
  TEST_ASSERT_EQ(v_bus_pop(&sa, &len, 0, &len, NULL), VA_FAIL); /* not yet */
  TEST_ASSERT_EQ(v_bus_commit(&tp, t, sizeof(uint32_t)), VA_PASS);
  TEST_ASSERT_EQ(v_bus_commit(&tp, t, sizeof(uint32_t)), V_BUS_EINVAL); /* twice */
  TEST_ASSERT_EQ(v_bus_cancel(&tp, t), V_BUS_EINVAL); /* already published */
  uint32_t v;
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 0xC0FFEEu);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  /* limits: one block's payload; commit no longer than that */
  TEST_ASSERT_NULL(v_bus_reserve(&tp, FIRST + 1, &t));
  TEST_ASSERT(v_bus_reserve(&tp, FIRST, &t) != NULL);
  TEST_ASSERT_EQ(v_bus_commit(&tp, t, FIRST + 1), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_cancel(&tp, t), VA_PASS); /* given back */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* committed with nobody subscribed any more: nothing is kept */
  v_bus_reserve(&tp, 4, &t);
  v_bus_unsubscribe(&sa);
  TEST_ASSERT_EQ(v_bus_commit(&tp, t, 4), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

static void test_bus_zc_peek_release(void) {
  mreset();
  v_bus_subscribe(&tp, &sa);
  v_bus_subscribe(&tp, &sb);
  uint32_t v = 41, missed = 9;
  v_bus_publish(&tp, &v, sizeof v);
  const uint32_t *p;
  uint16_t len;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, &missed), VA_PASS);
  TEST_ASSERT_EQ(*p, 41u);                /* read in place */
  TEST_ASSERT_EQ(len, sizeof v);
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), V_BUS_EBUSY);
  TEST_ASSERT_EQ(v_bus_pop(&sa, &v, sizeof v, &len, NULL), V_BUS_EBUSY);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  TEST_ASSERT_EQ(v_bus_release(&sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_release(&sa), V_BUS_EINVAL); /* nothing held */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 1);   /* B still owes it */
  TEST_ASSERT_EQ(v_bus_peek(&sb, (const void **)&p, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_release(&sb), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), VA_FAIL);
  /* a multi-block message isn't contiguous: peek refuses, pop reads it */
  static uint8_t big[FIRST + 1], back[sizeof big];
  v_bus_publish(&tp, big, sizeof big);
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), V_BUS_ESPLIT);
  TEST_ASSERT_EQ(v_bus_pop(&sa, back, sizeof back, &len, NULL), VA_PASS);
}

/* A peeked (pinned) oldest message is never evicted under its reader: the
 * overwrite topic drops instead until the peek is released. */
static void test_bus_zc_pinned_not_evicted(void) {
  wreset();
  v_bus_subscribe(&tw, &sa);
  for (uint32_t i = 0; i < MC; i++)
    v_bus_publish(&tw, &i, sizeof i); /* full */
  const uint32_t *p;
  uint16_t len, t;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), VA_PASS);
  uint32_t v = 99;
  TEST_ASSERT_EQ(v_bus_publish(&tw, &v, sizeof v), VA_FAIL);
  TEST_ASSERT_NULL(v_bus_reserve(&tw, 4, &t));
  TEST_ASSERT_EQ(*p, 0u); /* still intact */
  v_bus_release(&sa);     /* #0 consumed: a block is free again */
  TEST_ASSERT_EQ(v_bus_publish(&tw, &v, sizeof v), VA_PASS);
  TEST_ASSERT_EQ(v_bus_publish(&tw, &v, sizeof v), VA_PASS); /* evicts #1 */
  uint32_t missed;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);
  TEST_ASSERT_EQ(missed, 1u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* unsubscribing with a peek held drops the pin — also when another reader
   * still owes that message, so it stays queued and must be evictable. */
  v_bus_subscribe(&tw, &sb);
  v = 7;
  v_bus_publish(&tw, &v, sizeof v); /* owed by A and B */
  while (pop_m(&sa, &v, &missed) == VA_PASS && v != 7u)
    ; /* A catches up to it... */
  for (uint32_t i = 0; i < MC; i++)
    v_bus_publish(&tw, &i, sizeof i); /* ...then fill the pool again */
  TEST_ASSERT_EQ(v_bus_peek(&sb, (const void **)&p, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sb), VA_PASS); /* A still owes the rest */
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);       /* no stale pin left */
  TEST_ASSERT_EQ(v_bus_publish(&tw, &v, sizeof v), VA_PASS); /* evictable */
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Randomised, overwrite topic: copy publish, reserve+commit, reserve+cancel,
 * copy pop and peek+release — peeks HELD across other steps so pinning races
 * eviction. Every read is in order with missed == the exact gap; nothing
 * read in place ever changes under its reader; invariant every step. */
static void test_bus_fuzz_zero_copy(void) {
  wreset();
  v_bus_sub_t *subs[3] = {&sa, &sb, &sc};
  uint32_t expect[3] = {0, 0, 0}, next = 0, seen[3] = {0};
  const uint32_t *held[3] = {NULL, NULL, NULL};
  for (int k = 0; k < 3; k++)
    v_bus_subscribe(&tw, subs[k]);
  int failures = 0, pinned_drops = 0;
  uint32_t x = 0x85EBCA6Bu;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    int k = (int)((x >> 4) % 3u);
    uint16_t t, len;
    uint32_t v, missed;
    switch (x & 15u) {
    case 0: case 1: case 2: /* copy publish */
      if (v_bus_publish(&tw, &next, sizeof next) == VA_PASS)
        next++;
      else
        pinned_drops++; /* only a pinned head can make overwrite fail */
      break;
    case 3: case 4: { /* zero-copy publish */
      uint32_t *w = v_bus_reserve(&tw, sizeof next, &t);
      if (!w) {
        pinned_drops++;
        break;
      }
      *w = next;
      failures += v_bus_commit(&tw, t, sizeof next) != VA_PASS;
      next++;
      break;
    }
    case 5: /* reserve, then change our mind */
      if (v_bus_reserve(&tw, 4, &t))
        failures += v_bus_cancel(&tw, t) != VA_PASS;
      break;
    case 6: case 7: case 8: /* copy pop */
      if (!held[k] && pop_m(subs[k], &v, &missed) == VA_PASS) {
        failures += v != expect[k] + missed;
        expect[k] = v + 1;
      }
      break;
    case 9: case 10: case 11: /* peek and hold */
      if (!held[k] && v_bus_peek(subs[k], (const void **)&held[k], &len,
                                 &missed) == VA_PASS) {
        seen[k] = *held[k];
        failures += seen[k] != expect[k] + missed;
      }
      break;
    default: /* release a held peek — its value must not have changed */
      if (held[k]) {
        failures += *held[k] != seen[k];
        failures += v_bus_release(subs[k]) != VA_PASS;
        expect[k] = seen[k] + 1;
        held[k] = NULL;
      }
    }
    for (int j = 0; j < 3; j++) /* a held view stays valid throughout */
      failures += held[j] && *held[j] != seen[j];
    failures += v_bus_check(&mb) != VA_PASS;
  }
  for (int k = 0; k < 3; k++) {
    if (held[k])
      v_bus_release(subs[k]);
    v_bus_unsubscribe(subs[k]);
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(pinned_drops > 0); /* pinning really did block eviction */
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* ---- Soft reservations ------------------------------------------------------
 * Pool of 16 x 32-byte blocks (one u32 message = one block). */
static v_bus_topic_t tg, tx; /* reserved drop topic, reserved overwrite topic */

static int pub_u32(v_bus_topic_t *t, uint32_t v) {
  return v_bus_publish(t, &v, sizeof v);
}

static void test_bus_reserve_declare(void) {
  static const v_bus_topic_cfg_t R6 = {.reserve = 6}, R20 = {.reserve = 20};
  mreset();
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &tg, "cmd", &R6), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 6); /* moved into its stash */
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &tx, "big", &R20), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* a reserved topic's traffic cycles through its stash, never the pool */
  v_bus_subscribe(&tg, &sa);
  for (uint32_t i = 0; i < 50; i++) {
    TEST_ASSERT_EQ(pub_u32(&tg, i), VA_PASS);
    uint32_t v;
    TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  }
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 6);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* The guarantee: a reserved (drop) topic keeps its capacity while an
 * overwrite topic with a reader that never reads floods the shared pool. */
static void test_bus_reserve_survives_flood(void) {
  static const v_bus_topic_cfg_t R4 = {.reserve = 4};
  wreset();
  v_bus_topic_declare(&mb, &tg, "log", &R4);
  v_bus_subscribe(&tw, &sa); /* never reads */
  v_bus_subscribe(&tg, &sb);
  for (uint32_t i = 0; i < 100; i++)
    pub_u32(&tw, i); /* pool full, then borrows, then evicts itself */
  for (uint32_t i = 0; i < 4; i++)
    TEST_ASSERT_EQ(pub_u32(&tg, i), VA_PASS); /* all 4 reserved blocks */
  TEST_ASSERT_EQ(pub_u32(&tg, 4), VA_FAIL);   /* its own limit: drop */
  uint32_t v;
  TEST_ASSERT_EQ(pop_u32(&sb, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 0u);
  TEST_ASSERT_EQ(pub_u32(&tg, 5), VA_PASS); /* the freed block came back */
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Soft: an overwrite topic borrows idle reserved blocks when the pool is
 * empty; the owner takes them back by evicting the borrower's oldest. A drop
 * topic never borrows. */
static void test_bus_reserve_lend_and_reclaim(void) {
  static const v_bus_topic_cfg_t R4 = {.reserve = 4};
  static v_bus_topic_t td;
  wreset();
  v_bus_topic_declare(&mb, &tg, "log", &R4);
  v_bus_topic_declare(&mb, &td, "drop", NULL);
  v_bus_subscribe(&tw, &sa); /* overwrite, never reads for now */
  v_bus_subscribe(&tg, &sb);
  v_bus_subscribe(&td, &sc);
  for (uint32_t i = 0; i < MC - 4; i++)
    pub_u32(&tw, i); /* pool exhausted */
  TEST_ASSERT_EQ(pub_u32(&td, 0), VA_FAIL); /* drop topics don't borrow */
  TEST_ASSERT_EQ(tg.lent, 0);
  for (uint32_t i = MC - 4; i < MC; i++)
    TEST_ASSERT_EQ(pub_u32(&tw, i), VA_PASS); /* borrows log's 4 idle blocks */
  TEST_ASSERT_EQ(tg.lent, 4);
  TEST_ASSERT_EQ(tw.borrowed, 4);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* log wants its capacity back: reclaim evicts overwrite's oldest */
  TEST_ASSERT_EQ(pub_u32(&tg, 100), VA_PASS);
  TEST_ASSERT_EQ(pub_u32(&tg, 101), VA_PASS);
  TEST_ASSERT_EQ(tg.lent, 2);
  uint32_t v, missed;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);      /* #0 and #1 were evicted to repay */
  TEST_ASSERT_EQ(missed, 2u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  /* everyone leaves: every block comes home, loans settled */
  v_bus_unsubscribe(&sa);
  v_bus_unsubscribe(&sb);
  v_bus_unsubscribe(&sc);
  TEST_ASSERT_EQ(tg.lent + tw.borrowed, 0);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb) + tg.stash_count, MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* A peek pins the borrower's oldest message, so the owner can't reclaim
 * through it until it's released. */
static void test_bus_reserve_reclaim_blocked_by_pin(void) {
  static const v_bus_topic_cfg_t R2 = {.reserve = 2};
  wreset();
  v_bus_topic_declare(&mb, &tg, "log", &R2);
  v_bus_subscribe(&tw, &sa);
  v_bus_subscribe(&tg, &sb);
  for (uint32_t i = 0; i < MC; i++)
    pub_u32(&tw, i); /* uses the pool and borrows log's 2 blocks */
  TEST_ASSERT_EQ(tg.lent, 2);
  const uint32_t *p;
  uint16_t len;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(pub_u32(&tg, 1), VA_FAIL); /* oldest pinned: can't reclaim */
  TEST_ASSERT_EQ(*p, 0u);
  v_bus_release(&sa); /* consumed #0: its block repays log */
  TEST_ASSERT_EQ(tg.lent, 1);
  TEST_ASSERT_EQ(pub_u32(&tg, 1), VA_PASS);
  TEST_ASSERT_EQ(pub_u32(&tg, 2), VA_PASS); /* reclaims the second by eviction */
  TEST_ASSERT_EQ(tg.lent, 0);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Randomised: a reserved drop topic G (reserve 5), an overwrite topic W with
 * two readers (no reserve), and an overwrite topic X (reserve 3) that can
 * lend and borrow — copy and zero-copy publishes, pops and held peeks.
 * Invariants every step, and the guarantee itself: whenever G holds fewer than
 * its reserve and no peek is pinning a borrower, G's publish succeeds. */
static void test_bus_fuzz_reservations(void) {
  static const v_bus_topic_cfg_t G5 = {.reserve = 5},
      X3 = {.overflow = V_BUS_OVERWRITE, .reserve = 3};
  static v_bus_sub_t gs, ws1, ws2, xs;
  v_bus_init(&mb, mp_blocks, mp_desc, MS, MC);
  v_bus_topic_declare(&mb, &tg, "G", &G5);
  v_bus_topic_declare(&mb, &tw, "W", &OVERWRITE);
  v_bus_topic_declare(&mb, &tx, "X", &X3);
  v_bus_subscribe(&tg, &gs);
  v_bus_subscribe(&tw, &ws1);
  v_bus_subscribe(&tw, &ws2);
  v_bus_subscribe(&tx, &xs);
  v_bus_topic_t *topics[3] = {&tg, &tw, &tx};
  v_bus_sub_t *subs[4] = {&gs, &ws1, &ws2, &xs};
  const uint32_t *held[4] = {NULL, NULL, NULL, NULL};
  uint32_t next[3] = {0, 0, 0}, expect[4] = {0, 0, 0, 0};
  const int topic_of[4] = {0, 1, 1, 2};
  int failures = 0, guarantee_checks = 0, borrowed_seen = 0;
  uint32_t x = 0xCC9E2D51u;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    int t = (int)((x >> 4) % 3u), k = (int)((x >> 8) % 4u);
    uint16_t tk, len;
    uint32_t v, missed;
    switch (x & 7u) {
    case 0: case 1: { /* copy publish */
      int pinned = held[1] || held[2] || held[3];
      int must = t == 0 && tg.blocks < 5 && !pinned;
      int r = pub_u32(topics[t], next[t]);
      if (r == VA_PASS)
        next[t]++;
      if (must) {
        guarantee_checks++;
        failures += r != VA_PASS; /* the reservation must hold */
      }
      break;
    }
    case 2: { /* zero-copy publish */
      uint32_t *w = v_bus_reserve(topics[t], 4, &tk);
      if (w) {
        *w = next[t]++;
        failures += v_bus_commit(topics[t], tk, 4) != VA_PASS;
      }
      break;
    }
    case 3: case 4: /* copy pop */
      if (!held[k] && pop_m(subs[k], &v, &missed) == VA_PASS) {
        failures += v != expect[k] + missed;
        expect[k] = v + 1;
      }
      break;
    case 5: /* peek and hold */
      if (!held[k] && v_bus_peek(subs[k], (const void **)&held[k], &len,
                                 &missed) == VA_PASS) {
        failures += *held[k] != expect[k] + missed;
        expect[k] = *held[k] + 1;
      }
      break;
    default: /* release */
      if (held[k]) {
        failures += *held[k] != expect[k] - 1; /* unchanged under us */
        failures += v_bus_release(subs[k]) != VA_PASS;
        held[k] = NULL;
      }
    }
    (void)topic_of;
    borrowed_seen |= tw.borrowed || tx.borrowed;
    failures += tg.borrowed != 0; /* a drop topic never borrows */
    failures += v_bus_check(&mb) != VA_PASS;
  }
  for (int k = 0; k < 4; k++) {
    if (held[k])
      v_bus_release(subs[k]);
    v_bus_unsubscribe(subs[k]);
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(guarantee_checks > 100);
  TEST_ASSERT(borrowed_seen); /* loans really happened */
  TEST_ASSERT_EQ(tg.lent + tx.lent + tw.borrowed + tx.borrowed, 0);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb) + tg.stash_count + tx.stash_count, MC);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

static const test_case_t bus_cases[] = {
    TEST_CASE(test_bus_init_validates),
    TEST_CASE(test_bus_alloc_all_or_nothing),
    TEST_CASE(test_bus_topic_declare),
    TEST_CASE(test_bus_publish_pop_order),
    TEST_CASE(test_bus_refcount_reclaim),
    TEST_CASE(test_bus_late_subscriber),
    TEST_CASE(test_bus_multiblock_and_empty),
    TEST_CASE(test_bus_pop_too_small),
    TEST_CASE(test_bus_publish_full_pool),
    TEST_CASE(test_bus_fuzz_three_readers),
    TEST_CASE(test_bus_unsubscribe_releases),
    TEST_CASE(test_bus_overwrite_reports_missed),
    TEST_CASE(test_bus_overwrite_fast_reader_unaffected),
    TEST_CASE(test_bus_overwrite_no_futile_eviction),
    TEST_CASE(test_bus_evicted_mid_pop_rereads),
    TEST_CASE(test_bus_fuzz_churn_overwrite),
    TEST_CASE(test_bus_zc_reserve_commit),
    TEST_CASE(test_bus_zc_peek_release),
    TEST_CASE(test_bus_zc_pinned_not_evicted),
    TEST_CASE(test_bus_fuzz_zero_copy),
    TEST_CASE(test_bus_reserve_declare),
    TEST_CASE(test_bus_reserve_survives_flood),
    TEST_CASE(test_bus_reserve_lend_and_reclaim),
    TEST_CASE(test_bus_reserve_reclaim_blocked_by_pin),
    TEST_CASE(test_bus_fuzz_reservations),
};

const test_suite_t bus_suite = {
    .name = "Bus IPC: pool, topics, overwrite, zero copy, reservations",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
