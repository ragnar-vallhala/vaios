#ifndef VAIOS_HOST_PORT_H
#define VAIOS_HOST_PORT_H

/*
 * Host (POSIX / Linux) port — lets the scheduler actually RUN on the host, not
 * just have its data structures unit-tested (that is tests/stubs/). The four
 * things that are silicon on Cortex-M are emulated in software:
 *
 *   - context switch : ucontext (makecontext / swapcontext) — port.c
 *   - timer tick      : a POSIX interval timer raising SIGALRM — port_hw.c
 *   - critical section: block SIGALRM (the BASEPRI analogue) — below + port.c
 *   - console         : stdio — port_hw.c
 *
 * The kernel is reused verbatim; only this port layer is host-specific. See
 * docs/plan/HOST_PORT_PLAN.md.
 */

#include "vaios_config.h" // VAIOS_MAX_SYSCALL_PRIO_LEVEL
#include <stdint.h>

// --- NVIC priority model, host-emulated to match portable/cortex-m4/port.h so
// the FromISR priority predicate and any BASEPRI-level reasoning behave the same
// under host as on target. Hardcoded (the host arch has no NVIC_PRIO_BITS). ----
#define __NVIC_PRIO_BITS 4
#ifndef MAX_SYSCALL_INTERRUPT_PRIORITY
#define MAX_SYSCALL_INTERRUPT_PRIORITY                                          \
  (VAIOS_MAX_SYSCALL_PRIO_LEVEL << (8 - __NVIC_PRIO_BITS))
#endif
#define VAIOS_ARCH_FIRST_EXTERNAL_IRQ 16u
static inline int v_port_prio_is_more_urgent(uint32_t a, uint32_t b) {
  return a < b;
}

// Compiler barrier — no reordering across it. Host is a single execution context
// preempted by a signal, so a full memory fence is enough.
#define V_PORT_MB() __atomic_thread_fence(__ATOMIC_SEQ_CST)

// --- Critical sections. On ARM these mask the SysTick via BASEPRI; here they
// block SIGALRM (the timer that drives preemption), which is the exact analogue.
// Non-inline (unlike the ARM port): correctness over the cycle count, and the
// implementations touch signal state that does not belong in a header. ---------
extern volatile uint32_t critical_nesting; // defined in port.c
void v_enter_critical(void);
void v_exit_critical(void);
uint32_t v_enter_critical_from_isr(void);
void v_exit_critical_from_isr(uint32_t saved);

#define ENTER_CRITICAL() v_enter_critical()
#define EXIT_CRITICAL() v_exit_critical()
#define ENTER_CRITICAL_FROM_ISR() v_enter_critical_from_isr()
#define EXIT_CRITICAL_FROM_ISR(saved) v_exit_critical_from_isr(saved)

// Smallest task stack the host port can initialise. A ucontext stack must clear
// MINSIGSTKSZ (kilobytes), plus we store the ucontext_t itself at the base of
// the block — far larger than the 128 B the Cortex-M frame needs. Callers create
// host tasks with at least this much. The stack-overflow watermark is off on
// host (this port's stacks are ucontext-managed), so storing the context pointer
// in task->sp does not trip it.
#define VAIOS_ARCH_MIN_STACK (32u * 1024u)

// --- Core port wrappers ------------------------------------------------------
uint32_t v_port_get_psp(void);        // returns 0 on host -> SP-based guards skip
void v_port_disable_interrupts(void); // block SIGALRM permanently (shutdown path)
void v_port_halt(void);               // never returns
void v_port_trigger_pendsv(void);     // request a context switch (deferred)
int v_port_ptr_is_ram(const void *p); // no known map on host -> always 1

// CPU-relax spin hint: yield the host CPU rather than burn it.
void v_port_cpu_relax(void);
// Host is always privileged (no CONTROL.nPRIV).
static inline int v_port_is_privileged(void) { return 1; }

// --- MPU: no memory protection on host; all no-ops / "unavailable". ----------
void v_port_mpu_init(void);
int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]);
#if VAIOS_MPU_USER_SEPARATION
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]);
#endif
void v_port_mpu_apply(const uint32_t enc[2], uint32_t count);

// --- Port hardware facade (see portable/cortex-m4/port.h for the contract) ---
void v_port_hw_clock_init(uint8_t internal_clock_setup);
void v_port_hw_fpu_enable(void);
void v_port_hw_systick_init(uint32_t period_us);
void v_port_hw_sched_irq_init(void);
void v_port_hw_cpu_idle(void);
int v_port_hw_in_isr(void);
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out);
void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void));
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len);
void v_port_hw_console_write_string(const char *str);
char v_port_hw_console_read_char(void);
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void));
int v_port_hw_sdio_init(void);
int v_port_hw_sdio_card_init(void);
void v_port_hw_cycle_counter_init(void);
uint32_t v_port_hw_cycle_counter_read(void);

#endif // !VAIOS_HOST_PORT_H
