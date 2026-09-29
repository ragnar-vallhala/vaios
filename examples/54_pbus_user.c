/*
 * 54 — unprivileged tasks using the peripheral-bus arbiter through the fd +
 * transfer syscalls (v_pbus_open / v_pbus_xfer), on target. Runs on the STM32F4
 * under Renode (tools/renode_pbus_user.sh) or real hardware.
 *
 * With VAIOS_MPU_USER_SEPARATION the tasks run unprivileged: they can't touch
 * the peripheral or hand the kernel a callback, so main (privileged) registers
 * the bus with a board transfer hook, and each task only opens it by name.
 *
 * The "device" is a fake register file behind TIM3: the hook answers a read of
 * register r at address a with bytes (a ^ (r + k)) into the kernel bounce
 * buffer, then arms TIM3 for the wire time; the TIM3 IRQ is the transfer-
 * complete interrupt and calls v_pbus_done_isr(). So every transfer really
 * blocks its task in SYS_pbus_wait until an IRQ wakes it.
 *
 * Checks, per task: every transfer returns rc == rx_len with the right bytes
 * (two tasks at different priorities contend for the bus), and the kernel
 * refuses user pointers into kernel memory with -14 (EFAULT) instead of
 * faulting — tx and rx buffers are both checked at submit, before the
 * transfer is queued, so a refused one never leaves the handle busy. Flash is
 * readable by every task, so string literals are valid read-only arguments
 * (the bus name, console text), but flash as rx is refused.
 * Prints "[pbus] <task> PASS" / "FAIL".
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "navhal.h"
#include "periph_bus.h"
#include "port.h" // v_port_is_privileged
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"

#define XFERS 40
#define RX_LEN 6
#define WIRE_US 200
#define TIM_CLK_HZ 84000000u // APB1 timer clock at the default 84 MHz setup
#define KERNEL_SRAM ((void *)0x20000010u)
#define EFAULT_RC (-14)

static v_pbus_t bus;
static volatile int wire_armed;

// Board transfer hook (privileged: SVC handler or the TIM3 ISR chaining the
// next job). x points at kernel bounce buffers.
static int fake_i2c_start(const v_pbus_xfer_t *x) {
  uint8_t reg = x->tx_len ? ((const uint8_t *)x->tx)[0] : 0;
  for (uint32_t k = 0; k < x->rx_len; k++)
    ((uint8_t *)x->rx)[k] = (uint8_t)(x->addr ^ (reg + k));
  hal_timer_stop(TIM3);
  hal_timer_reset(TIM3);
  hal_timer_clear_interrupt_flag(TIM3);
  wire_armed = 1;
  hal_timer_start(TIM3);
  return 0;
}

static void tim3_isr(void) {
  hal_timer_stop(TIM3);
  if (!wire_armed)
    return; // update event from timer setup, not a transfer
  wire_armed = 0;
  v_pbus_done_isr(&bus, RX_LEN);
}

// --- unprivileged side: console via fd 1, bus via the pbus syscalls ---------
// Read-only syscall arguments may point into flash, so string literals go
// straight in; results only ever land in the task's own memory.
static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

static void user_task(void *arg) {
  int id = (int)(uintptr_t)arg; // 0 = "A", 1 = "B"
  uint16_t addr = id ? 0x77 : 0x68;
  int bad = 0;

  say(id ? "[pbus] B nPRIV=%d\r\n" : "[pbus] A nPRIV=%d\r\n",
      v_port_is_privileged() ? 0 : 1, 0);
  int fd = v_pbus_open("i2c1"); // name read straight from flash
  if (fd < 0) {
    say("[pbus] open failed %d\r\n", fd, 0);
    bad = 1;
  }

  for (int i = 0; i < XFERS && !bad; i++) {
    uint8_t reg = (uint8_t)(i * 3), rx[RX_LEN] = {0};
    v_pbus_xfer_t x = {.addr = addr, .tx = &reg, .tx_len = 1,
                       .rx = rx, .rx_len = RX_LEN};
    int rc = v_pbus_xfer(fd, &x, (uint8_t)(2 - id), 100);
    if (rc != RX_LEN) {
      say("[pbus] xfer %d rc=%d\r\n", i, rc);
      bad = 1;
    }
    for (int k = 0; k < RX_LEN; k++)
      if (rx[k] != (uint8_t)(addr ^ (reg + k)))
        bad = 1;
  }

  // Kernel memory as tx: refused at submit, no MemManage fault.
  v_pbus_xfer_t evil = {.addr = addr, .tx = KERNEL_SRAM, .tx_len = 4};
  int rc = v_pbus_xfer(fd, &evil, 1, 100);
  if (rc != EFAULT_RC) {
    say("[pbus] kernel tx rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // Kernel memory as rx: also refused at submit, before anything is queued.
  uint8_t reg = 0;
  v_pbus_xfer_t evil_rx = {.addr = addr, .tx = &reg, .tx_len = 1,
                           .rx = KERNEL_SRAM, .rx_len = 4};
  rc = v_pbus_xfer(fd, &evil_rx, 1, 100);
  if (rc != EFAULT_RC) {
    say("[pbus] kernel rx rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // Flash is readable, never writable: as rx it is refused too.
  v_pbus_xfer_t flash_rx = {.addr = addr, .tx = &reg, .tx_len = 1,
                            .rx = (void *)"flash!", .rx_len = 4};
  rc = v_pbus_xfer(fd, &flash_rx, 1, 100);
  if (rc != EFAULT_RC) {
    say("[pbus] flash rx rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // None of the refusals left the handle busy.
  uint8_t rx[RX_LEN];
  v_pbus_xfer_t ok = {.addr = addr, .tx = &reg, .tx_len = 1,
                      .rx = rx, .rx_len = RX_LEN};
  if (v_pbus_xfer(fd, &ok, 1, 100) != RX_LEN)
    bad = 1;

  v_file_close(fd);
  v_file_write(1, "[pbus] ", 7); // literals from flash
  v_file_write(1, id ? "B" : "A", 1);
  v_file_write(1, bad ? " FAIL\r\n" : " PASS\r\n", 7);
  for (;;)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  // TIM3: 1 MHz counter, one wire time per transfer. NavHAL's default IRQ
  // priority (8) sits inside the kernel-masked band.
  hal_timer_config_t wire = {.prescaler = TIM_CLK_HZ / 1000000u - 1,
                             .auto_reload = WIRE_US};
  hal_timer_init(TIM3, &wire);
  hal_timer_stop(TIM3);
  hal_timer_attach_callback(TIM3, tim3_isr);
  hal_timer_enable_interrupt(TIM3);

  v_pbus_register("i2c1", &bus, fake_i2c_start);
  v_log(LOG_INFO, "pbus_user: start (privileged main)");
  task_create(user_task, (void *)0, 2048, 2);
  task_create(user_task, (void *)1, 2048, 1);
  scheduler_start();
  for (;;)
    ;
}
