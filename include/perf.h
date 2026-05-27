#ifndef VAIOS_PERF_H
#define VAIOS_PERF_H

/*
 * vaios perf module — kernel performance counters.
 *
 * Excluded from the build when -DVAIOS_MODULE_PERF=OFF (see top-level
 * CMakeLists.txt). When excluded, callers in kernel/ are still safe to
 * include this header — the public API becomes empty inline stubs so
 * call sites compile to nothing. See docs/perf/IMPLEMENTATION_PLAN.md.
 */

#include "vaios_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if VAIOS_MODULE_PERF

void v_perf_init(void);

#else  /* VAIOS_MODULE_PERF == 0 */

static inline void v_perf_init(void) {}

#endif /* VAIOS_MODULE_PERF */

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_PERF_H */
