/**
 * @file syscall.c
 * @brief SVC syscall dispatch (Phase 3, Stage 1).
 *
 * SVCall_Handler decodes the exception frame and calls v_syscall_dispatch with
 * the syscall number (stacked r12) and a pointer to the stacked {r0..r3}. The
 * return value is written back into the stacked r0 by the handler. Stage 1
 * keeps tasks privileged, so these handlers simply call the existing kernel
 * primitives; pointer validation and the unprivileged flip come later.
 */

#include "syscall.h"

#if VAIOS_SYSCALL_SVC

#include "ipc.h"
#include "memory.h"
#include "port.h"
#include "task.h"
#include "vfile.h"
#include <stdint.h>

#if VAIOS_MPU_USER_SEPARATION
// Caller's CONTROL.nPRIV (bit 0). Exception entry does not modify
// CONTROL.nPRIV, so inside the SVCall handler this still reflects the trapping
// thread's privilege. Handler mode reads it fine (SPSEL is what's forced there,
// not nPRIV).
static inline int v_caller_unprivileged(void) {
  uint32_t control;
  __asm__ volatile("mrs %0, control" : "=r"(control));
  return (control & 1u) != 0u;
}
// The raw-handle (non-fd) IPC syscalls dereference a user-supplied kernel
// object pointer (args[0] -> sema_t*/rmutex_t*), which can't be bounds-checked.
// Under the unprivileged flip they are privileged-only; unprivileged tasks use
// the fd-typed IPC (SYS_sem_open ... SYS_wait). Pure predicate — host-testable.
static inline int v_syscall_privileged_only(uint32_t num) {
  return num == SYS_sem_give || num == SYS_sem_take || num == SYS_mutex_lock ||
         num == SYS_mutex_unlock;
}
#endif

int32_t v_syscall_dispatch(uint32_t num, uint32_t *args) {
#if VAIOS_MPU_USER_SEPARATION
  // Everything below applies only to UNPRIVILEGED callers (user tasks). The
  // kernel runs syscall bodies directly, never via svc; privileged tasks (flag
  // off, or a privileged task) are trusted. Bound the string scans generously —
  // the block bound in v_strnlen_user is the real limit.
#define V_SYSCALL_STR_MAX 256u
#define V_EPERM (-1)
#define V_EFAULT (-14)
  if (v_caller_unprivileged()) {
    // I-handle: raw kernel-pointer IPC is privileged-only; use fd-typed IPC.
    if (v_syscall_privileged_only(num))
      return V_EPERM;
    // I-ptr: validate any user pointer against the caller's own block.
    switch (num) {
    case SYS_open:
    case SYS_sem_open:
    case SYS_mtx_open:
      if (v_strnlen_user((const char *)(uintptr_t)args[0], V_SYSCALL_STR_MAX) <
          0)
        return V_EFAULT;
      break;
    case SYS_write:
      if (!v_access_ok((const void *)(uintptr_t)args[1], args[2], 0))
        return V_EFAULT;
      break;
    case SYS_read:
      if (!v_access_ok((void *)(uintptr_t)args[1], args[2], 1))
        return V_EFAULT;
      break;
    case SYS_wait:
      if (!v_access_ok((const void *)(uintptr_t)args[0],
                       args[1] * (uint32_t)sizeof(int), 0))
        return V_EFAULT;
      break;
    default:
      break;
    }
  }
#endif
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
    return v_file_write((int)args[0], (const void *)(uintptr_t)args[1],
                        args[2]);
  case SYS_read:
    return v_file_read((int)args[0], (void *)(uintptr_t)args[1], args[2]);
  case SYS_close:
    return v_file_close((int)args[0]);
#endif
#if VAIOS_IPC_FD
  case SYS_sem_open:
    /* find-or-create a named sem, allocate an fd in the caller's table.
       args[0] = name, args[1] = flags. */
    return v_sem_open((const char *)(uintptr_t)args[0], (int)args[1]);
  case SYS_sem_take_fd:
    /* Blocking, deferred-result (like SYS_sem_take). args[0] = fd,
       args[1] = ticks. */
    return v_sem_take((int)args[0], args[1]);
  case SYS_sem_give_fd:
    /* Non-blocking. args[0] = fd. */
    return v_sem_give((int)args[0]);
  case SYS_mtx_open:
    /* find-or-create a named mutex, allocate an fd. args[0]=name,
     * args[1]=flags. */
    return v_mtx_open((const char *)(uintptr_t)args[0], (int)args[1]);
  case SYS_mtx_lock_fd:
    /* Blocking, deferred-result (like SYS_mutex_lock). args[0]=fd,
     * args[1]=ticks. */
    return v_mtx_lock((int)args[0], args[1]);
  case SYS_mtx_unlock_fd:
    /* Non-blocking direct handoff. args[0]=fd. */
    return v_mtx_unlock((int)args[0]);
  case SYS_sem_poll:
    /* Non-consuming readiness probe. args[0]=fd. */
    return v_sem_poll((int)args[0]);
  case SYS_wait:
    /* Multi-fd wait: ready index, or V_SYSCALL_BLOCKED after arming + parking.
       args[0]=fds, args[1]=nfds, args[2]=ticks. */
    return v_wait_block_impl((const int *)(uintptr_t)args[0], (int)args[1],
                             args[2]);
  case SYS_wait_disarm:
    /* Unlink this task's wait observers and report the ready index. */
    return v_wait_disarm_impl();
#endif
#if VAIOS_TASK_HEAP && VAIOS_MPU_USER_SEPARATION
  /* Per-task heap. Reached in handler mode, so the allocator bodies run
     privileged (their thread-mode trap guard falls through) and read
     current_task / the task block directly. Pointers round-trip through r0. */
  case SYS_malloc:
    return (int32_t)(uintptr_t)malloc((size_t)args[0]);
  case SYS_free:
    free((void *)(uintptr_t)args[0]);
    return 0;
  case SYS_calloc:
    return (int32_t)(uintptr_t)calloc((size_t)args[0], (size_t)args[1]);
  case SYS_realloc:
    return (int32_t)(uintptr_t)realloc((void *)(uintptr_t)args[0],
                                       (size_t)args[1]);
  case SYS_heap_used:
    return (int32_t)v_task_heap_used();
#endif
#if VAIOS_MPU_USER_SEPARATION
  case SYS_exit:
    /* Terminate the calling task + pend the switch, privileged. Returns here;
       PendSV switches to the next task on SVCall return, and the terminated
       task never resumes. */
    v_task_exit_impl();
    return 0;
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
