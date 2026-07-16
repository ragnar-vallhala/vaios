/**
 * @file test_taskheap.c
 * @brief Per-task heap (VAIOS_TASK_HEAP) unit tests — kernel/memory/memory.c.
 *
 * This binary exists so the per-task allocator (malloc/free/calloc/realloc
 * serving current_task out of its own block) is exercised on the host. In the
 * main vaios_tests binary VAIOS_TASK_HEAP is off, so this code path never ran
 * host-side before (see docs/plan/STAGE5_REVIEW_FINDINGS.md, findings #3 & #9).
 *
 * The task's block is a real ASan-tracked allocation, so an allocator that
 * escapes the block trips AddressSanitizer rather than silently corrupting.
 *
 * These cases cover the *correct* behaviour that exists today. The finding-#3
 * (unvalidated realloc pointer) and finding-#9 (no hard cap at block top)
 * regression tests land with their fixes — asserting the fixed contract here
 * now would fail against the current buggy code.
 */
#include "framework.h"
#include "memory.h"
#include "task.h"
#include <stdint.h>
#include <string.h>

/* From tests/stubs/taskheap_stubs.c */
void taskheap_set_task(void *block, uint32_t size);
void taskheap_clear_task(void);
void taskheap_init_task(TCB *t, void *block, uint32_t size);
void taskheap_use(TCB *t);

/* The allocator's minimum alignment (kernel/memory/heap_internal.h VHEAP_ALIGN,
 * internal). Mirrored here so the test doesn't pull a private header. */
#define TASKHEAP_ALIGN 8u

/* A generously-aligned block for the synthetic task's heap. 8 KB is ample for
 * these allocations and leaves the growth check (task_live_sp == UINTPTR_MAX on
 * host) far away, so the tests stay in the intended region. */
#define BLOCK_SZ 8192u
static uint8_t g_block[BLOCK_SZ] __attribute__((aligned(16)));

static void setup(void) { taskheap_set_task(g_block, BLOCK_SZ); }

/* malloc returns a usable, aligned, in-block, writable pointer. */
static void test_malloc_basic(void) {
  setup();
  void *p = malloc(64);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT(((uintptr_t)p & (TASKHEAP_ALIGN - 1)) == 0); /* aligned */
  TEST_ASSERT((uint8_t *)p >= g_block &&
              (uint8_t *)p + 64 <= g_block + BLOCK_SZ); /* inside the block */
  memset(p, 0xAB, 64);                                  /* writable */
  free(p);
}

/* Two live allocations never overlap. */
static void test_two_allocs_disjoint(void) {
  setup();
  uint8_t *a = (uint8_t *)malloc(100);
  uint8_t *b = (uint8_t *)malloc(100);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT(a + 100 <= b || b + 100 <= a);
  free(a);
  free(b);
}

/* free() returns space so an equal-size malloc can reuse it (heap doesn't grow
 * unbounded under alloc/free churn). */
static void test_free_then_reuse(void) {
  setup();
  void *a = malloc(128);
  TEST_ASSERT_NOT_NULL(a);
  uintptr_t a_addr = (uintptr_t)a; /* capture before free (avoid UAF-value read) */
  free(a);
  void *b = malloc(128);
  TEST_ASSERT_EQ(a_addr, (uintptr_t)b); /* reused the freed slot */
  free(b);
}

/* calloc zeroes its payload. */
static void test_calloc_zeroes(void) {
  setup();
  uint8_t *p = (uint8_t *)calloc(16, 4);
  TEST_ASSERT_NOT_NULL(p);
  for (int i = 0; i < 64; i++)
    TEST_ASSERT_EQ(p[i], 0);
  free(p);
}

/* realloc(NULL, n) behaves as malloc; realloc(p, 0) frees and returns NULL. */
static void test_realloc_edges(void) {
  setup();
  void *p = realloc(NULL, 32);
  TEST_ASSERT_NOT_NULL(p);
  void *q = realloc(p, 0);
  TEST_ASSERT_NULL(q);
}

