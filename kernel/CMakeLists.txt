FILE(GLOB_RECURSE VAIOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

# Arch/hardware backends (semihosting, the QEMU interrupt shim) now live in
# portable/cortex-m4/ behind the port facade — the kernel proper is
# architecture-independent. Their build gating moved to portable/CMakeLists.

# Drop optional module sources when their CMake option is OFF (see top-level
# CMakeLists.txt). Excluding the .c keeps it out of flash entirely.
if(NOT VAIOS_MODULE_TERMINAL)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "terminal\\.c$")
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
# syscalls.c is bare-metal newlib retargeting (_sbrk/_write/_read/_exit ...). On
# the host those clash with glibc's own, and the host port needs none of them.
if(PORT_IS_HOSTED)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "syscalls\\.c$")
endif()

add_library(vaios STATIC ${VAIOS_SOURCES})

# Export the public headers as a usage requirement so projects that link vaios
# (e.g. a nav firmware consumer that pulls vaios in as a [dependencies] library)
# get include/ on their include path. The top-level include_directories() only
# covers vaios's own compilation, not consumers in another source tree.
target_include_directories(vaios PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Surface each module switch to C code as a 1/0 macro so init calls and APIs
# can be #if'd out to match the gated source list.
target_compile_definitions(vaios PUBLIC
  VAIOS_MODULE_TERMINAL=$<BOOL:${VAIOS_MODULE_TERMINAL}>
  VAIOS_MODULE_VFS=$<BOOL:${VAIOS_MODULE_VFS}>
  VAIOS_MODULE_SEMIHOSTING=$<BOOL:${VAIOS_MODULE_SEMIHOSTING}>
  VAIOS_MODULE_FIFO=$<BOOL:${VAIOS_MODULE_FIFO}>
  VAIOS_MODULE_PERF=$<BOOL:${VAIOS_MODULE_PERF}>
)
# On a NAVHAL build the `hal`/`common` targets exist whether vaios embedded its
# own NavHAL or a superproject (e.g. a nav firmware consumer) provided it; link
# them either way. The TARGET guard keeps a misconfigured NAVHAL build (flag on
# but no NavHAL present) from erroring on undefined targets.
if(NAVHAL AND TARGET hal)
target_link_libraries(vaios PUBLIC -Wl,--start-group hal common -Wl,--end-group -lc -lgcc portable)
# PRIVATE, not PUBLIC: only vaios's own TUs may resolve NavHAL headers, and even
# they shouldn't — the kernel reaches the HAL through the portable/ facade. A
# PUBLIC export put vendor headers on every consumer's include path, which is
# what let a stray hal_* call in kernel/ compile silently instead of erroring.
target_include_directories(vaios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../extern/NavHAL/include)
else()
target_link_libraries(vaios PUBLIC -lgcc -lc portable)
endif()
