#include "vaios.h"
#include "memory.h"
#include "port.h" // v_port_hw_* — all hardware bring-up goes through here
#include "task.h"
#include "utils.h"
#include "vaios_config.h"
#if VAIOS_MODULE_VFS
#include "vfs.h"
#endif
#include "perf.h"
#include <stddef.h>
#include <stdint.h>

extern uint32_t systick_count;

void v_init(vaios_init_config_t *cfg) {
  // resetting global variables
  systick_count = 0;

  /* Hardware bring-up, all behind the port facade. The backend (STM32 HAL /
   * QEMU semihosting / host stub) is chosen inside port_hw.c. */
  v_port_hw_clock_init(cfg->internal_clock_setup);
  v_port_hw_fpu_enable();
  v_port_hw_systick_init(SYSTICK_PERIOD);
  v_port_hw_sched_irq_init();

  /* Enable the MPU + MemManage fault once the core is up, so per-task stack
   * guards take effect on the first context switch. No-op without an MPU or
   * when VAIOS_MPU_ENABLE is off. */
  v_port_mpu_init();

#if UART_LOGGING_ENABLE == 1
  v_port_hw_console_init(UART_BAUDRATE, dma_tx_complete_callback);
  v_log(LOG_INFO, "[VAIOS INIT] SYSTICK started with time period of %d μs",
        SYSTICK_PERIOD);
  v_log(LOG_INFO, "[VAIOS INIT] console started with baudrate %d bps",
        UART_BAUDRATE);
#endif
}
// extern void start_scheduler(void);

void v_system_init(vaios_init_config_t *cfg) {
  /* 1. Core VAIOS Init (clocks, SysTick, console). */
  v_init(cfg);

  /* 2. Memory & Scheduler Init */
  v_heap_memory_init();
  scheduler_init();
  v_perf_init();

  /* 3. Optional SD card + filesystem. v_port_hw_sdio_* return non-zero on
   * any backend without an SDIO peripheral (QEMU/host), so this block is a
   * no-op unless the caller asked for it on real hardware. */
  if (cfg->internal_sd_card_setup) {
    if (v_port_hw_sdio_init() != 0) {
      v_log(LOG_ERROR, "SDIO Peripheral Init Failed!");
      while (1)
        ;
    }

    if (v_port_hw_sdio_card_init() != 0) {
      v_log(LOG_ERROR, "SD Card Handshake Failed!");
      while (1)
        ;
    }

#if VAIOS_MODULE_VFS
    /* Mount FatFS via the thread-safe VFS layer */
    if (vfs_init() != 0) {
      v_log(LOG_ERROR, "Failed to initialize VFS.");
      while (1)
        ;
    }
#endif
  }
}

void v_start(void) {
  // start_scheduler();
  // scheduler_state = SCHEDULER_RUNNING;
}
extern uint8_t scheduler_running;
void v_delay(uint32_t ms) {
  uint32_t delay_ticks = (ms * 1000) / SYSTICK_PERIOD;
  if (scheduler_running) {
    task_delay(delay_ticks);
  } else {
    // Busy wait if scheduler is not running (e.g., during sensor init)
    uint32_t initial_ticks = systick_count;
    while (systick_count < initial_ticks + delay_ticks) {
      v_port_cpu_relax();
    }
  }
}

// void v_stop(void) { scheduler_state = SCHEDULER_STOPPED; }
