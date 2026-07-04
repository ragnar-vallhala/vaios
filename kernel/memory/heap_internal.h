// =============================================================================
// kernel/memory/heap_internal.h — internal contract between the heap facade
// (memory.c) and a free-block *index* backend (seglist.c / tlsf.c).
//
// This is NOT a public API. The public heap interface is include/memory.h
// (v_malloc / v_free / v_heap_memory_init / v_get_heap_*), which never changes
// regardless of which allocator is compiled in.
//
// Division of labour — the facade owns everything that is identical across
// allocators, the backend owns only what actually differs:
//
//   facade (memory.c)          backend (seglist.c | tlsf.c)
//   -----------------          ----------------------------
//   public API + locking       organises FREE blocks for lookup:
//   block header + magic         vheap_index_reset / _insert / _remove / _find
//   split on alloc
//   coalesce on free
//   stats / watermark / perf
//
// The facade hands the backend whole Heap_Mem_Block objects and asks only
// "file this free block", "unfile this free block", "find a free block big
// enough". How the backend indexes them (flat size-class lists vs. a two-level
// bitmap) is its private business. This is analogous to how libc's malloc sits
// on a small, fixed low-level contract (sbrk/mmap) — except here the contract
// is the free-list index, and the names are vheap_-prefixed so they can never
// collide with the C standard library.
// =============================================================================
#ifndef VAIOS_HEAP_INTERNAL_H
#define VAIOS_HEAP_INTERNAL_H

#include "memory.h" // Heap_Mem_Block, SANITY_MAGIC_NUMBER, MEM_FREE/MEM_ALOC
#include <stdint.h>

// Payload alignment for every allocation (bytes). Also the small-block
// granularity the backends key their finest size buckets to.
#define VHEAP_ALIGN 8u

// Free-list links, overlaid on a free block's payload (valid only while the
// block is free — the header is never touched). Both backends use this exact
// layout, so it lives here rather than being duplicated. Its size sets the
// minimum block payload: every block must, once freed, be able to hold its
// links. On the 32-bit target that is 8 bytes; on the 64-bit host test build,
// 16.
typedef struct {
  Heap_Mem_Block *next_free;
  Heap_Mem_Block *prev_free;
} FreeNode;

#define VHEAP_FREE_NODE(blk)                                                    \
  ((FreeNode *)((uint8_t *)(blk) + sizeof(Heap_Mem_Block)))
#define VHEAP_MIN_PAYLOAD ((uint32_t)sizeof(FreeNode))

// ---- Free-block index backend contract -------------------------------------
// Implemented by exactly one of seglist.c / tlsf.c (chosen by VAIOS_HEAP_ALGO);
// the other compiles to an empty translation unit. All calls run inside the
// facade's critical section, so backends need no locking of their own.

// Empty the index. The facade calls this in v_heap_memory_init before filing
// the initial whole-heap free block.
void vheap_index_reset(void);

// File a free block. block->size is current and valid. O(1).
void vheap_index_insert(Heap_Mem_Block *block);

// Unfile a specific free block (before it is claimed or coalesced away). The
// block is currently in the index. O(1).
void vheap_index_remove(Heap_Mem_Block *block);

// Return a free block whose payload can hold `size` bytes (already aligned to
// VHEAP_ALIGN and >= VHEAP_MIN_PAYLOAD), or NULL if none. The block is left IN
// the index — the facade claims it with vheap_index_remove(). The returned
// block satisfies block->size >= size.
Heap_Mem_Block *vheap_index_find(uint32_t size);

// Human-readable backend name for the init log line ("segregated-fit"/"TLSF").
const char *vheap_index_name(void);

// Work done by the most recent vheap_index_find: the number of free blocks it
// had to inspect. A deterministic, platform-independent measure of search cost
// (no cycle counter needed) — the direct signal behind the O(n)-vs-O(1)
// difference. Segregated-fit reports its first-fit walk length (grows as free
// lists lengthen under fragmentation); TLSF reports a small constant (1, or 2
// when it climbs one level), since its lookup is O(1). Defined by the facade,
// written by the backend, read by profiling code (examples/mem_stress.c).
extern uint32_t vheap_find_probes;

#endif // VAIOS_HEAP_INTERNAL_H
