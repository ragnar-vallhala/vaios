#ifndef VAIOS_CORTEX_M4_CONFIG_EXAMPLE_H
#define VAIOS_CORTEX_M4_CONFIG_EXAMPLE_H

// Version settings
#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define VERSION_PATCH 0
#define VERSION "0.1.0"
#define AUTHOR "ASHUTOSH VISHWAKARMA"
// Pull in NAVHAL configs
#include "navhal.h"

// Init settings
#ifndef CORTEX_M4
#define CORTEX_M4
#endif

#define SYSTICK_PERIOD 1000 // in microseconds
#define UART_LOGGING_ENABLE 1
#define UART_BAUDRATE 115200
#define LOGGING_ENABLED 1

// Interrupts
#define __NVIC_PRIO_BITS 4
#define MAX_SYSCALL_INTERRUPT_PRIORITY (7 << (8 - __NVIC_PRIO_BITS))
#define DMA_MIN_THRESHOLD 16

// Scheduling
#define TIME_SLICE 2

// Memory
#define MAIN_STACK_SIZE 512
#define HEAP_SIZE 0x8000
#define STACK_ALIGN_SIZE 8
#define HEAP_WATERMARK_ENABLE 1 // 0: disable, 1: enable
#define HEAP_WATERMARK_THRESHOLD                                               \
  1024 // panics if heap free memory is less than this threshold

// Tasks
#define MAX_TASK_PRIORITY 7
#define IDLE_TASK_PRIORITY 0
#define IDLE_TASK_STACK_SIZE 2048
#define TASK_STACK_WATERMARK_ENABLE 1 // enable 1, disable 0
#define TASK_STACK_OVERFLOW_THRESHOLD                                          \
  64 // panics if left free stack size is less than this threshold

// IPC
#define MAX_SEMAPHORE_COUNT 0xFFFFFFFF
#define STATIC_SEMAPHORE_SIZE 32

// Logging
#define COLOR_LOGGING 1
#define LOG_BUFFER_SIZE 32 // number of log entries
#define LOG_MSG_MAX_LEN 64 // max chars per message
#define LOG_BUFFER_STORAGE_SIZE                                                \
  256                           // size of the log buffer
                                // storage in bytes (use 2 buffer for
                                // double buffering & couple with DMA)
#define BUFFERED_LOGGING 1      // 0: disable, 1: enable
#define MIN_LOG_LEVEL LOG_TRACE // Minimum log level to output
// #define ALLOWED_MODULES "TERM,VAIOS INIT,MEMORY,TASK"   // Comma-separated
// list of modules to log from, or "ALL"
// #define ALLOWED_MODULES "TERM,ATTITUDE,VAIOS INIT,BMX160" // Comma-separated
// list of modules to log from, or "ALL"
#define ALLOWED_MODULES "ALL"
// Terminal
#define ENABLE_TERMINAL 1                // 0: disable, 1: enable
#define TERMINAL_LOG_LEVEL MIN_LOG_LEVEL // Loglevel of terminal lines
#define CMD_BUFFER_SIZE 10               // Maximum buffer size in history
#define CMD_MAX_LEN 32                   // maximum length of one command
#define MAX_CMD_NUMBER 6                 // maximum number of commands defined
#define ESCAPE_SEQ_LEN 2                 // \r, \n or \r\n length
#define TERMINAL_TASK_STACK_SIZE 1024

// General macros
#if defined(NAVHAL) && defined(CORTEX_M4)
#define PANIC(x) uart2_write(x);
#else
#define PANIC(x) sh_write0(x);
#endif

#endif // !VAIOS_CORTEX_M4_CONFIG_EXAMPLE_H