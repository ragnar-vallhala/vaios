#include "vaios.h"
#include "utils.h"
#include "task.h"
#include "memory.h"

void task1(void *arg){
    v_log(LOG_INFO, "Entered task %d", GET_CURRENT_TASK_ID());
    return;
}

int main(){
    v_init();
    v_heap_memory_init();
    scheduler_init();
    task_create(task1,NULL,600,0);
    task_create(task1,NULL,600,0);
    scheduler_start();
    while (1)
        ;
}