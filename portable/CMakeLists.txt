set(PORTABLE_DIR "")

if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "cortex-m4")
  set(PORTABLE_DIR "cortex-m4")
endif()

FILE(GLOB_RECURSE PORTABLE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${PORTABLE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/${PORTABLE_DIR}/*.s)

add_library(portable STATIC ${PORTABLE_SOURCES})

if(NAVHAL)
  # port.c pulls in NavHAL via navhal.h. NavHAL's M3 restructure moved the
  # vendor register headers (family/*_reg.h) out of the top-level include/
  # tree; they are now reachable only through the hal target's PUBLIC
  # include directories. Link hal so compiling portable inherits them.
  # (vaios/kernel already links hal, but portable is built standalone.)
  target_link_libraries(portable PUBLIC hal)
endif()

