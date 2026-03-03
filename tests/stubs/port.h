/**
 * @file port.h
 * @brief Host-native stub replacing portable/cortex-m4/port.h
 *
 * Replaces ARM-specific inline assembly macros with no-ops so that kernel
 * C code can be compiled and tested on the host machine.
 *
 * This file is placed in tests/stubs/ and the stub include path is given
 * BEFORE the real portble/ include path so that it shadows the real port.h.
 */
#ifndef VAIOS_CORTEX_M4_PORT_H /* matches the real port.h guard */
#define VAIOS_CORTEX_M4_PORT_H

/* Critical section stubs — no-ops on host */
#define ENTER_CRITICAL()                                                       \
  do {                                                                         \
  } while (0)
#define EXIT_CRITICAL()                                                        \
  do {                                                                         \
  } while (0)

/* Cortex-M4 initial stack frame constants (same values as the real header) */
#define INITIAL_XPSR 0x01000000UL
#define TASK_ENTRY_MASK 0xFFFFFFFEUL

#endif /* VAIOS_CORTEX_M4_PORT_H */
