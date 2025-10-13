FILE(GLOB_RECURSE VAIOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_library(vaios STATIC ${VAIOS_SOURCES})
if(NAVHAL)
target_link_libraries(vaios PUBLIC hal -lc -lgcc portable)
target_include_directories(vaios PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../extern/NavHAL/include)
else()
target_link_libraries(vaios PUBLIC -lgcc -lc portable)
endif()
