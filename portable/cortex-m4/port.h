#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H

#include <stdint.h>

// Critical section macros
void v_enter_critical(void);
void v_exit_critical(void);

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