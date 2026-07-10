/**
 * @file port_hw.c
 * @brief Cortex-M4 port hardware facade.
 *
 * This is the ONLY translation unit in the target/QEMU build that is allowed
 * to touch hardware (NavHAL, raw MMIO, semihosting). The kernel sources call
 * the v_port_hw_* wrappers declared in port.h and never
 * names a hal_* symbol, a uartN_* shim, an IRQn enum, or a peripheral
 * register directly. All backend selection (real STM32 HAL vs. the QEMU
 * semihosting model) lives here, behind a single #ifdef NAVHAL.
 *
 * Host unit-test builds replace these wrappers with the stubs in
 * tests/stubs/stubs.c (they shadow this file via the stub port.h).
 */

#include "port.h"
#include "vaios_config.h"
#include <stdint.h>

#ifdef NAVHAL
#include "navhal.h"
#else
#include "qemu_irq.h"
#include "semihosting.h"

/* QEMU SysTick model: program the ARMv7-M system timer registers directly.
 * The netduino/STM32F4 Renode model exposes the standard SysTick block. */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define SYST_CSR_ENABLE (1 << 0)
#define SYST_CSR_TICKINT (1 << 1)
#define SYST_CSR_CLKSOURCE (1 << 2)
#define CPU_CLOCK_HZ 16000000UL
#endif /* NAVHAL */

/* ---------------------------------------------------------------------------
 * Clock / FPU bring-up
 * ------------------------------------------------------------------------- */

void v_port_hw_clock_init(uint8_t internal_clock_setup) {
#ifdef NAVHAL
  if (internal_clock_setup == 1) {
    hal_pll_config_t pll_cfg = {.input_src = HAL_CLOCK_SOURCE_HSI,
                                .pll_m = 16,
                                .pll_n = 336,
                                .pll_p = 4,
                                .pll_q = 7};
    hal_clock_config_t clk_cfg = {.source = HAL_CLOCK_SOURCE_PLL};
    hal_clock_init(&clk_cfg, &pll_cfg);
  }
#else
  (void)internal_clock_setup; /* QEMU boots with a usable clock already. */
#endif
}

void v_port_hw_fpu_enable(void) {
#if defined(NAVHAL) && defined(_FPU_ENABLED)
  hal_fpu_enable();
#endif
}

/* ---------------------------------------------------------------------------
 * System tick + scheduler interrupt priorities
 * ------------------------------------------------------------------------- */

void v_port_hw_systick_init(uint32_t period_us) {
#ifdef NAVHAL
  hal_timebase_init(period_us);
#else
  uint32_t period_ms = period_us / 1000u;
  uint32_t reload = (CPU_CLOCK_HZ / 1000u) * period_ms - 1u;
  if (reload > 0xFFFFFFu) {
    reload = 0xFFFFFFu; /* SysTick is 24-bit. */
  }
  SYST_RVR = reload;
  SYST_CVR = 0;
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
#endif
}

void v_port_hw_sched_irq_init(void) {
  /* SysTick below PendSV so a tick can pend a context switch that runs only
   * once all higher-priority IRQs have drained. PendSV is the lowest. */
#ifdef NAVHAL
  hal_interrupt_set_priority(SysTick_IRQn, 14);
  hal_interrupt_set_priority(PendSV_IRQn, 15);
#else
  set_systick_interrupt_priority(14);
  set_pendsv_interrupt_priority(15);
#endif
}

void v_port_hw_cpu_idle(void) {
#ifdef NAVHAL
  /* WFI until the next interrupt; NavHAL owns the barriers/event handling. */
  hal_cpu_idle();
#endif
  /* No-HAL target: fall through — the idle loop stays a busy-spin. */
}

uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out) {
  /* ICSR.VECTACTIVE (bits [8:0]) is the active exception number: 0 = thread
   * mode, 1..15 = system handlers, >=16 = external IRQ (IRQn = VECTACTIVE-16).
   * Only external IRQs carry an NVIC priority that BASEPRI masks. */
  uint32_t vectactive = (*(volatile uint32_t *)0xE000ED04u) & 0x1FFu;
  uint32_t prio = 0u;
  if (vectactive >= 16u) {
    uint32_t irqn = vectactive - 16u;
    /* NVIC_IPR is byte-per-IRQ from 0xE000E400. */
    prio = *(volatile uint8_t *)(0xE000E400u + irqn);
  }
  if (vectactive_out)
    *vectactive_out = vectactive;
  return prio;
}

