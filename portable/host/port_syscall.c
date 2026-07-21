/**
 * @file port_syscall.c
 * @brief Host SVC shim — runs the kernel syscall dispatch directly (no trap).
 *
 * On the host there is no privilege boundary, so a "syscall" is just a direct
 * call into v_syscall_dispatch. v_host_svc flips v_host_in_dispatch around it so
 * v_in_thread_mode() reports handler mode during the dispatch — a self-
 * implementing API (e.g. task_exit) then runs its body once instead of trapping
 * again. See portable/host/port_syscall.h.
 */

#include "syscall.h" // v_syscall_dispatch
#include <stdint.h>

int v_host_in_dispatch = 0;

intptr_t v_host_svc(uint32_t n, uintptr_t a0, uintptr_t a1, uintptr_t a2) {
  uintptr_t args[4] = {a0, a1, a2, 0};
  int prev = v_host_in_dispatch;
  v_host_in_dispatch = 1; // the dispatch runs in "handler mode"
  intptr_t r = v_syscall_dispatch(n, args);
  v_host_in_dispatch = prev;
  return r;
}
