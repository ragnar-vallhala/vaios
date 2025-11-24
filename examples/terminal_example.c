#include "vaios.h"
#include "task.h"
#include "memory.h"
#include "terminal.h"

int main()
{
    v_init();
    terminal_init();
    v_heap_memory_init();
    scheduler_init();
    uint32_t t2 = task_create(terminal_run, NULL, 1024, 0);
    scheduler_start();
    while (1)
        ;
}