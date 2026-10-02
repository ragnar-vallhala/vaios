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

/* --- the renderer ------------------------------------------------------------
 * neofetch-shaped: the name in figlet's standard font on the left, a panel of
 * what this image is on the right, a colour strip under it. One row at a time
 * through `put`, from const data and a stack buffer only -- so the boot path
 * (print) and an unprivileged shell (its fd-1 writer) render the same thing. */
#define B_LOGO "\x1B[1;36m"
#define B_KEY "\x1B[1;36m"
#define B_TITLE "\x1B[1;37m"
#define B_DIM "\x1B[2m"
#define B_RESET "\x1B[0m"

static const char *const _logo[] = {
    "                 _                ",
    "__   __   __ _  (_)   ___    ___  ",
    "\\ \\ / /  / _` | | |  / _ \\  / __| ",
    " \\ V /  | (_| | | | | (_) | \\__ \\ ",
    "  \\_/    \\__,_| |_|  \\___/  |___/ ",
};
#define LOGO_ROWS ((int)(sizeof _logo / sizeof _logo[0]))
#define LOGO_PAD "                                  " /* a logo row's width */

typedef struct {
  char *p, *end;
} wbuf_t;

static void put_s(wbuf_t *w, const char *s) {
  while (*s && w->p < w->end)
    *w->p++ = *s++;
  *w->p = '\0';
}

/* "key" padded to a column: the formatter has no %-Ns. */
static void put_key(wbuf_t *w, const char *k) {
  put_s(w, B_KEY);
  int n = 0;
  for (; k[n]; n++)
    ;
  put_s(w, k);
  put_s(w, B_RESET);
  while (n++ < 10)
    put_s(w, " ");
}

enum {
  ROW_TITLE,
  ROW_RULE,
  ROW_BOARD,
  ROW_SOC,
  ROW_MEMORY,
  ROW_KERNEL,
  ROW_UPTIME,
  ROW_BUILT,
  ROW_CC,
  ROW_GAP,
  ROW_PALETTE,
  ROW_PALETTE_BRIGHT,
  ROW_COUNT
};

static void info_row(int r, wbuf_t *w, uint32_t cpu_hz, int32_t uptime_s) {
  char num[48];
  switch (r) {
  case ROW_TITLE:
    put_s(w, B_TITLE "vaios " VAIOS_VERSION B_RESET "  " B_DIM VAIOS_BUILD_REV
                     B_RESET);
    break;
  case ROW_RULE: {
    /* as long as the title it underlines */
    int n = (int)sizeof("vaios " VAIOS_VERSION "  " VAIOS_BUILD_REV) - 1;
    while (n-- > 0)
      put_s(w, "-");
    break;
  }
  case ROW_BOARD:
    put_key(w, "board");
    put_s(w, VAIOS_PORT_BOARD);
    break;
  case ROW_SOC:
    put_key(w, "soc");
    put_s(w, VAIOS_PORT_SOC);
    if (cpu_hz) {
      print_fmt_buf(num, sizeof num, " @ %d MHz", (int)(cpu_hz / 1000000u));
      put_s(w, num);
    }
    break;
  case ROW_MEMORY:
    put_key(w, "memory");
    print_fmt_buf(num, sizeof num, "%d KiB kernel heap", (int)(HEAP_SIZE / 1024));
    put_s(w, num);
    break;
  case ROW_KERNEL:
    put_key(w, "kernel");
    put_s(w, v_features[0] ? v_features + 1 : "minimal");
    break;
  case ROW_UPTIME:
    put_key(w, "uptime");
    if (uptime_s >= 3600)
      print_fmt_buf(num, sizeof num, "%dh %dm %ds", (int)(uptime_s / 3600),
                    (int)(uptime_s / 60 % 60), (int)(uptime_s % 60));
    else if (uptime_s >= 60)
      print_fmt_buf(num, sizeof num, "%dm %ds", (int)(uptime_s / 60),
                    (int)(uptime_s % 60));
    else
      print_fmt_buf(num, sizeof num, "%ds", (int)uptime_s);
    put_s(w, num);
    break;
  case ROW_BUILT:
    put_key(w, "built");
    put_s(w, VAIOS_BUILD_TIME B_DIM "  " VAIOS_BUILD_BY B_RESET);
    break;
  case ROW_CC:
    put_key(w, "compiler");
    put_s(w, VAIOS_CC);
    break;
  case ROW_PALETTE:
  case ROW_PALETTE_BRIGHT:
    /* the eight colours as backgrounds (40-47), then their bright forms */
    for (char c = '0'; c <= '7'; c++) {
      char dark[] = {'\x1B', '[', '4', c, 'm', ' ', ' ', ' ', '\0'};
      char bright[] = {'\x1B', '[', '1', '0', c, 'm', ' ', ' ', ' ', '\0'};
      put_s(w, r == ROW_PALETTE ? dark : bright);
    }
    put_s(w, B_RESET);
    break;
  default: /* ROW_GAP */
    break;
  }
}

void v_banner_print(void (*put)(const char *line), uint32_t cpu_hz,
                    int32_t uptime_s) {
  char line[200];
  for (int r = 0; r < ROW_COUNT; r++) {
    if (r == ROW_UPTIME && uptime_s < 0)
      continue; /* at boot: there is no uptime yet */
    wbuf_t w = {line, line + sizeof line - 3};
    put_s(&w, "  ");
    if (r < LOGO_ROWS) {
      put_s(&w, B_LOGO);
      put_s(&w, _logo[r]);
      put_s(&w, B_RESET);
    } else {
      put_s(&w, LOGO_PAD);
    }
    put_s(&w, "  ");
    info_row(r, &w, cpu_hz, uptime_s);
    put_s(&w, "\r\n");
    put(line);
  }
}

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
  v_banner_print(print, v_port_hw_cpu_hz(), -1);
  print("\r\n");
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

  /* The boot log out before anything else prints: nothing is running yet, so
     waiting here costs nobody. */
  v_log_drain();
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

