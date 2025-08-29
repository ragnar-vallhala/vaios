#!/bin/bash

# Usage: ./run_qemu.sh <log_file>
LOG_FILE=${1:-../tools/alloc_logs.txt}  # default if not provided

# Clean and build
rm -rf build
mkdir build
cd build
cmake .. -DNAVHAL=OFF
cmake --build .

# Run QEMU, output to both terminal and log file
qemu-system-arm -M olimex-stm32-h405 \
  -cpu cortex-m4 \
  -kernel examples/main \
  -nographic \
  -s -S\
  -d unimp,guest_errors \
  -semihosting 2>&1 | tee "$LOG_FILE"

