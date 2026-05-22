FILE(GLOB_RECURSE VAIOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

if(NAVHAL)
# Remove QEMU interrupt stubs when building with real HAL
list(FILTER VAIOS_SOURCES EXCLUDE REGEX "qemu/.*" )
endif()

# Drop optional module sources when their CMake option is OFF (see top-level
# CMakeLists.txt). Excluding the .c keeps it out of flash entirely.
if(NOT VAIOS_MODULE_TERMINAL)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "terminal\\.c$")
endif()
if(NOT VAIOS_MODULE_VFS)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "vfs\\.c$")
endif()
if(NOT VAIOS_MODULE_SEMIHOSTING)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "semihosting\\.c$")
endif()
if(NOT VAIOS_MODULE_FIFO)
  list(FILTER VAIOS_SOURCES EXCLUDE REGEX "structure\\.c$")
endif()

add_library(vaios STATIC ${VAIOS_SOURCES})

# Surface each module switch to C code as a 1/0 macro so init calls and APIs
# can be #if'd out to match the gated source list.
target_compile_definitions(vaios PUBLIC
  VAIOS_MODULE_TERMINAL=$<BOOL:${VAIOS_MODULE_TERMINAL}>
  VAIOS_MODULE_VFS=$<BOOL:${VAIOS_MODULE_VFS}>
  VAIOS_MODULE_SEMIHOSTING=$<BOOL:${VAIOS_MODULE_SEMIHOSTING}>
  VAIOS_MODULE_FIFO=$<BOOL:${VAIOS_MODULE_FIFO}>
)
if(NAVHAL)
target_link_libraries(vaios PUBLIC -Wl,--start-group hal common -Wl,--end-group -lc -lgcc portable)
target_include_directories(vaios PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../extern/NavHAL/include)
else()
target_link_libraries(vaios PUBLIC -lgcc -lc portable)
endif()
