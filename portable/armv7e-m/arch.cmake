# ARMv7E-M port build description (Cortex-M4 and Cortex-M7).
#
# Included from the root CMakeLists BEFORE project() so it can set the cross
# toolchain, and the single place the port's compiler/assembler identity lives.
# Adding a port means adding portable/<port>/arch.cmake with these variables set;
# the root CMakeLists and examples/ consume them and never hardcode a toolchain
# prefix or a startup path.
#
# The port is named for the ISA because that is the granularity at which its
# sources are actually true: the context-switch asm, the MPU handling and the
# SysTick/PendSV/SVC vectors are identical on an M4 and an M7, and the FPU save
# covers s16-s31 either way (fpv5-d16 on the M7 is still sixteen doubles, so
# still s0-s31). What differs between the two cores is -mcpu and the -mfpu unit
# name, neither of which is a source-level fact.
#
# What is deliberately NOT here: the linker script, the float ABI, and — on a
# NAVHAL build — the core. The linker script is a per-BOARD file under
# extern/NavHAL/src/board/<board>/; the FPU flags come from NavHAL's own
# cmake/arch/<ISA>.cmake, which owns the fpv4-sp-d16 / fpv5-d16 branch; and the
# core is whatever NavHAL's Kconfig resolved. The root CMakeLists appends all
# three once NavHAL has been asked.

set(PORT_TOOLCHAIN_PREFIX "arm-none-eabi-")

# Pure-arch compiler/assembler flags (no FPU ABI — the root adds that from the
# config). -mthumb selects the Thumb ISA.
#
# The -mcpu here is only the standalone fallback, for a build with no NavHAL to
# ask (QEMU/Renode). On a NAVHAL build the root CMakeLists overwrites this with
# the core NavHAL resolved, which is what lets an F767 config build without a
# second port directory.
set(PORT_ARCH_FLAGS "-mcpu=cortex-m4 -mthumb")

# Reset/vector startup translation unit, linked into firmware and examples.
set(PORT_STARTUP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/startup.s")