/* realloc grows a block and preserves the original bytes. */
static void test_realloc_grow_preserves(void) {
  setup();
  uint8_t *p = (uint8_t *)malloc(32);
  TEST_ASSERT_NOT_NULL(p);
  for (int i = 0; i < 32; i++)
    p[i] = (uint8_t)(i + 1);
  uint8_t *q = (uint8_t *)realloc(p, 128);
  TEST_ASSERT_NOT_NULL(q);
  for (int i = 0; i < 32; i++)
    TEST_ASSERT_EQ(q[i], (uint8_t)(i + 1));
  free(q);
}

/* ===========================================================================
 * Regression tests for STAGE5_REVIEW_FINDINGS.md — these are EXPECTED TO FAIL
 * against the current (unfixed) code; they assert the fixed contract. They fail
 * gracefully (an assertion, not a crash) so the summary still prints.
 * =========================================================================== */

/* Finding #9: per-task malloc has no hard cap at the block top. task_live_sp()
 * is UINTPTR_MAX on host, so the only growth guard never fires and malloc hands
 * back memory past the end of the task's block instead of returning NULL.
 * A request larger than the whole block must fail. */
static uint8_t g_small[64] __attribute__((aligned(16)));
static void test_bug9_malloc_past_block_top_returns_null(void) {
  taskheap_set_task(g_small, sizeof g_small);
  /* Larger than the block. malloc only writes a header at the base (in-block),
   * so this doesn't itself corrupt — but it wrongly succeeds. The returned
   * payload would run past g_small; we never write through it. */
  void *p = malloc(sizeof g_small * 4);
  TEST_ASSERT_NULL(p); /* FAILS today: returns non-NULL (grew past the block) */
}

/* Finding #3: realloc() dereferences the user pointer without the range check
 * free() has, so a pointer into ANOTHER task's block is treated as a valid
 * block and its contents are copied into the caller's heap — cross-task
 * disclosure. Two tasks, each with its own block. */
static uint8_t g_blockA[2048] __attribute__((aligned(16)));
static uint8_t g_blockB[2048] __attribute__((aligned(16)));
static TCB g_tA, g_tB;
static void test_bug3_realloc_foreign_ptr_discloses(void) {
  const uint8_t SECRET = 0x5A;
  taskheap_init_task(&g_tA, g_blockA, sizeof g_blockA);
  taskheap_init_task(&g_tB, g_blockB, sizeof g_blockB);

  /* Task A allocates and fills a secret. */
  taskheap_use(&g_tA);
  uint8_t *a = (uint8_t *)malloc(64);
  TEST_ASSERT_NOT_NULL(a);
  memset(a, SECRET, 64);

  /* Task B realloc()s A's pointer (foreign to B). A correct realloc rejects a
   * pointer outside the caller's own heap (as free() does). The buggy one
   * copies A's block into B's new buffer. */
  taskheap_use(&g_tB);
  uint8_t *nb = (uint8_t *)realloc(a, 128);

  int disclosed = 0;
  if (nb) {
    for (int i = 0; i < 64; i++)
      if (nb[i] == SECRET)
        disclosed++;
  }
  /* FAILS today: nb holds A's secret bytes (disclosed == 64). */
  TEST_ASSERT_EQ(disclosed, 0);
}

static const test_case_t taskheap_cases[] = {
    TEST_CASE(test_malloc_basic),      TEST_CASE(test_two_allocs_disjoint),
    TEST_CASE(test_free_then_reuse),   TEST_CASE(test_calloc_zeroes),
    TEST_CASE(test_realloc_edges),     TEST_CASE(test_realloc_grow_preserves),
    /* expected-fail regression tests (see STAGE5_REVIEW_FINDINGS.md) */
    TEST_CASE(test_bug9_malloc_past_block_top_returns_null),
    TEST_CASE(test_bug3_realloc_foreign_ptr_discloses),
};

const test_suite_t taskheap_suite = {
    .name = "per-task heap (VAIOS_TASK_HEAP)",
    .cases = taskheap_cases,
    .count = TEST_COUNT(taskheap_cases),
};
