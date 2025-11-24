#include "vaios.h"
#include "task.h"
#include "memory.h"
#include "terminal.h"
#include "utils.h"
void inf(void *args)
{
    while (1)
    {
        v_log(LOG_ERROR, "Running");
    }
}
int main()
{
    v_init();
    terminal_init();
    register_command(inf, "inf");
    v_heap_memory_init();
    scheduler_init();
    uint32_t t2 = task_create(terminal_run, NULL, 1024, 0);
    scheduler_start();
    while (1)
        ;
}