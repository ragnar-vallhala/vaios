#ifndef VAIOS_ARENA_H
#define VAIOS_ARENA_H

/**
 * @file arena.h
 * @brief Per-task private heap arena (Phase 3, Stage 4).
 *
 * A task carves a private, contiguous heap region from the kernel heap with
 * v_task_arena_create(size); v_task_malloc/v_task_free then serve that task out
 * of its own arena only — no sharing between tasks, and no walk of the kernel
 * free-list index. The arena uses the same boundary-tag block header as the
 * kernel heap (Heap_Mem_Block) but an implicit free list (address-order walk,
 * first fit) since a per-task arena is small.
 *
 * Ownership model: creating the arena is a kernel action (it carves from the
 * shared heap), but malloc/free within an owned arena touch only the task's own
 * memory — so Stage 5 runs them unprivileged, with the MPU making the arena the
 * task's sole writable heap. Gated by VAIOS_TASK_ARENA.
 */

#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>

#if VAIOS_TASK_ARENA

struct Task_Control_Block;

// Carve a private arena of `size` bytes for the current task from the kernel
// heap and format it as one free block. Returns 0 on success, negative on bad
// size / exhaustion / an arena already existing. One arena per task.
int v_task_arena_create(uint32_t size);

// Allocate/free within the CURRENT task's arena. v_task_malloc returns NULL if
// the task has no arena or the arena is exhausted. v_task_free ignores pointers
// not from this task's arena.
void *v_task_malloc(size_t size);
void v_task_free(void *ptr);

// Bytes currently allocated (payload, excluding block headers) in the current
// task's arena. 0 if no arena. For tests / introspection.
uint32_t v_task_arena_used(void);

// --- internal core (operates on an explicit TCB; used by the above and the
// task teardown path). Not for application use. ---
void v_arena_format(void *base, uint32_t size);
void *v_arena_alloc(struct Task_Control_Block *t, size_t size);
void v_arena_free(struct Task_Control_Block *t, void *ptr);

#endif // VAIOS_TASK_ARENA

#endif // !VAIOS_ARENA_H
