#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#define CORTEX_M4
#include "navhal.h"
#include "vaios.h"
#include "utils.h"
#include "memory.h"
#include "task.h"

char msg[48];
int ptr = 0;
int available = 0;

void onRecieve()
{

    char c = uart2_read_char();
    msg[ptr++] = c;
    if (c == '\n')
    {
        available = 1;
        msg[ptr] = '\0';
    }
}

void send()
{
    while (1)
    {
        if (available)
        {
            v_log(LOG_INFO, msg);
            available = 0;
            ptr = 0;
        }
    }
}
int main()
{
    msg[47] = '\0';
    v_init();
    hal_interrupt_attach_callback(USART2_IRQn, onRecieve);
    hal_enable_interrupt(USART2_IRQn);
    v_heap_memory_init();
    scheduler_init();
    uint32_t t2 = task_create(send, NULL, 1024, 0);
    scheduler_start();
    while (1)
        ;
}
