/* Host backing for the vaios_syscall_tests binary — puts v_syscall_dispatch
 * (portable/cortex-m4/syscall.c) under host test with the real pointer
 * validators (kernel/task.c v_access_ok / v_strnlen_user).
 *
 * Two seams the ARM build implements in asm, provided here for the host:
 *   - v_caller_unprivileged() reads a test-set flag instead of CONTROL.nPRIV
 *     (the #if VAIOS_HOST_TEST branch in syscall.c reads v_test_caller_unprivileged).
 *   - v_svc*() funnel through v_host_svc, which flips v_test_in_handler around
 *     the dispatch so a primitive body reached from the dispatch sees handler
 *     mode (runs directly), exactly as on target (the #if VAIOS_HOST_TEST branch
 *     in include/syscall.h).
 */
#define _GNU_SOURCE /* MAP_32BIT / MAP_ANONYMOUS */
#include "syscall.h"
#include "task.h"
#include <stdint.h>
#include <sys/mman.h>

/* Simulated caller privilege for the validation switch. 1 = unprivileged. */
int v_test_caller_unprivileged = 0;

/* 0 = thread mode (a task), nonzero = inside the dispatch (handler mode). */
int v_test_in_handler = 0;

intptr_t v_host_svc(uint32_t n, uintptr_t a0, uintptr_t a1, uintptr_t a2) {
  uintptr_t args[4] = {a0, a1, a2, 0};
  int prev = v_test_in_handler;
  v_test_in_handler = 1; /* dispatch runs in "handler mode" */
  intptr_t r = v_syscall_dispatch(n, args);
  v_test_in_handler = prev;
  return r;
}

/* current_task is defined in kernel/task.c (linked). Point it at a synthetic
 * caller whose block bounds v_access_ok / v_strnlen_user.
 *
 * The syscall ABI carries pointers as uint32_t (32-bit target). On a 64-bit
 * host a normal BSS/heap block sits above 4 GB and would truncate passing
 * through args[], so the block is backed by a MAP_32BIT mapping — its address
 * fits in uint32_t and round-trips losslessly, exactly as on target. Returns
 * the block base (a valid in-block user pointer for positive tests). */
extern TCB *current_task;
static TCB g_caller;
static uint8_t *g_low = 0;
static uint32_t g_low_sz = 0;

uint32_t syscall_set_caller(uint32_t size, int unprivileged) {
  if (g_low == 0 || g_low_sz < size) {
    if (g_low)
      munmap(g_low, g_low_sz);
    g_low = (uint8_t *)mmap(0, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    g_low_sz = (g_low == MAP_FAILED) ? 0 : size;
    if (g_low == MAP_FAILED)
      g_low = 0;
  }
  g_caller.mem_block = (uint32_t *)g_low;
  g_caller.stack_size = size;
  current_task = &g_caller;
  v_test_caller_unprivileged = unprivileged;
  return (uint32_t)(uintptr_t)g_low; /* 0 if the low mapping failed */
}

/* Perf accounting hooks (real home kernel/perf.c, not linked here). The
 * scheduler / IPC / heap call them; no-ops for this binary. */
void v_perf_on_sched_switch(TCB *prev, TCB *next) {
  (void)prev;
  (void)next;
}
void v_perf_on_isr_systick_exit(uint32_t d, int p) {
  (void)d;
  (void)p;
}
void v_perf_on_ipc_take_attempt(void) {}
void v_perf_on_ipc_take_blocked(void) {}
void v_perf_on_ipc_give(void) {}
void v_perf_on_ipc_timeout(void) {}
void v_perf_on_heap_alloc(uint32_t size, int split) {
  (void)size;
  (void)split;
}
void v_perf_on_heap_free(int coalesces) { (void)coalesces; }
void v_perf_on_heap_oom(void) {}

/* /dev/kmsg backing read — devfs.c needs it; unused by these tests. */
int v_kmsg_read(char *out, uint32_t len) {
  (void)out;
  (void)len;
  return 0;
}
