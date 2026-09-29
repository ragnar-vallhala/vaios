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

#include "vaios_config.h"
#if VAIOS_TASK_HEAP
// ---- Per-task heap (Stage 4) -----------------------------------------------
// Standard-named allocators for TASK code: they serve the CURRENT task out of
// its own region (heap grows up from the low end of the task's block, toward
// the down-growing stack). Distinct from the kernel's v_malloc/v_free, which
// always use the global kernel heap. malloc returns NULL when the task has no
// heap (e.g. called before the scheduler) or when the heap would meet the live
// stack — sizing the task region is the caller's responsibility.
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
// Bytes currently allocated (payload only) in the current task's heap; 0 if the
// task has no heap. Introspection / tests.
uint32_t v_task_heap_used(void);
#endif // VAIOS_TASK_HEAP
#endif //! VAIOS_MEMORY_H
