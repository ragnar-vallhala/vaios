
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

/* ---------------- Basepri helpers ---------------- */
static inline void set_basepri(uint32_t v)
{
  __asm volatile("msr basepri, %0\n dsb\n isb\n" : : "r"(v) : "memory");
}

static inline uint32_t get_basepri(void)
{
  uint32_t v;
  __asm volatile("mrs %0, basepri" : "=r"(v));
  return v;
}

/* ---------------- Naked assembly helpers ---------------- */

/* Save r4-r11 on PSP, update current_task->stack_ptr */
__attribute__((naked)) void save_context(void)
{
  __asm volatile(
      "mrs r0, psp            \n" // Get process stack pointer
      "stmdb r0!, {r4-r11}    \n" // Save callee-saved registers

      "ldr r1, =current_task   \n"
      "ldr r2, [r1]            \n"
      "str r0, [r2]            \n" // Save updated PSP to TCB

      "bx lr                   \n");
}

/* Restore r4-r11 from current_task->stack_ptr, set PSP, return to thread mode */
__attribute__((naked)) void restore_context_and_return(void)
{
  __asm volatile(
      "ldr r1, =current_task   \n"
      "ldr r1, [r1]            \n"
      "ldr r0, [r1]            \n"

      "ldmia r0!, {r4-r11}     \n" // Restore callee-saved registers
      "msr psp, r0             \n"
      "mov lr, #0xFFFFFFFD     \n" // Return to Thread mode using PSP
      "bx lr                   \n");
}

/* ---------------- Context switch logic (C) ---------------- */
/* ---------------- Context switch logic (C) ---------------- */

void _context_switch(void)
{
  TCB *curr = task_get_current();
  TCB *next = task_get_next_ready();

  if (curr)
  {
    if (curr->state == TASK_RUNNING)
      curr->state = TASK_READY;
    if (curr->state == TASK_READY)
      task_insert_sorted(&ready_queue, curr);
  }

  if (next)
  {
    task_remove(&ready_queue, next);
    next->state = TASK_RUNNING;
    task_set_current(next);
  }

  v_log(LOG_DEBUG, "Context switch: curr=0x%x next=0x%x", curr, next);
}

void start_scheduler(void)
{
  current_task = task_get_next_ready();
  if (!current_task)
    return;
  task_remove(&ready_queue, current_task);
  current_task->state = TASK_RUNNING;
  task_set_current(current_task);
  uint32_t *stack_top = current_task->stack_ptr;
  // for (int i = 0; i < 16; i++)
  // {
  //   v_log(LOG_DEBUG, "Stack[%d] = 0x%x", i, *stack_top++);
  // }

  __asm volatile(
      "ldr r0, =current_task\n"
      "ldr r1, [r0]\n"             // current_task pointer
      "ldr r0, [r1]\n"             // SP of first task
      "ldmia r0!, {r4-r11}     \n" // Restore callee-saved registers
      "msr psp, r0             \n"
      "isb\n"
      "mrs r0, control   \n"
      "orr r0, r0, #2    \n" // set bit 1 = use PSP
      "msr control, r0   \n"
      "isb               \n" // flush pipeline
                             // "movs r0, #2\n"
                             // "msr control, r0\n" // Switch to PSP, unprivileged

      // Return using exception return, triggers hardware to pop xPSR, PC, LR, R0-R3, r12
      // "mov lr, #0xFFFFFFFD\n" // EXC_RETURN value: return to Thread mode, PSP
      // "bx lr\n"
  );
}

/* This handler is NOT naked, but only calls save/restore naked functions */
void PendSV_Handler(void)
{
  if (scheduler_state != SCHEDULER_RUNNING)
    return;

  v_log(LOG_DEBUG, "PendSV triggered");

  TCB *curr = task_get_current();
  if (curr)
  {
    save_context(); // Assembly: push r4-r11 to PSP, save PSP in TCB
  }
  else
  {

    return;
  }

  set_basepri(MAX_SYSCALL_INTERRUPT_PRIORITY);
  _context_switch(); // Pure C logic: choose next task
  set_basepri(0);

  restore_context_and_return(); // Assembly: restore r4-r11, set PSP, return
}

//// ===================== HardFault Handler =====================
// Define a structure to represent the stack frame
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
