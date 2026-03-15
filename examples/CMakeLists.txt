include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
message(STATUS "Startup File: ${STARTUP_FILE}")

if(NOT NAVHAL)
  set(STARTUP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../startup.s)
endif()

if(VAIOS_EXAMPLE STREQUAL "DMA_DUMP")
  add_executable(main dma_dump.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "FPU_USAGE")
  add_executable(main fpu_usage.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "HEAP_ALLOCATOR")
  add_executable(main heap_alloc_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "BASIC_TASK")
  add_executable(main increment.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "IPC_TEST")
  add_executable(main ipc_test.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "RACE_CONDITION")
  add_executable(main main_race_cond.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MULTI_TASK")
  add_executable(main main_task.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STARVATION")
  add_executable(main starvation.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "SWITCHING")
  add_executable(main switching.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "TERMINAL")
  add_executable(main terminal_example.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "UART")
  add_executable(main uart_comm.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "MEM_LEAK")
  add_executable(main mem_leak.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "STACK_OVERFLOW")
  add_executable(main stack_overflow.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "BENCHMARK")
  add_subdirectory(benchmark)
elseif(VAIOS_EXAMPLE STREQUAL "BLOCKING")
  add_executable(main block_wake_task.c ${STARTUP_FILE})
elseif(VAIOS_EXAMPLE STREQUAL "VFS_CONCURRENT")
  add_executable(main vfs_concurrent.c ${STARTUP_FILE})
endif()

if(TARGET main)
  if(NAVHAL)
    target_link_libraries(main PRIVATE
        -lgcc
        -lm
        vaios
    )
  else()
    target_link_libraries(main PRIVATE
        -lgcc
        -lm
        vaios
    )
  endif()

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

