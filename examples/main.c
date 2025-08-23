#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

// Dummy tasks
void task1(void *arg) { v_log(LOG_INFO, "Task 1 running"); }
void task2(void *arg) { v_log(LOG_INFO, "Task 2 running"); }
void task3(void *arg) { v_log(LOG_INFO, "Task 3 running"); }

extern TCB *ready_queue;

int main() {
    v_init();
    memory_init();

    // Allocate TCBs on heap
    TCB *t1 = (TCB *)v_malloc(sizeof(TCB));
    TCB *t2 = (TCB *)v_malloc(sizeof(TCB));
    TCB *t3 = (TCB *)v_malloc(sizeof(TCB));

    if (t1) {
        t1->task_id = 1;
        t1->entry = task1;
        t1->priority = 5;
        t1->state = TASK_READY;
        task_enqueue(&ready_queue, t1);
    }

    if (t2) {
        t2->task_id = 2;
        t2->entry = task2;
        t2->priority = 10;
        t2->state = TASK_READY;
        task_enqueue(&ready_queue, t2);
    }

    if (t3) {
        t3->task_id = 3;
        t3->entry = task3;
        t3->priority = 7;
        t3->state = TASK_READY;
        task_enqueue(&ready_queue, t3);
    }

    v_log(LOG_INFO, "Enqueued 3 tasks into ready_queue");

    // Dequeue and run one task
    TCB *next_task = task_dequeue(&ready_queue);
    if (next_task && next_task->entry)
        next_task->entry(NULL);

    while (1)
        ;
}

