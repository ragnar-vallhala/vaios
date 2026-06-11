/**
 * @file utils_stubs.c
 * @brief Stubs for kernel/utils.c's extern dependencies — used only by the
 *        vaios_utils_tests executable. The main vaios_tests binary uses a
 *        no-op v_log/v_memset/v_panic stub set in stubs.c; building utils.c
 *        there would clash, so this isolated binary links the real
 *        utils.c against a focused stub set.
 */
#include "task.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Globals real utils.c references via extern. */
uint8_t scheduler_running = 0;
volatile uint32_t critical_nesting = 0;
TCB *current_task = NULL;
TCB *idle_task = NULL;
/* kernel/perf.c's per-task dump walks the scheduler lists via this; task.c
 * isn't linked into this utils-focused binary, so stub it to "no tasks". */
int task_snapshot_list(TCB **out, int max) {
  (void)out;
  (void)max;
  return 0;
}
/* kernel/perf.c reads memory.c's allocation_size to track heap peak.
 * memory.c isn't linked into this binary, so stub the symbol. */
uint32_t allocation_size = 0;

/* Port helpers — utils.c's stack-overflow check reads PSP, v_panic disables
 * interrupts then halts. All trivialised here; the formatter tests never
 * actually trigger them. */
uint32_t v_port_get_psp(void) { return 0; }
void v_port_disable_interrupts(void) {}
void v_port_halt(void) { abort(); }
void v_port_trigger_pendsv(void) {}

/* The low-level character sinks utils.c's v_print and SysTick path call.
 * direct_dma_print is defined in utils.c itself (empty when DMA defines are
 * off) so we DON'T stub it here. v_print routes to sh_write0 (semihosting)
 * in the non-NAVHAL branch — stubbed to nothing. SysTick_Handler calls
 * wake_up_delayed_tasks_isr from task.c, which isn't in this binary. */
void sh_write0(const char *s) { (void)s; }
int wake_up_delayed_tasks_isr(void) { return 0; }
