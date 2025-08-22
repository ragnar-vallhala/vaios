FILE(GLOB_RECURSE VAIOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_library(vaios STATIC ${VAIOS_SOURCES})
if(NAVHAL)
target_link_libraries(vaios PUBLIC hal)
else()
target_link_libraries(vaios PUBLIC gcc)
endif()
