#ifndef VAIOS_CORTEX_M4_CONFIG_H
#define VAIOS_CORTEX_M4_CONFIG_H

// Init settings
#define CORTEX_M4
#define SYSTICK_PERIOD 1000 // in microseconds
#define UART_LOGGING_ENABLE 1
#define UART_BAUDRATE 9600
#define LOGGING_ENABLED 1

// Interrupts
#define __NVIC_PRIO_BITS 4
#define MAX_SYSCALL_INTERRUPT_PRIORITY (7 << (8 - __NVIC_PRIO_BITS))

// Scheduling
#define TIME_SLICE 10

// Memory
#define MAIN_STACK_SIZE 128
#define HEAP_SIZE 0x8000
#define STACK_ALIGN_SIZE 8

// Tasks
#define MAX_TASK_PRIORITY 7
#define IDLE_TASK_PRIORITY 0
#define IDLE_TASK_STACK_SIZE 1024

// IPC
#define MAX_SEMAPHORE_COUNT 0xFFFFFFFF
#define STATIC_SEMAPHORE_SIZE 32

// Logging
#define LOG_BUFFER_SIZE 64       // number of log entries
#define LOG_MSG_MAX_LEN 64       // max chars per message
#define BUFFERED_LOGGING 1       // 0: disable, 1: enable
#define MIN_LOG_LEVEL LOG_TRACE  // Minimum log level to output
#define ALLOWED_MODULES "ALL" // Comma-separated list of modules to log from, or "ALL"

#endif // !VAIOS_CORTEX_M4_CONFIG_H
