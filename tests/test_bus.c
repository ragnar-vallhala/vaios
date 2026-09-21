/**
 * @file test_bus.c
 * @brief Bus IPC subsystem (kernel/bus.c), per docs/plans/bus-subsystem.md:
 *        B1 the block-pool allocator (gate: invariant H1 holds under fuzz),
 *        B2 topics, publish and polling pop (gate: ordering, ref-count reclaim),
 *        B3 unsubscribe, overwrite, slow subscribers (gate: H4/H5/H6),
 *        and the zero-copy path (reserve/commit, peek/release, pinning).
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

static int chain_len(uint16_t head) {
  int n = 0;
  for (uint16_t i = head; i != V_BUS_NIL; i = v_bus_block_next(&bus, i))
    if (++n > BC)
      return -1; // cycle
  return n;
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

/* H1: a request either gets all n blocks or none — never a partial chain. */
static void test_bus_alloc_all_or_nothing(void) {
  reset();
  uint16_t a = v_bus_block_alloc(&bus, 5);
  TEST_ASSERT(a != V_BUS_NIL);
  TEST_ASSERT_EQ(chain_len(a), 5);
  TEST_ASSERT_EQ(v_bus_block_alloc(&bus, 4), V_BUS_NIL); /* only 3 left */
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), 3);             /* untouched */
  uint16_t b = v_bus_block_alloc(&bus, 3);
  TEST_ASSERT_EQ(chain_len(b), 3);
  TEST_ASSERT_EQ(v_bus_block_alloc(&bus, 1), V_BUS_NIL);
  TEST_ASSERT_EQ(v_bus_block_alloc(&bus, 0), V_BUS_NIL);
  TEST_ASSERT_EQ(v_bus_check(&bus), VA_PASS);
}

/* Freeing returns every block; the pool can be drained and refilled whole. */
static void test_bus_free_restores(void) {
  reset();
  uint16_t a = v_bus_block_alloc(&bus, 3), b = v_bus_block_alloc(&bus, 5);
  TEST_ASSERT_EQ(v_bus_block_free(&bus, a), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), 3);
  TEST_ASSERT_EQ(v_bus_block_free(&bus, b), VA_PASS);
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), BC);
  TEST_ASSERT_EQ(chain_len(v_bus_block_alloc(&bus, BC)), BC);
  TEST_ASSERT_EQ(v_bus_check(&bus), VA_PASS);
}

/* A double free or a stray index is refused and changes nothing — it must not
 * splice a live block or a cycle into the free stack. */
static void test_bus_bad_free_rejected(void) {
  reset();
  uint16_t a = v_bus_block_alloc(&bus, 2);
  uint16_t b = v_bus_block_alloc(&bus, 2);
  TEST_ASSERT_EQ(v_bus_block_free(&bus, a), VA_PASS);
  TEST_ASSERT_EQ(v_bus_block_free(&bus, a), VA_FAIL); /* double free */
  TEST_ASSERT_EQ(v_bus_block_free(&bus, V_BUS_NIL), VA_FAIL);
  TEST_ASSERT_EQ(v_bus_block_free(&bus, BC), VA_FAIL); /* out of range */
  TEST_ASSERT_EQ(v_bus_free_blocks(&bus), BC - 2);
  TEST_ASSERT_EQ(chain_len(b), 2); /* the live chain is intact */
  TEST_ASSERT_EQ(v_bus_check(&bus), VA_PASS);
}

/* Blocks are disjoint block_size windows of the pool. */
static void test_bus_block_data_disjoint(void) {
  reset();
  uint16_t h = v_bus_block_alloc(&bus, BC);
  for (uint16_t i = h; i != V_BUS_NIL; i = v_bus_block_next(&bus, i))
    memset(v_bus_block_data(&bus, i), (int)i + 1, BS);
  for (uint16_t i = 0; i < BC; i++) {
    const uint8_t *d = v_bus_block_data(&bus, i);
    TEST_ASSERT(d >= pool_blocks && d + BS <= pool_blocks + sizeof pool_blocks);
    TEST_ASSERT_EQ(d[0], (uint8_t)(i + 1));
    TEST_ASSERT_EQ(d[BS - 1], (uint8_t)(i + 1));
  }
  TEST_ASSERT_NULL(v_bus_block_data(&bus, BC));
}

/* Gate: H1 holds under a long random alloc/free sequence, checked after every
 * operation against a shadow model: free_count == pool - live blocks, and no
 * block belongs to two live chains. */
#define FZ_BC 64
V_BUS_POOL(fz, 16, FZ_BC);
static void test_bus_fuzz_invariant(void) {
  static v_bus_t fb;
  v_bus_init(&fb, fz_blocks, fz_desc, 16, FZ_BC);
  uint16_t live[FZ_BC];
  int nlive = 0, live_blocks = 0, failures = 0;
  uint32_t x = 0x2545F491u; // xorshift32, fixed seed: reproducible
  for (int op = 0; op < 20000 && !failures; op++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (nlive && (x & 1u)) { // free a random live chain
      int k = (int)((x >> 1) % (uint32_t)nlive);
      int len = 0;
      for (uint16_t i = live[k]; i != V_BUS_NIL; i = v_bus_block_next(&fb, i))
        len++;
      failures += v_bus_block_free(&fb, live[k]) != VA_PASS;
      live_blocks -= len;
      live[k] = live[--nlive];
    } else { // allocate 1..6 blocks
      uint16_t n = (uint16_t)(1 + (x >> 1) % 6);
      uint16_t h = v_bus_block_alloc(&fb, n);
      if (h == V_BUS_NIL) {
        failures += FZ_BC - live_blocks >= n; // refused although it fit
      } else {
        failures += FZ_BC - live_blocks < n; // granted although it didn't
        live[nlive++] = h;
        live_blocks += n;
      }
    }
    failures += v_bus_check(&fb) != VA_PASS;
    failures += v_bus_free_blocks(&fb) != FZ_BC - live_blocks;
    uint8_t owner[FZ_BC] = {0}; // each block in at most one live chain
    for (int k = 0; k < nlive; k++)
      for (uint16_t i = live[k]; i != V_BUS_NIL; i = v_bus_block_next(&fb, i))
        failures += owner[i]++ != 0;
  }
  TEST_ASSERT_EQ(failures, 0);
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

static const test_case_t bus_cases[] = {
    TEST_CASE(test_bus_init_validates),
    TEST_CASE(test_bus_alloc_all_or_nothing),
    TEST_CASE(test_bus_free_restores),
    TEST_CASE(test_bus_bad_free_rejected),
    TEST_CASE(test_bus_block_data_disjoint),
    TEST_CASE(test_bus_fuzz_invariant),
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
};

const test_suite_t bus_suite = {
    .name = "Bus IPC: pool, topics, churn + overwrite, zero copy",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
