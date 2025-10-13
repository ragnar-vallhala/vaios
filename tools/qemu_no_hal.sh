#!/bin/bash

# Usage: ./run_qemu.sh <log_file>
LOG_FILE=${1:-../logs/run_$(date +"%Y-%m-%d_%H:%M:%S").log}  # default if not provided

# Clean and build
rm -rf build
mkdir build
cd build
cmake .. -DNAVHAL=OFF -DEXAMPLES=ON
cmake --build .

# Run QEMU, output to both terminal and log file
qemu-system-arm -M netduinoplus2 \
  -cpu cortex-m4 \
  -kernel examples/main \
  -nographic \
  -d unimp,guest_errors \
  -semihosting 2>&1 | tee "$LOG_FILE"

