cmake_minimum_required(VERSION 3.20)

# Include the parent VAIOS project's toolchain (already set up)
# This CMakeLists is intended to be compiled via the parent with -DEXAMPLES=ON

# -----------------------------------------------------------------------
# Source files for the benchmark project
# -----------------------------------------------------------------------
set(BENCH_SRCS
    main.c
    bench_fpu.c
    bench_dma.c
    bench_tasks.c
    bench_ipc.c
    bench_memory.c
    bench_stress.c
    bench_bus.c
)

add_executable(benchmark ${BENCH_SRCS} ${STARTUP_FILE})

# The Bus IPC suite alone (plan B8). The full set takes far longer than one
# emulator run is worth, and these are the numbers the plan asks to be committed.
# A CMake option, not -DCMAKE_C_FLAGS: the root CMakeLists overwrites that.
option(VAIOS_BENCH_ONLY_BUS "Run only the Bus IPC benchmark suite" OFF)
if(VAIOS_BENCH_ONLY_BUS)
  target_compile_definitions(benchmark PRIVATE VAIOS_BENCH_ONLY_BUS=1)
  message(STATUS "benchmark: Bus IPC suite only")
endif()

# Include the benchmark's own directory for benchmark.h
target_include_directories(benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# -----------------------------------------------------------------------
# Link against vaios (always) and NavHAL (when -DNAVHAL=ON)
# -----------------------------------------------------------------------
if(NAVHAL)
    target_link_libraries(benchmark PRIVATE
        hal
        vaios
        -lgcc
        -lm
    )
else()
    target_link_libraries(benchmark PRIVATE
        vaios
        -lgcc
        -lm
    )
endif()

# -----------------------------------------------------------------------
# Enable DMA driver — required for the DMA benchmarks
# When building with NavHAL, pass _DMA_ENABLED so both NavHAL's dma.c
# and our bench_dma.c are compiled with DMA support.
# -----------------------------------------------------------------------
if(NAVHAL)
    target_compile_definitions(benchmark PRIVATE _DMA_ENABLED)
endif()

# -----------------------------------------------------------------------
# Flash target: objcopy → bin → st-flash
# -----------------------------------------------------------------------
add_custom_target(flash
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:benchmark>
            ${CMAKE_CURRENT_BINARY_DIR}/benchmark.bin
    COMMAND ${FLASHER} --connect-under-reset write
            ${CMAKE_CURRENT_BINARY_DIR}/benchmark.bin
            ${FLASH_ADDRESS}
    DEPENDS benchmark
    COMMENT "Flashing benchmark to board"
)

# -----------------------------------------------------------------------
# Print ELF size after every build
# -----------------------------------------------------------------------
add_custom_command(
    TARGET benchmark
    POST_BUILD
    COMMAND ${CMAKE_BINARY_SIZE} $<TARGET_FILE:benchmark>
    COMMENT "Benchmark ELF size:"
)

message(STATUS "Benchmark target configured: ${CMAKE_CURRENT_BINARY_DIR}/benchmark")
