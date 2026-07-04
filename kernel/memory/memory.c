// =============================================================================
// kernel/memory/memory.c — heap allocator facade.
//
// Implements the public heap API (include/memory.h) on top of a pluggable
// free-block index backend (heap_internal.h, provided by seglist.c or tlsf.c
// per VAIOS_HEAP_ALGO). Everything that does not depend on how free blocks are
// indexed lives here and is written exactly once:
//
//   * the boundary-tag block header + magic/status discipline,
//   * request alignment and the not-initialised / zero-size guards,
//   * splitting an oversized block on allocation,
//   * forward+backward coalescing on free (via the Heap_Mem_Block::prev
//     physical back-link — O(1), backend-independent),
//   * the allocation counters, heap watermark, perf hooks and logging.
//
// The backend is asked only to file/unfile/find free blocks. This is the split
// libc draws between malloc and sbrk, applied at the free-list boundary.
// =============================================================================
#include "heap_internal.h"
#include "memory.h"
#include "perf_hooks.h"
#include "port.h"
#include "utils.h"
// Aggregator (not vaios_config_default.h) so an app's HEAP_SIZE override reaches
// the heap sizing here — the default header is #ifndef-guarded and the
// aggregator includes the app config first.
#include "vaios_config.h"
#include <stdint.h>

extern uint32_t _heap_start;

// `heap_mem_head` doubles as the not-initialised sentinel (v_malloc/v_free
// refuse while NULL) and the test hook test_memory.c toggles; keep the name.
// perf.c reads `allocation_size` (payload bytes in use) directly.
uint32_t allocation_size = 0;
uint32_t allocation_count = 0;
Heap_Mem_Block *heap_mem_head = NULL;

// Search-cost probe counter, written by the active backend's vheap_index_find.
// See heap_internal.h. Lives here so it exists whichever backend is compiled.
uint32_t vheap_find_probes = 0;

static inline int in_heap(void *p) {
  return ((uint8_t *)p >= (uint8_t *)heap_mem_head) &&
         ((uint8_t *)p < (uint8_t *)heap_mem_head + HEAP_SIZE);
}

// After a block's size changes (split or coalesce), the physically following
// block's prev back-link must be repointed at it. Centralised here since both
// paths need it.
static inline void fixup_next_prev(Heap_Mem_Block *blk) {
  Heap_Mem_Block *after =
      (Heap_Mem_Block *)((uint8_t *)blk + sizeof(Heap_Mem_Block) + blk->size);
  if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
    after->prev = blk;
}

void v_heap_memory_init(void) {
  heap_mem_head = (Heap_Mem_Block *)&_heap_start;
  v_memset(heap_mem_head, 0, HEAP_SIZE);

  vheap_index_reset();
  allocation_size = 0;
  allocation_count = 0;

  // The whole heap starts life as a single free block.
  heap_mem_head->magic_number = SANITY_MAGIC_NUMBER;
  heap_mem_head->status = MEM_FREE;
  heap_mem_head->size = HEAP_SIZE - sizeof(Heap_Mem_Block);
  heap_mem_head->prev = NULL;
  vheap_index_insert(heap_mem_head);

  V_KLOG(LOG_INFO, "[MEMORY] %s heap initialized at 0x%x size 0x%x",
         vheap_index_name(), (void *)heap_mem_head, (unsigned)HEAP_SIZE);
}

