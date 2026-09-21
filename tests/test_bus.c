/**
 * @file test_bus.c
 * @brief Bus IPC subsystem, phase B1: the block-pool allocator (kernel/bus.c).
 *        Gate from docs/plans/bus-subsystem.md: invariant H1 holds under fuzz.
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
V_BUS_POOL(fz, 8, FZ_BC);
static void test_bus_fuzz_invariant(void) {
  static v_bus_t fb;
  v_bus_init(&fb, fz_blocks, fz_desc, 8, FZ_BC);
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

static const test_case_t bus_cases[] = {
    TEST_CASE(test_bus_init_validates),
    TEST_CASE(test_bus_alloc_all_or_nothing),
    TEST_CASE(test_bus_free_restores),
    TEST_CASE(test_bus_bad_free_rejected),
    TEST_CASE(test_bus_block_data_disjoint),
    TEST_CASE(test_bus_fuzz_invariant),
};

const test_suite_t bus_suite = {
    .name = "Bus IPC: block pool (B1)",
    .cases = bus_cases,
    .count = TEST_COUNT(bus_cases),
};
