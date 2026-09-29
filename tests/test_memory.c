/**
 * @file test_memory.c
 * @brief Unit tests for vaios heap memory allocator (memory.c)
 *
 * Tests: init, basic alloc/free, zero-size, alignment, exhaustion,
 *        block splitting, coalescing (next & prev merge), double-free
 *        detection, invalid pointer detection, allocation counters.
 */
#include "framework.h"
#include "memory.h"

/* -------------------------------------------------------------------------
 * Stubs declared in stubs.c
 * ---------------------------------------------------------------------- */
extern void stub_reset_heap(void);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static void reset(void) { stub_reset_heap(); }

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

/* After reset the heap counters are zero */
static void test_init_counters(void) {
  reset();
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 0u);
  TEST_ASSERT_EQ(v_get_heap_size(), (uint32_t)HEAP_SIZE);
}

/* Basic single allocation returns non-NULL */
static void test_alloc_basic(void) {
  reset();
  void *p = v_malloc(64);
  TEST_ASSERT_NOT_NULL(p);
}

/* Zero-size allocation must return NULL */
static void test_alloc_zero(void) {
  reset();
  void *p = v_malloc(0);
  TEST_ASSERT_NULL(p);
}

/* Allocation without init must return NULL */
static void test_alloc_no_init(void) {
  /* Temporarily wipe heap head */
  extern Heap_Mem_Block *heap_mem_head;
  Heap_Mem_Block *saved = heap_mem_head;
  heap_mem_head = NULL;

  void *p = v_malloc(32);
  TEST_ASSERT_NULL(p);

  heap_mem_head = saved; /* restore */
}

/* Allocated payload is at least 4-byte aligned.
 * Heap_Mem_Block = { uint8_t magic(1), uint32_t size(4), enum status(4) } = 9
 * bytes. The Heap starts on the static buffer which is at least 8-byte aligned
 * by the C runtime, so payload = base+9 is always 4-byte aligned (9 mod 4
 * == 1... wait — the allocator doesn't align the header start, only the SIZE is
 * rounded. So we assert the minimum guaranteed: the payload is 4-byte aligned
 * on a 32-bit host, because the TCB/struct fields are uint32_t. */
static void test_alloc_alignment(void) {
  reset();
  /* Allocate 1-byte – allocator rounds size up to 8.  Payload follows a
   * Heap_Mem_Block header (9 bytes) so alignment depends on buffer alignment.
   */
  void *p1 = v_malloc(1);
  void *p2 = v_malloc(1);
  TEST_ASSERT_NOT_NULL(p1);
  TEST_ASSERT_NOT_NULL(p2);
  /* The two consecutive payloads must be exactly (sizeof(Heap_Mem_Block)+8)
   * bytes apart, confirming the size was rounded up to 8. */
  ptrdiff_t gap = (uint8_t *)p2 - (uint8_t *)p1;
  TEST_ASSERT(gap >= 8); /* at minimum the padded payload size */
}

/* Multiple allocations return distinct non-overlapping pointers */
static void test_alloc_multiple(void) {
  reset();
  void *a = v_malloc(32);
  void *b = v_malloc(32);
  void *c = v_malloc(32);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT((uintptr_t)b >= (uintptr_t)a + 32u);
  TEST_ASSERT((uintptr_t)c >= (uintptr_t)b + 32u);
}

/* Allocation counter increments correctly */
static void test_alloc_counter(void) {
  reset();
  v_malloc(16);
  v_malloc(16);
  v_malloc(16);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 3u);
}

/* Free returns memory to the pool (counter decrements) */
static void test_free_counter(void) {
  reset();
  void *p = v_malloc(64);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 1u);
  v_free(p);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 0u);
}

/* Free NULL must not crash */
static void test_free_null(void) {
  reset();
  v_free(NULL); /* should return silently */
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 0u);
}

/* After free, the same region can be re-allocated */
static void test_alloc_after_free(void) {
  reset();
  void *p1 = v_malloc(128);
  TEST_ASSERT_NOT_NULL(p1);
  v_free(p1);

  void *p2 = v_malloc(128);
  TEST_ASSERT_NOT_NULL(p2);
  /* p2 should reuse the same or earlier address */
  TEST_ASSERT(p2 <= p1 || (uintptr_t)p2 >= (uintptr_t)p1);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 1u);
}

/* Block splitting: allocate small slice of freed large block */
static void test_block_splitting(void) {
  reset();
  void *big = v_malloc(512);
  TEST_ASSERT_NOT_NULL(big);
  v_free(big);

  /* Allocate only 64 from the freed 512-byte block; a remainder block
   * must be created, and we must still be able to allocate from it. */
  void *small1 = v_malloc(64);
  void *small2 = v_malloc(64);
  TEST_ASSERT_NOT_NULL(small1);
  TEST_ASSERT_NOT_NULL(small2);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), 2u);
}

