// =============================================================================
// kernel/memory/tlsf.c — TLSF (Two-Level Segregated Fit) free-block index.
//
// Implements the heap_internal.h contract with two-level, bitmap-directed size
// classes so that find, insert and remove are all O(1) worst-case — the
// property the segregated-fit backend lacks (its find walks a class list). The
// facade (memory.c) still owns splitting, coalescing and stats; this file only
// organises free blocks for lookup.
//
//   * First level  (FL): the block's octave, floor(log2 size).
//   * Second level (SL): the octave split linearly into SL_INDEX_COUNT slots.
//
// A one-word FL bitmap marks non-empty octaves; a per-octave SL bitmap marks
// non-empty slots. find rounds the request UP to a slot boundary (good-fit) so
// the chosen list's head is guaranteed to fit and can be taken with no walk.
// List empty/non-empty transitions flip the bitmaps in O(1). On Cortex-M4 the
// find-first/last-set reduce to single-cycle CLZ.
//
// Selected when VAIOS_HEAP_ALGO == VAIOS_HEAP_TLSF; otherwise this whole file
// compiles to an empty translation unit.
//
// Reference: M. Masmano et al., "TLSF: a New Dynamic Memory Allocator for
// Real-Time Systems" (2004).
// =============================================================================
#include "heap_internal.h"
#include "vaios_config.h"
#include <stdint.h>

#if VAIOS_HEAP_ALGO == VAIOS_HEAP_TLSF

//-----------------------------------------------------------------------------
// Tuning. Sized for the vaios heap (HEAP_SIZE, default 88 KB) with headroom.
//-----------------------------------------------------------------------------
#define ALIGN_SIZE_LOG2 3             // 8-byte payload alignment (== VHEAP_ALIGN)
#define SL_INDEX_COUNT_LOG2 4         // 16 second-level lists per octave
#define SL_INDEX_COUNT (1u << SL_INDEX_COUNT_LOG2)
// Largest supported block is < 2^FL_INDEX_MAX. 20 -> up to ~1 MB, comfortably
// above the default heap while keeping the control block small.
#define FL_INDEX_MAX 20
#define FL_INDEX_SHIFT (SL_INDEX_COUNT_LOG2 + ALIGN_SIZE_LOG2)   // 7
#define FL_INDEX_COUNT (FL_INDEX_MAX - FL_INDEX_SHIFT + 1)       // 14
// Below this size FL is 0 and SL is a plain linear split (granularity
// SMALL_BLOCK_SIZE / SL_INDEX_COUNT == VHEAP_ALIGN bytes).
#define SMALL_BLOCK_SIZE (1u << FL_INDEX_SHIFT)                  // 128

_Static_assert(HEAP_SIZE < (1ull << FL_INDEX_MAX),
               "HEAP_SIZE exceeds TLSF FL_INDEX_MAX; raise FL_INDEX_MAX");
_Static_assert((1u << ALIGN_SIZE_LOG2) == VHEAP_ALIGN,
               "TLSF alignment must match the facade's VHEAP_ALIGN");

// One list head per (fl, sl), plus the bitmaps summarising which are non-empty.
// ~1 KB of .bss on the target.
static Heap_Mem_Block *blocks[FL_INDEX_COUNT][SL_INDEX_COUNT];
static uint32_t fl_bitmap;                  // bit fl set => octave non-empty
static uint32_t sl_bitmap[FL_INDEX_COUNT];  // bit sl set => that list non-empty

// Bit primitives — both fold to a single-cycle CLZ on Cortex-M4. The builtins
// are undefined at 0, so every caller guarantees a non-zero argument.
__attribute__((always_inline)) static inline int tlsf_fls(uint32_t x) {
  return 31 - __builtin_clz(x); // index of most-significant set bit
}
__attribute__((always_inline)) static inline int tlsf_ffs(uint32_t x) {
  return __builtin_ctz(x); // index of least-significant set bit
}

