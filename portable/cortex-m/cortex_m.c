
/* cortex_context_switch.c
 *
 * Cortex-M4 context switch primitives and PendSV-driven scheduler switch.
 *
 * Requirements: TCB layout matches this code (stack_ptr at offset 12).
 */

#include "config.h"
#include "structures.h" // TCB definition
#include "task.h"       // prototypes for task helpers (task_get_current, task_set_current, task_get_next_ready, task_remove, task_insert_sorted)
#include "utils.h"      // v_log
#include <stdint.h>
/* externs from your system */
extern TCB *ready_queue;
// extern TCB *blocked_queue;
// extern TCB *sleep_queue;
extern TCB *current_task;

/* PendSV should call _context_switch; you may keep a small PendSV handler that
 * invokes it. */
void _context_switch(void);

extern Scheduler_Status_Type scheduler_state;

void _context_switch(void);

void PendSV_Handler(void)
{
  if (scheduler_state == SCHEDULER_RUNNING)
  {
    // v_log(LOG_DEBUG, "PENDSV Triggered");
    _context_switch();
  }
}
// Cortex-M registers stacked automatically on exception
typedef struct
{
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xPSR;
} HardFault_StackFrame;

__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile("tst lr, #4                 \n" // Check which stack pointer
                                                 // was used (MSP or PSP)
                 "ite eq                     \n"
                 "mrseq r0, msp              \n"
                 "mrsne r0, psp              \n"
                 "b HardFault_HandlerC       \n");
}

void HardFault_HandlerC(HardFault_StackFrame *frame)
{
  // You can log the stacked registers
  v_log(LOG_FATAL, "[HardFault] PC=0x%x LR=0x%x xPSR=0x%x", frame->pc,
        frame->lr, frame->xPSR);

  v_log(LOG_FATAL, "[HardFault] R0=0x%x R1=0x%x R2=0x%x R3=0x%x R12=0x%x",
        frame->r0, frame->r1, frame->r2, frame->r3, frame->r12);

  // Optionally, halt here
  while (1)
  {
  }
}
static inline void set_basepri(uint32_t v)
{
  __asm volatile("msr basepri, %0\ndsb\nisb\n" : : "r"(v) : "memory");
}
static inline uint32_t get_basepri(void)
{
  uint32_t v;
  __asm volatile("mrs %0, basepri" : "=r"(v));
  return v;
}
/* ------------------------------------------------------------------
   Naked assembly helpers (PSP-based). These functions are "naked"
   so the compiler does not emit prologue/epilogue which would
   corrupt the special stack operations we perform.
   ------------------------------------------------------------------ */

/* Save r4-r11 onto PSP and store updated PSP in current_task->stack_ptr.
   NOTE: This implementation assumes `current_task` symbol is available
   and that stack_ptr is at offset 12 in TCB (as in your struct). */

__attribute__((naked)) void save_context(void)
{
  __asm volatile(
      /* r0 := PSP */
      "mrs   r0, psp             \n"

      /* "tst r14, #0x10            \n" /1* Is the task using the FPU */
      /*                                          context?  If so, push high */
      /*                                          vfp registers. *1/ */
      /* "it eq                     \n" */
      /* "vstmdbeq r0!, {s16-s31}   \n" */

      /* push r4-r11 onto process stack, update r0 */
      "stmdb r0!, {r4-r11}   \n"
      /* r1 := &current_task ; r1 := current_task */
      "ldr   r1, =current_task   \n"
      "ldr   r1, [r1]            \n"
      /* store updated PSP into current_task->stack_ptr (offset 0) */
      "str   r0, [r1]       \n"
      /* return */
      "bx    lr                  \n");
}

/* Restore r4-r11 from current_task->stack_ptr, set PSP and perform EXC_RETURN.
   Uses EXC_RETURN = 0xFFFFFFFD (return to Thread mode, use PSP). */
__attribute__((naked)) void restore_context_and_return(void)
{
  __asm volatile(
      /* r1 := &current_task ; r1 := current_task */
      "ldr r1, =current_task \n"
      "ldr r2, [r1] \n"
      "ldr r0, [r2] \n"

      "ldmia r0!, {r4-r11} \n"

      /* "tst r14, #0x10         \n" /1* Is the task using the FPU */
      /*                                context?  If so, pop the */
      /*                                high vfp registers too. *1/ */
      /* "it eq                  \n" */
      /* "vldmiaeq r0!, {s16-s31}\n" */

      "msr psp, r0 \n"
      "mov lr, #0xFFFFFFFD \n" // EXC_RETURN value
      "bx lr \n");
}

/* ------------------------------------------------------------------
   _context_switch
   High-level switch logic called from PendSV (C code). This:
   - obtains curr and next
   - if curr exists, saves its context and re-enqueues based on its state
   - removes next from ready queue and sets it running/current
   - calls restore_context_and_return() to resume next
   ------------------------------------------------------------------ */
void _context_switch(void)
{
  /* NOTE: This should be called with interrupts configured so that
     PendSV runs safely as the scheduler context switch (PendSV lowest
     priority). */

  TCB *curr = task_get_current();
  TCB *next = task_dequeue(&ready_queue);
  /* If nothing to run -> return */
  if (next == NULL)
  {
    return;
  }

  /* If the same task was chosen -> nothing to do */
  if (curr == next)
  {
    return;
  }

  /* If there is a current task, save its context and re-enqueue it
  appropriately. save_context() will store the updated PSP into
  curr->stack_ptr. */
  set_basepri(MAX_SYSCALL_INTERRUPT_PRIORITY);
  if (curr)
  {
    save_context();

    /* If task was running, mark it READY for re-insertion */
    if (curr->state == TASK_RUNNING)
    {
      curr->state = TASK_READY;
    }

    /* Re-insert depending on its state (if it wasn't removed/changed elsewhere)
     */
    switch (curr->state)
    {
    case TASK_READY:
      /* Insert by priority so scheduler policy remains intact */
      task_insert_sorted(&ready_queue, curr);
      break;
    // case TASK_BLOCKED:
    //   task_insert_sorted(&blocked_queue, curr);
    //   break;
    // case TASK_SLEEPING:
    //   task_insert_sorted(&sleep_queue, curr);
    //   break;
    default:
      /* TASK_RUNNING should not remain here; other states ignored */
      break;
    }
  }

  // /* Remove next from ready queue so it does not remain listed while running */
  task_remove(&ready_queue, next);
  v_log(LOG_DEBUG, "Context switch: curr=0x%x next=0x%x", ready_queue, next);
  // /* Set next as running and current */
  next->state = TASK_RUNNING;
  task_set_current(next);
  set_basepri(0);
  // /* Restore next task's context and return into it (never returns here) */
  // restore_context_and_return();

  __asm__ volatile( // EXC_RETURN value
      "bx lr \n");

  // /* Should never reach here */
  // for (;;)
  // {
  //   __asm volatile("wfi");
  // }
}