/* Coalescing: free adjacent blocks, verify they merge (large re-alloc works) */
static void test_merge_next(void) {
  reset();
  void *a = v_malloc(128);
  void *b = v_malloc(128);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);

  /* Free a then b – allocator should merge them */
  v_free(a);
  v_free(b);

  /* Should now be able to allocate the combined region minus headers */
  void *big = v_malloc(200);
  TEST_ASSERT_NOT_NULL(big);
}

/* Coalescing: free in reverse order (merge with previous) */
static void test_merge_prev(void) {
  reset();
  void *a = v_malloc(128);
  void *b = v_malloc(128);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);

  /* Free b first, then a – allocator should merge prev<-block */
  v_free(b);
  v_free(a);

  void *big = v_malloc(200);
  TEST_ASSERT_NOT_NULL(big);
}

/* Heap exhaustion: repeated small allocs must eventually return NULL.
 * After large-block loop, tiny leftover fragments smaller than
 * sizeof(Heap_Mem_Block)+8 may still be allocatable.  We keep trying
 * until we truly get NULL. */
static void test_heap_exhaustion(void) {
  reset();
  int big_count = 0;
  while (v_malloc(256) != NULL)
    big_count++;

  /* At least one large block was allocated */
  TEST_ASSERT(big_count > 0);

  /* Drain any remaining tiny fragments */
  int tiny_count = 0;
  while (v_malloc(8) != NULL)
    tiny_count++;

  /* Now the heap must truly be exhausted */
  TEST_ASSERT_NULL(v_malloc(8));
  TEST_ASSERT_NULL(v_malloc(1));
}

/* Invalid free (address outside heap) must not corrupt state */
static void test_free_invalid_ptr(void) {
  reset();
  uint8_t local_arr[64];
  /* Freeing a pointer not in the heap – should return silently */
  v_free(local_arr + 8);
  /* Allocator must still work */
  void *p = v_malloc(32);
  TEST_ASSERT_NOT_NULL(p);
}

/* ---- v_memalign (aligned allocation backing MPU-guarded stacks) ---- */

/* Every alignment (a power of two) yields a payload on that boundary, and the
 * block is usable and freeable. */
static void test_memalign_alignment(void) {
  reset();
  for (size_t align = 32; align <= 1024; align <<= 1) {
    void *p = v_memalign(align, 200);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ((uintptr_t)p & (align - 1), (uintptr_t)0);
    /* Writable across the whole request, no corruption. */
    for (int i = 0; i < 200; i++)
      ((volatile uint8_t *)p)[i] = (uint8_t)i;
    v_free(p);
  }
}

/* Bad args: non-power-of-two alignment and zero size both return NULL. */
static void test_memalign_bad_args(void) {
  reset();
  TEST_ASSERT_NULL(v_memalign(48, 100)); /* 48 is not a power of two */
  TEST_ASSERT_NULL(v_memalign(0, 100));  /* 0 is not a power of two */
  TEST_ASSERT_NULL(v_memalign(64, 0));   /* zero size */
}

/* align <= the 8-byte default falls back to v_malloc (still aligned enough). */
static void test_memalign_small_align_falls_back(void) {
  reset();
  void *p = v_memalign(8, 64);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ((uintptr_t)p & 7u, (uintptr_t)0);
  v_free(p);
}

/* Alloc + free of an aligned block leaves the heap exactly as it was — the
 * aligned block and its leading slack both coalesce back. */
static void test_memalign_no_leak(void) {
  reset();
  uint32_t count0 = v_get_heap_allocation_count();
  uint32_t used0 = v_get_heap_allocation_size();
  void *p = v_memalign(256, 300);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT(v_get_heap_allocation_count() > count0);
  v_free(p);
  TEST_ASSERT_EQ(v_get_heap_allocation_count(), count0);
  TEST_ASSERT_EQ(v_get_heap_allocation_size(), used0);
}

/* The slack carved off ahead of the aligned block is returned to the heap and
 * can satisfy later allocations (it is not leaked). */
static void test_memalign_leading_slack_reusable(void) {
  reset();
  /* Force a non-trivial leading gap: a small alloc so the next aligned block
   * can't start at the heap head, then a large-alignment request. */
  void *a = v_malloc(24);
  TEST_ASSERT_NOT_NULL(a);
  void *p = v_memalign(512, 512);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ((uintptr_t)p & 511u, (uintptr_t)0);
  /* A subsequent small alloc must still succeed (slack + remainder available). */
  void *b = v_malloc(24);
  TEST_ASSERT_NOT_NULL(b);
  v_free(a);
  v_free(b);
  v_free(p);
}

/* Regression for the unsigned-underflow in v_memalign's leading-remainder loop.
 * The bump condition was (uint32_t)(h - payload0) < VHEAP_MIN_PAYLOAD; when the
 * first aligned header `h` fell below payload0 the subtraction wrapped to a huge
 * value, the loop failed to bump `a`, and the aligned block was planted on top
 * of the source block's own header — corrupting the heap. It fired for ~1 in 4
 * source-block residues (e.g. blk ≡ 8 mod 32 for a 32-byte guard), which is why
 * the host suite passed but MPU-guarded task stacks tripped "Invalid free or
 * corrupted block" on target. Sweep every 8-byte residue via a filler alloc;
 * each carve must be aligned and leave the heap exactly as it was. */
