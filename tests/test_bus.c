/**
 * @file test_bus.c
 * @brief Bus IPC subsystem (kernel/bus.c), per docs/plans/bus-subsystem.md:
 *        B1 the block-pool allocator (gate: invariant H1 holds under fuzz),
 *        B2 topics, publish and polling pop (gate: ordering, ref-count reclaim),
 *        B3 unsubscribe, overwrite, slow subscribers (gate: H4/H5/H6),
 *        the zero-copy path (reserve/commit, peek/release, pinning), and
 *        soft reservations (per-topic stash, loans, reclaim), and pipe topics
 *        (lock-free SPSC ring: drop, overwrite, zero copy).
 */
#include "bus.h"
#include "framework.h"
#include "stubs/v_fs_stub.h" // the filesystem under the snapshotter
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

/* ---- B7: the snapshotter -----------------------------------------------------
 * A best-effort recorder that reads like any other consumer. What matters: it
 * takes part in reclaim (so it can never pin messages forever), it keeps going
 * when the filesystem refuses a write, and it counts what it lost.
 */
static void test_bus_snapshotter(void) {
  wreset();
  vfs_stub_reset();
  vfs_stub.open_ret = 4;
  v_bus_snap_t snap;
  TEST_ASSERT_EQ(v_bus_snapshot_start(&snap, &tw, "/rec.bin"), VA_PASS);
  TEST_ASSERT_EQ(vfs_stub.open_called, 1);
  TEST_ASSERT_EQ(tw.nsubs, 1); /* it subscribed like a reader */

  /* Nothing published yet: a pump moves nothing and says so. */
  TEST_ASSERT_EQ(v_bus_snapshot_pump(&snap, 8), 0);

  for (uint32_t i = 0; i < 3; i++)
    TEST_ASSERT_EQ(v_bus_publish(&tw, &i, sizeof i), VA_PASS);
  v_bus_topic_stats_t ts;
  v_bus_topic_stats(&tw, &ts);
  TEST_ASSERT_EQ(ts.queued, 3); /* the recorder owes all three */

  vfs_stub.write_ret = 8; /* the stub accepts writes */
  TEST_ASSERT_EQ(v_bus_snapshot_pump(&snap, 8), 3);
  TEST_ASSERT_EQ(snap.written, 3u);
  TEST_ASSERT_EQ(snap.failed, 0u);
  /* Reading them released the blocks: it participates in reclaim. */
  v_bus_topic_stats(&tw, &ts);
  TEST_ASSERT_EQ(ts.queued, 0);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);

  /* A filesystem that refuses keeps the recording going and counts the loss. */
  vfs_stub.write_ret = -1;
  for (uint32_t i = 0; i < 2; i++)
    v_bus_publish(&tw, &i, sizeof i);
  TEST_ASSERT_EQ(v_bus_snapshot_pump(&snap, 8), 0); /* nothing written */
  TEST_ASSERT_EQ(snap.failed, 2u);
  TEST_ASSERT_EQ(snap.written, 3u);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC); /* still no messages pinned */

  /* Stopping unsubscribes and closes, and is safe to repeat. */
  TEST_ASSERT_EQ(v_bus_snapshot_stop(&snap), VA_PASS);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  TEST_ASSERT_EQ(tw.nsubs, 0);
  TEST_ASSERT_EQ(v_bus_snapshot_stop(&snap), VA_PASS);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1); /* not closed twice */
  TEST_ASSERT_EQ(v_bus_snapshot_pump(&snap, 4), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);

  /* A filesystem that refuses the open leaves nothing subscribed. */
  vfs_stub.open_ret = -7;
  TEST_ASSERT_EQ(v_bus_snapshot_start(&snap, &tw, "/nope.bin"), -7);
  TEST_ASSERT_EQ(tw.nsubs, 0);
  TEST_ASSERT_EQ(v_bus_snapshot_start(NULL, &tw, "/x"), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_snapshot_start(&snap, &tw, NULL), V_BUS_EINVAL);
}

/* ---- B7: statistics ----------------------------------------------------------
 * Explicit getters, counted where the state they describe already changes. Each
 * value must mean what it says: published counts what became visible, dropped
 * what the full pool refused, evicted what a reader lost.
 */
