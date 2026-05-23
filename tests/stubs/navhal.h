/**
 * @file navhal.h (host test stub)
 * @brief Empty stub so vaios_config_default.h can be included on the host.
 *
 * vaios_config_default.h unconditionally `#include "navhal.h"` near the top.
 * The kernel sources compiled into the host test binary (memory.c, task.c,
 * ipc.c) do not call into any NavHAL function; we just need the include to
 * resolve so the config header processes to completion and all
 * MAX_TASK_PRIORITY / IDLE_TASK_PRIORITY / ... defaults are defined.
 */
#ifndef NAVHAL_H
#define NAVHAL_H

/* terminal.c references USART2_IRQn for hal_interrupt_attach_callback —
 * matches the value in NavHAL's family/interrupt_reg.h, but any int will
 * do (the host test build never actually invokes an ISR). */
#define USART2_IRQn 38

#endif /* NAVHAL_H */
