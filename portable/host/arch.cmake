# Host (POSIX) port build description. Unlike a bare-metal port, this builds a
# NATIVE executable that runs the scheduler on the host: native toolchain, no
# cross flags, no reset vector, no linker script. PORT_IS_HOSTED tells the root
# CMakeLists to skip the ARM-only linker script / --specs / FPU flags and to
# leave CMAKE_SYSTEM_NAME at its native default.

set(PORT_TOOLCHAIN_PREFIX "")  # native gcc / clang
set(PORT_ARCH_FLAGS "")        # no -mcpu/-mthumb
set(PORT_STARTUP_SOURCE "")    # no startup vector
set(PORT_IS_HOSTED TRUE)
