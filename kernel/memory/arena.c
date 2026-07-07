// =============================================================================
// kernel/memory/arena.c — per-task private heap arena (Phase 3, Stage 4).
//
// A task's arena is a contiguous region carved from the kernel heap. It is laid
// out as a chain of boundary-tag blocks using the SAME header as the kernel heap
// (Heap_Mem_Block: magic/size/status/prev), but indexed with an IMPLICIT free
// list: alloc first-fits by walking blocks in address order from the base, and
// free coalesces with its physical neighbours (forward via size, backward via
// the prev back-link). No size-class index — a per-task arena is small, so the
// O(n) walk is cheap and the code stays tiny and self-contained.
//
// Everything runs under the kernel critical section; the arena is per-task but
// v_task_malloc/free target the *current* task, and a task only allocates from
// its own arena, so there is no cross-task contention on the region itself.
// =============================================================================
#include "arena.h"

#if VAIOS_TASK_ARENA

#include "heap_internal.h" // VHEAP_ALIGN, VHEAP_MIN_PAYLOAD
#include "memory.h"        // Heap_Mem_Block, SANITY_MAGIC_NUMBER, MEM_FREE/ALOC
#include "port.h"          // ENTER_CRITICAL / EXIT_CRITICAL
#include "task.h"
#include "utils.h"

#define HDR ((uint32_t)sizeof(Heap_Mem_Block))

extern TCB *current_task;

// Lay the region out as one free block spanning [base, base + size).
void v_arena_format(void *base, uint32_t size) {
  Heap_Mem_Block *b = (Heap_Mem_Block *)base;
  b->magic_number = SANITY_MAGIC_NUMBER;
  b->status = MEM_FREE;
  b->size = size - HDR;
  b->prev = NULL;
}

static inline int arena_contains(TCB *t, void *p) {
  return (uint8_t *)p >= (uint8_t *)t->arena_base &&
         (uint8_t *)p < (uint8_t *)t->arena_base + t->arena_size;
}

// Repoint the physically-following block's prev back-link at `blk` after a split
// or coalesce changed blk->size.
static inline void fixup_next_prev(TCB *t, Heap_Mem_Block *blk) {
  Heap_Mem_Block *after =
      (Heap_Mem_Block *)((uint8_t *)blk + HDR + blk->size);
  if (arena_contains(t, after) && after->magic_number == SANITY_MAGIC_NUMBER)
    after->prev = blk;
}

void *v_arena_alloc(TCB *t, size_t size) {
  if (!t || !t->arena_base || size == 0)
    return NULL;
  uint32_t need =
      (uint32_t)((size + (VHEAP_ALIGN - 1)) & ~(size_t)(VHEAP_ALIGN - 1));
  if (need < VHEAP_MIN_PAYLOAD)
    need = VHEAP_MIN_PAYLOAD;

  ENTER_CRITICAL();
  // First-fit: walk blocks in address order until one free block fits.
  for (Heap_Mem_Block *b = (Heap_Mem_Block *)t->arena_base;
       arena_contains(t, b) && b->magic_number == SANITY_MAGIC_NUMBER;
       b = (Heap_Mem_Block *)((uint8_t *)b + HDR + b->size)) {
    if (b->status != MEM_FREE || b->size < need)
      continue;
    // Split when the residual can carry its own header + minimum payload.
    if (b->size >= need + HDR + VHEAP_MIN_PAYLOAD) {
      Heap_Mem_Block *res = (Heap_Mem_Block *)((uint8_t *)b + HDR + need);
      res->magic_number = SANITY_MAGIC_NUMBER;
      res->status = MEM_FREE;
      res->size = b->size - need - HDR;
      res->prev = b;
      fixup_next_prev(t, res);
      b->size = need;
    }
    b->status = MEM_ALOC;
    EXIT_CRITICAL();
    return (void *)((uint8_t *)b + HDR);
  }
  EXIT_CRITICAL();
  return NULL; // no fit
}

void v_arena_free(TCB *t, void *ptr) {
  if (!t || !ptr)
    return;
  Heap_Mem_Block *block = (Heap_Mem_Block *)((uint8_t *)ptr - HDR);

  ENTER_CRITICAL();
  if (!arena_contains(t, block) ||
      block->magic_number != SANITY_MAGIC_NUMBER || block->status != MEM_ALOC) {
    EXIT_CRITICAL();
    V_KLOG(LOG_ERROR, "[ARENA] invalid/foreign/double free at 0x%x", ptr);
    return;
  }
  block->status = MEM_FREE;

  // Coalesce forward: absorb the next block if it is free.
  Heap_Mem_Block *next =
      (Heap_Mem_Block *)((uint8_t *)block + HDR + block->size);
  if (arena_contains(t, next) && next->magic_number == SANITY_MAGIC_NUMBER &&
      next->status == MEM_FREE) {
    block->size += HDR + next->size;
    fixup_next_prev(t, block);
  }
  // Coalesce backward: let the previous block absorb this one (O(1) via prev).
  Heap_Mem_Block *prev = block->prev;
  if (prev && prev->magic_number == SANITY_MAGIC_NUMBER &&
      prev->status == MEM_FREE) {
    prev->size += HDR + block->size;
    fixup_next_prev(t, prev);
    block = prev;
  }
  EXIT_CRITICAL();
}

// --- public task-facing API (operate on the current task's arena) -----------
int v_task_arena_create(uint32_t size) {
  if (!current_task)
    return -1;
  if (size < HDR + VHEAP_MIN_PAYLOAD)
    return -1; // too small to hold even one allocation
  if (current_task->arena_base)
    return -1; // one arena per task
  void *base = v_malloc(size);
  if (!base)
    return -1; // kernel heap exhausted
  v_arena_format(base, size);
  ENTER_CRITICAL();
  current_task->arena_base = base;
  current_task->arena_size = size;
  EXIT_CRITICAL();
  V_KLOG(LOG_INFO, "[ARENA] task %u arena at 0x%x size %u",
         current_task->task_id, base, (unsigned)size);
  return 0;
}

void *v_task_malloc(size_t size) { return v_arena_alloc(current_task, size); }

void v_task_free(void *ptr) { v_arena_free(current_task, ptr); }

uint32_t v_task_arena_used(void) {
  TCB *t = current_task;
  if (!t || !t->arena_base)
    return 0;
  uint32_t used = 0;
  ENTER_CRITICAL();
  for (Heap_Mem_Block *b = (Heap_Mem_Block *)t->arena_base;
       arena_contains(t, b) && b->magic_number == SANITY_MAGIC_NUMBER;
       b = (Heap_Mem_Block *)((uint8_t *)b + HDR + b->size))
    if (b->status == MEM_ALOC)
      used += b->size;
  EXIT_CRITICAL();
  return used;
}

#endif // VAIOS_TASK_ARENA
