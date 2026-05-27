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

// Critical-section masking strategy. When 1, v_enter_critical / v_exit_critical
// use BASEPRI so IRQs with priority numerically below MAX_SYSCALL_INTERRUPT_-
// PRIORITY (i.e. more urgent — motor PWM, IMU EXTI, ESC telemetry) stay
// unmaskable. When 0, fall back to global cpsid i / cpsie i.
//
// Required NVIC priority bands when running with BASEPRI:
//   0  .. (MAX_SYSCALL_INTERRUPT_PRIORITY >> (8-__NVIC_PRIO_BITS)) - 1
//     Unmaskable. Must NOT call any vaios API. Reserve for hard-real-time
//     hardware service (motor PWM, IMU EXTI, ESC telemetry).
//   MAX_SYSCALL_INTERRUPT_PRIORITY >> (8-__NVIC_PRIO_BITS) .. 14
//     Maskable by critical sections. May call *_from_isr APIs.
//   15 (lowest)
//     PendSV and SysTick.
#ifndef VAIOS_USE_BASEPRI
#define VAIOS_USE_BASEPRI 1
#endif

// Optional kernel modules. The CMake build owns these (option() in the
// top-level CMakeLists.txt) — it both excludes the module's .c file and
// passes -DVAIOS_MODULE_X=0/1. These #ifndef fallbacks only apply when a
// translation unit is compiled outside that CMake; they also document the
// knobs. Set a module to 0 to drop it from the build and #if out its API.
#ifndef VAIOS_MODULE_TERMINAL
#define VAIOS_MODULE_TERMINAL 1
#endif
#ifndef VAIOS_MODULE_VFS
#define VAIOS_MODULE_VFS 1
#endif
#ifndef VAIOS_MODULE_SEMIHOSTING
#define VAIOS_MODULE_SEMIHOSTING 1
#endif
#ifndef VAIOS_MODULE_FIFO
#define VAIOS_MODULE_FIFO 1
#endif
#ifndef VAIOS_MODULE_PERF
#define VAIOS_MODULE_PERF 1
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

// Maximum depth the transitive priority-inheritance chain walk will follow
// before panicking. A chain deeper than this is almost certainly a
// dependency cycle (deadlock) rather than legitimate nesting.
#ifndef MAX_PI_DEPTH
#define MAX_PI_DEPTH 4
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

// Kernel hot-path log gate. See V_KLOG in utils.h. Setting this to LOG_ERROR
// or LOG_FATAL drops every DEBUG/INFO/WARN call site in memory.c, task.c,
// ipc.c at compile time — required for flight builds to meet the latency
// budget.
#ifndef VAIOS_KERNEL_LOG_LEVEL
#define VAIOS_KERNEL_LOG_LEVEL LOG_WARN
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
