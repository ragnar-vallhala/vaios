include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
message(STATUS "Startup File: ${STARTUP_FILE}")

if(NOT NAVHAL)
  set(STARTUP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../portable/cortex-m4/startup.s)
endif()

if(VAIOS_EXAMPLE STREQUAL "DMA_DUMP")
  add_executable(main 19_dma_dump.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "FPU_USAGE")
  add_executable(main 18_fpu_usage.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "HEAP_ALLOCATOR")
  add_executable(main 13_heap_alloc_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "BASIC_TASK")
  add_executable(main 02_increment.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "CTX_SWITCH_SMOKE")
  add_executable(main 03_ctx_switch_smoke.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "CLOCK_BOOT_SMOKE")
  add_executable(main 01_clock_boot_smoke.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "IPC_TEST")
  add_executable(main 10_ipc_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "RACE_CONDITION")
  add_executable(main 08_main_race_cond.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MULTI_TASK")
  add_executable(main 05_main_task.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STARVATION")
  add_executable(main 06_starvation.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "SWITCHING")
  add_executable(main 04_switching.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "TERMINAL")
  add_executable(main 32_terminal_example.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "UART")
  add_executable(main 20_uart_comm.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MEM_LEAK")
  add_executable(main 14_mem_leak.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STACK_OVERFLOW")
  add_executable(main 21_stack_overflow.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MPU_FAULT")
  add_executable(main 22_mpu_fault.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "DEVFS_TEST")
  add_executable(main 23_devfs_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "FD_IPC_TEST")
  add_executable(main 25_fd_ipc_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "FD_WAIT_TEST")
  add_executable(main 26_fd_wait_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "TASK_HEAP_TEST")
  add_executable(main 15_task_heap_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MPU_USER_DEMO")
  add_executable(main 33_mpu_user_demo.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STAGE5_LIFECYCLE")
  add_executable(main 34_stage5_lifecycle.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STAGE5_FDIPC")
  add_executable(main 35_stage5_fdipc.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "COVERAGE_SCHED")
  add_executable(main 98_coverage_sched.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "KMSG_TEST")
  add_executable(main 24_kmsg_test.c ${STARTUP_FILE})
  # Probe select: 0=W^X (default), 1=NULL guard, 2=flash-RO. -DMPU_FAULT_MODE=N
  if(DEFINED MPU_FAULT_MODE)
    target_compile_definitions(main PRIVATE MPU_FAULT_MODE=${MPU_FAULT_MODE})
  endif()
elseif(VAIOS_EXAMPLE STREQUAL "MEM_STRESS")
  add_executable(main 16_mem_stress.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MEM_FRAG")
  add_executable(main 17_mem_frag.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "BENCHMARK")
  add_subdirectory(benchmark)
elseif(VAIOS_EXAMPLE STREQUAL "BLOCKING")
  add_executable(main 11_block_wake_task.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "VFS_CONCURRENT")
  add_executable(main 28_vfs_concurrent.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "VFS_PERF")
  add_executable(main 29_vfs_perf.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "SD_WRITE_TEST")
  add_executable(main 27_writing_sd_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "PRIORITY_INVERSION")
  add_executable(main 07_priority_inversion.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "RACE_MUTEX_GUARD")
  add_executable(main 09_race_mutex_guard.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "FIFO_TEST")
  add_executable(main 12_fifo_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "PERF")
  add_executable(main 30_perf_example.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "PERF_DEMO")
  add_executable(main 31_perf_demo.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "UNIT_TESTS")
  # On-target (PIL) runner: link the host unit-test suites against the real
  # kernel + port. Curated to suites that need no scheduler / SD / DWT.
  add_executable(main
      99_unit_tests.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/framework.c  # counters + suite runners
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_memory.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_structure.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_utils.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_uaccess.c
      ${STARTUP_FILE})
  target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
endif()

if(TARGET main)
  # The SVC dispatch (portable/syscall.c) calls kernel functions (ipc, devfs),
  # and the kernel calls back into portable — a static-lib cycle. RESCAN so the
  # linker resolves the cross-references regardless of order, even for examples
  # that don't otherwise pull a syscall's target (e.g. MULTI_TASK vs sems).
  target_link_libraries(main PRIVATE
      -lgcc
      -lm
      "$<LINK_GROUP:RESCAN,vaios,portable>"
  )

  add_custom_target(flash
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:main> ${CMAKE_CURRENT_BINARY_DIR}/main.bin
    COMMAND ${FLASHER} --connect-under-reset write ${CMAKE_CURRENT_BINARY_DIR}/main.bin ${FLASH_ADDRESS}
    DEPENDS main
    COMMENT "Converting ELF to BIN and flashing to board"
  )

  # elf file size calculation
  add_custom_command(
    TARGET main
    POST_BUILD
    COMMAND ${CMAKE_BINARY_SIZE} $<TARGET_FILE:main>
    COMMENT "Calculating size of elf file")
endif()

