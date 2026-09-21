# The port is VAIOS_PORT (set in the root CMakeLists, default cortex-m4).
# portable/<port>/ is the ONLY source tree compiled. Fail loudly on an unknown
# port rather than the old behaviour: an empty PORTABLE_DIR made the glob match
# ALL of portable/**, sweeping every port's sources (avr/*.c AND cortex-m4/*.c +
# startup.s) into one library.
if(NOT VAIOS_PORT OR NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}")
  message(FATAL_ERROR
    "portable: no port directory for VAIOS_PORT='${VAIOS_PORT}' "
    "(expected ${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}/).")
endif()

file(GLOB_RECURSE PORTABLE_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/${VAIOS_PORT}/*.s")

# The SVC dispatch (v_syscall_dispatch) is portable C that happens to live under
# cortex-m4/. The host port needs it for SVC too; pull it in (it compiles to
# nothing when VAIOS_SYSCALL_SVC is off, being wholly #if-guarded).
if(PORT_IS_HOSTED)
  list(APPEND PORTABLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cortex-m4/syscall.c")
endif()

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

