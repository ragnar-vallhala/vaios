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

#define ENTER_CRITICAL() v_enter_critical()
#define EXIT_CRITICAL() v_exit_critical()
#define V_PORT_MB() __asm__ volatile("dmb" : : : "memory")

// Stack setup for new task
#define INITIAL_XPSR 0x01000000UL // Thumb bit set

// Architecture-specific portable wrappers
uint32_t v_port_get_psp(void);
void v_port_disable_interrupts(void);
void v_port_halt(void);
void v_port_trigger_pendsv(void);

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