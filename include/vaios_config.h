#ifndef VAIOS_CONFIG_H
#define VAIOS_CONFIG_H

#ifdef VAIOS_CONFIG_FILE
#include VAIOS_CONFIG_FILE
#else
// If not defined, look for a file named "vaios_config.h" by default
// This can be provided by the user in their include path.
// However, since we include "vaios_config_default.h" anyway,
// the user can simply provide a custom vaios_config.h that defines
// what it needs and then the rest will be filled by defaults.

#if __has_include("vaios_app_config.h")
#include "vaios_app_config.h"
#endif

#endif

#include "vaios_config_default.h"

#endif // !VAIOS_CONFIG_H
