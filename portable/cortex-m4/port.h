#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H

#include "vaios_config.h" // VAIOS_USE_BASEPRI, MAX_SYSCALL_INTERRUPT_PRIORITY
#include <stdint.h>

// Critical-section nesting counter (defined in port.c).
extern volatile uint32_t critical_nesting;

// Critical section entry/exit. Defined inline here — rather than as calls
// into port.c — so every ENTER_CRITICAL/EXIT_CRITICAL site emits the bare
// `msr basepri` instead of a bl round-trip. This is on the hot path of every
// kernel primitive (malloc/free, semaphores, mutexes, the scheduler).
__attribute__((always_inline)) static inline void v_enter_critical(void) {
#if VAIOS_USE_BASEPRI
  uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
  __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
  __asm volatile("cpsid i" ::: "memory");
#endif
  __asm volatile("" ::: "memory"); // compiler barrier
  critical_nesting++;
}

__attribute__((always_inline)) static inline void v_exit_critical(void) {
  critical_nesting--;
  if (critical_nesting == 0) {
#if VAIOS_USE_BASEPRI
    uint32_t pri = 0;
    __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
    __asm volatile("cpsie i" ::: "memory");
#endif
  }
}

// FromISR-safe critical section. Saves/restores the interrupt mask in a LOCAL
// variable instead of the global `critical_nesting`, so it is safe to use from
// an ISR — an ISR must never touch a preempted task's nesting count — and from
// task context alike (a save/restore of the current BASEPRI nests correctly).
// Use this, not ENTER/EXIT_CRITICAL, in any code reachable from a *_from_isr
// path.
__attribute__((always_inline)) static inline uint32_t
v_enter_critical_from_isr(void) {
  uint32_t saved;
#if VAIOS_USE_BASEPRI
  uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
  __asm volatile("mrs %0, basepri" : "=r"(saved)::"memory");
  __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
  __asm volatile("mrs %0, primask" : "=r"(saved)::"memory");
  __asm volatile("cpsid i" ::: "memory");
#endif
  return saved;
}

__attribute__((always_inline)) static inline void
v_exit_critical_from_isr(uint32_t saved) {
#if VAIOS_USE_BASEPRI
  __asm volatile("msr basepri, %0" : : "r"(saved) : "memory");
#else
  __asm volatile("msr primask, %0" : : "r"(saved) : "memory");
#endif
}

#define ENTER_CRITICAL() v_enter_critical()
#define EXIT_CRITICAL() v_exit_critical()
#define ENTER_CRITICAL_FROM_ISR() v_enter_critical_from_isr()
#define EXIT_CRITICAL_FROM_ISR(saved) v_exit_critical_from_isr(saved)
#define V_PORT_MB() __asm__ volatile("dmb" : : : "memory")

// Stack setup for new task
#define INITIAL_XPSR 0x01000000UL // Thumb bit set

// Architecture-specific portable wrappers
uint32_t v_port_get_psp(void);
void v_port_disable_interrupts(void);
void v_port_halt(void);
void v_port_trigger_pendsv(void);

// CPU hint for a short busy-wait spin (keeps the arch `nop` out of the kernel).
__attribute__((always_inline)) static inline void v_port_cpu_relax(void) {
  __asm volatile("nop" ::: "memory");
}

// ---------------------------------------------------------------------------
// Port hardware facade.
//
// The kernel reaches hardware ONLY through these wrappers; the backend (real
// STM32 HAL vs. the QEMU semihosting model) is selected inside port_hw.c. No
// kernel source may name a hal_* symbol, a uartN_* shim, an IRQn enum, or a
// peripheral register directly. Host unit-test builds supply equivalents from
// tests/stubs/stubs.c (declared in the stub tests/stubs/port.h).
// ---------------------------------------------------------------------------

// Clock / FPU bring-up.
void v_port_hw_clock_init(uint8_t internal_clock_setup);
void v_port_hw_fpu_enable(void);

// System tick timer + scheduler interrupt priorities (SysTick=14, PendSV=15).
void v_port_hw_systick_init(uint32_t period_us);
void v_port_hw_sched_irq_init(void);

// Active-exception NVIC priority, for the FromISR priority assert. Returns the
// priority byte of the currently-executing external IRQ (or 0 in thread mode /
// a system handler) and writes ICSR.VECTACTIVE to *vectactive_out.
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out);

// Console: log/terminal UART on hardware, semihosting under QEMU.
void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void));
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len);
void v_port_hw_console_write_string(const char *str);
char v_port_hw_console_read_char(void);
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void));

// SD/MMC over SDIO (VFS backend). Return 0 on success, non-zero on failure.
int v_port_hw_sdio_init(void);
int v_port_hw_sdio_card_init(void);

// Cycle counter (DWT CYCCNT) backing the perf module.
void v_port_hw_cycle_counter_init(void);
uint32_t v_port_hw_cycle_counter_read(void);

// Atomic operations (LL/SC)
static inline uint32_t v_port_ldrex(volatile uint32_t *addr) {
  uint32_t result;
  __asm__ volatile("ldrex %0, [%1]" : "=r"(result) : "r"(addr) : "memory");
  return result;
}

static inline uint32_t v_port_strex(uint32_t val, volatile uint32_t *addr) {
  uint32_t result;
  __asm__ volatile("strex %0, %1, [%2]"
                   : "=&r"(result)
                   : "r"(val), "r"(addr)
                   : "memory");
  return result;
}

static inline void v_port_clrex(void) {
  __asm__ volatile("clrex" : : : "memory");
}

#endif // !VAIOS_CORTEX_M4_PORT_H