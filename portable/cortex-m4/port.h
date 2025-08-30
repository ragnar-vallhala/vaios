#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H


// Critical section macros
#define ENTER_CRITICAL() __asm volatile("cpsid i" ::: "memory")
#define EXIT_CRITICAL() __asm volatile("cpsie i" ::: "memory")

// Stack setup for new task
#define INITIAL_XPSR 0x01000000UL    // Thumb bit set
#define TASK_ENTRY_MASK 0xFFFFFFFEUL // Set last bit 0


#endif // !VAIOS_CORTEX_M4_PORT_H