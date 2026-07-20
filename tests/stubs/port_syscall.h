#ifndef VAIOS_HOST_PORT_SYSCALL_H
#define VAIOS_HOST_PORT_SYSCALL_H

/*
 * Host model of the SVC trap ABI — shadows portable/cortex-m4/port_syscall.h in
 * the test build (portable/cortex-m4 is not on the host include path, so
 * include/syscall.h's `#include "port_syscall.h"` resolves here). No ARM
 * svc/mrs: v_svc* funnel through v_host_svc, which flips v_test_in_handler
 * around v_syscall_dispatch so the dispatch-reached primitive bodies see
 * handler mode and run directly — exactly as on target. Tests drive the public
 * API and it routes through the dispatch. Both live in
 * tests/stubs/syscall_stubs.c.
 */

#include <stdint.h>

extern int v_test_in_handler; // 0 = thread mode (a task), nonzero = in dispatch
int32_t v_host_svc(uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2);
static inline int v_in_thread_mode(void) { return v_test_in_handler == 0; }
static inline int32_t v_svc0(uint32_t n) { return v_host_svc(n, 0, 0, 0); }
static inline int32_t v_svc1(uint32_t n, uint32_t a0) {
  return v_host_svc(n, a0, 0, 0);
}
static inline int32_t v_svc2(uint32_t n, uint32_t a0, uint32_t a1) {
  return v_host_svc(n, a0, a1, 0);
}
static inline int32_t v_svc3(uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2) {
  return v_host_svc(n, a0, a1, a2);
}

#endif /* VAIOS_HOST_PORT_SYSCALL_H */
