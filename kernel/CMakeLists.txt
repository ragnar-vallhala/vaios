FILE(GLOB_RECURSE VAIOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

if(NAVHAL)
# Remove QEMU interrupt stubs when building with real HAL
list(FILTER VAIOS_SOURCES EXCLUDE REGEX "qemu/.*" )
endif()

add_library(vaios STATIC ${VAIOS_SOURCES})
if(NAVHAL)
target_link_libraries(vaios PUBLIC -Wl,--start-group hal common -Wl,--end-group -lc -lgcc portable)
target_include_directories(vaios PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../extern/NavHAL/include)
else()
target_link_libraries(vaios PUBLIC -lgcc -lc portable)
endif()
