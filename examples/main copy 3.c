#include "task.h"
#include "structures.h"
#include "memory.h"
#include "utils.h"
#include <stdint.h>

extern void task_exit(void);
extern TCB *ready_queue;
// Dummy task functions
void task_fn1(void *arg) {}
void task_fn2(void *arg) {}
void task_fn3(void *arg) {}

// Verify stack pointer alignment & initial context
void check_task_stack(TCB *task) {
    if (!task) return;

    v_log(LOG_INFO, "Checking Task %d stack pointer...", task->task_id);

    // Check 8-byte alignment
    if (((uintptr_t)task->stack_ptr & 0x7) != 0) {
        v_log(LOG_ERROR, "Task %d stack pointer NOT 8-byte aligned: 0x%x",
              task->task_id, (unsigned)task->stack_ptr);
    } else {
        v_log(LOG_INFO, "Task %d stack pointer is 8-byte aligned: 0x%x",
              task->task_id, (unsigned)task->stack_ptr);
    }

    uint32_t *sp = task->stack_ptr;

    // Hardware-saved registers (PSP context)
    uint32_t r0   = sp[0];
    uint32_t r1   = sp[1];
    uint32_t r2   = sp[2];
    uint32_t r3   = sp[3];
    uint32_t r12  = sp[4];
    uint32_t lr   = sp[5];
    uint32_t pc   = sp[6];
    uint32_t xpsr = sp[7];

    v_log(LOG_INFO, "Task %d initial context:", task->task_id);
    v_log(LOG_INFO, "  xPSR=0x%x, PC=0x%x, LR=0x%x", xpsr, pc, lr);
    v_log(LOG_INFO, "  R12=0x%x, R3=0x%x, R2=0x%x, R1=0x%x, R0=0x%x",
          r12, r3, r2, r1, r0);

    // R4-R11 callee-saved
    int ok = 1;
    for (int i = 8; i < 16; i++) {
        if (sp[i] != 0) { ok = 0; break; }
    }
    v_log(LOG_INFO, "Callee-saved registers R4-R11 %s",
          ok ? "initialized to 0" : "incorrect values");

    // Check R0 = arg, PC = entry, LR = task_exit
    if (r0 != (uint32_t)task->arg) {
        v_log(LOG_ERROR, "R0 != task->arg (0x%x != 0x%x)", r0, (unsigned)task->arg);
    }
    if ((pc & ~1) != ((uint32_t)task->entry & ~1)) {
        v_log(LOG_ERROR, "PC != task->entry (0x%x != 0x%x)", pc, (unsigned)task->entry);
    }
    if ((lr & ~1) != ((uint32_t)task_exit & ~1)) {
        v_log(LOG_ERROR, "LR != task_exit (0x%x != 0x%x)", lr, (unsigned)task_exit);
    }
}

// Print queue for verification
void print_queue(TCB *head, const char *name) {
    if (!head) {
        v_log(LOG_INFO, "%s is empty", name);
        return;
    }
    v_log(LOG_INFO, "Queue %s contents:", name);
    TCB *curr = head;
    do {
        v_log(LOG_INFO, "  Task id=%d, priority=%d, state=%d", curr->task_id, curr->priority, curr->state);
        curr = curr->next;
    } while (curr != head);
}

// Test v_task_create end-to-end
void test_task_create() {
    v_log(LOG_INFO, "== Test: v_task_create ==");

    // Create tasks
    uint32_t t1_id = v_task_create(task_fn1, (void*)0x1111AAAA, 1, 128);
    uint32_t t2_id = v_task_create(task_fn2, (void*)0x2222BBBB, 3, 256);
    uint32_t t3_id = v_task_create(task_fn3, (void*)0x3333CCCC, 2, 192);

    // Verify ready_queue insertion order (priority sorted)
    print_queue(ready_queue, "Ready Queue after creation");

    // Verify stack for each task
    TCB *curr = ready_queue;
    do {
        check_task_stack(curr);
        curr = curr->next;
    } while (curr != ready_queue);

    // Cleanup
    TCB *tmp;
    while ((tmp = task_dequeue(&ready_queue)) != NULL) {
        v_free(tmp->stack_allocation);
        v_free(tmp);
    }

    v_log(LOG_INFO, "== v_task_create Test Completed ==");
}

// Main
int main(void) {
    v_init();
    heap_memory_init();

    test_task_create();

    return 0;
}
