rm -rf build
mkdir build
cd build
cmake .. -DNAVHAL=ON
cmake --build .
arm-none-eabi-objcopy -O binary examples/main main.bin
st-flash --connect-under-reset write main.bin 0x8000000
