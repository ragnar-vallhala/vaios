/* Synthetic task backing for the vaios_taskheap_tests binary (built with
 * VAIOS_TASK_HEAP=1, VAIOS_SYSCALL_SVC=0 so the per-task malloc/free/calloc/
 * realloc bodies in kernel/memory/memory.c run directly against current_task).
 *
 * The per-task allocator only needs current_task->{heap_base, heap_brk,
 * mem_block, stack_size}; it does NOT need the scheduler, so this binary links
 * memory.c without task.c (mirroring how devfs_stubs backs vaios_devfs_tests).
 * taskheap_set_task() points current_task at a caller-owned block, so an
 * allocator that runs off the end of the block (finding #9) writes past a real
 * heap object and AddressSanitizer catches it. */
#include "memory.h"
#include "task.h"
#include <stdint.h>

TCB *current_task = 0;

static TCB g_synth_task;

/* Point current_task at `block` (size bytes), with an empty heap that grows up
 * from the block base — matching task.c's per-task heap init (guard_off = 0 on
 * host, no MPU stack guard). Call before each test to reset the heap. */
void taskheap_set_task(void *block, uint32_t size) {
  g_synth_task.mem_block = (uint32_t *)block;
  g_synth_task.stack_size = size;
  g_synth_task.heap_base = (uint8_t *)block;
  g_synth_task.heap_brk = (uint8_t *)block;
  g_synth_task.heap_peak_brk = (uint8_t *)block;
  current_task = &g_synth_task;
}

void taskheap_clear_task(void) { current_task = 0; }

/* Two-task helpers for cross-task tests (finding #3): the caller owns the TCBs
 * and blocks, and switches which one is "running". */
void taskheap_init_task(TCB *t, void *block, uint32_t size) {
  t->mem_block = (uint32_t *)block;
  t->stack_size = size;
  t->heap_base = (uint8_t *)block;
  t->heap_brk = (uint8_t *)block;
  t->heap_peak_brk = (uint8_t *)block;
}

void taskheap_use(TCB *t) { current_task = t; }

/* memory.c's global allocator calls the perf accounting hooks (real home
 * kernel/perf.c, not linked here). No-ops for this binary. */
void v_perf_on_heap_alloc(uint32_t size, int split) {
  (void)size;
  (void)split;
}
void v_perf_on_heap_free(int coalesces) { (void)coalesces; }
void v_perf_on_heap_oom(void) {}