/* ---------------------------------------------------------------------------
 * Console (log/terminal UART, or semihosting under QEMU)
 * ------------------------------------------------------------------------- */

void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void)) {
#ifdef NAVHAL
  hal_uart_config_t uart_cfg = {.baudrate = baudrate};
  hal_uart_init(HAL_UART_2, &uart_cfg);
#if defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA) &&                     \
    (BUFFERED_LOGGING == 1)
  if (dma_tx_done_cb) {
    hal_interrupt_attach_callback(DMA1_Stream6_IRQn, dma_tx_done_cb);
  }
#else
  (void)dma_tx_done_cb;
#endif
#else
  (void)baudrate;      /* Semihosting needs no init. */
  (void)dma_tx_done_cb;
#endif
}

void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len) {
#if defined(NAVHAL) && defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA)
  hal_uart_write_dma(HAL_UART_2, bytes, len);
#else
  (void)bytes;
  (void)len;
#endif
}

void v_port_hw_console_write_string(const char *str) {
#ifdef NAVHAL
  hal_uart_write_string(HAL_UART_2, str);
#else
  sh_write0(str);
#endif
}

char v_port_hw_console_read_char(void) {
#ifdef NAVHAL
  return hal_uart_read_char(HAL_UART_2);
#else
  return sh_readc();
#endif
}

void v_port_hw_console_rx_irq_init(void (*rx_cb)(void)) {
#ifdef NAVHAL
  hal_interrupt_attach_callback(USART2_IRQn, rx_cb);
  hal_interrupt_enable(USART2_IRQn);
#else
  /* QEMU semihosting has no async RX IRQ; the terminal polls instead. */
  (void)rx_cb;
#endif
}

/* ---------------------------------------------------------------------------
 * SD/MMC (SDIO) — used by the VFS init path
 * ------------------------------------------------------------------------- */

int v_port_hw_sdio_init(void) {
#ifdef NAVHAL
  /* clock_div is auto-calculated from the system clock. */
  hal_sdio_config_t sd_config = {.clock_div = 118, .bus_width = 1};
  return (hal_sdio_init(&sd_config) == HAL_SDIO_OK) ? 0 : -1;
#else
  return -1; /* No SDIO model under QEMU. */
#endif
}

int v_port_hw_sdio_card_init(void) {
#ifdef NAVHAL
  return (hal_sdio_card_init() == HAL_SDIO_OK) ? 0 : -1;
#else
  return -1;
#endif
}

/* ---------------------------------------------------------------------------
 * MPU — Phase 1 per-task stack-overflow guard (docs/plan/MPU_CACHE_INTEGRATION_
 * PLAN.md). Backed by NavHAL hal_mpu; no-op stubs without an MPU / when
 * VAIOS_MPU_ENABLE is off. Runtime hal_mpu_present() keeps QEMU/M4-less targets
 * safe. MPU is enabled with PRIVDEFENA so uncovered addresses keep the default
 * map. The per-task guard is the TOP region (VAIOS_MPU_GUARD_REGION) so it wins
 * on overlap with the Phase 2 static regions (0..3) below it.
 *
 * Phase 2 (VAIOS_MPU_STATIC_PROTECT) adds a static background map that bites even
 * while tasks are still privileged (PRIVDEFENA's privileged default is otherwise
 * RWX everywhere): SRAM execute-never (W^X) and peripherals privileged-only
 * device+XN as the safe core, plus optional flash-RO (VAIOS_MPU_FLASH_RO) and a
 * NULL/low-alias no-access trap (VAIOS_MPU_NULL_GUARD). Programmed once at init.
 * ------------------------------------------------------------------------- */
#define SCB_SHCSR (*(volatile uint32_t *)0xE000ED24)
#define SHCSR_MEMFAULTENA (1u << 16)
#define SHCSR_BUSFAULTENA (1u << 17)
#define SHCSR_USGFAULTENA (1u << 18)
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
/* Top region on the F401 (8 regions, 0..7). Higher number wins on overlap, so
 * the guard overrides the Phase 2 SRAM region at the stack base. */
#define VAIOS_MPU_GUARD_REGION 7u
/* Per-task RW-unprivileged region over the whole block (Stage 5). Above the
 * static SRAM region 0 (so it grants the task its own block) and below the guard
 * region 7 (so the guard still wins at the base). Regions 5,6 stay free. */
