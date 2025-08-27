#ifndef VAIOS_TASK_H
#define VAIOS_TASK_H
#include "structures.h"
#include <stddef.h>
#include <stdint.h>

typedef enum { SCHEDULER_RUNNING, SCHEDULER_STOPPED } Scheduler_Status_Type;

uint32_t v_task_create(void (*entry)(void *), void *args, uint8_t priority,
                       uint32_t stack_size);
TCB *task_get_current(void);
void task_set_current(TCB *task);
void task_enqueue(TCB **head, TCB *task);
void task_get_count(void);
TCB *task_dequeue(TCB **head);
void task_remove(TCB **head, TCB *task);
void task_insert_sorted(TCB **head, TCB *task);
TCB *task_get_next_ready(void);
void task_block(TCB *task);
void task_unblock(TCB *task);
void task_sleep(TCB *task, uint32_t ticks);
#endif // !VAIOS_TASK_H
