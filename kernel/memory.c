#include "memory.h"
#include "perf_hooks.h"
#include "port.h"
#include "utils.h"
// Use the aggregator (not vaios_config_default.h directly) so an app's
// vaios_app_config.h HEAP_SIZE override actually reaches the heap sizing below;
// the default header is #ifndef-guarded and the aggregator includes the app
// config before it. Every other kernel TU already goes through the aggregator.
#include "vaios_config.h"
#include <stdint.h>

extern uint32_t _heap_start;
uint32_t allocation_size = 0;
uint32_t allocation_count = 0;
Heap_Mem_Block *heap_mem_head = NULL;

//-----------------------------------------------------------------------------
// Segregated free lists (§3.3 Stage B)
//
// The heap is one contiguous run of blocks — no "wilderness" region. At init
// it is a single free block; malloc splits, free coalesces. Every block is
// either allocated or on exactly one of NUM_SIZE_CLASSES free lists, indexed
// by payload size. malloc looks up the right class instead of walking the
// whole heap, so small-block allocation is O(1) amortised.
//
// Each free block stores its doubly-linked free-list pointers inside its own
// (unused-while-free) payload via FreeNode — the 16-byte header is untouched.
// This requires every block's payload to be >= 8 bytes, which holds: the
// minimum allocation is 8 and every split residual is >= MIN_PAYLOAD.
//-----------------------------------------------------------------------------
#define NUM_SIZE_CLASSES 8
#define MIN_PAYLOAD 8 // room for the two FreeNode pointers

// Free-list links, overlaid on a free block's payload (valid only while free).
typedef struct {
  Heap_Mem_Block *next_free;
  Heap_Mem_Block *prev_free;
} FreeNode;

#define FREE_NODE(blk) ((FreeNode *)((uint8_t *)(blk) + sizeof(Heap_Mem_Block)))

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

// Push a free block onto the head of its size-class list.
__attribute__((always_inline)) static inline void
fl_insert(Heap_Mem_Block *blk) {
  int c = size_class(blk->size);
  FreeNode *n = FREE_NODE(blk);
  n->prev_free = NULL;
  n->next_free = free_lists[c];
  if (free_lists[c])
    FREE_NODE(free_lists[c])->prev_free = blk;
  free_lists[c] = blk;
}

// Splice a free block out of whatever list it currently sits on.
__attribute__((always_inline)) static inline void
fl_remove(Heap_Mem_Block *blk) {
  FreeNode *n = FREE_NODE(blk);
  if (n->prev_free)
    FREE_NODE(n->prev_free)->next_free = n->next_free;
  else
    free_lists[size_class(blk->size)] = n->next_free; // blk was the head
  if (n->next_free)
    FREE_NODE(n->next_free)->prev_free = n->prev_free;
}

static inline int in_heap(void *p) {
  return ((uint8_t *)p >= (uint8_t *)heap_mem_head) &&
         ((uint8_t *)p < (uint8_t *)heap_mem_head + HEAP_SIZE);
}

void v_heap_memory_init(void) {
  heap_mem_head = (Heap_Mem_Block *)&_heap_start;
  v_memset(heap_mem_head, 0, HEAP_SIZE);

  for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    free_lists[i] = NULL;
  allocation_size = 0;
  allocation_count = 0;

  // The whole heap starts life as a single free block.
  heap_mem_head->magic_number = SANITY_MAGIC_NUMBER;
  heap_mem_head->status = MEM_FREE;
  heap_mem_head->size = HEAP_SIZE - sizeof(Heap_Mem_Block);
  heap_mem_head->prev = NULL;
  fl_insert(heap_mem_head);

  V_KLOG(LOG_INFO, "[MEMORY] Heap memory head initialized at 0x%x size 0x%x",
         (void *)heap_mem_head, (unsigned)HEAP_SIZE);
}

// Find the smallest free block that can satisfy `size`.
static Heap_Mem_Block *find_free_block(uint32_t size) {
  int c = size_class(size);
  // Blocks in the requested class straddle the size boundary, so first-fit.
  for (Heap_Mem_Block *b = free_lists[c]; b; b = FREE_NODE(b)->next_free) {
    if (b->size >= size)
      return b;
  }
  // Any block in a higher class is strictly larger than this class's whole
  // range, hence guaranteed to fit — take the head, no walk.
  for (int k = c + 1; k < NUM_SIZE_CLASSES; k++) {
    if (free_lists[k])
      return free_lists[k];
  }
  return NULL;
}

void *v_malloc(size_t size) {
  if (!heap_mem_head) {
    V_KLOG(LOG_ERROR,
           "[MEMORY] Dynamic allocation not allowed, init memory first");
    return NULL;
  }
  if (size == 0)
    return NULL;

  size = (size + 7) & ~((size_t)7); // 8-byte aligned

  ENTER_CRITICAL();

  Heap_Mem_Block *blk = find_free_block(size);
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

  fl_remove(blk); // claim it off its free list

  int did_split = 0;
  // Split only if the residual can hold a header plus the minimum payload.
  if (blk->size >= size + sizeof(Heap_Mem_Block) + MIN_PAYLOAD) {
    Heap_Mem_Block *res =
        (Heap_Mem_Block *)((uint8_t *)blk + sizeof(Heap_Mem_Block) + size);
    res->magic_number = SANITY_MAGIC_NUMBER;
    res->status = MEM_FREE;
    res->size = blk->size - size - sizeof(Heap_Mem_Block);
    res->prev = blk;

    // The block that followed `blk` now follows the residual.
    Heap_Mem_Block *after =
        (Heap_Mem_Block *)((uint8_t *)res + sizeof(Heap_Mem_Block) + res->size);
    if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
      after->prev = res;

    blk->size = size;
    fl_insert(res);
    did_split = 1;
  }

  blk->status = MEM_ALOC;
  allocation_count++;
  allocation_size += blk->size;
  PERF_HEAP_ALLOC(blk->size, did_split);

  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG, "[MEMORY] Allocated 0x%x size %u",
         (void *)((uint8_t *)blk + sizeof(Heap_Mem_Block)),
         (unsigned)blk->size);
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
    fl_remove(next); // off its size-class list before it is absorbed
    block->size += sizeof(Heap_Mem_Block) + next->size;
    Heap_Mem_Block *after =
        (Heap_Mem_Block *)((uint8_t *)block + sizeof(Heap_Mem_Block) +
                           block->size);
    if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
      after->prev = block;
    coalesces++;
  }

  // Coalesce backward: let the previous block absorb this one. O(1) via prev.
  Heap_Mem_Block *prev = block->prev;
  if (prev && prev->magic_number == SANITY_MAGIC_NUMBER &&
      prev->status == MEM_FREE) {
    fl_remove(prev);
    prev->size += sizeof(Heap_Mem_Block) + block->size;
    Heap_Mem_Block *after =
        (Heap_Mem_Block *)((uint8_t *)prev + sizeof(Heap_Mem_Block) +
                           prev->size);
    if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
      after->prev = prev;
    block = prev; // the merged block is `prev`
    coalesces++;
  }

  fl_insert(block); // file the coalesced block on its size-class list
  PERF_HEAP_FREE(coalesces);

  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG, "[MEMORY] Deallocated pointer at 0x%x", ptr);
}

uint32_t v_get_heap_size(void) { return HEAP_SIZE; }
uint32_t v_get_heap_allocation_count(void) { return allocation_count; }
uint32_t v_get_heap_allocation_size(void) {
  return allocation_size + sizeof(Heap_Mem_Block) * allocation_count;
}
