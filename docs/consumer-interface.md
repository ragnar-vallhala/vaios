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

## What vaios asks of NavHAL

nav reads `navhal.config` from the vaios repo and unions every `CONFIG_X=y` line
into the app's `.config` (an app that sets one to something else fails the
build), so the file is the price of using vaios. It carries only what vaios
cannot boot without:

| Symbol | Why |
|---|---|
| `DRV_CLOCK`, `DRV_TIMER` | `hal_clock_init`, `hal_timebase_init`: the boot clock and the kernel tick |
| `DRV_INTERRUPT` | SysTick/PendSV/SVC priorities, IRQ attach for the console and bus managers |
| `DRV_UART`, `DRV_GPIO` | the console (`hal_uart_*`); the UART driver takes its pins through GPIO |

It does not name a board, an architecture, a flasher or a float ABI: the app picks
its own. For the float ABI, state `CONFIG_USE_FPU=y` and `CONFIG_DRV_FPU=y` for
hard-float, or neither for soft-float (see `tests/configs/navhal_softfp.config`).
vaios saves s16-s31 across a context switch when the FPU is on, and reads the
choice from the `.config` it is given (`VAIOS_CONFIG`), so it follows the app; the
build stops with a message if that disagrees with the `USE_FPU` NavHAL was
configured with.

What a module needs on top is stated by the app, and vaios stops the build with
a message rather than failing later at link:

| vaios | needs | why |
|---|---|---|
| `VAIOS_MODULE_VFS=ON` | `CONFIG_DRV_SDIO=y` | `v_open`/`v_read`/... and the card driver come from NavHAL's FatFs/SDIO layer (`#error` in `port_hw.c`) |
| `VAIOS_MPU_ENABLE` | `CONFIG_DRV_MPU=y` | `hal_mpu_*` (`#error` in `port_hw.c`) |

Used when present, never required: `DRV_RTC` (the `date` command, `v_port_hw_rtc_*`),
`DRV_DWT` (cycle counts in `v_perf`; zero without), `DRV_UART_DMA`/`DRV_DMA` (the
buffered console; blocking writes without), `DRV_USB_CDC`, `DRV_I2C`, `DRV_SPI`
(the bus managers), `FS_LFN` (long names; `VFS_NAME_MAX` follows
`NAVHAL_CONFIG_FS_LFN_MAX`). In particular vaios no longer forces FatFs long file
names (a static, non-thread-safe buffer and 65 bytes of stack per `FILINFO`) or
`FS_RTC` on a consumer: enable them in the app's `.config` if the app wants them.

vaios's own standalone build adds its board, those extras and the module drivers
itself (`defconfig`, and `CMakeLists.txt` for `DRV_SDIO`/`DRV_MPU`); a consumer
never reads `defconfig`.

## newlib retargeting (`_sbrk`, `_write`, ...)

On a bare-metal build vaios defines the newlib system calls `_sbrk`, `_write`,
`_read`, `_close`, `_lseek` and `_exit` (`kernel/syscalls.c`). `_sbrk` returns 0
on purpose: vaios has its own heap allocator, and newlib's `malloc` must not grow.

**The guarantee: these definitions win over libnosys, whatever order the
consumer links in.** nav's `libc = "newlib"` puts `-lc -lm -lnosys` ahead of the
dependency archives, and an archive member loses to anything the linker has
already seen, so when `syscalls.c` was only a member of `libvaios.a` the linker
took `_sbrk`/`_write` from `libnosys.a`. nosys's `_sbrk` grows from the linker
symbol `end`, which is where vaios's heap starts: `malloc` (and so `printf`,
`fopen`, ...) would have handed out vaios heap memory, silently.

Now `syscalls.c` is an OBJECT library (`vaios_syscalls`) whose objects travel
with the `vaios` target as INTERFACE sources. Linking `vaios` puts
`syscalls.c.obj` on the consumer's link line as an object file, and objects are
always linked and define their symbols before any `-l` library is opened. The
consumer does nothing; no link-order rule applies. The objects are also kept
inside `libvaios.a`, for a consumer that links the archive by path.

A guard backs this up. `kernel/syscalls.c` defines `v_newlib_retarget` and
`kernel/vaios.c` references it, so a firmware that uses vaios but lost the
retargeting (for example a path-linked `libvaios.a` after `-lnosys`) fails to
link with `multiple definition of '_sbrk'` instead of building a heap-corrupting
image.

Consequences for a consumer:

* Do not define your own `_sbrk`/`_write`/`_read`/`_close`/`_lseek`/`_exit`: it is
  a duplicate definition. To supply them (a firmware that routes stdio through
  its own driver), configure vaios with `-DVAIOS_NEWLIB_RETARGET=OFF`; nothing in
  vaios calls them, only newlib does. vaios's own `UNIT_TESTS` example does this.
* `printf` and friends then go to vaios's `_write`, which discards the output
  (it only reports success). Use `v_printf`/`v_log` for console output.
* The host port (`portable/host`, which uses the real libc) builds none of this.

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
* That vaios's newlib retargeting beats libnosys (see above), and the
  `VAIOS_NEWLIB_RETARGET` switch.

Not promised: the individual Kconfig macros in `vaios_autoconf.h` (they follow
`Kconfig`), the `portable/<port>/` header set beyond `port.h`, and anything
under `kernel/`.
