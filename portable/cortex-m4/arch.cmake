# Cortex-M4 port build description.
#
# Included from the root CMakeLists BEFORE project() so it can set the cross
# toolchain, and the single place the arch's compiler/assembler identity lives.
# Adding a port means adding portable/<port>/arch.cmake with these variables set;
# the root CMakeLists and examples/ consume them and never hardcode -mcpu / a
# toolchain prefix / a startup path.
#
# What is deliberately NOT here: the linker script and the float ABI. On this
# project both are owned by layers below the arch — the linker script is a
# per-BOARD file under extern/NavHAL/src/board/<board>/, and the FPU flags are
# derived from the (NavHAL) config, not the CPU. The root CMakeLists keeps those
# and appends them to PORT_ARCH_FLAGS.

set(PORT_TOOLCHAIN_PREFIX "arm-none-eabi-")

# Pure-arch compiler/assembler flags (no FPU ABI — the root adds that from the
# config). -mcpu names this port's core; -mthumb selects the Thumb ISA.
set(PORT_ARCH_FLAGS "-mcpu=cortex-m4 -mthumb")

# Reset/vector startup translation unit, linked into firmware and examples.
set(PORT_STARTUP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/startup.s")
