/**
 * @file port.h
 * @brief Host-native stub replacing portable/cortex-m4/port.h
 *
 * Replaces ARM-specific inline assembly macros with no-ops so that kernel
 * C code can be compiled and tested on the host machine.
 *
 * This file is placed in tests/stubs/ and the stub include path is given
 * BEFORE the real portble/ include path so that it shadows the real port.h.
 */
#ifndef VAIOS_CORTEX_M4_PORT_H /* matches the real port.h guard */
#define VAIOS_CORTEX_M4_PORT_H

#include <stdint.h>

/* Critical section stubs — no-ops on host */
#define ENTER_CRITICAL()                                                       \
  do {                                                                         \
  } while (0)
#define EXIT_CRITICAL()                                                        \
  do {                                                                         \
  } while (0)
#define ENTER_CRITICAL_FROM_ISR() ((uint32_t)0)
#define EXIT_CRITICAL_FROM_ISR(saved)                                          \
  do {                                                                         \
    (void)(saved);                                                             \
  } while (0)
#define V_PORT_MB()                                                            \
  do {                                                                         \
  } while (0)

/* NVIC priority model, host-emulated to match portable/cortex-m4/port.h — the
 * FromISR priority predicate (vaios_isr_priority_is_safe) is exercised by
 * test_ipc.c. __NVIC_PRIO_BITS is hardcoded here, not taken from Kconfig: the
 * host arch has no VAIOS_ARCH_HAS_IRQ_PRIORITY, so NVIC_PRIO_BITS isn't emitted. */
#define __NVIC_PRIO_BITS 4
#ifndef MAX_SYSCALL_INTERRUPT_PRIORITY
#define MAX_SYSCALL_INTERRUPT_PRIORITY                                          \
  (VAIOS_MAX_SYSCALL_PRIO_LEVEL << (8 - __NVIC_PRIO_BITS))
#endif
#define VAIOS_ARCH_FIRST_EXTERNAL_IRQ 16u
static inline int v_port_prio_is_more_urgent(uint32_t a, uint32_t b) {
  return a < b;
}

/* Minimum task stack, mirroring portable/cortex-m4/port.h so the scheduler's
 * size validation behaves identically under host test as on target. */
#define VAIOS_ARCH_MIN_STACK 128u

/* CPU-relax spin hint — no-op on host. */
static inline void v_port_cpu_relax(void) {}

/* Host is always privileged (no CONTROL.nPRIV). Mirrors the real port.h. */
static inline int v_port_is_privileged(void) { return 1; }

/* Scheduler / CPU control seam. Same prototypes as the real port.h; the host
 * implementations are no-ops/counters in tests/stubs/{stubs,port_hw_stub}.c.
 * Declared here so the kernel TUs don't hit -Wimplicit-function-declaration. */
uint32_t v_port_get_psp(void);
void v_port_disable_interrupts(void);
void v_port_halt(void);
void v_port_trigger_pendsv(void);
int v_port_ptr_is_ram(const void *p);
void v_port_mpu_init(void);
void v_port_hw_cpu_idle(void);
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out);
int v_port_hw_in_isr(void);

/* Port hardware facade — same prototypes as the real port.h. Host
 * implementations are no-op/stub equivalents in tests/stubs/port_hw_stub.c. */
void v_port_hw_clock_init(uint8_t internal_clock_setup);
void v_port_hw_fpu_enable(void);
void v_port_hw_systick_init(uint32_t period_us);
void v_port_hw_sched_irq_init(void);
void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void));
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len);
void v_port_hw_console_write_string(const char *str);
char v_port_hw_console_read_char(void);
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void));
int v_port_hw_sdio_init(void);
int v_port_hw_sdio_card_init(void);
void v_port_hw_cycle_counter_init(void);
uint32_t v_port_hw_cycle_counter_read(void);

#endif /* VAIOS_CORTEX_M4_PORT_H */
