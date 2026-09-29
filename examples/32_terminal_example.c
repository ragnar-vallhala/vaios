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
    vaios_init_config_t cfg = {.internal_clock_setup = 1,
                               .internal_sd_card_setup = 0};
    v_init(&cfg);
    terminal_init();
    register_command(inf, "inf");
    register_command(inf, "inf1");
    v_heap_memory_init();
    scheduler_init();
    uint32_t t2 = task_create(terminal_run, NULL, 1024, 0);
    scheduler_start();
    while (1)
        ;
}