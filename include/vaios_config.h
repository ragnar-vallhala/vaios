#ifndef VAIOS_CONFIG_H
#define VAIOS_CONFIG_H
/*
 * vaios configuration aggregator.
 *
 * Scalar config macros are Kconfig-generated (vaios_autoconf.h); the composite
 * ones (MAX_SYSCALL_INTERRUPT_PRIORITY, the VAIOS_HEAP_* selector, PANIC) are
 * derived in vaios_config_derived.h. The build force-includes both ahead of
 * every vaios translation unit (see CMakeLists.txt / tools/kconfig.py), so most
 * sources need nothing here. Re-including the derived layer keeps a source that
 * only pulls in "vaios_config.h" self-contained even if the force-include is
 * ever absent.
 *
 * There is no static default header any more — run the menuconfig target (or
 * edit the build-directory .config) to change values.
 */
#include "vaios_config_derived.h"
#endif // !VAIOS_CONFIG_H
