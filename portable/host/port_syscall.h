#ifndef VAIOS_HOST_PORT_SYSCALL_H
#define VAIOS_HOST_PORT_SYSCALL_H

/*
 * Host SVC trap ABI. Included from include/syscall.h when VAIOS_SYSCALL_SVC is
 * on. There is no privilege trap on the host, so v_svc* call the kernel dispatch
 * (v_syscall_dispatch) directly through a shim that flips an "in dispatch" flag,
 * so a self-implementing API (task_exit) still sees handler mode and runs its
 * body exactly once — same model as the ARM svc path.
 *
 * Args and result are pointer-width (the widened syscall ABI), so pointer
 * arguments round-trip on a 64-bit host without the MAP_32BIT low-heap hack the
 * unit-test stub used to need.
 */

#include <stdint.h>

extern int v_host_in_dispatch; // 0 = thread mode (a task), nonzero = in dispatch
intptr_t v_host_svc(uint32_t n, uintptr_t a0, uintptr_t a1, uintptr_t a2);

static inline int v_in_thread_mode(void) { return v_host_in_dispatch == 0; }
static inline intptr_t v_svc0(uint32_t n) { return v_host_svc(n, 0, 0, 0); }
static inline intptr_t v_svc1(uint32_t n, uintptr_t a0) {
  return v_host_svc(n, a0, 0, 0);
}
static inline intptr_t v_svc2(uint32_t n, uintptr_t a0, uintptr_t a1) {
  return v_host_svc(n, a0, a1, 0);
}
static inline intptr_t v_svc3(uint32_t n, uintptr_t a0, uintptr_t a1,
                              uintptr_t a2) {
  return v_host_svc(n, a0, a1, a2);
}

#endif // VAIOS_HOST_PORT_SYSCALL_H
