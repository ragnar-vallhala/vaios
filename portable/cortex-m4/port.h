#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H

// Critical section macros
void v_enter_critical(void);
void v_exit_critical(void);

#define ENTER_CRITICAL() v_enter_critical()
#define EXIT_CRITICAL() v_exit_critical()

// Stack setup for new task
#define INITIAL_XPSR 0x01000000UL // Thumb bit set

#endif // !VAIOS_CORTEX_M4_PORT_H