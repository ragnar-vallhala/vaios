set(PORTABLE_DIR "")

if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "cortex-m4")
  set(PORTABLE_DIR "cortex-m4")
endif()

FILE(GLOB_RECURSE PORTABLE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${PORTABLE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/${PORTABLE_DIR}/*.s)

# qemu_irq.c is the non-NAVHAL interrupt shim (it defines
# hal_interrupt_attach_callback / hal_enable_interrupt). On a NAVHAL build the
# real HAL provides those, so drop the shim to avoid duplicate definitions.
if(NAVHAL)
  list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "qemu_irq\\.c$")
endif()

# semihosting.c is the optional debug-I/O backend — and, on a non-NAVHAL build,
# the port's console + SysTick/PendSV priority source (called from port_hw.c).
# Gate it on the module switch; the top-level CMakeLists forces the switch ON
# for the non-NAVHAL builds that depend on it.
if(NOT VAIOS_MODULE_SEMIHOSTING)
  list(FILTER PORTABLE_SOURCES EXCLUDE REGEX "semihosting\\.c$")
endif()

add_library(portable STATIC ${PORTABLE_SOURCES})

if(NAVHAL)
  # port.c pulls in NavHAL via navhal.h. NavHAL's M3 restructure moved the
  # vendor register headers (family/*_reg.h) out of the top-level include/
  # tree; they are now reachable only through the hal target's PUBLIC
  # include directories. Link hal so compiling portable inherits them.
  # (vaios/kernel already links hal, but portable is built standalone.)
  target_link_libraries(portable PUBLIC hal)
endif()

