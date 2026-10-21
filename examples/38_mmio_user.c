#include "ipc.h" // VA_PASS
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * MMIO windows for unprivileged tasks (VAIOS_TASK_MMIO). `driver` is granted
 * GPIOA's 1 KB register block: it sets PA5 (the Nucleo LED) to output and
 * writes and reads back ODR itself, with no syscall. Then it reaches for RCC,
 * outside its window: it faults and is restarted (RESTART, cap 1), and the new
 * task still has the window -- "gpio ok" twice. `snoop`, with no grant, reads
 * GPIOA and is killed. Same addresses on the F401 and the F767.
 */

#define GPIOA 0x40020000u
#define GPIO_MODER (*(volatile uint32_t *)(GPIOA + 0x00u))
#define GPIO_ODR (*(volatile uint32_t *)(GPIOA + 0x14u))
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830u)
#define STK 2048

extern void v_print(const char *s); /* polling console write (privileged) */

static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

static void driver(void *arg) {
  (void)arg;
  GPIO_MODER = (GPIO_MODER & ~(3u << 10)) | (1u << 10); // PA5 output
  int ok = 1;
  for (int i = 0; i < 6; i++) {
    GPIO_ODR ^= 1u << 5; // blink
    ok &= ((GPIO_ODR >> 5) & 1u) == (uint32_t)(~i & 1);
    v_delay(50);
  }
  say(ok ? "[mm] gpio ok\r\n" : "[mm] gpio FAIL\r\n", 0, 0);
  say("[mm] driver reaches for RCC\r\n", 0, 0);
  say("[mm] BUG: driver read RCC 0x%x\r\n", (int)RCC_AHB1ENR, 0);
  for (;;)
    v_delay(1000);
}

static void snoop(void *arg) {
  (void)arg;
  v_delay(100);
  say("[mm] snoop reads GPIOA\r\n", 0, 0);
  say("[mm] BUG: snoop read ODR 0x%x\r\n", (int)GPIO_ODR, 0);
  for (;;)
    v_delay(1000);
}

static void monitor(void *arg) {
  (void)arg;
  for (;;) {
    task_delay(2000);
    char buf[96];
    print_fmt_buf(buf, sizeof buf, "[mm] faults=%u restarts=%u\r\n",
                  (unsigned)v_mpu_fault_stats.faults,
                  (unsigned)v_mpu_fault_stats.restarts);
    v_print(buf);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  RCC_AHB1ENR |= 1u; // GPIOA clock: privileged, before the tasks run
  v_print("[mm] mmio_user: start\r\n");
  uint32_t d = task_create_named(driver, NULL, STK, 3, "driver");
  task_set_fault_policy(d, V_FAULT_RESTART, 1);
  if (v_task_grant_mmio(d, GPIOA, 0x400u) != VA_PASS)
    v_print("[mm] BUG: grant refused\r\n");
  if (v_task_grant_mmio(d, 0x20000000u, 0x400u) == VA_PASS) // SRAM: not MMIO
    v_print("[mm] BUG: SRAM grant accepted\r\n");
  if (v_task_grant_mmio(d, GPIOA + 0x200u, 0x400u) == VA_PASS) // misaligned
    v_print("[mm] BUG: misaligned grant accepted\r\n");
  uint32_t s = task_create_named(snoop, NULL, STK, 2, "snoop");
  task_set_fault_policy(s, V_FAULT_KILL, 0);
  task_create_privileged(monitor, NULL, STK, 1, "monitor");
  scheduler_start();
  for (;;)
    ;
}
