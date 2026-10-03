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
/* kernel/perf.c's v_perf_self_stats resolves the caller through this. */
TCB *get_current_task(void) { return current_task; }
TCB *idle_task = NULL;
/* kernel/perf.c's per-task dump walks the scheduler lists via this; task.c
 * isn't linked into this utils-focused binary, so stub it to "no tasks". */
int task_snapshot_list(TCB **out, int max) {
  (void)out;
  (void)max;
  return 0;
}
/* kernel/perf.c's [tasks] dump labels each task via task_get_name (task.c is
 * not linked into this binary). */
const char *task_get_name(const TCB *task) {
  return (task && task->name) ? task->name : "";
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
 * in the non-NAVHAL branch — stubbed to nothing. v_kernel_tick calls
 * wake_up_delayed_tasks_isr from task.c, which isn't in this binary. */
void sh_write0(const char *s) { (void)s; }
int wake_up_delayed_tasks_isr(void) { return 0; }
/* v_log's full-buffer wait sleeps via task_delay once the scheduler runs;
 * scheduler_running is 0 here, so it is never reached. */
void task_delay(uint32_t ticks) { (void)ticks; }

/* v_printf's fd 1. What it wrote is kept in g_out; g_take caps how much one
   call accepts (0 = all) and g_refuse_once makes the next call take nothing,
   as a full console queue does -- so the retry loop is exercised. */
char g_out[2048];
int g_out_len;
int g_take;
int g_refuse_once;
int v_file_write(int fd, const void *buf, uint32_t len) {
  if (fd != 1)
    return -1;
  if (g_refuse_once) {
    g_refuse_once = 0;
    return 0;
  }
  uint32_t n = (g_take && len > (uint32_t)g_take) ? (uint32_t)g_take : len;
  for (uint32_t i = 0; i < n && g_out_len < (int)sizeof g_out - 1; i++)
    g_out[g_out_len++] = ((const char *)buf)[i];
  g_out[g_out_len] = '\0';
  return (int)n;
}
