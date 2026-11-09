# Consumer interface

What a project gets by building vaios as a library (a nav `[dependencies]`
entry, or `add_subdirectory(vaios)` by hand) and linking the `vaios` target.

## Using it

```cmake
# NavHAL first: the `hal` target must exist (vaios reuses it, and NAVHAL_DIR
# points at its tree). Then vaios, with the options the project needs.
add_subdirectory(extern/NavHAL)
set(NAVHAL ON CACHE BOOL "")
set(VAIOS_MODULE_VFS ON CACHE BOOL "")
add_subdirectory(vaios)
target_link_libraries(app PRIVATE vaios)
```

Linking `vaios` is all a consumer does. Everything below arrives as a usage
requirement of that target (`portable` and `hal` come with it).

## What the consumer receives

| What | How | Notes |
|---|---|---|
| Public headers `include/*.h` | include dir, `SYSTEM` | `#include "task.h"`, `"vaios.h"`, ... |
| `port.h` | include dir of `portable`, `SYSTEM` | `portable/<VAIOS_PORT>/` |
| `vaios_version.h` (generated) | include dir (the build dir), `SYSTEM` | see Version |
| `vaios_autoconf.h` | `-include`, on every consumer TU | the Kconfig macros: `HEAP_SIZE`, `MAX_TASK_PRIORITY`, `VAIOS_ARCH_*`, ... |
| `vaios_config_derived.h` | `-include`, on every consumer TU | composite macros built on the above, e.g. `VAIOS_HEAP_ALGO` |
| `VAIOS_MODULE_{SHELL,VFS,SEMIHOSTING,FIFO,PERF,BUS}=0/1` | `-D` | what was *compiled*; wins over the autoconf value |
| NavHAL headers and `navhal_target.h` | through `hal` | not vaios's to promise |

The two `-include`d headers are what the public headers read their config from
without including them, so they must be on the consumer's command line; do not
`#include` them by hand and do not add your own `-include` of them. Both are
include-guarded. vaios's own sources get exactly the same set, so the library
and its users cannot disagree about a struct layout or a module switch.

The include directories are `SYSTEM`, so a consumer built with
`-Wall -Wextra -Werror` is not stopped by a warning inside a vaios header. The
force-included config headers are macros only and are clean under
`-Wall -Wextra -Wpedantic`.

NavHAL is not touched: its sources do not get vaios's `-include`s.

## Version

`nav.toml` `[project].version` is the only place the version is written; nav
requires the release tag to equal it. The build derives everything else:

* `#include "vaios_version.h"` (also pulled in by `vaios.h`): `VAIOS_VERSION_MAJOR`,
  `VAIOS_VERSION_MINOR`, `VAIOS_VERSION_PATCH`, `VAIOS_VERSION` (string
  `"0.2.2"`) and `VAIOS_VERSION_NUMBER` (`MAJOR*10000 + MINOR*100 + PATCH`).
* CMake: `VAIOS_VERSION`, `VAIOS_VERSION_MAJOR`, `VAIOS_VERSION_MINOR`,
  `VAIOS_VERSION_PATCH`, set when `add_subdirectory(vaios)` returns.

A consumer pins what it was written against:

```c
#if VAIOS_VERSION_MAJOR != 0 || VAIOS_VERSION_MINOR < 2
#error "needs vaios 0.2 or newer"
#endif
```

## What is stable

While the major version is 0, a MINOR bump may change any of this; a PATCH bump
will not. From 1.0, only a MAJOR bump may.

* The target names `vaios` and `portable`, and linking `vaios` being sufficient.
* The names and meaning of the version macros and CMake variables above.
* The `VAIOS_MODULE_*` macros, and that they reflect what was compiled.

Not promised: the individual Kconfig macros in `vaios_autoconf.h` (they follow
`Kconfig`), the `portable/<port>/` header set beyond `port.h`, and anything
under `kernel/`.
