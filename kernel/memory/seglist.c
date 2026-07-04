// =============================================================================
// kernel/memory/seglist.c — segregated-fit free-block index backend.
//
// Implements the heap_internal.h contract with 8 doubly-linked size-class free
// lists. find is first-fit within the request's class (classes straddle a size
// range) then head-of-first-non-empty-higher-class (guaranteed to fit, no
// walk). insert/remove are O(1). Coalescing, splitting, and stats belong to the
// facade (memory.c); this file only organises free blocks for lookup.
//
// Selected when VAIOS_HEAP_ALGO == VAIOS_HEAP_SEGLIST; otherwise this whole file
// compiles to an empty translation unit so its symbols never collide with the
// TLSF backend (both are always in the build — kernel/*.c is globbed).
// =============================================================================
#include "heap_internal.h"
#include "vaios_config.h"
#include <stdint.h>

#if VAIOS_HEAP_ALGO == VAIOS_HEAP_SEGLIST

#define NUM_SIZE_CLASSES 8

static Heap_Mem_Block *free_lists[NUM_SIZE_CLASSES];

// Map a payload size to its free-list class (upper bounds 8/16/32/64/128/
// 256/512, then a catch-all for anything larger).
__attribute__((always_inline)) static inline int size_class(uint32_t size) {
  if (size <= 8)
    return 0;
  if (size <= 16)
    return 1;
  if (size <= 32)
    return 2;
  if (size <= 64)
    return 3;
  if (size <= 128)
    return 4;
  if (size <= 256)
    return 5;
  if (size <= 512)
    return 6;
  return 7;
}

void vheap_index_reset(void) {
  for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    free_lists[i] = NULL;
}

void vheap_index_insert(Heap_Mem_Block *blk) {
  int c = size_class(blk->size);
  FreeNode *n = VHEAP_FREE_NODE(blk);
  n->prev_free = NULL;
  n->next_free = free_lists[c];
  if (free_lists[c])
    VHEAP_FREE_NODE(free_lists[c])->prev_free = blk;
  free_lists[c] = blk;
}

void vheap_index_remove(Heap_Mem_Block *blk) {
  FreeNode *n = VHEAP_FREE_NODE(blk);
  if (n->prev_free)
    VHEAP_FREE_NODE(n->prev_free)->next_free = n->next_free;
  else
    free_lists[size_class(blk->size)] = n->next_free; // blk was the head
  if (n->next_free)
    VHEAP_FREE_NODE(n->next_free)->prev_free = n->prev_free;
}

Heap_Mem_Block *vheap_index_find(uint32_t size) {
  uint32_t probes = 0;
  int c = size_class(size);
  // Blocks in the requested class straddle the size boundary, so first-fit.
  for (Heap_Mem_Block *b = free_lists[c]; b; b = VHEAP_FREE_NODE(b)->next_free) {
    probes++;
    if (b->size >= size) {
      vheap_find_probes = probes;
      return b;
    }
  }
  // Any block in a higher class is strictly larger than this class's whole
  // range, hence guaranteed to fit — take the head, no walk.
  for (int k = c + 1; k < NUM_SIZE_CLASSES; k++) {
    probes++;
    if (free_lists[k]) {
      vheap_find_probes = probes;
      return free_lists[k];
    }
  }
  vheap_find_probes = probes;
  return NULL;
}

const char *vheap_index_name(void) { return "segregated-fit"; }

#endif // VAIOS_HEAP_ALGO == VAIOS_HEAP_SEGLIST
