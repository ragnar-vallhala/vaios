#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H


// Critical section macros
#define ENTER_CRITICAL() __asm volatile("cpsid i" ::: "memory")
#define EXIT_CRITICAL() __asm volatile("cpsie i" ::: "memory")


#endif // !VAIOS_CORTEX_M4_PORT_H
