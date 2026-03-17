#ifndef VAIOS_CONFIG_DEFAULT_H
#define VAIOS_CONFIG_DEFAULT_H

// Version settings
#ifndef VERSION_MAJOR
#define VERSION_MAJOR 0
#endif

#ifndef VERSION_MINOR
#define VERSION_MINOR 1
#endif

#ifndef VERSION_PATCH
#define VERSION_PATCH 0
#endif

#ifndef VERSION
#define VERSION "0.1.0"
#endif

#ifndef AUTHOR
#define AUTHOR "ASHUTOSH VISHWAKARMA"
#endif

// Pull in NAVHAL configs
#include "navhal.h"

// Init settings
#ifndef CORTEX_M4
#define CORTEX_M4
#endif

#ifndef SYSTICK_PERIOD
#define SYSTICK_PERIOD 1000 // in microseconds
#endif

#ifndef UART_LOGGING_ENABLE
#define UART_LOGGING_ENABLE 1
#endif

#ifndef UART_BAUDRATE
#define UART_BAUDRATE 115200
#endif

#ifndef LOGGING_ENABLED
#define LOGGING_ENABLED 1
#endif

// Interrupts
#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4
#endif

#ifndef MAX_SYSCALL_INTERRUPT_PRIORITY
#define MAX_SYSCALL_INTERRUPT_PRIORITY (7 << (8 - __NVIC_PRIO_BITS))
#endif

#ifndef DMA_MIN_THRESHOLD
#define DMA_MIN_THRESHOLD 16
#endif

// Scheduling
#ifndef TIME_SLICE
#define TIME_SLICE 2
#endif

// Memory
#ifndef MAIN_STACK_SIZE
#define MAIN_STACK_SIZE 1024
#endif

#ifndef HEAP_SIZE
#define HEAP_SIZE 0x16000 // 88kB
#endif

#ifndef STACK_ALIGN_SIZE
#define STACK_ALIGN_SIZE 8
#endif

#ifndef HEAP_WATERMARK_ENABLE
#define HEAP_WATERMARK_ENABLE 1
#endif

#ifndef HEAP_WATERMARK_THRESHOLD
#define HEAP_WATERMARK_THRESHOLD 1024
#endif

// Tasks
#ifndef MAX_TASK_PRIORITY
#define MAX_TASK_PRIORITY 7
#endif

#ifndef IDLE_TASK_PRIORITY
#define IDLE_TASK_PRIORITY 0
#endif

#ifndef IDLE_TASK_STACK_SIZE
#define IDLE_TASK_STACK_SIZE 2048
#endif

#ifndef TASK_STACK_WATERMARK_ENABLE
#define TASK_STACK_WATERMARK_ENABLE 1
#endif

#ifndef TASK_STACK_OVERFLOW_THRESHOLD
#define TASK_STACK_OVERFLOW_THRESHOLD 64
#endif

// IPC
#ifndef MAX_SEMAPHORE_COUNT
#define MAX_SEMAPHORE_COUNT 0xFFFFFFFF
#endif

#ifndef STATIC_SEMAPHORE_SIZE
#define STATIC_SEMAPHORE_SIZE 32
#endif

// Logging
#ifndef COLOR_LOGGING
#define COLOR_LOGGING 1
#endif

#ifndef LOG_BUFFER_SIZE
#define LOG_BUFFER_SIZE 32
#endif

#ifndef LOG_MSG_MAX_LEN
#define LOG_MSG_MAX_LEN 64
#endif

#ifndef LOG_BUFFER_STORAGE_SIZE
#define LOG_BUFFER_STORAGE_SIZE 256
#endif

#ifndef BUFFERED_LOGGING
#define BUFFERED_LOGGING 1
#endif

#ifndef MIN_LOG_LEVEL
#define MIN_LOG_LEVEL LOG_INFO
#endif

#ifndef ALLOWED_MODULES
#define ALLOWED_MODULES "ALL"
#endif

// Terminal
#ifndef ENABLE_TERMINAL
#define ENABLE_TERMINAL 1
#endif

#ifndef TERMINAL_LOG_LEVEL
#define TERMINAL_LOG_LEVEL MIN_LOG_LEVEL
#endif

#ifndef CMD_BUFFER_SIZE
#define CMD_BUFFER_SIZE 10
#endif

#ifndef CMD_MAX_LEN
#define CMD_MAX_LEN 32
#endif

#ifndef MAX_CMD_NUMBER
#define MAX_CMD_NUMBER 6
#endif

#ifndef ESCAPE_SEQ_LEN
#define ESCAPE_SEQ_LEN 2
#endif

#ifndef TERMINAL_TASK_STACK_SIZE
#define TERMINAL_TASK_STACK_SIZE 1024
#endif

// General macros
#ifndef PANIC
#define PANIC(msg) v_panic(__FILE__, __LINE__, msg)
#endif
#if defined(PANIC) && defined(NAVHAL)
#include "navhal.h"
#endif
#include "utils.h"
#endif // !VAIOS_CONFIG_DEFAULT_H
