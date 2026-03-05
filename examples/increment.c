#include "vaios.h"
#include "utils.h"
#include "task.h"
#include "memory.h"

int count=0;

void task1(void *arg){
    while(1){
        count++;
        v_log(LOG_INFO, "Entered task %d, count %d", GET_CURRENT_TASK_ID(), count);
    }
}

int main(){
    v_init();
    v_heap_memory_init();
    scheduler_init();
    task_create(task1,NULL,1024,1);
    task_create(task1,NULL,1024,1);
    scheduler_start();
    while (1)
        ;
}