// Size -> (fl, sl). mapping_insert places a block by its EXACT size (filing);
// mapping_search rounds the request UP to the next slot boundary first, so the
// selected list holds only blocks >= the request (the good-fit no-walk trick).
static inline void mapping_insert(uint32_t size, int *fli, int *sli) {
  int fl, sl;
  if (size < SMALL_BLOCK_SIZE) {
    fl = 0;
    sl = (int)(size / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT));
  } else {
    fl = tlsf_fls(size);
    sl = (int)((size >> (fl - SL_INDEX_COUNT_LOG2)) ^ SL_INDEX_COUNT);
    fl -= (FL_INDEX_SHIFT - 1);
  }
  *fli = fl;
  *sli = sl;
}

static inline void mapping_search(uint32_t size, int *fli, int *sli) {
  if (size >= SMALL_BLOCK_SIZE) {
    uint32_t round = (1u << (tlsf_fls(size) - SL_INDEX_COUNT_LOG2)) - 1;
    size += round;
  }
  mapping_insert(size, fli, sli);
}

void vheap_index_reset(void) {
  fl_bitmap = 0;
  for (int i = 0; i < FL_INDEX_COUNT; i++) {
    sl_bitmap[i] = 0;
    for (unsigned j = 0; j < SL_INDEX_COUNT; j++)
      blocks[i][j] = NULL;
  }
}

void vheap_index_insert(Heap_Mem_Block *blk) {
  int fl, sl;
  mapping_insert(blk->size, &fl, &sl);
  FreeNode *n = VHEAP_FREE_NODE(blk);
  n->prev_free = NULL;
  n->next_free = blocks[fl][sl];
  if (blocks[fl][sl])
    VHEAP_FREE_NODE(blocks[fl][sl])->prev_free = blk;
  blocks[fl][sl] = blk;
  fl_bitmap |= (1u << fl);
  sl_bitmap[fl] |= (1u << sl);
}

void vheap_index_remove(Heap_Mem_Block *blk) {
  int fl, sl;
  mapping_insert(blk->size, &fl, &sl);
  FreeNode *n = VHEAP_FREE_NODE(blk);
  if (n->next_free)
    VHEAP_FREE_NODE(n->next_free)->prev_free = n->prev_free;
  if (n->prev_free) {
    VHEAP_FREE_NODE(n->prev_free)->next_free = n->next_free;
  } else {
    blocks[fl][sl] = n->next_free; // blk was the list head
    if (!n->next_free) {
      // List went empty — clear its SL bit, and the FL bit if the octave is now
      // wholly empty.
      sl_bitmap[fl] &= ~(1u << sl);
      if (!sl_bitmap[fl])
        fl_bitmap &= ~(1u << fl);
    }
  }
}

Heap_Mem_Block *vheap_index_find(uint32_t size) {
  int fl, sl;
  mapping_search(size, &fl, &sl);
  // Non-empty list at or above (fl, sl); good-fit rounding guarantees its head
  // fits, so no per-block size check is needed. "Probes" is 1 for a direct hit,
  // 2 when we climb one octave — constant, never a list walk. This is the O(1)
  // counterpart to the segregated heap's variable walk length.
  uint32_t sl_map = sl_bitmap[fl] & (~0u << sl);
  if (!sl_map) {
    uint32_t fl_map = fl_bitmap & (~0u << (fl + 1));
    if (!fl_map) {
      vheap_find_probes = 1;
      return NULL; // out of memory
    }
    fl = tlsf_ffs(fl_map);
    sl_map = sl_bitmap[fl];
    vheap_find_probes = 2; // climbed one level — still O(1)
  } else {
    vheap_find_probes = 1;
  }
  sl = tlsf_ffs(sl_map);
  return blocks[fl][sl];
}

const char *vheap_index_name(void) { return "TLSF"; }

#endif // VAIOS_HEAP_ALGO == VAIOS_HEAP_TLSF
