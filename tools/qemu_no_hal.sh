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

# Anchor everything to the repo root so the build dir and the log path resolve
# the same way regardless of where the script is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# Example to build. FIFO_TEST runs cleanly on the netduinoplus2 QEMU model.
VAIOS_EXAMPLE=${VAIOS_EXAMPLE:-FIFO_TEST}

# Default log path if not provided. Make it absolute (anchored at the repo
# root) so it still resolves after we `cd build`; a relative override is taken
# relative to the repo root. logs/ is .gitignored, so create it.
LOG_FILE=${1:-$REPO_ROOT/logs/run_$(date +"%Y-%m-%d_%H:%M:%S").log}
case "$LOG_FILE" in /*) ;; *) LOG_FILE="$REPO_ROOT/$LOG_FILE" ;; esac
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
