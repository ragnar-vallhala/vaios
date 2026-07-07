#include "port.h"
#include "task.h"
#include "utils.h"
#include <stdint.h>

extern void v_print(const char *str);

// Nesting counter for v_enter_critical / v_exit_critical. The functions
// themselves are defined inline in port.h so each critical section emits a
// bare `msr basepri` instead of a bl round-trip; only the shared counter
// lives here.
volatile uint32_t critical_nesting = 0;

uint32_t v_port_get_psp(void) {
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));
  return psp;
}

void v_port_disable_interrupts(void) { __asm volatile("cpsid i" ::: "memory"); }

void v_port_halt(void) {
  while (1) {
    __asm volatile("nop");
  }
}

#define ICSR (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET (1 << 28)

void v_port_trigger_pendsv(void) { ICSR |= ICSR_PENDSVSET; }

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

  v_panic("HardFault", 0,
          "System encountered a HardFault! Check above for registers.");
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

/* ---------------------------------------------------------------------------
 * System-fault vectors — OS-owned placeholders.
 *
 * NavHAL's startup.s names these vectors and, under SUBMODULE, cedes them to
 * the OS: what a fault *means* (kill the task, panic, recover) is kernel policy,
 * not the HAL's to decide. NMI/BusFault/UsageFault/DebugMon are still minimal
 * shims (behaviour TBD; BusFault/UsageFault will route into the CFSR + v_panic
 * diagnostics like HardFault). MemManage_Handler is the MPU stack-overflow trap
 * (docs/plan/MPU_CACHE_INTEGRATION_PLAN.md).
 * ------------------------------------------------------------------------- */
void NMI_Handler(void) {
  for (;;) {
  }
}

// MPU violation. Phase 1: a task overran its stack into the no-access guard.
// Phase 2 (VAIOS_MPU_STATIC_PROTECT) adds execute-from-RAM (W^X), flash-write,
// NULL-deref, and bad-peripheral faults. Decode CFSR/MMFAR and panic with the
// offending task + address. (Recovery/task-kill is a later phase.) MMFSR bits:
// IACCVIOL(0) = instruction-fetch (XN) violation, DACCVIOL(1) = data AP.
#define SCB_CFSR (*(volatile uint32_t *)0xE000ED28)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34)
#define MMFSR_MMARVALID (1u << 7)
void MemManage_Handler(void) {
#if VAIOS_MPU_STACK_GUARD || VAIOS_MPU_STATIC_PROTECT
  extern TCB *current_task;
  uint32_t mmfsr = SCB_CFSR & 0xFFu; /* MemManage status is CFSR[7:0] */
  uint32_t addr = (mmfsr & MMFSR_MMARVALID) ? SCB_MMFAR : 0u;
  uint32_t id = current_task ? current_task->task_id : 0u;
  v_panic(__FILE__, __LINE__,
          "MPU fault in task %u | fault addr 0x%x MMFSR 0x%x", (unsigned)id,
          (unsigned)addr, (unsigned)mmfsr);
#endif
  for (;;) {
  }
}
void BusFault_Handler(void) {
  for (;;) {
  }
}
void UsageFault_Handler(void) {
  for (;;) {
  }
}
void DebugMon_Handler(void) {
  for (;;) {
  }
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
      /* Mask interrupts for the whole first-task setup. v_init left interrupts
         enabled (hal_delay needs the tick), so without this a SysTick landing
         after scheduler_running is set — but before the svc establishes PSP —
         pends PendSV, which then saves to PSP=0 (fault at 0xFFFFFFDC). The
         cpsie i further down re-enables them right before the svc. */
      "cpsid i                \n"
      "ldr r0, =scheduler_running\n"
      "mov r1, #1             \n"
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
      /* First-task launch must not be preempted by SysTick. SysTick was started
         in v_init, so the instant interrupts are enabled a pending tick would
         take PendSV BEFORE the svc below establishes this task's PSP; PendSV then
         saves context to PSP=0, writing 9 words down to 0xFFFFFFDC (bus fault on
         QEMU; silent corruption on Renode/hardware). Disable the SysTick
         interrupt and clear any pending PendSV here; SVCall_Handler re-enables
         SysTick once PSP is valid, at priority 0 where a tick cannot preempt. */
      " ldr r0, =0xE000E010   \n" /* SysTick CSR: stop the counter entirely    */
      " ldr r1, [r0]          \n"
      " bic r1, r1, #3        \n" /* clear ENABLE|TICKINT                       */
      " str r1, [r0]          \n"
      " ldr r0, =0xE000ED04   \n" /* ICSR: drop any ALREADY-pending exceptions */
      " ldr r1, =0x0A000000   \n" /* PENDSVCLR (bit27) | PENDSTCLR (bit25)     */
      " str r1, [r0]          \n"
      " dsb                   \n"
      " isb                   \n"
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
      "   bl v_port_apply_current_mpu         \n" /* swap task MPU regions */
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
      /* PSP is now valid and we are in SVCall at priority 0 (un-preemptible).
         Re-enable the SysTick interrupt that scheduler_start disabled, so the
         first preemptive tick only arrives after we return into the first task
         with a good PSP. */
      "   ldr r0, =0xE000E010             \n" /* SysTick CSR */
      "   ldr r1, [r0]                    \n"
      "   orr r1, r1, #3                  \n" /* set ENABLE|TICKINT */
      "   str r1, [r0]                    \n"
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
  // Stacked PC: the Thumb state comes from INITIAL_XPSR's T bit, so the
  // entry address itself must be halfword-aligned. A C function pointer in
  // Thumb has bit[0] set; leaving it set makes the exception-return PC odd,
  // which is UNPREDICTABLE on v7-M (QEMU warns "return from interrupt with
  // misaligned PC" and may execute from the bad address). Mask it off.
  *sp = (uint32_t)task->entry & ~1UL;
  sp--;
  *sp = (uint32_t)TASK_EXIT; // Hardware LR (entered via BX, keeps its Thumb bit)

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

#if VAIOS_MPU_STACK_GUARD
  // Pre-encode the no-access guard region at the stack base (lowest address).
  // Applied on every switch to this task; a store past the bottom traps into
  // MemManage_Handler. Encode fails cleanly (invalid) on MPU-less targets.
  task->mpu_guard_valid =
      (v_port_stack_guard_encode(task->mem_block, VAIOS_MPU_GUARD_SIZE,
                                 task->mpu_guard) == 0);
#endif
}

// Apply the running task's MPU region set — the context-switch fast path, called
// from PendSV/ISR switch after set_next_task updates current_task.
void v_port_apply_current_mpu(void) {
#if VAIOS_MPU_STACK_GUARD
  extern TCB *current_task;
  if (current_task && current_task->mpu_guard_valid)
    v_port_mpu_apply(current_task->mpu_guard, 1);
#endif
}

void load_next_task_from_isr(void) {
  __asm volatile("   mov r0, %0                          \n"
                 "   msr basepri, r0                     \n"
                 "   dsb                                 \n"
                 "   isb                                 \n"
                 "   bl set_next_task                    \n"
                 "   bl v_port_apply_current_mpu         \n"
                 "   mov r0, #0                          \n"
                 "   msr basepri, r0                     \n" ::"i"(
                     MAX_SYSCALL_INTERRUPT_PRIORITY)
                 : "r0");
}
