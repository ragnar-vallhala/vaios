/**
 * @file syscall.c
 * @brief SVC syscall dispatch (Phase 3, Stage 1).
 *
 * SVCall_Handler decodes the exception frame and calls v_syscall_dispatch with
 * the syscall number (stacked r12) and a pointer to the stacked {r0..r3}. The
 * return value is written back into the stacked r0 by the handler. Stage 1 keeps
 * tasks privileged, so these handlers simply call the existing kernel
 * primitives; pointer validation and the unprivileged flip come later.
 */

#include "syscall.h"

#if VAIOS_SYSCALL_SVC

#include "ipc.h"
#include "port.h"
#include "task.h"
#include <stdint.h>

int32_t v_syscall_dispatch(uint32_t num, uint32_t *args) {
  switch (num) {
  case SYS_yield:
    /* Pend a context switch. PendSV is lower priority than SVCall, so the
       switch runs once this handler returns. */
    v_port_trigger_pendsv();
    return 0;
  case SYS_delay:
    /* task_delay marks the caller DELAYED and pends PendSV; the switch happens
       on SVCall return. Reached in handler mode, so its thread-mode trap guard
       runs the body. args[0] = ticks. */
    task_delay(args[0]);
    return 0;
  case SYS_sem_give:
    /* Non-blocking: signal + maybe wake a waiter (result known immediately).
       args[0] = semaphore handle. */
    return v_semaphore_give((SemaphoreHandle_t)(uintptr_t)args[0]);
  default:
    return -1; /* unknown syscall */
  }
}

#endif /* VAIOS_SYSCALL_SVC */
