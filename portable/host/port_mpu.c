#define _GNU_SOURCE
/**
 * @file port_mpu.c
 * @brief Host software MPU — models the Cortex-M MPU behind the v_port_mpu_*
 *        facade using POSIX memory protection, so the kernel's isolation code
 *        runs authentically without a full CPU emulator.
 *
 * The MPU's region access control is mprotect(); a violating access raises
 * SIGSEGV, which this layer translates into the same fault path as the ARM
 * MemManage_Handler (v_panic with the offending task + address). This slice
 * implements the per-task stack-overflow guard: init_task_stack (port.c) places
 * a PROT_NONE guard page just below each task's stack, and an overflow into it
 * faults cleanly instead of silently corrupting memory. Per-task RW regions
 * (user separation) layer on next. See docs/plan/HOST_PORT_PLAN.md.
 */

#include "port.h"
#include "task.h" // TCB, current_task
#include "utils.h" // v_panic
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h> // mprotect
#include <ucontext.h>
#include <unistd.h> // sysconf

extern TCB *current_task;

#if VAIOS_MPU_USER_SEPARATION
// Per-task region enforcement: a task's stack is readable/writable only while it
// runs. On a context switch the port opens the incoming task's stack and closes
// the outgoing one (PROT_NONE), so a running task that reaches into another
// task's memory faults — the MPU per-task RW region, modelled with mprotect.
// (This isolates tasks from EACH OTHER; isolating a task from KERNEL memory needs
// a privilege level the host lacks — deferred. See HOST_PORT_PLAN.md.)
static void host_region_protect(TCB *t, int prot) {
  if (!t || !t->sp)
    return;
  ucontext_t *uc = (ucontext_t *)t->sp;
  mprotect(uc->uc_stack.ss_sp, uc->uc_stack.ss_size, prot);
}
void v_host_region_open(TCB *t) { host_region_protect(t, PROT_READ | PROT_WRITE); }
void v_host_region_close(TCB *t) { host_region_protect(t, PROT_NONE); }
#endif

#if VAIOS_MPU_ENABLE

// True if `addr` is in the current task's no-access stack guard page — i.e. the
// task overflowed its stack. The guard sits one page below the ucontext stack
// base (see init_task_stack), so it is derivable from the task itself; no
// separate registry is needed.
static int addr_in_current_guard(const void *addr) {
  if (!current_task || !current_task->sp)
    return 0;
  ucontext_t *uc = (ucontext_t *)current_task->sp;
  char *ss = (char *)uc->uc_stack.ss_sp;
  long pg = sysconf(_SC_PAGESIZE);
  const char *a = (const char *)addr;
  return a >= ss - pg && a < ss;
}

// SIGSEGV == the MemManage fault. Runs on the alternate signal stack so it works
// even when the task stack is exhausted. Terminal, like the ARM handler
// (recovery / task-kill is a later phase).
static void mpu_fault_handler(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)uc;
  uint32_t id = current_task ? current_task->task_id : 0u;
  const char *what =
      addr_in_current_guard(si->si_addr) ? "stack overflow" : "memory access";
  // The vaios logger formatter has no %p; print the low 32 bits like the ARM
  // MemManage handler does for MMFAR.
  v_panic(__FILE__, __LINE__, "MPU fault in task %u | %s at 0x%x", (unsigned)id,
          what, (unsigned)(uintptr_t)si->si_addr);
}

void v_port_mpu_init(void) {
  // Alternate stack for the fault handler (the task's own stack may be the thing
  // that overflowed).
  static char altstack[64 * 1024];
  stack_t ss = {
      .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0};
  sigaltstack(&ss, NULL);

  struct sigaction sa;
  sa.sa_sigaction = mpu_fault_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, NULL);
}

#else
void v_port_mpu_init(void) {}
#endif // VAIOS_MPU_ENABLE

// --- Region encode/apply facade ---------------------------------------------
// The stack guard is a real mprotect'd page set up once in init_task_stack, so
// there is nothing to encode or re-apply per context switch — report available.
int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  out[0] = out[1] = 0;
  return 0;
}
#if VAIOS_MPU_USER_SEPARATION
// Per-task RW regions ARE enforced on host, but by the context switch toggling
// mprotect on the incoming/outgoing stacks (v_host_region_open/close above), not
// by an encoded region the kernel re-applies each switch. So the encode path
// reports unavailable and the kernel skips its apply loop — the mechanism lives
// in the switch, where the host has the running task in hand.
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  (void)out;
  return -1;
}
#endif
void v_port_mpu_apply(const uint32_t enc[2], uint32_t count) {
  (void)enc;
  (void)count;
}
