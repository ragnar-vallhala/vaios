#ifndef VAIOS_SYSCALL_H
#define VAIOS_SYSCALL_H

/**
 * @file syscall.h
 * @brief SVC syscall ABI for kernel/user separation (Phase 3, Stage 1).
 *
 * A task enters the kernel with `svc 1`; the syscall NUMBER is passed in r12
 * (auto-stacked in the exception frame, and — unlike r7 — not the Thumb frame
 * pointer, so the ABI never depends on -fomit-frame-pointer), and arguments are
 * passed in r0-r3 per the C ABI (AAPCS), untouched. The result is returned in
 * r0 as a signed value: >= 0 on success (a value or fd), < 0 is a negated errno.
 *
 * `svc 0` is reserved for the first-task launch trampoline (scheduler_start);
 * SVCall_Handler dispatches only `svc 1` through the table in kernel/syscall.c.
 *
 * Stage 1 keeps tasks PRIVILEGED — this validates the mechanism with zero
 * isolation risk. The unprivileged flip and pointer validation land in later
 * stages. Gated by VAIOS_SYSCALL_SVC; with it off, the public wrappers below
 * call straight through to the kernel (no trap), so callers are unaffected.
 */

#include "vaios_config.h"
#include <stdint.h>

// Syscall numbers. Append only — these are an ABI. 0 is reserved (launch).
typedef enum {
  SYS_yield = 1,
  SYS_delay = 2,
  SYS_sem_give = 3,
  /* SYS_sem_take (blocking) is deferred: its result depends on post-wake state,
     which needs deferred-result delivery (the switch is deferred to handler
     return, so an inline resume-check would spuriously time out). */
  SYS_MAX
} v_syscall_t;

// C-side dispatch entry, called from SVCall_Handler with the stacked frame.
// `args` points at the task's stacked {r0,r1,r2,r3}; the return value is written
// back into args[0] by the handler.
int32_t v_syscall_dispatch(uint32_t num, uint32_t *args);

#if VAIOS_SYSCALL_SVC

// True in thread mode (a task), false in a handler (the syscall dispatch). A
// task-facing API whose implementation IS the dispatch target uses this to trap
// exactly once: a task (thread) traps via svc; the same function reached from
// the dispatch (handler) runs its body. (task_yield needs no such guard — its
// implementation is the separate v_port_trigger_pendsv.)
__attribute__((always_inline)) static inline int v_in_thread_mode(void) {
  uint32_t ipsr;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  return ipsr == 0u;
}

// Thin `svc 1` trampolines: number in r12, args in r0-r3, result in r0.
__attribute__((always_inline)) static inline int32_t v_svc0(uint32_t n) {
  register uint32_t r12 __asm__("r12") = n;
  register int32_t ret __asm__("r0");
  __asm__ volatile("svc 1" : "=r"(ret) : "r"(r12) : "memory");
  return ret;
}
__attribute__((always_inline)) static inline int32_t v_svc1(uint32_t n,
                                                            uint32_t a0) {
  register uint32_t r12 __asm__("r12") = n;
  register uint32_t r0 __asm__("r0") = a0;
  __asm__ volatile("svc 1" : "+r"(r0) : "r"(r12) : "memory");
  return (int32_t)r0;
}

#endif // VAIOS_SYSCALL_SVC

#endif // !VAIOS_SYSCALL_H