#define VAIOS_MPU_USER_REGION 4u

#if defined(NAVHAL) && VAIOS_MPU_ENABLE
/* hal_mpu_size_t encodes SIZE as log2(bytes) - 1. */
static inline hal_mpu_size_t v_mpu_size_enum(uint32_t bytes) {
  return (hal_mpu_size_t)(__builtin_ctz(bytes) - 1);
}

#if VAIOS_MPU_STATIC_PROTECT
/* Phase 2 static background protection, programmed once beneath the per-task
 * guard (region VAIOS_MPU_GUARD_REGION wins on overlap). Regions 0..3; 4..6 stay
 * free for the Phase 3 per-task set. Safe core = SRAM W^X + peripherals priv+XN;
 * flash-RO and the NULL trap are separately gated (each has a hazard, below). */
static void v_mpu_static_protect(void) {
  /* 0: SRAM (128 KB covers the 96 KB part): RW, execute-never (W^X). vaios runs
   *    wholly from flash (no ramfuncs), so XN on RAM catches execute-from-stack/
   *    heap without breaking anything. */
  static const hal_mpu_region_t sram = {
      .base = 0x20000000u, .size = HAL_MPU_SIZE_128KB,
#if VAIOS_MPU_USER_SEPARATION
      /* Kernel-only: unprivileged tasks reach only their own block, granted by
       * the higher-numbered per-task region (VAIOS_MPU_USER_REGION) on switch-in.
       * Privileged code (kernel + still-privileged tasks) keeps RW. */
      .ap = HAL_MPU_AP_PRIV_RW,
#else
      .ap = HAL_MPU_AP_RW,
#endif
      .mem = HAL_MPU_MEM_NORMAL_WB, .executable = false,
      .shareable = false, .srd_mask = 0};
  hal_mpu_configure_region(0u, &sram);

  /* 1: Peripherals [0x40000000, 512 MB): privileged RW device, execute-never.
   *    Privileged tasks keep access today (AP_PRIV_RW); the unprivileged boundary
   *    lands in Phase 3. The PPB (0xE0000000, NVIC/SCB/SysTick) is exempt from
   *    the MPU by architecture, so kernel core access is unaffected. */
  static const hal_mpu_region_t mmio = {
      .base = 0x40000000u, .size = HAL_MPU_SIZE_512MB, .ap = HAL_MPU_AP_PRIV_RW,
      .mem = HAL_MPU_MEM_DEVICE, .executable = false,
      .shareable = true, .srd_mask = 0};
  hal_mpu_configure_region(1u, &mmio);

#if VAIOS_MPU_FLASH_RO || VAIOS_MPU_USER_SEPARATION
  /* 2: Flash code+rodata (512 KB), executable. Needed under USER_SEPARATION so an
   *    unprivileged task can still fetch instructions and read .rodata (the
   *    PRIVDEFENA background map does not cover unprivileged code). AP:
   *      - FLASH_RO: AP_RO — read-only for privileged too, to catch code/const
   *        corruption. HAZARD: incompatible with hal_flash's KV store, which
   *        writes 0x08xxxxxx directly (enable only when the KV store is unused).
   *      - else (USER_SEPARATION only): AP_PRIV_RW_UNPRIV_RO — unprivileged tasks
   *        read+execute but cannot write flash; privileged code is unrestricted,
   *        so the KV store keeps working. */
  static const hal_mpu_region_t flash = {
      .base = 0x08000000u, .size = HAL_MPU_SIZE_512KB,
#if VAIOS_MPU_FLASH_RO
      .ap = HAL_MPU_AP_RO,
#else
      .ap = HAL_MPU_AP_PRIV_RW_UNPRIV_RO,
#endif
      .mem = HAL_MPU_MEM_NORMAL_WT, .executable = true,
      .shareable = false, .srd_mask = 0};
  hal_mpu_configure_region(2u, &flash);
#endif

#if VAIOS_MPU_NULL_GUARD
  /* 3: NULL / low-alias trap [0, 1 MB): no access — catch NULL+offset derefs.
   *    vaios's own startup.s leaves SCB->VTOR at the 0x0 reset alias, so relocate
   *    the vector table to flash FIRST — otherwise this region would fault every
   *    exception vector fetch. The table lives at flash origin (.isr_vector). */
  SCB_VTOR = 0x08000000u;
  __asm volatile("dsb" ::: "memory");
  __asm volatile("isb" ::: "memory");
  static const hal_mpu_region_t null_guard = {
      .base = 0x00000000u, .size = HAL_MPU_SIZE_1MB, .ap = HAL_MPU_AP_NONE,
      .mem = HAL_MPU_MEM_STRONGLY_ORDERED, .executable = false,
      .shareable = false, .srd_mask = 0};
  hal_mpu_configure_region(3u, &null_guard);
#endif
}
#endif /* VAIOS_MPU_STATIC_PROTECT */

