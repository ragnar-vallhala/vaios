#ifndef VAIOS_TASK_H
#define VAIOS_TASK_H
#include "structures.h"
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
