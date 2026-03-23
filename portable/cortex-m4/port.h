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

#endif // !VAIOS_CORTEX_M4_PORT_H