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
#include "vfile.h"
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
  case SYS_sem_take:
    /* Blocking: the body runs in handler mode; if it blocks it returns
       V_SYSCALL_BLOCKED and the real result (VA_PASS/VA_FAIL) is delivered on
       resume. args[0] = handle, args[1] = ticks. */
    return v_semaphore_take((SemaphoreHandle_t)(uintptr_t)args[0], args[1]);
  case SYS_mutex_lock:
    /* Blocking, deferred-result (like sem_take). Ownership is handed over by
       the unlocker; the lock body only sets it for an uncontended acquire.
       args[0] = mutex handle, args[1] = ticks. */
    return v_mutex_lock((MutexHandle_t)(uintptr_t)args[0], args[1]);
  case SYS_mutex_unlock:
    /* Non-blocking: direct handoff to the highest-priority waiter. args[0] =
       mutex handle. */
    return v_mutex_unlock((MutexHandle_t)(uintptr_t)args[0]);
#if VAIOS_DEVFS
  case SYS_open:
    return v_file_open((const char *)(uintptr_t)args[0], (int)args[1]);
  case SYS_write:
    return v_file_write((int)args[0], (const void *)(uintptr_t)args[1], args[2]);
  case SYS_read:
    return v_file_read((int)args[0], (void *)(uintptr_t)args[1], args[2]);
  case SYS_close:
    return v_file_close((int)args[0]);
#endif
  default:
    return -1; /* unknown syscall */
  }
}

/* --- Deferred blocking-syscall result delivery --------------------------- */
extern TCB *current_task;

void v_syscall_wake_result(TCB *t, int32_t result) {
  t->syscall_result = result;
  t->has_syscall_result = 1;
}

/* Called from PendSV after set_next_task, before the register restore. If the
   incoming task has a pending blocking-syscall result, write it into its
   hardware-stacked r0 (the syscall's return value). Layout from task->sp:
   [r4-r11, r14=EXC_RETURN] (9 words), [s16-s31 if FPU], then the HW frame
   {r0,r1,r2,r3,r12,lr,pc,xpsr}. */
void v_syscall_deliver_result(void) {
  TCB *t = current_task;
  if (!t || !t->has_syscall_result)
    return;
  uint32_t *sp = t->sp;
  uint32_t off = 9u; /* r4-r11, r14 */
#ifdef _FPU_ENABLED
  if (!(sp[8] & 0x10u)) /* EXC_RETURN bit4 == 0: FPU context was stacked */
    off += 16u;
#endif
  sp[off] = (uint32_t)t->syscall_result; /* hardware-frame r0 */
  t->has_syscall_result = 0;
}

#endif /* VAIOS_SYSCALL_SVC */
