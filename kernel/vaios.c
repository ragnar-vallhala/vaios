#include "vaios.h"
#include "config.h"
#include "structures.h"
#include "task.h"
#include "utils.h"
#include <stddef.h>
#ifdef NAVHAL
#include "navhal.h"
#else
#include "semihosting.h"
#endif /* ifdef NAVHAL */

#ifndef NAVHAL
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define SYST_CALIB (*(volatile uint32_t *)0xE000E01C)

#define SYST_CSR_ENABLE (1 << 0)
#define SYST_CSR_TICKINT (1 << 1)
#define SYST_CSR_CLKSOURCE (1 << 2)

/* Define CPU clock for QEMU (adjust if needed) */
#define CPU_CLOCK_HZ 16000000UL

void _qemu_systick_init_ms(uint32_t period_ms)
{
  uint32_t reload = (CPU_CLOCK_HZ / 1000) * period_ms - 1;

  if (reload > 0xFFFFFF)
  {
    // SysTick is only 24-bit, so max reload is 0xFFFFFF
    reload = 0xFFFFFF;
  }

  SYST_RVR = reload;
  SYST_CVR = 0; // Clear current value
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

#endif /* ifndef NAVHAL */

extern uint32_t systick_count;
extern TCB *ready_queue;
// extern TCB *blocked_queue;
// extern TCB *sleep_queue;
extern TCB *current_task;
extern Scheduler_Status_Type scheduler_state;

void v_init(void)
{
  // resetting global variables
  systick_count = 0;
  ready_queue = NULL;
  // blocked_queue = NULL;
  // sleep_queue = NULL;
  current_task = NULL;
  scheduler_state = SCHEDULER_STOPPED;
#ifdef NAVHAL
#ifdef CORTEX_M4
  systick_init(SYSTICK_PERIOD);
  // Interrupt priority setup
  hal_set_interrupt_priority(SysTick_IRQn, 15);
  hal_set_interrupt_priority(PendSV_IRQn, 14);
  hal_set_interrupt_priority(SVCall_IRQn, 0); // system service
#if UART_LOGGING_ENABLE == 1
  uart2_init(UART_BAUDRATE);
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
  v_log(LOG_INFO, "[VAIOS INIT] UART started with baudrate %d bps",
        UART_BAUDRATE);
#endif
#else
#error "CPU NOT RECOGNIZED"
#endif
#else
  _qemu_systick_init_ms(SYSTICK_PERIOD / 1000);
  // set_systick_interrupt_priority(0x20);
  // set_pendsv_interrupt_priority(0xFF);
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
#endif
}
extern void start_scheduler(void);
void v_start(void)
{
  start_scheduler();
  scheduler_state = SCHEDULER_RUNNING;
}
void v_stop(void) { scheduler_state = SCHEDULER_STOPPED; }
