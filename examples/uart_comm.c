#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#define CORTEX_M4
#include "navhal.h"
#include "vaios.h"
#include "utils.h"
#include "memory.h"
#include "task.h"

char msg[100];
int available = 0;
void recieve()
{
    while (1)
    {
        if (uart2_available())
        {
            v_log(LOG_WARN, "Recieved msg");
            int n = uart2_read_until(msg, 100, '\n');
            msg[n + 1] = '\0';
            available = 1;
        }
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
        }
    }
}
int main()
{
    msg[99] = '\0';
    v_init();
    v_heap_memory_init();
    scheduler_init();
    uint32_t t1 = task_create(recieve, NULL, 1024, 0);
    uint32_t t2 = task_create(send, NULL, 1024, 0);
    scheduler_start();
    while (1)
        ;
}