static void test_bus_statistics(void) {
  wreset();
  v_bus_topic_stats_t ts;
  v_bus_stats_t bs;
  TEST_ASSERT_EQ(v_bus_topic_stats(&tw, &ts), VA_PASS);
  TEST_ASSERT_EQ(ts.published, 0u);
  TEST_ASSERT_EQ(ts.subs, 0);
  TEST_ASSERT_EQ(v_bus_stats(&mb, &bs), VA_PASS);
  TEST_ASSERT_EQ(bs.block_count, MC);
  TEST_ASSERT_EQ(bs.free_blocks, MC);
  TEST_ASSERT_EQ(bs.topics, 1);

  v_bus_subscribe(&tw, &sa);
  for (uint32_t i = 0; i < 3; i++)
    TEST_ASSERT_EQ(v_bus_publish(&tw, &i, sizeof i), VA_PASS);
  TEST_ASSERT_EQ(v_bus_topic_stats(&tw, &ts), VA_PASS);
  TEST_ASSERT_EQ(ts.published, 3u);
  TEST_ASSERT_EQ(ts.queued, 3);
  TEST_ASSERT_EQ(ts.blocks, 3);
  TEST_ASSERT_EQ(ts.subs, 1);
  TEST_ASSERT_EQ(ts.dropped, 0u);
  TEST_ASSERT_EQ(ts.evicted, 0u);
  TEST_ASSERT_EQ(v_bus_stats(&mb, &bs), VA_PASS);
  TEST_ASSERT_EQ(bs.free_blocks, MC - 3);

  /* Reading one frees its block and takes it off the queue. */
  uint32_t v;
  TEST_ASSERT_EQ(pop_m(&sa, &v, NULL), VA_PASS);
  TEST_ASSERT_EQ(v_bus_topic_stats(&tw, &ts), VA_PASS);
  TEST_ASSERT_EQ(ts.queued, 2);
  TEST_ASSERT_EQ(ts.published, 3u); /* a total, not a level */

  /* Fill the pool: this overwrite topic evicts rather than dropping. */
  for (uint32_t i = 0; i < MC + 2u; i++)
    v_bus_publish(&tw, &i, sizeof i);
  TEST_ASSERT_EQ(v_bus_topic_stats(&tw, &ts), VA_PASS);
  TEST_ASSERT(ts.evicted > 0u);
  TEST_ASSERT_EQ(ts.dropped, 0u); /* overwrite policy never drops */

  /* A DROP topic, on a pool the overwrite topic has emptied, does drop. */
  static v_bus_topic_t dt;
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &dt, "drops", NULL), VA_PASS);
  v_bus_sub_t ds;
  v_bus_subscribe(&dt, &ds);
  uint32_t big = 0;
  int drops = 0;
  for (int i = 0; i < 40; i++)
    if (v_bus_publish(&dt, &big, sizeof big) != VA_PASS)
      drops++;
  TEST_ASSERT(drops > 0);
  TEST_ASSERT_EQ(v_bus_topic_stats(&dt, &ts), VA_PASS);
  TEST_ASSERT_EQ(ts.dropped, (uint32_t)drops);
  TEST_ASSERT_EQ(v_bus_stats(&mb, &bs), VA_PASS);
  TEST_ASSERT_EQ(bs.topics, 2);

  /* Bad arguments. */
  TEST_ASSERT_EQ(v_bus_topic_stats(NULL, &ts), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_topic_stats(&tw, NULL), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_stats(NULL, &bs), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_stats(&mb, NULL), V_BUS_EINVAL);
}

/* ---- B5: several producers on one topic --------------------------------------
 * The plan proposed a three-stage mutex pipeline for this. It is not needed, and
 * these tests are why: a publisher allocates under the allocator's critical
 * section, copies into blocks IT ALREADY OWNS (marked used, so no other
 * publisher can take them), and links under a critical section that also assigns
 * seq. Nothing is held across the copy, and nothing about the copy is shared —
 * so publishers are independent by construction, including an ISR preempting a
 * task. What must be proven is exactly that: a second publisher landing in the
 * window between another's copy and its link changes nothing for either.
 */
extern void (*v_bus_test_mid_publish)(void);
static uint32_t mid_pub_value;
static void publish_during_publish(void) {
  v_bus_test_mid_publish = NULL; /* once */
  v_bus_publish(&tw, &mid_pub_value, sizeof mid_pub_value);
}

static void test_bus_second_producer_mid_copy(void) {
  wreset();
  v_bus_subscribe(&tw, &sa);

  /* B publishes in A's window, so B links FIRST and must get the lower seq —
   * order is defined at link, which is what keeps `missed` honest. */
  mid_pub_value = 200;
  v_bus_test_mid_publish = publish_during_publish;
  uint32_t a = 100;
  TEST_ASSERT_EQ(v_bus_publish(&tw, &a, sizeof a), VA_PASS);
  TEST_ASSERT_NULL(v_bus_test_mid_publish); /* B really did interleave */

  uint32_t v = 0, missed = 9;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 200u); /* B linked first */
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 100u); /* then A, uncorrupted by the interleave */
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
}