void *v_malloc(size_t size) {
  if (!heap_mem_head) {
    V_KLOG(LOG_ERROR,
           "[MEMORY] Dynamic allocation not allowed, init memory first");
    return NULL;
  }
  if (size == 0)
    return NULL;

  uint32_t need =
      (uint32_t)((size + (VHEAP_ALIGN - 1)) & ~(size_t)(VHEAP_ALIGN - 1));
  if (need < VHEAP_MIN_PAYLOAD)
    need = VHEAP_MIN_PAYLOAD; // a freed block must be able to hold its links

  ENTER_CRITICAL();

  Heap_Mem_Block *blk = vheap_index_find(need);
  if (!blk) {
#if HEAP_WATERMARK_ENABLE == 1
    if (v_get_heap_allocation_size() > HEAP_SIZE - HEAP_WATERMARK_THRESHOLD) {
      v_panic(__FILE__, __LINE__, "Heap watermark exceeded! Used: %u, Limit: %u",
              (unsigned)v_get_heap_allocation_size(),
              (unsigned)(HEAP_SIZE - HEAP_WATERMARK_THRESHOLD));
    }
#endif
    EXIT_CRITICAL();
    PERF_HEAP_OOM();
    V_KLOG(LOG_ERROR, "[MEMORY] Dynamic allocation failed, memory exhausted.");
    return NULL;
  }

  vheap_index_remove(blk); // claim it off the free index

  int did_split = 0;
  // Split only if the residual can hold a header plus the minimum payload.
  if (blk->size >= need + sizeof(Heap_Mem_Block) + VHEAP_MIN_PAYLOAD) {
    Heap_Mem_Block *res =
        (Heap_Mem_Block *)((uint8_t *)blk + sizeof(Heap_Mem_Block) + need);
    res->magic_number = SANITY_MAGIC_NUMBER;
    res->status = MEM_FREE;
    res->size = blk->size - need - sizeof(Heap_Mem_Block);
    res->prev = blk;
    fixup_next_prev(res); // the block after `blk` now follows the residual

    blk->size = need;
    vheap_index_insert(res);
    did_split = 1;
  }

  blk->status = MEM_ALOC;
  allocation_count++;
  allocation_size += blk->size;
  PERF_HEAP_ALLOC(blk->size, did_split);

  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG, "[MEMORY] Allocated 0x%x size %u",
         (void *)((uint8_t *)blk + sizeof(Heap_Mem_Block)), (unsigned)blk->size);
  return (void *)((uint8_t *)blk + sizeof(Heap_Mem_Block));
}

void v_free(void *ptr) {
  if (ptr == NULL) {
    V_KLOG(LOG_ERROR, "[MEMORY] Deallocation not allowed of NULL pointers");
    return;
  }
  if (!heap_mem_head) {
    V_KLOG(LOG_ERROR, "[MEMORY] Heap not initialized");
    return;
  }

  ENTER_CRITICAL();

  Heap_Mem_Block *block =
      (Heap_Mem_Block *)((uint8_t *)ptr - sizeof(Heap_Mem_Block));

  if (!in_heap(block) || block->magic_number != SANITY_MAGIC_NUMBER) {
    V_KLOG(LOG_ERROR, "[MEMORY] Invalid free or corrupted block at 0x%x", ptr);
    EXIT_CRITICAL();
    return;
  }
  if (block->status != MEM_ALOC) {
    V_KLOG(LOG_ERROR,
           "[MEMORY] Double free or freeing non-allocated block at 0x%x", ptr);
    EXIT_CRITICAL();
    return;
  }

  block->status = MEM_FREE;
  allocation_count--;
  allocation_size -= block->size;

  int coalesces = 0;
  // Coalesce forward: absorb the next block if it is free.
  Heap_Mem_Block *next =
      (Heap_Mem_Block *)((uint8_t *)block + sizeof(Heap_Mem_Block) +
                         block->size);
  if (in_heap(next) && next->magic_number == SANITY_MAGIC_NUMBER &&
      next->status == MEM_FREE) {
    vheap_index_remove(next); // off the index before it is absorbed
    block->size += sizeof(Heap_Mem_Block) + next->size;
    fixup_next_prev(block);
    coalesces++;
  }

  // Coalesce backward: let the previous block absorb this one. O(1) via prev.
  Heap_Mem_Block *prev = block->prev;
  if (prev && prev->magic_number == SANITY_MAGIC_NUMBER &&
      prev->status == MEM_FREE) {
    vheap_index_remove(prev);
    prev->size += sizeof(Heap_Mem_Block) + block->size;
    fixup_next_prev(prev);
    block = prev; // the merged block is `prev`
    coalesces++;
  }

  vheap_index_insert(block); // file the coalesced block
  PERF_HEAP_FREE(coalesces);

  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG, "[MEMORY] Deallocated pointer at 0x%x", ptr);
}

uint32_t v_get_heap_size(void) { return HEAP_SIZE; }
uint32_t v_get_heap_allocation_count(void) { return allocation_count; }
uint32_t v_get_heap_allocation_size(void) {
  return allocation_size + sizeof(Heap_Mem_Block) * allocation_count;
}
