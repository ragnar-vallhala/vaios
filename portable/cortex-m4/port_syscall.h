#ifndef VAIOS_CORTEX_M4_PORT_SYSCALL_H
#define VAIOS_CORTEX_M4_PORT_SYSCALL_H

/*
 * Cortex-M4 SVC trap ABI. Included from include/syscall.h only when
 * VAIOS_SYSCALL_SVC is on; keeps the ARMv7-M `svc`/`mrs` inline asm and the
 * r12/r0-r3 register pinning in the port instead of the portable kernel header.
 * A new port supplies its own port_syscall.h (the host build shadows this with
 * tests/stubs/port_syscall.h); include/syscall.h owns the arch-neutral syscall
 * numbers and dispatch prototypes.
 */

#include <stdint.h>

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

// Thin `svc 1` trampolines: number in r12, args in r0-r3, result in r0. Args and
// result are pointer-width; on ARMv7-M uintptr_t/intptr_t are 32-bit, so the
// registers and the ABI are byte-identical to a uint32_t ABI.
__attribute__((always_inline)) static inline intptr_t v_svc0(uint32_t n) {
  register uint32_t r12 __asm__("r12") = n;
  register intptr_t ret __asm__("r0");
  __asm__ volatile("svc 1" : "=r"(ret) : "r"(r12) : "memory");
  return ret;
}
__attribute__((always_inline)) static inline intptr_t v_svc1(uint32_t n,
                                                             uintptr_t a0) {
  register uint32_t r12 __asm__("r12") = n;
  register uintptr_t r0 __asm__("r0") = a0;
  __asm__ volatile("svc 1" : "+r"(r0) : "r"(r12) : "memory");
  return (intptr_t)r0;
}
__attribute__((always_inline)) static inline intptr_t
v_svc2(uint32_t n, uintptr_t a0, uintptr_t a1) {
  register uint32_t r12 __asm__("r12") = n;
  register uintptr_t r0 __asm__("r0") = a0;
  register uintptr_t r1 __asm__("r1") = a1;
  __asm__ volatile("svc 1" : "+r"(r0) : "r"(r12), "r"(r1) : "memory");
  return (intptr_t)r0;
}
__attribute__((always_inline)) static inline intptr_t
v_svc3(uint32_t n, uintptr_t a0, uintptr_t a1, uintptr_t a2) {
  register uint32_t r12 __asm__("r12") = n;
  register uintptr_t r0 __asm__("r0") = a0;
  register uintptr_t r1 __asm__("r1") = a1;
  register uintptr_t r2 __asm__("r2") = a2;
  __asm__ volatile("svc 1" : "+r"(r0) : "r"(r12), "r"(r1), "r"(r2) : "memory");
  return (intptr_t)r0;
}

#endif /* VAIOS_CORTEX_M4_PORT_SYSCALL_H */
