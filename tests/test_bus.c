/**
 * @file test_bus.c
 * @brief Bus IPC subsystem (kernel/bus.c), per docs/plans/bus-subsystem.md:
 *        B1 the block-pool allocator (gate: invariant H1 holds under fuzz),
 *        B2 topics, publish and polling pop (gate: ordering, ref-count reclaim).
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
  v_bus_topic_declare(&mb, &tp, "imu.raw");
}

static int pop_u32(v_bus_sub_t *s, uint32_t *v) {
  uint16_t len = 0;
  int r = v_bus_pop(s, v, sizeof *v, &len);
  return r == VA_PASS && len == sizeof *v ? VA_PASS : r;
}

static void test_bus_topic_declare(void) {
  static v_bus_topic_t dup, other;
  mreset();
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &dup, "imu.raw"), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &tp, "again"), V_BUS_EINVAL);
  TEST_ASSERT_EQ(v_bus_topic_declare(&mb, &other, "baro"), VA_PASS);
  TEST_ASSERT_EQ(v_bus_topic_declare(NULL, &other, "x"), V_BUS_EINVAL);
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
  TEST_ASSERT_EQ(v_bus_pop(&sa, back, sizeof back, &len), VA_PASS);
  TEST_ASSERT_EQ(len, sizeof big);
  TEST_ASSERT_EQ(memcmp(big, back, sizeof big), 0);
  TEST_ASSERT_EQ(v_bus_pop(&sa, back, sizeof back, &len), VA_PASS);
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
  TEST_ASSERT_EQ(v_bus_pop(&sa, got, 4, &len), V_BUS_EMSGSIZE);
  TEST_ASSERT_EQ(len, sizeof two);
  TEST_ASSERT_EQ(v_bus_pop(&sa, got, sizeof got, &len), VA_PASS);
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
};

const test_suite_t bus_suite = {
    .name = "Bus IPC: block pool (B1) + topics (B2)",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
