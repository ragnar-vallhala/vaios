#include "config.h"
#include "task.h"
#include "utils.h"
#include <stddef.h>
#include <stdint.h>

// Exception stack frame automatically pushed by Cortex-M on exception
typedef struct {
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
} ExceptionStackFrame;

void print_registers(ExceptionStackFrame *frame) {
  v_log(LOG_FATAL, "HardFault Register Dump:");
  v_log(LOG_FATAL, " R0  = 0x%x", frame->r0);
  v_log(LOG_FATAL, " R1  = 0x%x", frame->r1);
  v_log(LOG_FATAL, " R2  = 0x%x", frame->r2);
  v_log(LOG_FATAL, " R3  = 0x%x", frame->r3);
  v_log(LOG_FATAL, " R12 = 0x%x", frame->r12);
  v_log(LOG_FATAL, " LR  = 0x%x", frame->lr);
  v_log(LOG_FATAL, " PC  = 0x%x", frame->pc);
  v_log(LOG_FATAL, " xPSR= 0x%x", frame->xpsr);
}

// Optional: simple backtrace by scanning stack for plausible return addresses
void print_backtrace(uint32_t *stack, uint32_t stack_size) {
  v_log(LOG_FATAL, "HardFault Backtrace (approx):");
  for (uint32_t i = 0; i < stack_size; i++) {
    uint32_t addr = stack[i];
    // crude check: skip null and small addresses
    if (addr > 0x1000) {
      v_log(LOG_TRACE, " 0x%x", addr);
    }
  }
}

// This function is called by the naked HardFault_Handler
void hardfault_handler_c(ExceptionStackFrame *frame, uint32_t *stack_pointer) {
  v_log(LOG_ERROR, "HARDFAULT occurred!");
  print_registers(frame);

  // Optional backtrace: scan 128 words from stack
  print_backtrace(stack_pointer, 128);

  while (1)
    ;
}

__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
      "tst lr, #4                   \n" // Check EXC_RETURN, which stack to use
      "ite eq                       \n"
      "mrseq r0, msp                \n"  // Main Stack Pointer
      "mrsne r0, psp                \n"  // Process Stack Pointer
      "mov r1, lr                    \n" // Pass LR as second arg
      "ldr r2, =0                    \n" // dummy, not used here
      "b hardfault_handler_entry     \n");
}

// Entry point that reconstructs ExceptionStackFrame
void hardfault_handler_entry(uint32_t *stack_pointer, uint32_t lr_unused,
                             uint32_t dummy) {
  ExceptionStackFrame frame;
  frame.r0 = stack_pointer[0];
  frame.r1 = stack_pointer[1];
  frame.r2 = stack_pointer[2];
  frame.r3 = stack_pointer[3];
  frame.r12 = stack_pointer[4];
  frame.lr = stack_pointer[5];
  frame.pc = stack_pointer[6];
  frame.xpsr = stack_pointer[7];

  hardfault_handler_c(&frame, stack_pointer);
}

extern TCB *current_task;
extern void set_next_task(void);
#define TCB_SP_OFF ((int)offsetof(TCB, sp))

__attribute__((naked)) void PendSV_Handler(void) {
  __asm volatile(
      "   mrs r0, psp                         \n"
      "   isb                                 \n"
      "                                       \n"
      "   ldr r3, =current_task               \n" /* Get the location of the
                                                     current TCB. */
      "   ldr r2, [r3]                        \n"
      "                                       \n"
      "                                       \n"
      "   stmdb r0!, {r4-r11,r14}            \n"  /* Save the core registers. */
      "   str r0, [r2]                        \n" /* Save the new top of stack
                                                     into the first member of
                                                     the TCB. */
      "                                       \n"
      // "   stmdb sp!, {r0, r3}                 \n"
      "   mov r0, %0                          \n"
      "   msr basepri, r0                     \n"
      "   dsb                                 \n"
      "   isb                                 \n"
      "   bl set_next_task                    \n"
      "   mov r0, #0                          \n"
      "   msr basepri, r0                     \n"
      "ldr r3, =current_task\n"

      // "   ldmia sp!, {r0, r3}                 \n"
      "                                       \n"
      "   ldr r1, [r3]                        \n" /* The first item in
                                                     pxCurrentTCB is the task
                                                     top of stack. */
      "   ldr r0, [r1]                        \n"
      "                                       \n"
      "   ldmia r0!, {r4-r11,r14}            \n" /* Pop the core registers. */
      "                                       \n"
      "                                       \n"
      "   msr psp, r0                         \n"
      "   isb                                 \n"
      "                                       \n"
      "                                       \n"
      "   bx lr                              \n"
      "                                       \n"
      "   .ltorg                              \n" ::"i"(
          MAX_SYSCALL_INTERRUPT_PRIORITY)
      : "r0", "r1", "r2", "r3");
}

__attribute__((naked)) void SVCall_Handler(void) {
  __asm volatile(
      "   ldr r3, =current_task           \n" /* Restore the context. */
      "   ldr r1, [r3]                    \n" /* Get the pxCurrentTCB address.
                                               */
      "   ldr r0, [r1]                    \n" /* The first item in pxCurrentTCB
                                                 is the task top of stack. */
      "   ldmia r0!, {r4-r11,r14}        \n"  /* Pop the registers that are not
                                                  automatically saved on
                                                  exception entry and the
                                                  critical nesting count. */
      "   msr psp, r0                     \n" /* Restore the task stack pointer.
                                               */
      "   isb                             \n"
      "   mov r0, #0                      \n"
      "   msr basepri, r0                 \n"
      "   bx r14                          \n"
      "                                   \n"
      "   .ltorg                          \n");
}
