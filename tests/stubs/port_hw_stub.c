/**
 * @file port_hw_stub.c
 * @brief Host-native stubs for the port hardware facade (v_port_hw_*).
 *
 * The real implementations live in portable/cortex-m4/port_hw.c and drive
 * NavHAL / QEMU. On the host test build there is no hardware, so every
 * wrapper is a no-op — except the cycle counter, which returns a strictly
 * increasing value so the perf module's monotonicity and per-task accounting
 * tests behave (the old kernel-internal fake counter, relocated here).
 *
 * Linked into both vaios_tests and vaios_utils_tests; the prototypes come
 * from the stub tests/stubs/port.h.
 */
#include "port.h"
#include <stdint.h>

void v_port_hw_clock_init(uint8_t internal_clock_setup) {
  (void)internal_clock_setup;
}
void v_port_hw_fpu_enable(void) {}
void v_port_hw_systick_init(uint32_t period_us) { (void)period_us; }
void v_port_hw_sched_irq_init(void) {}
void v_port_hw_cpu_idle(void) {} /* host idle loop just spins */
void v_port_mpu_init(void) {} /* no MPU on the host */

/* FromISR priority-assert seam. Tests may set these to simulate the active
 * exception; the default (VECTACTIVE 0 = thread mode) is always "safe". */
uint32_t g_stub_active_vectactive = 0;
uint32_t g_stub_active_irq_prio = 0;
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out) {
  if (vectactive_out) *vectactive_out = g_stub_active_vectactive;
  return g_stub_active_irq_prio;
}

void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void)) {
  (void)baudrate;
  (void)dma_tx_done_cb;
}
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len) {
  (void)bytes;
  (void)len;
}
void v_port_hw_console_write_string(const char *str) { (void)str; }
char v_port_hw_console_read_char(void) { return 0; }
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void)) { (void)rx_cb; }

int v_port_hw_sdio_init(void) { return -1; }
int v_port_hw_sdio_card_init(void) { return -1; }

void v_port_hw_cycle_counter_init(void) {}
uint32_t v_port_hw_cycle_counter_read(void) {
  static uint32_t fake;
  fake += 100; /* stride > 0 so deltas show up in per-task accounting */
  return fake;
}
