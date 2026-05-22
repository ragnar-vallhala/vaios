#include "memory.h"
#include "port.h"
#include "utils.h"
#include "vaios_config_default.h"
#include <stdint.h>

extern uint32_t _heap_start;
uint32_t allocation_size = 0;
uint32_t allocation_count = 0;
Heap_Mem_Block *heap_mem_head = NULL;

void v_heap_memory_init(void) {
  heap_mem_head = (Heap_Mem_Block *)&_heap_start;
  // Clear heap region so uninitialized area has magic==0
  // for (uint8_t *ptr = (uint8_t *)heap_mem_head;
  //      ptr - (uint8_t *)heap_mem_head < HEAP_SIZE; ptr++)
  //   *ptr = 0;
  v_memset(heap_mem_head, 0, HEAP_SIZE);

  allocation_size = 0;
  allocation_count = 0;

  V_KLOG(LOG_INFO, "[MEMORY] Heap memory head initialized at 0x%x size 0x%x",
        (void *)heap_mem_head, (unsigned)HEAP_SIZE);
}

static inline int in_heap(void *p) {
  return ((uint8_t *)p >= (uint8_t *)heap_mem_head) &&
         ((uint8_t *)p < (uint8_t *)heap_mem_head + HEAP_SIZE);
}

void *v_malloc(size_t size) {
  if (!heap_mem_head) {
    V_KLOG(LOG_ERROR,
          "[MEMORY] Dynamic allocation not allowed, init memory first");
    return NULL;
  }

  if (size == 0)
    return NULL;

  // align to 8 bytes
  size = (size + 7) & ~((size_t)7);

  // Protect heap (uncomment or replace with your RTOS protection)
  V_KLOG(LOG_DEBUG, "[MEMORY] Allocation request on heap of size: %u",
        (unsigned)size);

  ENTER_CRITICAL();

  Heap_Mem_Block *head = heap_mem_head;
  Heap_Mem_Block *prev_block = NULL; // block immediately before `head`
  while (in_heap(head)) {
    // If block has never been touched (magic == 0), treat remaining region as
    // fresh
    if (head->magic_number != SANITY_MAGIC_NUMBER) {
      // Ensure there is enough space for header + payload within heap
      uint8_t *payload = (uint8_t *)head + sizeof(Heap_Mem_Block);
      uint8_t *after = payload + size;
      if (!in_heap(after - 1))
        break; // not enough room

      head->magic_number = SANITY_MAGIC_NUMBER;
      head->status = MEM_ALOC;
      head->size = size;
      head->prev = prev_block;

      allocation_count++;
      allocation_size += size;

      EXIT_CRITICAL();
      V_KLOG(LOG_DEBUG, "[MEMORY] Allocated fresh block at 0x%x size %u",
            (void *)payload, (unsigned)size);
      return (void *)payload;
    }

    // If block is free and fits, use it (maybe split)
    if (head->status == MEM_FREE && head->size >= size) {
      // If remaining space after allocation is too small to hold header+1 byte,
      // consume whole block
      if (head->size <= size + sizeof(Heap_Mem_Block)) {
        head->status = MEM_ALOC;
        allocation_count++;
        allocation_size += head->size;

        EXIT_CRITICAL();
        V_KLOG(LOG_DEBUG, "[MEMORY] Reused block at 0x%x size %u",
              (void *)((uint8_t *)head + sizeof(Heap_Mem_Block)),
              (unsigned)head->size);
        return (void *)((uint8_t *)head + sizeof(Heap_Mem_Block));
      } else {
        // Split block: create newBlock after requested payload
        Heap_Mem_Block *newBlock =
            (Heap_Mem_Block *)((uint8_t *)head + sizeof(Heap_Mem_Block) + size);

        // ensure newBlock is in heap
        if (!in_heap((uint8_t *)newBlock))
          break;

        newBlock->magic_number = SANITY_MAGIC_NUMBER;
        newBlock->status = MEM_FREE;
        newBlock->size = head->size - size - sizeof(Heap_Mem_Block);
        newBlock->prev = head;

        // The block that previously followed `head` now follows newBlock;
        // repoint its prev. Skip if that slot is the fresh region or beyond
        // the heap.
        Heap_Mem_Block *after_new =
            (Heap_Mem_Block *)((uint8_t *)newBlock + sizeof(Heap_Mem_Block) +
                               newBlock->size);
        if (in_heap(after_new) &&
            after_new->magic_number == SANITY_MAGIC_NUMBER)
          after_new->prev = newBlock;

        head->status = MEM_ALOC;
        head->size = size;

        allocation_count++;
        allocation_size += size;

        EXIT_CRITICAL();
        V_KLOG(LOG_DEBUG,
              "[MEMORY] Split block at 0x%x size %u (remaining %u at 0x%x)",
              (void *)((uint8_t *)head + sizeof(Heap_Mem_Block)),
              (unsigned)size, (unsigned)newBlock->size, (void *)newBlock);
        return (void *)((uint8_t *)head + sizeof(Heap_Mem_Block));
      }
    }

    // advance to next block
    uint8_t *next_addr = (uint8_t *)head + sizeof(Heap_Mem_Block) + head->size;
    if (!in_heap(next_addr))
      break;
    prev_block = head;
    head = (Heap_Mem_Block *)next_addr;
  }

#if HEAP_WATERMARK_ENABLE == 1
  if (v_get_heap_allocation_size() > HEAP_SIZE - HEAP_WATERMARK_THRESHOLD) {
    v_panic(__FILE__, __LINE__, "Heap watermark exceeded! Used: %u, Limit: %u",
            (unsigned)v_get_heap_allocation_size(),
            (unsigned)(HEAP_SIZE - HEAP_WATERMARK_THRESHOLD));
  }
#endif

  EXIT_CRITICAL();
  V_KLOG(LOG_ERROR, "[MEMORY] Dynamic allocation failed, memory exhausted.");
  return NULL;
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

  // Get header
  Heap_Mem_Block *block =
      (Heap_Mem_Block *)((uint8_t *)ptr - sizeof(Heap_Mem_Block));

  // Validate pointer lies in heap and magic is valid
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

  // mark free and update counters
  block->status = MEM_FREE;
  allocation_count--;
  allocation_size -= block->size;

  // Coalesce forward: absorb the immediately-following block if it is free.
  Heap_Mem_Block *next =
      (Heap_Mem_Block *)((uint8_t *)block + sizeof(Heap_Mem_Block) +
                         block->size);
  if (in_heap(next) && next->magic_number == SANITY_MAGIC_NUMBER &&
      next->status == MEM_FREE) {
    block->size += sizeof(Heap_Mem_Block) + next->size;
    // The block that followed the absorbed `next` now follows `block`.
    Heap_Mem_Block *after =
        (Heap_Mem_Block *)((uint8_t *)block + sizeof(Heap_Mem_Block) +
                           block->size);
    if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
      after->prev = block;
  }

  // Coalesce backward: let the previous block absorb `block` if it is free.
  // block->prev makes this O(1) — no heap walk from the head.
  Heap_Mem_Block *prev = block->prev;
  if (prev && prev->magic_number == SANITY_MAGIC_NUMBER &&
      prev->status == MEM_FREE) {
    prev->size += sizeof(Heap_Mem_Block) + block->size;
    // Whatever now follows the enlarged `prev` must point its prev back to it.
    Heap_Mem_Block *after =
        (Heap_Mem_Block *)((uint8_t *)prev + sizeof(Heap_Mem_Block) +
                           prev->size);
    if (in_heap(after) && after->magic_number == SANITY_MAGIC_NUMBER)
      after->prev = prev;
  }

  EXIT_CRITICAL();
  V_KLOG(LOG_DEBUG, "[MEMORY] Deallocated pointer at 0x%x", ptr);
}

uint32_t v_get_heap_size(void) { return HEAP_SIZE; }
uint32_t v_get_heap_allocation_count(void) { return allocation_count; }
uint32_t v_get_heap_allocation_size(void) {
  return allocation_size + sizeof(Heap_Mem_Block) * allocation_count;
}
