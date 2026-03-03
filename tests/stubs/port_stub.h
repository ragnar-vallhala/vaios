/**
 * @file port_stub.h
 * @brief Host-native stub replacing portable/cortex-m4/port.h
 *
 * Replaces ARM-specific inline assembly macros with no-ops so that kernel
 * C code can be compiled and tested on the host machine.
 */
#ifndef VAIOS_PORT_STUB_H
#define VAIOS_PORT_STUB_H

/* Critical section stubs – no-ops on host */
#define ENTER_CRITICAL()                                                       \
  do {                                                                         \
  } while (0)
#define EXIT_CRITICAL()                                                        \
  do {                                                                         \
  } while (0)

/* Cortex-M4 initial stack frame constants */
#define INITIAL_XPSR 0x01000000UL
#define TASK_ENTRY_MASK 0xFFFFFFFEUL

#endif /* VAIOS_PORT_STUB_H */
