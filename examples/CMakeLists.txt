include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
message(STATUS "Startup File: ${STARTUP_FILE}")

if(NOT NAVHAL)
  set(STARTUP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../startup.s)
endif()
add_executable(main uart_comm.c ${STARTUP_FILE})

if(NAVHAL)
target_link_libraries(main PRIVATE
    hal
    -lgcc
    vaios
)
else()
target_link_libraries(main PRIVATE
    -lgcc
    vaios
)
endif()

add_custom_target(flash
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:main> ${CMAKE_CURRENT_BINARY_DIR}/main.bin
  COMMAND ${FLASHER} write ${CMAKE_CURRENT_BINARY_DIR}/main.bin ${FLASH_ADDRESS}
  DEPENDS main
  COMMENT "Converting ELF to BIN and flashing to board"
)

message(STATUS "TARGET File ${CMAKE_CURRENT_BINARY_DIR}/main.bin")
# elf file size calculation
add_custom_command(
  TARGET main
  POST_BUILD
  COMMAND ${CMAKE_BINARY_SIZE} $<TARGET_FILE:main>
  COMMENT "Calculating size of elf file")