/* Two producers, each with its own counter, interleaved every message: the
 * reader must see every value exactly once, in link order, with no duplicates,
 * no losses and no torn payloads. Each message carries its producer id so the
 * reader can check both streams stayed monotonic. */
static void test_bus_two_producers_fuzz(void) {
  wreset();
  v_bus_subscribe(&tw, &sa);
  uint32_t next[2] = {0, 0}, seen[2] = {0, 0};
  int failures = 0;
  uint32_t x = 0x1234567u;

  for (int op = 0; op < 4000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x & 1u) {
      int who = (int)((x >> 2) & 1u);
      /* value = producer id in the low bit, counter above it */
      uint32_t val = (next[who] << 1) | (uint32_t)who;
      if (v_bus_publish(&tw, &val, sizeof val) == VA_PASS)
        next[who]++;
    } else {
      uint32_t v = 0, missed = 0;
      if (pop_m(&sa, &v, &missed) == VA_PASS) {
        int who = (int)(v & 1u);
        uint32_t counter = v >> 1;
        /* Its own stream must never go backwards or repeat. An overwrite topic
         * may skip (reported via missed), so counter >= what we expect. */
        failures += counter < seen[who];
        seen[who] = counter + 1;
      }
    }
    failures += v_bus_check(&mb) != VA_PASS;
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(next[0] > 100 && next[1] > 100); /* both really produced */
  TEST_ASSERT(seen[0] > 0 && seen[1] > 0);     /* both really arrived */
  while (pop_m(&sa, &(uint32_t){0}, &(uint32_t){0}) == VA_PASS)
    ;
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC);
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

/* ---- Pipe topics ------------------------------------------------------------ */
static v_bus_topic_t pd, po; /* drop pipe, overwrite pipe */
static const v_bus_topic_cfg_t PIPE4 = {.pipe = 1, .reserve = 4};
static const v_bus_topic_cfg_t PIPE4_OW = {.pipe = 1, .reserve = 4,
                                           .overflow = V_BUS_OVERWRITE};

static void preset(void) {
  v_bus_init(&mb, mp_blocks, mp_desc, MS, MC);
  v_bus_topic_declare(&mb, &pd, "pipe", &PIPE4);
  v_bus_topic_declare(&mb, &po, "pipe.ow", &PIPE4_OW);
}

