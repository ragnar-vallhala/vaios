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
#endif /* NAVHAL_H */
