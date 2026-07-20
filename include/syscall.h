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
  SYS_sem_take = 4,
  SYS_mutex_lock = 5,
  SYS_mutex_unlock = 6,
  SYS_open = 7,
  SYS_write = 8,
  SYS_read = 9,
  SYS_close = 10,
  SYS_sem_open = 11,     // fd-typed IPC (VAIOS_IPC_FD)
  SYS_sem_take_fd = 12,
  SYS_sem_give_fd = 13,
  SYS_mtx_open = 14,
  SYS_mtx_lock_fd = 15,
  SYS_mtx_unlock_fd = 16,
  SYS_sem_poll = 17,
  SYS_wait = 18,        // multi-fd wait: arm observers + block
  SYS_wait_disarm = 19, // unlink observers + report ready index
  SYS_malloc = 20,      // per-task heap (VAIOS_TASK_HEAP + VAIOS_MPU_USER_SEPARATION):
  SYS_free = 21,        //   routed through SVC so the allocator runs privileged
  SYS_calloc = 22,      //   (reads current_task / the task block) once tasks are
  SYS_realloc = 23,     //   unprivileged and can no longer reach them directly.
  SYS_heap_used = 24,
  SYS_exit = 25,        // task_exit body (VAIOS_MPU_USER_SEPARATION): terminate
                        //   the caller + reschedule, run privileged.
  SYS_MAX
} v_syscall_t;

// C-side dispatch entry, called from SVCall_Handler with the stacked frame.
// `args` points at the task's stacked {r0,r1,r2,r3}; the return value is written
// back into args[0] by the handler.
int32_t v_syscall_dispatch(uint32_t num, uint32_t *args);

// --- Deferred-result blocking ------------------------------------------------
// A blocking syscall (sem_take, mutex_lock) can't return its result inline: the
// context switch is deferred to SVCall return, so the outcome (granted vs
// timeout) isn't known when the handler runs. The blocking primitive returns
// V_SYSCALL_BLOCKED (a placeholder, overwritten later); the waker stashes the
// real result in the TCB via v_syscall_wake_result(); and PendSV writes it into
// the resumed task's stacked r0 via v_syscall_deliver_result() at schedule-in
// (sp is stable then, so there is no race with the window between blocking and
// switching out).
#define V_SYSCALL_BLOCKED 0x7FFF0000 /* placeholder r0; replaced on resume */

struct Task_Control_Block;
// Waker: stash `result` to hand to blocked task `t` when it next runs.
void v_syscall_wake_result(struct Task_Control_Block *t, int32_t result);
// PendSV hook: deliver any pending result into the incoming task's stacked r0.
void v_syscall_deliver_result(void);

#if VAIOS_SYSCALL_SVC
// SVC trap ABI (svc/mrs + register pinning on ARM; a host model in the test
// build). Provided by the port so the arch inline asm lives in
// portable/<arch>/port_syscall.h, not in this portable header. v_in_thread_mode
// and v_svc0..v_svc3 come from there.
#include "port_syscall.h"
#endif // VAIOS_SYSCALL_SVC

#endif // !VAIOS_SYSCALL_H
