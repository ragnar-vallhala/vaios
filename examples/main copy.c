#include "task.h"
#include "structures.h"
#include "memory.h"
#include "utils.h"
#include <stdint.h>

// Dummy v_log implementation (replace with your actual v_log)
extern void task_exit(void);

// Dummy task functions
void task_fn1(void *arg) {}
void task_fn2(void *arg) {}

// Verify stack alignment and initial context
void test_task_stack(TCB *task) {
    if (!task) return;

    // Check 8-byte alignment
    if (((uintptr_t)task->stack_ptr & 0x7) != 0) {
        v_log(LOG_ERROR, "Task %d stack pointer NOT 8-byte aligned: 0x%x",
              task->task_id, (unsigned)task->stack_ptr);
    } else {
        v_log(LOG_INFO, "Task %d stack pointer is 8-byte aligned: 0x%x",
              task->task_id, (unsigned)task->stack_ptr);
    }

    uint32_t *sp = task->stack_ptr;

    // Cortex-M stack layout: R4-R11 (callee-saved) at bottom
    v_log(LOG_INFO, "Task %d initial stack context:", task->task_id);
    for (int i = 0; i < 8; i++) {
        v_log(LOG_INFO, "R%d = 0x%x", i + 4, sp[i]);
    }

    // Hardware-saved registers (R0-R3, R12, LR, PC, xPSR)
    uint32_t r0   = sp[8];
    uint32_t r1   = sp[9];
    uint32_t r2   = sp[10];
    uint32_t r3   = sp[11];
    uint32_t r12  = sp[12];
    uint32_t lr   = sp[13];
    uint32_t pc   = sp[14];
    uint32_t xpsr = sp[15];

    v_log(LOG_INFO, "R0=0x%x, R1=0x%x, R2=0x%x, R3=0x%x",
          r0, r1, r2, r3);
    v_log(LOG_INFO, "R12=0x%x, LR=0x%x, PC=0x%x, xPSR=0x%x",
          r12, lr, pc, xpsr);

    // Validate values
    if (r0 != (uint32_t)task->arg) {
        v_log(LOG_ERROR, "R0 != task->arg (0x%x != 0x%x)", r0, (unsigned)task->arg);
    }
    if ((pc & ~1) != ((uint32_t)task->entry & ~1)) {
        v_log(LOG_ERROR, "PC != task->entry (0x%x != 0x%x)", pc, (unsigned)task->entry);
    }
    if ((lr & ~1) != ((uint32_t)task_exit & ~1)) {
        v_log(LOG_ERROR, "LR != task_exit (0x%x != 0x%x)", lr, (unsigned)task_exit);
    }

    // Check callee-saved registers are zero
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (sp[i] != 0) {
            ok = 0;
            break;
        }
    }
    v_log(LOG_INFO, "Callee-saved registers R4-R11 %s", ok ? "initialized to 0" : "incorrect values");
}

// Test creating multiple tasks and checking stacks
void test_task_stack_creation() {
    v_log(LOG_INFO, "== Test: Task Stack Creation ==");

    // Task 1
    TCB *task1 = (TCB *)v_malloc(sizeof(TCB));
    task1->task_id = 1;
    task1->entry = task_fn1;
    task1->arg = (void*)0x1234ABCD;
    task1->stack_size = 128;
    uint32_t *alloc1 = (uint32_t *)v_malloc(task1->stack_size + 8);
    task1->stack_allocation = alloc1;
    task1->stack_base = (uint32_t *)(((uintptr_t)alloc1 + 7) & ~0x7);
    prepare_task_stack(task1);
    test_task_stack(task1);

    // Task 2
    TCB *task2 = (TCB *)v_malloc(sizeof(TCB));
    task2->task_id = 2;
    task2->entry = task_fn2;
    task2->arg = (void*)0xDEADBEEF;
    task2->stack_size = 256;
    uint32_t *alloc2 = (uint32_t *)v_malloc(task2->stack_size + 8);
    task2->stack_allocation = alloc2;
    task2->stack_base = (uint32_t *)(((uintptr_t)alloc2 + 7) & ~0x7);
    prepare_task_stack(task2);
    test_task_stack(task2);

    // Free memory
    v_free(task1->stack_allocation);
    v_free(task1);
    v_free(task2->stack_allocation);
    v_free(task2);

    v_log(LOG_INFO, "== Task Stack Creation Test Completed ==");
}

// Main
int main(void) {
    v_init();
    heap_memory_init();

    test_task_stack_creation();

    return 0;
}
