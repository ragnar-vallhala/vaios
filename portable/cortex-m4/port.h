#ifndef VAIOS_CORTEX_M4_PORT_H
#define VAIOS_CORTEX_M4_PORT_H

#include "vaios_config.h" // VAIOS_USE_BASEPRI, VAIOS_MAX_SYSCALL_PRIO_LEVEL
#include <stdint.h>

// NVIC priority model. ARMv7-M's priority register is 8-bit and high-bit
// justified, with a numerically LOWER value meaning MORE urgent. __NVIC_PRIO_BITS
// is how many top bits the silicon implements (Kconfig NVIC_PRIO_BITS);
// MAX_SYSCALL_INTERRUPT_PRIORITY is the BASEPRI threshold — the configured
// syscall level shifted into those bits. This whole model is ARMv7-M, so it
// lives in the port, not in the portable config header. The host build's
// tests/stubs/port.h mirrors it.
//
// NVIC_PRIO_BITS comes from Kconfig on a real ARM build (VAIOS_ARCH_HAS_IRQ_
// PRIORITY is set, so the symbol is emitted). The fallback covers the host test
// build, which compiles this header via portable/cortex-m4/syscall.c — a
// same-directory "port.h" include that outranks the tests/stubs shadow — where
// the arch config symbol is absent.
#ifndef NVIC_PRIO_BITS
#define NVIC_PRIO_BITS 4
#endif
#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS NVIC_PRIO_BITS
#endif
#ifndef MAX_SYSCALL_INTERRUPT_PRIORITY
#define MAX_SYSCALL_INTERRUPT_PRIORITY                                          \
  (VAIOS_MAX_SYSCALL_PRIO_LEVEL << (8 - __NVIC_PRIO_BITS))
#endif

// Exception number of the first external IRQ: ARMv7-M has 16 system exceptions
// (thread mode = 0, system handlers 1..15) before IRQ0. Anything below this is
// thread mode or a system handler with no maskable NVIC priority.
#define VAIOS_ARCH_FIRST_EXTERNAL_IRQ 16u

// Arch priority ordering: on ARM a numerically LOWER value is MORE urgent. The
// kernel's FromISR priority predicate calls this instead of hardcoding `<`.
static inline int v_port_prio_is_more_urgent(uint32_t a, uint32_t b) {
  return a < b;
}

// Critical-section nesting counter (defined in port.c).
extern volatile uint32_t critical_nesting;

// Critical section entry/exit. Defined inline here — rather than as calls
// into port.c — so every ENTER_CRITICAL/EXIT_CRITICAL site emits the bare
// `msr basepri` instead of a bl round-trip. This is on the hot path of every
// kernel primitive (malloc/free, semaphores, mutexes, the scheduler).
__attribute__((always_inline)) static inline void v_enter_critical(void) {
#if VAIOS_USE_BASEPRI
  uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
  __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
  __asm volatile("cpsid i" ::: "memory");
#endif
  __asm volatile("" ::: "memory"); // compiler barrier
  critical_nesting++;
}

__attribute__((always_inline)) static inline void v_exit_critical(void) {
  critical_nesting--;
  if (critical_nesting == 0) {
#if VAIOS_USE_BASEPRI
    uint32_t pri = 0;
    __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
    __asm volatile("cpsie i" ::: "memory");
#endif
  }
}

// FromISR-safe critical section. Saves/restores the interrupt mask in a LOCAL
// variable instead of the global `critical_nesting`, so it is safe to use from
// an ISR — an ISR must never touch a preempted task's nesting count — and from
// task context alike (a save/restore of the current BASEPRI nests correctly).
// Use this, not ENTER/EXIT_CRITICAL, in any code reachable from a *_from_isr
// path.
__attribute__((always_inline)) static inline uint32_t
v_enter_critical_from_isr(void) {
  uint32_t saved;
#if VAIOS_USE_BASEPRI
  uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
  __asm volatile("mrs %0, basepri" : "=r"(saved)::"memory");
  __asm volatile("msr basepri, %0" : : "r"(pri) : "memory");
#else
  __asm volatile("mrs %0, primask" : "=r"(saved)::"memory");
  __asm volatile("cpsid i" ::: "memory");
#endif
  return saved;
}

__attribute__((always_inline)) static inline void
v_exit_critical_from_isr(uint32_t saved) {
#if VAIOS_USE_BASEPRI
  __asm volatile("msr basepri, %0" : : "r"(saved) : "memory");
#else
  __asm volatile("msr primask, %0" : : "r"(saved) : "memory");
#endif
}

#define ENTER_CRITICAL() v_enter_critical()
#define EXIT_CRITICAL() v_exit_critical()
#define ENTER_CRITICAL_FROM_ISR() v_enter_critical_from_isr()
#define EXIT_CRITICAL_FROM_ISR(saved) v_exit_critical_from_isr(saved)
#define V_PORT_MB() __asm__ volatile("dmb" : : : "memory")

