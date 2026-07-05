#ifndef VAIOS_MEMORY_H
#define VAIOS_MEMORY_H
#include <stddef.h>
#include <stdint.h>

typedef enum { MEM_FREE, MEM_ALOC } Heap_Mem_Type;

#define SANITY_MAGIC_NUMBER 0x14U

typedef struct Heap_Mem_Block {
  uint32_t magic_number;
  uint32_t size; // in bytes
  uint32_t status;
  // Previous block in address order (NULL for the first block). Lets v_free
  // coalesce backward in O(1) instead of walking the heap from the head.
  // Occupies the former padding slot — sizeof stays 16 (8-byte aligned).
  struct Heap_Mem_Block *prev;
} Heap_Mem_Block;

// Initialize heap memory
void v_heap_memory_init(void);

// Allocate memory
void *v_malloc(size_t size);

// Allocate `size` bytes whose payload is aligned to `align` (a power of two).
// The returned pointer is freeable with v_free. Backs MPU-guarded task stacks
// whose base must land on a region-size boundary. Returns NULL on bad args or
// exhaustion; falls back to v_malloc when align <= the default 8-byte alignment.
void *v_memalign(size_t align, size_t size);

// Free memory
void v_free(void *ptr);

uint32_t v_get_heap_size(void);
uint32_t v_get_heap_allocation_count(void);
uint32_t v_get_heap_allocation_size(void);
#endif //! VAIOS_MEMORY_H
