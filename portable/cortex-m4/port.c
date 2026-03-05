#include "port.h"
#include "config.h"
#include "task.h"
#include "utils.h"
#include <stddef.h>
#include <stdint.h>

extern void v_print(const char *str);

volatile uint32_t critical_nesting = 0;

void v_enter_critical(void) {
  __asm volatile("cpsid i" ::: "memory");
  __asm volatile("" ::: "memory"); // compiler barrier
  critical_nesting++;
}

void v_exit_critical(void) {
  critical_nesting--;
  if (critical_nesting == 0) {
    __asm volatile("cpsie i" ::: "memory");
  }
}

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
  v_log(LOG_TRACE, " R0  = 0x%08X", frame->r0);
  v_log(LOG_TRACE, " R1  = 0x%08X", frame->r1);
  v_log(LOG_TRACE, " R2  = 0x%08X", frame->r2);
  v_log(LOG_TRACE, " R3  = 0x%08X", frame->r3);
  v_log(LOG_TRACE, " R12 = 0x%08X", frame->r12);
  v_log(LOG_TRACE, " LR  = 0x%08X", frame->lr);
  v_log(LOG_TRACE, " PC  = 0x%08X", frame->pc);
  v_log(LOG_TRACE, " xPSR= 0x%08X", frame->xpsr);
}

static void print_hex_blocking(uint32_t val) {
  char buf[9];
  buf[8] = '\0';
  for (int i = 7; i >= 0; i--) {
    uint8_t nibble = val & 0xF;
    buf[i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
    val >>= 4;
  }
  v_print("0x");
  v_print(buf);
  v_print("\r\n");
}

// Optional: simple backtrace by scanning stack for plausible return addresses
void print_backtrace(uint32_t *stack, uint32_t stack_size) {
  v_print("HardFault Backtrace (approx):\r\n");
  for (uint32_t i = 0; i < stack_size; i++) {
    uint32_t addr = stack[i];
    // crude check: skip null and small addresses
    if (addr > 0x1000) {
      v_print(" ");
      print_hex_blocking(addr);
    }
  }
}

// This function is called by the naked HardFault_Handler
void hardfault_handler_c(ExceptionStackFrame *frame, uint32_t *stack_pointer,
                         uint32_t exc_return) {
  v_print("\r\n\r\n********************************\r\n");
  v_print("**** SYSTEM HARDFAULT! ****\r\n");
  v_print("********************************\r\n\r\n");
  v_print("EXC_RET: ");
  print_hex_blocking(exc_return);
  v_print("PC: ");
  print_hex_blocking(frame->pc);
  v_print("LR: ");
  print_hex_blocking(frame->lr);
  v_print("R0: ");
  print_hex_blocking(frame->r0);
  v_print("SP: ");
  print_hex_blocking((uint32_t)stack_pointer);

  // Print SCB fault registers
  uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
  uint32_t hfsr = *(volatile uint32_t *)0xE000ED2C;
  uint32_t mmfar = *(volatile uint32_t *)0xE000ED34;
  uint32_t bfar = *(volatile uint32_t *)0xE000ED38;
  v_print("CFSR: ");
  print_hex_blocking(cfsr);
  v_print("HFSR: ");
  print_hex_blocking(hfsr);
  v_print("MMFAR: ");
  print_hex_blocking(mmfar);
  v_print("BFAR: ");
  print_hex_blocking(bfar);

  // Optional backtrace: scan 32 words from stack
  print_backtrace(stack_pointer, 32);

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
void hardfault_handler_entry(uint32_t *stack_pointer, uint32_t exc_return,
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

  hardfault_handler_c(&frame, stack_pointer, exc_return);
}

#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04)
#define PENDSVSET (1U << 28)
void task_yield(void) {
  SCB_ICSR |= PENDSVSET;

  /* Data/Instruction barriers manually */
  asm volatile("dsb");
  asm volatile("isb");
}

__attribute__((naked)) void scheduler_start(void) {
  __asm volatile(
      "ldr r0, =scheduler_running\n"
      "mov r1, #123             \n"
      "strb r1, [r0]          \n"
      " ldr r0, =0xE000ED08   \n" /* Use the NVIC offset register to locate the
                                     stack. */
      " ldr r0, [r0]          \n"
      " ldr r0, [r0]          \n"
      " msr msp, r0           \n" /* Set the msp back to the start of the stack.
                                   */
      " mov r0, #0            \n" /* Clear the bit that indicates the FPU is in
                                     use, see comment above. */
      " msr control, r0       \n"
      " cpsie i               \n" /* Globally enable interrupts. */
      " cpsie f               \n"
      " dsb                   \n"
      "svc 0                  \n"
      " nop                   \n"
      " .ltorg                \n");
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
#ifdef _FPU_ENABLED
      "   tst r14, #0x10                      \n" /* Check if FPU was used. */
      "   it eq                               \n"
      "   vstmdbeq r0!, {s16-s31}             \n" /* Save FPU registers s16-s31.
                                                   */
#endif
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
      "                                       \n"
      "   ldr r1, [r3]                        \n" /* The first item in
                                                     pxCurrentTCB is the task
                                                     top of stack. */
      "   ldr r0, [r1]                        \n"
      "                                       \n"
      "   ldmia r0!, {r4-r11,r14}            \n" /* Pop the core registers. */
      "                                       \n"
#ifdef _FPU_ENABLED
      "   tst r14, #0x10                      \n" /* Check if FPU was used. */
      "   it eq                               \n"
      "   vldmiaeq r0!, {s16-s31}             \n" /* Restore FPU registers
                                                     s16-s31. */
#endif
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
      "   ldmia r0!, {r4-r11,r14}        \n"  /* Pop core registers. */
      "                                   \n"
#ifdef _FPU_ENABLED
      "   tst r14, #0x10                  \n" /* Check if FPU was used. */
      "   it eq                           \n"
      "   vldmiaeq r0!, {s16-s31}         \n" /* Restore FPU registers s16-s31.
                                               */
#endif
      "   msr psp, r0                     \n" /* Restore the task stack pointer.
                                               */
      "   isb                             \n"
      "   mov r0, #0                      \n"
      "   msr basepri, r0                 \n"
      "   bx r14                          \n"
      "                                   \n"
      "   .ltorg                          \n");
}

// Task stack initialization
void init_task_stack(TCB *task) {
  // Align sp to 8 bytes
  uint32_t *sp = (uint32_t *)((uint32_t)(task->sp) & (~7UL));

  // --- Hardware Stack Frame ---
  sp--;
  *sp = INITIAL_XPSR;
  sp--;
  *sp = ((uint32_t)task->entry) & TASK_ENTRY_MASK;
  sp--;
  *sp = (uint32_t)TASK_EXIT; // Hardware LR

  // R12, R3, R2, R1
  sp -= 4;

  // R0 (argument)
  sp--;
  *sp = (uint32_t)task->arg;

  // --- Software Stack Frame ---
  // Save EXC_RETURN (r14)
  sp--;
  *sp = 0xfffffffd; // Initial EXC_RETURN: Basic frame (no FPU)

  // Save R4-R11
  sp -= 8;

  task->sp = sp;
}

void load_next_task_from_isr(void) {
  __asm volatile("   mov r0, %0                          \n"
                 "   msr basepri, r0                     \n"
                 "   dsb                                 \n"
                 "   isb                                 \n"
                 "   bl set_next_task                    \n"
                 "   mov r0, #0                          \n"
                 "   msr basepri, r0                     \n" ::"i"(
                     MAX_SYSCALL_INTERRUPT_PRIORITY)
                 : "r0");
}
