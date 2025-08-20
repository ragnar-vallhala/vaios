cmake_minimum_required(VERSION 3.20)

project(vaios C ASM)

set(CMAKE_C_STANDARD 11)

add_subdirectory(kernel) # The heart of your RTOS (scheduling, tasks, synchronization, etc.) — totally architecture independent
add_subdirectory(portable) # implementations for specific architectures (context switching, interrupt handling)
add_subdirectory(examples) # Small test apps showing how to use the RTOS


if(TEST)
  enable_testing()
  add_subdirectory(tests) # For unit tests 
endif()
