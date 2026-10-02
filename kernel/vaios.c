#include "syscall.h"
#include "vaios.h"
#include "memory.h"
#include "port.h" // v_port_hw_* — all hardware bring-up goes through here
#include "task.h"
#include "utils.h"
#include "vaios_config.h"
#if VAIOS_MODULE_VFS
#include "vfile.h"
#include "vfs.h"
#endif
#include "perf.h"
#include <stddef.h>
#include <stdint.h>

extern uint32_t systick_count;

/* --- the boot banner ---------------------------------------------------------
 * What a Linux kernel says first: what this is, built from what, by whom and
 * when, and what it runs on. All compile-time, so it sits in flash and the
 * unprivileged shell prints the same text (its `version`, and on attach). The
 * build identity comes from vaios_build.h, regenerated on every build. */
#if __has_include("vaios_build.h")
#include "vaios_build.h"
#else
#define VAIOS_BUILD_REV "unknown"
#define VAIOS_BUILD_TIME "unknown"
#define VAIOS_BUILD_BY "unknown"
#endif
#ifndef VAIOS_VERSION /* from Kconfig; absent only to a bare analyser run */
#define VAIOS_VERSION "unknown"
#endif
#if defined(__VERSION__) && defined(__GNUC__) && !defined(__clang__)
#define VAIOS_CC "gcc " __VERSION__
#elif defined(__VERSION__)
#define VAIOS_CC __VERSION__
#else
#define VAIOS_CC "an unknown compiler"
#endif

/* Each enabled kernel feature, space-led; "" if none. */
const char v_features[] =
#if VAIOS_SYSCALL_SVC
    " svc"
#endif
#if VAIOS_DEVFS
    " devfs"
#endif
#if VAIOS_MPU_ENABLE
    " mpu"
#endif
#if VAIOS_MPU_USER_SEPARATION
    " user-separation"
#endif
#if VAIOS_TASK_HEAP
    " task-heap"
#endif
#if VAIOS_MODULE_PERF
    " perf"
#endif
#if VAIOS_MODULE_BUS
    " bus"
#endif
#if VAIOS_MODULE_VFS
    " vfs"
#endif
#if VAIOS_MODULE_SHELL
    " shell"
#endif
    "";

const char v_banner[] =
    "vaios " VAIOS_VERSION " (" VAIOS_BUILD_REV ") #" VAIOS_BUILD_TIME "\r\n"
    "  built by " VAIOS_BUILD_BY " with " VAIOS_CC "\r\n"
    "  on " VAIOS_PORT_TARGET "\r\n";

void v_init(vaios_init_config_t *cfg) {
  // resetting global variables
  systick_count = 0;

  /* Hardware bring-up, all behind the port facade. The backend (STM32 HAL /
   * QEMU semihosting / host stub) is chosen inside port_hw.c. */
  v_port_hw_clock_init(cfg->internal_clock_setup);
  v_port_hw_debug_init(); /* before anything can sleep */
  v_port_hw_fpu_enable();
  v_port_hw_systick_init(TICK_PERIOD_US);
  v_port_hw_sched_irq_init();

  /* Enable the MPU + MemManage fault once the core is up, so per-task stack
   * guards take effect on the first context switch. No-op without an MPU or
   * when VAIOS_MPU_ENABLE is off. */
  v_port_mpu_init();

#if UART_LOGGING_ENABLE == 1
  v_port_hw_console_init(CONSOLE_BAUDRATE, dma_tx_complete_callback);
  /* First thing out, as on Linux. Through the kernel print path, so it lands
     in /dev/kmsg as well and `dmesg` can show it to a terminal that attached
     after boot -- on USB CDC, every terminal. */
  print("\r\n");
  print(v_banner);
  print("  features:");
  print(v_features[0] ? v_features : " none");
  print("\r\n");
  v_log(LOG_INFO, "[VAIOS INIT] cpu %d MHz, heap %d KiB",
        (int)(v_port_hw_cpu_hz() / 1000000u), (int)(HEAP_SIZE / 1024));
  v_log(LOG_INFO, "[VAIOS INIT] kernel tick started with period %d us",
        TICK_PERIOD_US);
  v_log(LOG_INFO, "[VAIOS INIT] console started with baudrate %d bps",
        CONSOLE_BAUDRATE);
#endif
}

void v_system_init(vaios_init_config_t *cfg) {
  /* 1. Core VAIOS Init (clocks, SysTick, console). */
  v_init(cfg);

  /* 2. Memory & Scheduler Init */
  v_heap_memory_init();
  scheduler_init();
  v_perf_init();
#if VAIOS_DEVFS
  v_devfs_init(); // register /dev nodes (console) before any task is created
#endif

  /* 3. Optional SD card + filesystem. v_port_hw_sdio_* return non-zero on
   * any backend without an SDIO peripheral (QEMU/host), so this block is a
   * no-op unless the caller asked for it on real hardware. */
  if (cfg->internal_sd_card_setup) {
    if (v_port_hw_sdio_init() != 0) {
      v_log(LOG_ERROR, "SDIO Peripheral Init Failed!");
      while (1)
        ;
    }

    if (!v_port_hw_sdio_card_present()) {
      v_log(LOG_ERROR, "SD slot is empty (card-detect reads no card).");
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

extern uint8_t scheduler_running;
void v_delay(uint32_t ms) {
  uint32_t delay_ticks = (ms * 1000) / TICK_PERIOD_US;
#if VAIOS_MPU_USER_SEPARATION && VAIOS_SYSCALL_SVC
  // An unprivileged task cannot read scheduler_running / systick_count (kernel
  // globals). It is always post-scheduler-start, so trap straight to the delay
  // syscall via task_delay. The privileged pre-scheduler path (sensor init etc.)
  // runs in handler/privileged context and falls through to the checks below.
  if (v_in_thread_mode()) {
    task_delay(delay_ticks);
    return;
  }
#endif
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

