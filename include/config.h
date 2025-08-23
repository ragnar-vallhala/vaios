#ifndef VAIOS_CONFIG_H
#define VAIOS_CONFIG_H

// Init settings
#define SYSTICK_PERIOD 1000
#define UART_LOGGING_ENABLE 1
#define UART_BAUDRATE 9600
#define LOGGING_ENABLED 1

// Scheduling
#define TIME_SLICE 10

// Memory
#define MAIN_STACK_SIZE 128
#define HEAP_SIZE 4096
#endif // !VAIOS_CONFIG_H
