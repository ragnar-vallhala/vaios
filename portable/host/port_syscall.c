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

void v_host_sched_after_svc(void); // port.c: tail-chain a pended switch

int v_host_in_dispatch = 0;

intptr_t v_host_svc(uint32_t n, uintptr_t a0, uintptr_t a1, uintptr_t a2) {
  uintptr_t args[4] = {a0, a1, a2, 0};
  int prev = v_host_in_dispatch;
  v_host_in_dispatch = 1; // the dispatch runs in "handler mode"
  intptr_t r = v_syscall_dispatch(n, args);
  v_host_in_dispatch = prev;
  // Reproduce the ARM SVC->PendSV tail-chain: if the dispatch pended a switch
  // (e.g. task_delay / sem take blocked us), take it NOW, before returning to
  // the task. Otherwise the task runs on while marked BLOCKED and can re-enqueue
  // itself on a wait list. Only the outermost (thread-mode) SVC switches; a
  // nested dispatch leaves it for its own caller.
  if (!prev)
    v_host_sched_after_svc();
  return r;
}