void v_port_mpu_init(void) {
  if (!hal_mpu_present())
    return;
#if VAIOS_MPU_STATIC_PROTECT
  v_mpu_static_protect(); /* program the static background map before enabling */
#endif
  /* MPU violations -> MemManage. Also enable Bus/Usage faults so an unprivileged
   * task's stray bus access or privileged-instruction attempt reports cleanly
   * (via their decoded handlers) instead of escalating straight to HardFault. */
  SCB_SHCSR |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA | SHCSR_USGFAULTENA;
  hal_mpu_enable(true); /* PRIVDEFENA: default map for uncovered addrs */
}

int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]) {
  if (!hal_mpu_present())
    return -1;
  hal_mpu_region_t r = {
      .base = (uint32_t)base,
      .size = v_mpu_size_enum(size),
      .ap = HAL_MPU_AP_NONE, /* no access in either mode -> fault on overflow */
      .mem = HAL_MPU_MEM_NORMAL_WB,
      .executable = false,
      .shareable = false,
      .srd_mask = 0,
  };
  hal_mpu_encoded_t enc;
  if (hal_mpu_encode(VAIOS_MPU_GUARD_REGION, &r, &enc) != HAL_OK)
    return -1;
  out[0] = enc.rbar;
  out[1] = enc.rasr;
  return 0;
}

#if VAIOS_MPU_USER_SEPARATION
/* Encode the per-task RW-unprivileged region covering the whole block. base must
 * be size-aligned (task_create aligns to `size`). W^X: not executable, so code
 * can't run from stack/heap. */
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]) {
  if (!hal_mpu_present())
    return -1;
  hal_mpu_region_t r = {
      .base = (uint32_t)base,
      .size = v_mpu_size_enum(size),
      .ap = HAL_MPU_AP_RW, /* unprivileged RW to the task's own block */
      .mem = HAL_MPU_MEM_NORMAL_WB,
      .executable = false,
      .shareable = false,
      .srd_mask = 0,
  };
  hal_mpu_encoded_t enc;
  if (hal_mpu_encode(VAIOS_MPU_USER_REGION, &r, &enc) != HAL_OK)
    return -1;
  out[0] = enc.rbar;
  out[1] = enc.rasr;
  return 0;
}
#endif

void v_port_mpu_apply(const uint32_t enc[2], uint32_t count) {
  if (count == 0 || !hal_mpu_present())
    return;
  hal_mpu_apply((const hal_mpu_encoded_t *)enc, count);
}
#else
void v_port_mpu_init(void) {}
int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  (void)out;
  return -1;
}
#if VAIOS_MPU_USER_SEPARATION
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  (void)out;
  return -1;
}
#endif
void v_port_mpu_apply(const uint32_t enc[2], uint32_t count) {
  (void)enc;
  (void)count;
}
#endif

/* ---------------------------------------------------------------------------
 * Cycle counter (DWT CYCCNT) — perf module backend
 * ------------------------------------------------------------------------- */

void v_port_hw_cycle_counter_init(void) {
#ifdef NAVHAL
  hal_cycle_counter_init();
#endif
}

uint32_t v_port_hw_cycle_counter_read(void) {
#ifdef NAVHAL
  return hal_cycle_counter_get();
#else
  /* No DWT CYCCNT on the QEMU model, but semihosting SYS_ELAPSED exposes the
   * emulator's virtual clock — a real, high-resolution monotonic source. Map
   * the DWT read onto it so v_perf_cycles() yields genuine per-operation timing
   * under QEMU (deterministic when QEMU runs with -icount). Truncate to 32 bits;
   * v_perf_cycles extends wraps. If the host lacks SYS_ELAPSED, sh_elapsed
   * returns 0 and perf timing reads zero rather than a fabricated value. */
  return (uint32_t)sh_elapsed();
#endif
}