// Stack setup for new task
#define INITIAL_XPSR 0x01000000UL // Thumb bit set

// Architecture-specific portable wrappers
uint32_t v_port_get_psp(void);
void v_port_disable_interrupts(void);
void v_port_halt(void);
void v_port_trigger_pendsv(void);

// True if `p` points into RAM: the ARMv7-M SRAM region base (0x20000000, an
// architectural constant) up to this board's top of RAM (the linker's _estack).
// A corruption sanity check on scheduler-popped TCB pointers; ports with no
// known map (the host stub) return 1.
int v_port_ptr_is_ram(const void *p);

// --- MPU (memory protection) — implemented in port_hw.c, no-ops without an MPU
// or when VAIOS_MPU_ENABLE is off. See docs/plan/MPU_CACHE_INTEGRATION_PLAN.md.
// Enable the MPU (background region privileged) + MemManage fault, once at init.
void v_port_mpu_init(void);
// Pre-encode a no-access stack-guard region of `size` bytes at `base` into the
// two-word RBAR/RASR pair `out`. Returns 0 on success, non-zero if unavailable.
int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]);
#if VAIOS_MPU_USER_SEPARATION
// Pre-encode the per-task RW-unprivileged region over the whole `size`-byte
// block at `base` (Stage 5). Returns 0 on success, non-zero if unavailable.
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]);
#endif
// Apply a pre-encoded region pair to the hardware (context-switch fast path).
void v_port_mpu_apply(const uint32_t enc[2], uint32_t count);

// CPU hint for a short busy-wait spin (keeps the arch `nop` out of the kernel).
__attribute__((always_inline)) static inline void v_port_cpu_relax(void) {
  __asm volatile("nop" ::: "memory");
}

// Non-zero if the CPU is currently privileged (ARMv7-M: CONTROL.nPRIV == 0).
// Lets examples/tests probing the unprivileged-task flip ask the question
// without spelling `mrs control` themselves.
static inline int v_port_is_privileged(void) {
  uint32_t control;
  __asm volatile("mrs %0, control" : "=r"(control));
  return (control & 1u) == 0u;
}

// ---------------------------------------------------------------------------
// Port hardware facade.
//
// The kernel reaches hardware ONLY through these wrappers; the backend (real
// STM32 HAL vs. the QEMU semihosting model) is selected inside port_hw.c. No
// kernel source may name a hal_* symbol, a uartN_* shim, an IRQn enum, or a
// peripheral register directly. Host unit-test builds supply equivalents from
// tests/stubs/stubs.c (declared in the stub tests/stubs/port.h).
// ---------------------------------------------------------------------------

// Clock / FPU bring-up.
void v_port_hw_clock_init(uint8_t internal_clock_setup);
void v_port_hw_fpu_enable(void);

// System tick timer + scheduler interrupt priorities (SysTick=14, PendSV=15).
void v_port_hw_systick_init(uint32_t period_us);
void v_port_hw_sched_irq_init(void);

// Idle-task CPU sleep: WFI until the next interrupt on HAL targets; a no-op
// (the idle loop stays a busy-spin) on no-HAL and host builds. Keeps NavHAL out
// of the portable kernel — the idle task calls only this.
void v_port_hw_cpu_idle(void);

// Non-zero when an exception handler is executing (ICSR.VECTACTIVE != 0), zero
// in thread mode. Used by the logger to avoid spinning for a DMA completion IRQ
// that cannot fire from inside an ISR.
int v_port_hw_in_isr(void);

// Active-exception NVIC priority, for the FromISR priority assert. Returns the
// priority byte of the currently-executing external IRQ (or 0 in thread mode /
// a system handler) and writes ICSR.VECTACTIVE to *vectactive_out.
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out);

// Console: log/terminal UART on hardware, semihosting under QEMU.
void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void));
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len);
void v_port_hw_console_write_string(const char *str);
char v_port_hw_console_read_char(void);
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void));

// SD/MMC over SDIO (VFS backend). Return 0 on success, non-zero on failure.
int v_port_hw_sdio_init(void);
int v_port_hw_sdio_card_init(void);

// Cycle counter (DWT CYCCNT) backing the perf module.
void v_port_hw_cycle_counter_init(void);
uint32_t v_port_hw_cycle_counter_read(void);

// Atomic operations (LL/SC)
static inline uint32_t v_port_ldrex(volatile uint32_t *addr) {
  uint32_t result;
  __asm__ volatile("ldrex %0, [%1]" : "=r"(result) : "r"(addr) : "memory");
  return result;
}

static inline uint32_t v_port_strex(uint32_t val, volatile uint32_t *addr) {
  uint32_t result;
  __asm__ volatile("strex %0, %1, [%2]"
                   : "=&r"(result)
                   : "r"(val), "r"(addr)
                   : "memory");
  return result;
}

static inline void v_port_clrex(void) {
  __asm__ volatile("clrex" : : : "memory");
}

#endif // !VAIOS_CORTEX_M4_PORT_H