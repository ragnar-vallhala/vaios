rm -rf build
mkdir build
cd build
cmake .. -DNAVHAL=ON
cmake --build .
qemu-system-arm -M olimex-stm32-h405 -cpu cortex-m4 -kernel examples/main -nographic -d unimp,guest_errors -semihosting