static void test_bus_pipe_declare_and_one_reader(void) {
  static v_bus_topic_t bad;
  static const v_bus_topic_cfg_t NOSLOTS = {.pipe = 1};
  preset();
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &bad, "p0", &NOSLOTS), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 8); /* two rings of 4 */
  TEST_ASSERT_EQ(v_bus_subscribe(&pd, &sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_subscribe(&pd, &sb), V_BUS_EBUSY); /* one reader */
  static uint8_t two_blocks[FIRST + 1];
  TEST_ASSERT_EQ(v_bus_publish(&pd, two_blocks, sizeof two_blocks), V_BUS_ESPLIT);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Drop pipe: full after `depth`, never loses a message, wraps many times,
 * and never touches the shared pool. */
static void test_bus_pipe_drop_order_and_wrap(void) {
  preset();
  v_bus_subscribe(&pd, &sa);
  for (uint32_t i = 0; i < 4; i++)
    TEST_ASSERT_EQ(pub_u32(&pd, i), VA_PASS);
  TEST_ASSERT_EQ(pub_u32(&pd, 99), VA_FAIL); /* full */
  uint32_t v, missed, next = 4, got = 0;
  for (int round = 0; round < 25; round++) {
    while (pop_m(&sa, &v, &missed) == VA_PASS) {
      TEST_ASSERT_EQ(v, got);
      TEST_ASSERT_EQ(missed, 0u);
      got++;
    }
    for (int k = 0; k < 3; k++)
      if (pub_u32(&pd, next) == VA_PASS)
        next++;
  }
  while (pop_m(&sa, &v, &missed) == VA_PASS)
    TEST_ASSERT_EQ(v, got++);
  TEST_ASSERT_EQ(got, next);
  TEST_ASSERT_EQ(v_bus_free_blocks(&mb), MC - 8);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Zero copy on a drop pipe: the peeked slot can't be overwritten, even when
 * the producer fills the rest of the ring. */
static void test_bus_pipe_zero_copy(void) {
  preset();
  v_bus_subscribe(&pd, &sa);
  uint16_t t, len;
  uint32_t *w = v_bus_reserve(&pd, 4, &t);
  TEST_ASSERT(w != NULL);
  *w = 7;
  TEST_ASSERT_EQ(v_bus_commit(&pd, t, 4), VA_PASS);
  TEST_ASSERT_EQ(v_bus_commit(&pd, t, 4), V_BUS_EINVAL); /* stale ticket */
  const uint32_t *p;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(*p, 7u);
  for (uint32_t i = 0; i < 3; i++)
    TEST_ASSERT_EQ(pub_u32(&pd, 100 + i), VA_PASS);
  TEST_ASSERT_EQ(pub_u32(&pd, 200), VA_FAIL); /* would overwrite the peek */
  TEST_ASSERT_EQ(*p, 7u);
  TEST_ASSERT_EQ(v_bus_release(&sa), VA_PASS);
  uint32_t v;
  TEST_ASSERT_EQ(pop_u32(&sa, &v), VA_PASS);
  TEST_ASSERT_EQ(v, 100u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Overwrite pipe: the producer never waits; a lapped reader jumps to the
 * oldest slot and reports exactly what it lost. */
static void test_bus_pipe_overwrite_missed(void) {
  preset();
  v_bus_subscribe(&po, &sa);
  for (uint32_t i = 0; i < 10; i++)
    TEST_ASSERT_EQ(pub_u32(&po, i), VA_PASS);
  uint32_t v, missed;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 6u);
  TEST_ASSERT_EQ(missed, 6u);
  for (uint32_t i = 7; i < 10; i++) {
    TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
    TEST_ASSERT_EQ(v, i);
    TEST_ASSERT_EQ(missed, 0u);
  }
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Overwrite pipe, lock-free seqlock: a slot overwritten WHILE copied is never
 * returned torn; a peek overwritten while viewed is reported stale. */
static void lap_overwrite_pipe(void) {
  v_bus_test_mid_pop = NULL;
  for (uint32_t i = 0; i < 4; i++)
    pub_u32(&po, 1000 + i); /* a whole lap: every slot rewritten */
}
static void test_bus_pipe_overwrite_seqlock(void) {
  preset();
  v_bus_subscribe(&po, &sa);
  for (uint32_t i = 0; i < 4; i++)
    pub_u32(&po, i);
  v_bus_test_mid_pop = lap_overwrite_pipe;
  uint32_t v = 0, missed = 0;
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_NULL(v_bus_test_mid_pop); /* the lap ran mid-copy */
  TEST_ASSERT_EQ(v, 1000u);             /* not the torn #0 */
  TEST_ASSERT_EQ(missed, 4u);           /* #0..#3 were lost */
  const uint32_t *p;
  uint16_t len;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, NULL), VA_PASS);
  TEST_ASSERT_EQ(*p, 1001u);
  for (uint32_t i = 0; i < 4; i++)
    pub_u32(&po, 2000 + i); /* laps under the view */
  TEST_ASSERT_EQ(v_bus_release(&sa), V_BUS_ESTALE);
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 2000u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Cancelling a claim on a full overwrite pipe loses the oldest (it was
 * marked in-progress); a reader subscribing late sees only what's next. */
static void test_bus_pipe_cancel_and_late_reader(void) {
  preset();
  uint32_t v = 5, missed;
  TEST_ASSERT_EQ(pub_u32(&po, 5), VA_PASS); /* nobody subscribed: dropped */
  v_bus_subscribe(&po, &sa);
  for (uint32_t i = 0; i < 4; i++)
    pub_u32(&po, i);
  uint16_t t, len;
  uint32_t *w = v_bus_reserve(&po, 4, &t); /* claims #0's slot: in progress */
  TEST_ASSERT(w != NULL);
  *w = 0xBAD; /* half-written, never committed */
  const uint32_t *p;
  TEST_ASSERT_EQ(v_bus_peek(&sa, (const void **)&p, &len, &missed), VA_PASS);
  TEST_ASSERT_EQ(*p, 1u);     /* the claimed slot is skipped, not viewed */
  TEST_ASSERT_EQ(missed, 1u); /* #0 is lost */
  TEST_ASSERT_EQ(v_bus_release(&sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_cancel(&po, t), VA_PASS);
  TEST_ASSERT_EQ(pop_m(&sa, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 2u);
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(v_bus_unsubscribe(&sa), VA_PASS);
  TEST_ASSERT_EQ(v_bus_subscribe(&po, &sb), VA_PASS);
  pub_u32(&po, 77);
  TEST_ASSERT_EQ(pop_m(&sb, &v, &missed), VA_PASS);
  TEST_ASSERT_EQ(v, 77u);
  TEST_ASSERT_EQ(missed, 0u);
  TEST_ASSERT_EQ(v_bus_check(&mb), VA_PASS);
}

/* Randomised, both pipes plus an ordinary topic sharing the pool: copy and
 * zero-copy publishes, copy pops, held peeks, cancels. Drop pipe: every
 * message arrives, in order, none missed. Overwrite pipe: in order, missed ==
 * the exact gap, a stale release discards. Invariants every step. */
static void test_bus_fuzz_pipes(void) {
  preset();
  v_bus_topic_declare(&mb, &tp, "plain", NULL);
  v_bus_subscribe(&pd, &sa);
  v_bus_subscribe(&po, &sb);
  v_bus_subscribe(&tp, &sc);
  v_bus_topic_t *tops[3] = {&pd, &po, &tp};
  v_bus_sub_t *subs[3] = {&sa, &sb, &sc};
  const uint32_t *held[3] = {NULL, NULL, NULL};
  uint32_t next[3] = {0, 0, 0}, expect[3] = {0, 0, 0}, seen[3] = {0, 0, 0};
  int failures = 0, laps = 0, stale = 0;
  uint32_t x = 0x27D4EB2Fu;
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    int k = (int)((x >> 4) % 3u);
    uint16_t t, len;
    uint32_t v, missed;
    switch (x & 7u) {
    case 0: case 1: /* copy publish */
      if (pub_u32(tops[k], next[k]) == VA_PASS)
        next[k]++;
      break;
    case 2: { /* zero-copy publish, sometimes cancelled */
      uint32_t *w = v_bus_reserve(tops[k], 4, &t);
      if (!w)
        break;
      if ((x >> 9) & 1u) { /* nothing published, no seq used; on a full */
        failures += v_bus_cancel(tops[k], t) != VA_PASS; /* overwrite pipe */
        break; /* the oldest was lost to the claim: the reader sees a gap */
      }
      *w = next[k]++;
      failures += v_bus_commit(tops[k], t, 4) != VA_PASS;
      break;
    }
    case 3: case 4: /* copy pop */
      if (!held[k] && pop_m(subs[k], &v, &missed) == VA_PASS) {
        failures += v != expect[k] + missed;
        failures += k == 0 && missed != 0; /* a drop pipe never loses */
        laps += missed != 0;
        expect[k] = v + 1;
      }
      break;
    case 5: /* peek and hold */
      if (!held[k] && v_bus_peek(subs[k], (const void **)&held[k], &len,
                                 &missed) == VA_PASS) {
        seen[k] = *held[k];
        failures += seen[k] != expect[k] + missed;
      }
      break;
    default: /* release */
      if (held[k]) {
        int r = v_bus_release(subs[k]);
        if (r == V_BUS_ESTALE) {
          failures += k != 1; /* only an overwrite pipe can go stale */
          stale++;
        } else {
          failures += r != VA_PASS || *held[k] != seen[k];
        }
        expect[k] = seen[k] + 1;
        held[k] = NULL;
      }
    }
    for (int j = 0; j < 3; j++) /* drop-pipe and pool views never change */
      failures += j != 1 && held[j] && *held[j] != seen[j];
    failures += v_bus_check(&mb) != VA_PASS;
  }
  TEST_ASSERT_EQ(failures, 0);
  TEST_ASSERT(laps > 0);
  TEST_ASSERT(stale > 0); /* the seqlock path really ran */
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
    TEST_CASE(test_bus_statistics),
    TEST_CASE(test_bus_snapshotter),
    TEST_CASE(test_bus_second_producer_mid_copy),
    TEST_CASE(test_bus_two_producers_fuzz),
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
    TEST_CASE(test_bus_pipe_declare_and_one_reader),
    TEST_CASE(test_bus_pipe_drop_order_and_wrap),
    TEST_CASE(test_bus_pipe_zero_copy),
    TEST_CASE(test_bus_pipe_overwrite_missed),
    TEST_CASE(test_bus_pipe_overwrite_seqlock),
    TEST_CASE(test_bus_pipe_cancel_and_late_reader),
    TEST_CASE(test_bus_fuzz_pipes),
};

const test_suite_t bus_suite = {
    .name = "Bus IPC: pool, topics, overwrite, zero copy, reservations, pipes",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
