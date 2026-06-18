#!/bin/bash
# =============================================================================
# tools/qemu_no_hal.sh — build a no-HAL example and run it under QEMU.
#
# QEMU's -kernel needs an ELF, and the examples/ build only emits the `main`
# target when VAIOS_EXAMPLE names a known example (see examples/CMakeLists.txt).
# A bare `cmake -DNAVHAL=OFF -DEXAMPLES=ON` therefore builds the libs but NO
# `examples/main`, and QEMU then has nothing to load. So we always pass an
# example; override it with the VAIOS_EXAMPLE env var.
#
# Usage:
#   tools/qemu_no_hal.sh [log_file]
#   VAIOS_EXAMPLE=PERF_DEMO tools/qemu_no_hal.sh
# =============================================================================
set -euo pipefail

# Example to build. FIFO_TEST runs cleanly on the netduinoplus2 QEMU model.
VAIOS_EXAMPLE=${VAIOS_EXAMPLE:-FIFO_TEST}

# Default log path if not provided. Ensure the directory exists so `tee`
# does not fail when ../logs is absent (it is .gitignored / not checked in).
LOG_FILE=${1:-../logs/run_$(date +"%Y-%m-%d_%H:%M:%S").log}
mkdir -p "$(dirname "$LOG_FILE")"

# Clean and build
rm -rf build
mkdir build
cd build
cmake .. -DNAVHAL=OFF -DEXAMPLES=ON -DVAIOS_EXAMPLE="$VAIOS_EXAMPLE"
cmake --build .
mv compile_commands.json ../compile_commands.json
# Run QEMU, output to both terminal and log file. stdbuf line-buffers tee so
# the kernel's output streams live when stdout is a pipe (e.g. `docker run`)
# instead of being held in tee's block buffer.
qemu-system-arm -M netduinoplus2 \
  -cpu cortex-m4 \
  -kernel examples/main \
  -nographic \
  -d unimp,guest_errors \
  -semihosting 2>&1 | stdbuf -oL tee "$LOG_FILE"
