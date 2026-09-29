/**
 * @file ctx_switch_smoke.c
 * @brief Minimal first-context-switch smoke test.
 *
 * Two cooperative tasks print a marker over semihosting and yield to each other;
 * task A only declares PASS once it has observed task B run, proving the
 * scheduler performed real context switches (not just launched one task). On a
 * broken first switch the image bus-faults in PendSV before any task runs.
 *
 * Used by tools/test_qemu_smoke.sh as a regression guard for the PSP=0
 * first-context-switch race (problems/vaios-issues.md, Issue 1). Build no-HAL
 * soft-float so it runs on QEMU's FPU-less cortex-m4:
 *   cmake -S . -B build -DNAVHAL=OFF -DVAIOS_FPU=OFF -DEXAMPLES=ON \
 *         -DVAIOS_EXAMPLE=CTX_SWITCH_SMOKE
 */
#include "vaios.h"
#include "task.h"
#include "memory.h"
#include "semihosting.h"

static volatile int a_ran = 0;
static volatile int b_ran = 0;

static void task_a(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        sh_write0("[A] tick\n");
        a_ran = 1;
        task_yield();
    }
    /* Wait until B has also been scheduled, then declare success and stop. */
    int spins = 0;
    while (!b_ran && spins++ < 1000) task_yield();
    if (b_ran)
        sh_write0("[SMOKE] PASS: both tasks ran\n");
    else
        sh_write0("[SMOKE] FAIL: task B never scheduled\n");
    sh_exit(0);
}

static void task_b(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        sh_write0("[B] tick\n");
        b_ran = 1;
        task_yield();
    }
    task_block();
}

int main(void) {
    vaios_init_config_t cfg = {.internal_clock_setup = 0, .internal_sd_card_setup = 0};
    v_init(&cfg);
    v_heap_memory_init();
    scheduler_init();
    task_create(task_a, NULL, 1024, 1);
    task_create(task_b, NULL, 1024, 1);
    scheduler_start();
    while (1) { }
    return 0;
}
