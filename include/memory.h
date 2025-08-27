#ifndef VAIOS_MEMORY_H
#define VAIOS_MEMORY_H
#include <stddef.h>
#include <stdint.h>

typedef enum { MEM_FREE, MEM_ALOC } Heap_Mem_Type;

#define SANITY_MAGIC_NUMBER 0x14U

typedef struct {
  uint8_t magic_number;
  uint32_t size; // in bytes
  Heap_Mem_Type status;
} Heap_Mem_Block;

// Initialize heap memory
void heap_memory_init(void);

// Allocate memory
void *v_malloc(size_t size);

// Free memory
void v_free(void *ptr);

uint32_t v_get_heap_size(void);
uint32_t v_get_heap_allocation_count(void);
uint32_t v_get_heap_allocation_size(void);
#endif //! VAIOS_MEMORY_H
