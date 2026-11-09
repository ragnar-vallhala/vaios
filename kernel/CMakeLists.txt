FILE(GLOB_RECURSE VAIOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

# Arch/hardware backends (semihosting, the QEMU interrupt shim) now live in
# portable/armv7e-m/ behind the port facade — the kernel proper is
# architecture-independent. Their build gating moved to portable/CMakeLists.

# Drop optional module sources when their CMake option is OFF (see top-level
# CMakeLists.txt). Excluding the .c keeps it out of flash entirely.
if(NOT VAIOS_MODULE_SHELL)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "shell\\.c$")
endif()
if(NOT VAIOS_MODULE_VFS)
  # Anchor to the filename (leading /) so this does not also drop devfs.c.
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "/vfs\\.c$")
endif()
if(NOT VAIOS_MODULE_FIFO)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "structure\\.c$")
endif()
if(NOT VAIOS_MODULE_PERF)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "perf\\.c$")
endif()
if(NOT VAIOS_MODULE_BUS)
  # Anchored on the leading / so periph_bus.c (the arbiter) always builds.
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "/bus\\.c$")
endif()
# syscalls.c is bare-metal newlib retargeting (_sbrk/_write/_read/_exit ...). On
# the host those clash with glibc's own, and the host port needs none of them.
if(PORT_IS_HOSTED)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "syscalls\\.c$")
endif()

add_library(vaios STATIC ${VAIOS_SOURCES})

# The boot banner's build identity (git revision, time, builder): regenerated on
# every build by a target, so it describes the image actually being linked.
# kernel/vaios.c falls back to "unknown" when built without it (host tests).
set(VAIOS_BUILD_H "${CMAKE_BINARY_DIR}/vaios_build.h")
add_custom_target(vaios_build_info
  COMMAND ${CMAKE_COMMAND} -DOUT=${VAIOS_BUILD_H}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
          -P ${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_info.cmake
  BYPRODUCTS ${VAIOS_BUILD_H}
  VERBATIM)
add_dependencies(vaios vaios_build_info)

# The consumer interface (docs/consumer-interface.md). Everything a project that
# links vaios needs to compile against its public headers travels with the
# target; the top-level include_directories()/add_compile_options() only cover
# vaios's own translation units, not a consumer in another source tree.
#
#  - include/ (public headers) and the build dir (the generated
#    vaios_version.h; vaios_autoconf.h sits beside it). portable/<port>/, which
#    holds port.h, comes from `portable`, linked PUBLIC below.
#  - The generated config, force-included. The public headers read macros from
#    vaios_autoconf.h and the derived layer (vaios_config_derived.h) without
#    including them, so a consumer without the same -include silently sees a
#    different config. INTERFACE, not PUBLIC: vaios's own sources already get
#    these from the top-level add_compile_options(), so naming them again here
#    would only repeat them. Both headers are include-guarded regardless.
#    SHELL: keeps each `-include <file>` pair together (see the top level).
# Normalised so the path spells exactly as the top level's does (CMake only
# de-duplicates identical option strings).
cmake_path(SET _vaios_include NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
# Consumers see these as SYSTEM: a project built with -Wall -Wextra -Werror
# should not fail on warnings in headers it does not own. vaios's own sources
# keep plain -I (PRIVATE, and the top level's include_directories()).
target_include_directories(vaios PRIVATE ${_vaios_include} ${CMAKE_BINARY_DIR})
target_include_directories(vaios SYSTEM INTERFACE ${_vaios_include} ${CMAKE_BINARY_DIR})
target_compile_options(vaios INTERFACE
  "SHELL:-include ${VAIOS_AUTOCONF}"
  "SHELL:-include ${_vaios_include}/vaios_config_derived.h")

# Surface each module switch to C code as a 1/0 macro so init calls and APIs
# can be #if'd out to match the gated source list.
target_compile_definitions(vaios PUBLIC
  VAIOS_MODULE_SHELL=$<BOOL:${VAIOS_MODULE_SHELL}>
  VAIOS_MODULE_VFS=$<BOOL:${VAIOS_MODULE_VFS}>
  VAIOS_MODULE_SEMIHOSTING=$<BOOL:${VAIOS_MODULE_SEMIHOSTING}>
  VAIOS_MODULE_FIFO=$<BOOL:${VAIOS_MODULE_FIFO}>
  VAIOS_MODULE_PERF=$<BOOL:${VAIOS_MODULE_PERF}>
  VAIOS_MODULE_BUS=$<BOOL:${VAIOS_MODULE_BUS}>
)
# On a NAVHAL build the `hal`/`common` targets exist whether vaios embedded its
# own NavHAL or a superproject (e.g. a nav firmware consumer) provided it; link
# them either way. The TARGET guard keeps a misconfigured NAVHAL build (flag on
# but no NavHAL present) from erroring on undefined targets.
if(NAVHAL AND TARGET hal)
target_link_libraries(vaios PUBLIC -Wl,--start-group hal common -Wl,--end-group -lc -lgcc portable)
# NavHAL's headers come from linking hal above, not from a path: hal publishes
# its include dirs, the generated board.h dir and driver definitions such as
# _DMA_ENABLED as usage requirements, so vaios compiles against whichever NavHAL
# is actually built, with the same definitions hal itself was built with.
else()
target_link_libraries(vaios PUBLIC -lgcc -lc portable)
endif()