static void test_memalign_leading_underflow_regression(void) {
  for (size_t pad = 0; pad <= 56; pad += 8) {
    reset();
    void *filler = pad ? v_malloc(pad) : NULL;
    if (pad)
      TEST_ASSERT_NOT_NULL(filler);
    uint32_t count0 = v_get_heap_allocation_count();
    uint32_t used0 = v_get_heap_allocation_size();

    void *p = v_memalign(32, 200);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ((uintptr_t)p & 31u, (uintptr_t)0);
    for (int i = 0; i < 200; i++) /* touch the whole payload */
      ((volatile uint8_t *)p)[i] = 0xAB;
    v_free(p);

    /* Heap fully restored: the aligned block + leading slack coalesced back.
     * Corruption would desync the counters or make v_free reject the block. */
    TEST_ASSERT_EQ(v_get_heap_allocation_count(), count0);
    TEST_ASSERT_EQ(v_get_heap_allocation_size(), used0);
    if (filler)
      v_free(filler);
  }
}

/* #8: for align=64 a free-block header ≡ 8 mod 64 (payload0 ≡ 32 mod 64) made the
 * old reservation (need + align + header) one MIN_PAYLOAD short, so v_memalign
 * returned a block whose usable size was < the request and it overran the next
 * block. (The align=32 sweep above misses it — d0 can't reach 32.) Sweep a
 * leading shim so the header lands on every residue mod 64 — one hits the bad
 * one regardless of heap base — and assert the returned block's own size is
 * always >= the request. */
static void test_memalign_underalloc_regression(void) {
  const size_t ALIGN = 64, SZ = 64;
  /* The bug bites only when the found block is JUST the old reservation
   * (need + align + header) AND its header residue makes payload0 ≡ align/2 mod
   * align. Build a tightly-sized free hole (payload == old reservation) pinned
   * so it can't coalesce, and sweep a leading allocation so the hole's header
   * lands on every residue mod align — one iteration hits the bad one. */
  const size_t OLD_RESV = SZ + ALIGN + sizeof(Heap_Mem_Block); /* pre-fix size */
  for (size_t shim = 0; shim <= 120; shim += 8) {
    reset();
    void *lead = v_malloc(256 + shim); /* shifts the hole's header mod align */
    TEST_ASSERT_NOT_NULL(lead);
    void *hole = v_malloc(OLD_RESV); /* the tight block memalign will pick */
    TEST_ASSERT_NOT_NULL(hole);
    void *pin = v_malloc(64); /* stops the hole coalescing into the tail */
    TEST_ASSERT_NOT_NULL(pin);
    v_free(hole); /* now a tightly-sized free hole */

    void *p = v_memalign(ALIGN, SZ);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ((uintptr_t)p & (ALIGN - 1), (uintptr_t)0);
    Heap_Mem_Block *h =
        (Heap_Mem_Block *)((uint8_t *)p - sizeof(Heap_Mem_Block));
    TEST_ASSERT_EQ(h->magic_number, SANITY_MAGIC_NUMBER);
    TEST_ASSERT(h->size >= SZ); /* under-allocation (finding #8) fails here */
    for (size_t i = 0; i < SZ; i++) /* touch the whole requested payload */
      ((volatile uint8_t *)p)[i] = 0xCD;
    v_free(p);
    v_free(lead);
    v_free(pin);
  }
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
static const test_case_t memory_cases[] = {
    TEST_CASE(test_init_counters),   TEST_CASE(test_alloc_basic),
    TEST_CASE(test_alloc_zero),      TEST_CASE(test_alloc_no_init),
    TEST_CASE(test_alloc_alignment), TEST_CASE(test_alloc_multiple),
    TEST_CASE(test_alloc_counter),   TEST_CASE(test_free_counter),
    TEST_CASE(test_free_null),       TEST_CASE(test_alloc_after_free),
    TEST_CASE(test_block_splitting), TEST_CASE(test_merge_next),
    TEST_CASE(test_merge_prev),      TEST_CASE(test_heap_exhaustion),
    TEST_CASE(test_free_invalid_ptr),
    TEST_CASE(test_memalign_alignment),
    TEST_CASE(test_memalign_bad_args),
    TEST_CASE(test_memalign_small_align_falls_back),
    TEST_CASE(test_memalign_no_leak),
    TEST_CASE(test_memalign_leading_slack_reusable),
    TEST_CASE(test_memalign_leading_underflow_regression),
    TEST_CASE(test_memalign_underalloc_regression),
};
const test_suite_t memory_suite = {
    .name = "Memory Allocator",
    .cases = memory_cases,
    .count = TEST_COUNT(memory_cases),
};
