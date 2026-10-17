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
#include "utils.h" // v_log (report a console that refused to start)
#include <stdint.h>

#ifdef NAVHAL
#include "navhal.h"
// Generated from the board's Kconfig: BOARD_CONSOLE_UART and friends. The
// console is a board property, so it is read from here rather than named in
// this file — see v_port_hw_console_init.
#include "board.h"
#else
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
    // 84 MHz SYSCLK, PLLQ=7 keeping the 48 MHz SDIO/USB clock. APB dividers left
    // 0: the F4 backend defaults them to /2 and clamps APB1 to its 42 MHz limit.
    //
    // M is chosen so the VCO input is 1 MHz whichever source is used, which is
    // what keeps N/P/Q — and therefore SYSCLK and the 48 MHz domain — identical
    // between them. HSI is the default because it needs no board support; HSE is
    // required for USB, because hal_usb_cdc_init refuses a PLL Q that did not
    // come from a crystal rather than enumerate unreliably.
#if VAIOS_CLOCK_HSE
    const uint32_t src = HAL_CLOCK_SOURCE_HSE;
    const uint32_t vco_div = VAIOS_CLOCK_HSE_HZ / 1000000u;
#else
    const uint32_t src = HAL_CLOCK_SOURCE_HSI;
    const uint32_t vco_div = 16u; // the HSI is 16 MHz
#endif
    hal_clock_config_t clk_cfg = {.source = HAL_CLOCK_SOURCE_PLL,
                                  .pll = {.input_src = (hal_clock_source_t)src,
                                          .pll_m = (uint8_t)vco_div,
                                          .pll_n = 336,
                                          .pll_p = 4,
                                          .pll_q = 7}};
    hal_clock_init(&clk_cfg);
  }
#else
  (void)internal_clock_setup; /* QEMU boots with a usable clock already. */
#endif
}

void v_port_hw_debug_init(void) {
#if VAIOS_DEBUG_IN_SLEEP
  // DBGMCU_CR: keep the debug interface clocked through WFI/WFE (DBG_SLEEP) and
  // the stop/standby modes. Without this, the first time the idle task sleeps
  // the debug AP loses bus access — the probe then reads a chip ID of 0 and any
  // attempt to inspect RAM fails, which is exactly what happens when reading a
  // log ring over SWD from a running system.
  // Cortex-M4 debug block, ARMv7-M: 0xE0042004, bits 0..2.
  volatile uint32_t *dbgmcu_cr = (volatile uint32_t *)0xE0042004u;
  *dbgmcu_cr |= 0x7u; // DBG_SLEEP | DBG_STOP | DBG_STANDBY
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
   * once all higher-priority IRQs have drained. PendSV is the lowest.
   *
   * SVCall gets VAIOS_MAX_SYSCALL_PRIO_LEVEL — the same level the BASEPRI
   * ceiling is derived from. It used to get nothing at all: SHPR2 kept the
   * ARMv7-M reset value of 0, so a syscall ran at the highest priority in the
   * machine for its whole duration. Nobody chose that. Leaving SVCall at 0 is
   * conventional where SVC is a one-shot trampoline to start the first task,
   * but here it is the entire syscall surface — the allocator, fd lookups, bus
   * publishes — and at 0 every one of those masked even the IRQs that
   * VAIOS_MAX_SYSCALL_PRIO_LEVEL promises are "never masked by a kernel
   * critical section". A syscall was strictly better protected than any
   * critical section in the kernel, which is not an invariant anything relies
   * on. NavHAL's own hal_interrupt_enable docs make the same argument for why
   * it refuses to leave peripheral IRQs at 0.
   *
   * At the ceiling instead: IRQs above it (levels 0..LEVEL-1, the band that
   * must not call any vaios API) preempt a syscall as documented, and
   * everything at or below it stays masked for the syscall's duration —
   * exactly what ENTER_CRITICAL already guarantees. This does NOT loosen "a
   * syscall must never wait on hardware": that follows from SysTick sitting at
   * 14, below the ceiling, not from SVCall having been at 0. */
#ifdef NAVHAL
  hal_interrupt_set_priority(SysTick_IRQn, 14);
  hal_interrupt_set_priority(PendSV_IRQn, 15);
  hal_interrupt_set_priority(SVCall_IRQn, VAIOS_MAX_SYSCALL_PRIO_LEVEL);
#else
  set_systick_interrupt_priority(14);
  set_pendsv_interrupt_priority(15);
  set_svcall_interrupt_priority(VAIOS_MAX_SYSCALL_PRIO_LEVEL);
#endif
}

// SysTick vector. The kernel tick body (tick count, delayed-task wakeups, PendSV
// pend) lives in kernel/utils.c as v_kernel_tick(); this shim keeps the CMSIS
// vector name and NavHAL's timebase poke in the port. In SUBMODULE builds NavHAL
// compiles out its own SysTick_Handler and cedes the vector to us, so its
// millisecond timebase (backing every hal_delay_*()) only advances if we drive
// it here.
extern void v_kernel_tick(void);
#ifdef NAVHAL
void hal_timebase_tick(void); // NavHAL timebase.c; not in the public navhal.h
#endif
#if defined(NAVHAL) && !NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
/* Console input on a UART. With UART DMA, DMA fills a circular ring by
 * itself and the console is handed what arrived on three events: the line
 * going idle after a burst, and the ring reaching its half-way and wrap
 * points -- so a burst costs a handful of interrupts, not one a byte, and one
 * longer than the ring is drained before DMA can overwrite it. Without DMA,
 * one RXNE interrupt per byte. (NavHAL's USART vectors are defined only with
 * UART DMA, but Default_Handler dispatches to whatever is attached, so the
 * RXNE path works without them -- navhal todo 171.) */
#define CONSOLE_UART_RX_IRQ 1
static void (*_console_rx_sink)(const uint8_t *, uint16_t);

static void console_uart_rx_isr(void) {
  /* RXNEIE also fires on overrun; reading SR (available) then DR clears both. */
  while (hal_uart_available(BOARD_CONSOLE_UART)) {
    uint8_t c = (uint8_t)hal_uart_read_char(BOARD_CONSOLE_UART);
    _console_rx_sink(&c, 1);
  }
}

#if NAVHAL_CONFIG_DRV_DMA && NAVHAL_CONFIG_DRV_UART_DMA
#define CONSOLE_RX_DMA 1
/* A whole number of cache lines, line-aligned: on the F7 the ring is
   invalidated before it is read, and invalidation works on whole lines. The
   half-way interrupt drains it every 128 bytes whatever the line does. */
#ifndef NAVHAL_CACHE_LINE
#define NAVHAL_CACHE_LINE 32
#endif
#define CONSOLE_RX_RING 256
static uint8_t _console_rx_ring[CONSOLE_RX_RING]
    __attribute__((aligned(NAVHAL_CACHE_LINE)));
static uint16_t _console_rx_tail;

/* From the idle-line and the DMA half/wrap interrupts, which share one
   priority and so never interrupt each other: one producer at a time. Hands
   the console every byte DMA has written since last time, in at most two
   pieces when the ring wrapped. */
static void console_rx_dma_drain(void) {
  uint16_t head;
  if (hal_uart_dma_rx_index(BOARD_CONSOLE_UART, &head) != HAL_OK ||
      head >= CONSOLE_RX_RING)
    return;
  while (_console_rx_tail != head) {
    uint16_t end = head > _console_rx_tail ? head : CONSOLE_RX_RING;
    uint16_t n = (uint16_t)(end - _console_rx_tail);
    hal_uart_dma_rx_sync(BOARD_CONSOLE_UART, _console_rx_tail, n);
    _console_rx_sink(&_console_rx_ring[_console_rx_tail], n);
    _console_rx_tail = end == CONSOLE_RX_RING ? 0u : end;
  }
}
#endif
#endif

#if defined(NAVHAL) && NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
static void (*_console_rx_sink)(const uint8_t *, uint16_t);

/* A host opening the port raises DTR, and the console reports it as one ENQ
 * (0x05, "who are you") into the RX path -- the byte a terminal answers with
 * its identity -- and the shell answers with the banner and a prompt. Without
 * it both were printed once at boot, before anyone listened, and the first
 * thing a user saw was nothing.
 *
 * Polled from the tick because NavHAL tracks DTR (SET_CONTROL_LINE_STATE) but
 * has no callback for it -- one load per tick, and nothing wakes until a host
 * actually attaches. The OTG_FS interrupt (level 8) feeds the same sink and
 * outranks SysTick (14), so the push is made with it masked: the sink's ring has
 * one producer at a time. */
/* Not at the edge itself, but this long after it, and only if DTR is still up.
 * Linux raises DTR inside open(), while the tty is still in its default cooked,
 * echoing mode; the terminal program switches it to raw a moment later. A
 * prompt sent into that gap is echoed straight back as typing -- seen on the
 * F401 as "vaios> v" fragments in the line. */
#define CONSOLE_ATTACH_SETTLE_TICKS (100000u / TICK_PERIOD_US) /* 100 ms */

static void console_attach_poll(void) {
  static uint32_t up; /* ticks DTR has been high; 0 = down */
  if (!_console_rx_sink)
    return;
  if (!hal_usb_cdc_connected()) {
    up = 0;
    return;
  }
  if (up <= CONSOLE_ATTACH_SETTLE_TICKS && ++up == CONSOLE_ATTACH_SETTLE_TICKS) {
    static const uint8_t enq = 0x05;
    uint32_t st = ENTER_CRITICAL_FROM_ISR();
    _console_rx_sink(&enq, 1);
    EXIT_CRITICAL_FROM_ISR(st);
  }
}
#endif

void SysTick_Handler(void) {
#ifdef NAVHAL
  hal_timebase_tick();
#endif
#if defined(NAVHAL) && NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  console_attach_poll();
#endif
  v_kernel_tick();
}

void v_port_hw_cpu_idle(void) {
#ifdef NAVHAL
  /* WFI until the next interrupt; NavHAL owns the barriers/event handling. */
  hal_cpu_idle();
#endif
  /* No-HAL target: fall through — the idle loop stays a busy-spin. */
}

/* SCB->ICSR. Architectural (ARMv7-M), not vendor, so it is read directly here
 * rather than through NavHAL — which has no wrapper for it. VECTACTIVE lives in
 * bits [8:0]. */
#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define SCB_ICSR_VECTACTIVE 0x1FFu

int v_port_hw_in_isr(void) {
  /* VECTACTIVE == 0 is thread mode; anything else means an exception handler is
   * executing (a system handler or an external IRQ). */
  return (SCB_ICSR & SCB_ICSR_VECTACTIVE) != 0u;
}

uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out) {
  /* ICSR.VECTACTIVE (bits [8:0]) is the active exception number: 0 = thread
   * mode, 1..15 = system handlers, >=16 = external IRQ (IRQn = VECTACTIVE-16).
   * Only external IRQs carry an NVIC priority that BASEPRI masks. */
  uint32_t vectactive = SCB_ICSR & SCB_ICSR_VECTACTIVE;
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

#if defined(NAVHAL) && !NAVHAL_CONFIG_CONSOLE_ROUTE_CDC && VAIOS_PORT_CONSOLE_DMA
/* A console DMA transfer in flight. A direct write (the console writer, a
 * kernel print) used to go straight to the data register while the logger's
 * DMA was still sending, and the two came out interleaved byte by byte -- on a
 * Nucleo-F401RE, the shell's first prompt woven into the boot log's last line.
 * So the port tracks its own transfer and a direct write waits for it. */
static volatile uint8_t _console_dma_busy;
static uint8_t _console_dma_tracked; /* completion is wired, so busy clears */
static void (*_console_dma_done_cb)(void);

static void console_dma_done(void) {
  _console_dma_busy = 0;
  if (_console_dma_done_cb)
    _console_dma_done_cb();
}

static void console_dma_wait(void) {
  /* Bounded: a panic printing from an interrupt may mask the very DMA
     interrupt it would wait for. ~100 ms at 84 MHz, far past one log flush
     (a full 128-byte buffer is 11 ms at 115200). */
  for (uint32_t guard = 2000000u; _console_dma_busy && guard; guard--)
    ;
  _console_dma_busy = 0;
}
#endif

void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void)) {
#ifdef NAVHAL
#if NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  // The console is the board's own USB port, not USART2. The point is a board
  // whose debug probe has no VCP: the log needs no second cable and no probe.
  // Nothing is transmitted until the host opens the port, so early output is
  // lost unless VAIOS_CONSOLE_TO_KMSG is on to keep it in RAM — boot does NOT
  // wait for a host here, because a flight build must not depend on one.
  (void)baudrate;
  (void)dma_tx_done_cb;
  if (hal_usb_cdc_init() != HAL_OK) {
    // No console to complain on — but with VAIOS_CONSOLE_TO_KMSG the ring keeps
    // this, and it is readable over SWD. The usual cause is a PLL Q that did not
    // come from a crystal: see VAIOS_CLOCK_HSE.
    v_log(LOG_ERROR, "console: USB CDC init refused (needs a 48 MHz PLL Q from "
                     "HSE — see VAIOS_CLOCK_HSE)");
  }
  return;
#endif
  // BOARD_CONSOLE_UART, not USART2: which UART carries the console is a board
  // fact and the board already states it. The F767 answers UART3 and the qemu
  // board UART1, so naming USART2 here was correct on this bench and wrong
  // anywhere else.
  hal_uart_config_t uart_cfg = {.baudrate = baudrate};
  hal_uart_init(BOARD_CONSOLE_UART, &uart_cfg);
#if VAIOS_PORT_CONSOLE_DMA
  if (dma_tx_done_cb) {
    // Ask the UART driver which stream it bound, rather than naming one. The
    // answer is not a constant: several requests have a second stream (the
    // driver notes USART6_TX is 6 or 7) and hal_uart_dma_set_binding can move
    // it, so DMA1_Stream6_IRQn was right only for USART2 on an F4 that nothing
    // had rebound. This follows whatever the console UART actually got.
    hal_dma_binding_t tx;
    _console_dma_done_cb = dma_tx_done_cb;
    if (hal_uart_dma_get_binding(BOARD_CONSOLE_UART, true, &tx) == HAL_OK &&
        hal_interrupt_attach_callback(tx.irq, console_dma_done) == HAL_OK)
      _console_dma_tracked = 1;
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
#if defined(NAVHAL) && NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  // CDC has no DMA path of its own; the driver's write already copies into the
  // peripheral FIFO, so the buffered logger's "DMA" write is a plain write.
  hal_usb_cdc_write(bytes, (uint16_t)len);
#elif VAIOS_PORT_CONSOLE_DMA
  console_dma_wait(); /* one transfer at a time, whoever started the last */
  _console_dma_busy = _console_dma_tracked;
  if (hal_uart_write_dma(BOARD_CONSOLE_UART, bytes, len) != HAL_OK)
    _console_dma_busy = 0; /* never started: nothing will complete it */
#else
  (void)bytes;
  (void)len;
#endif
}

void v_port_hw_console_write_string(const char *str) {
#ifdef NAVHAL
#if NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  hal_usb_cdc_write_string(str);
#else
#if VAIOS_PORT_CONSOLE_DMA
  console_dma_wait();
#endif
  hal_uart_write_string(BOARD_CONSOLE_UART, str);
#endif
#else
  sh_write0(str);
#endif
}

int v_port_hw_console_rx_irq_init(void (*sink)(const uint8_t *data,
                                                uint16_t len)) {
  if (!sink)
    return 0;
#if defined(NAVHAL) && NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  /* The CDC driver calls this from the OTG_FS interrupt instead of queueing
     for hal_usb_cdc_read. Its callback type is exactly this signature. Kept
     here as well, for the attach ^L (console_attach_poll). */
  _console_rx_sink = sink;
  return hal_usb_cdc_set_rx_callback(sink) == HAL_OK;
#elif defined(CONSOLE_UART_RX_IRQ)
  _console_rx_sink = sink;
#if defined(CONSOLE_RX_DMA)
  /* Wire the events first and start DMA last: once DMA is reading the data
     register the RXNE fallback could never see a byte, so DMA starts only when
     everything that drains it is in place, and anything wired is undone if it
     cannot. */
  if (hal_uart_attach_idle_callback(BOARD_CONSOLE_UART, console_rx_dma_drain) ==
          HAL_OK &&
      hal_uart_dma_rx_attach_callback(BOARD_CONSOLE_UART,
                                      console_rx_dma_drain) == HAL_OK &&
      hal_uart_init_dma_rx(BOARD_CONSOLE_UART, _console_rx_ring,
                           CONSOLE_RX_RING) == HAL_OK)
    return 1;
  hal_uart_dma_rx_attach_callback(BOARD_CONSOLE_UART, NULL);
  hal_uart_detach_idle_callback(BOARD_CONSOLE_UART);
  /* no DMA stream for it: fall back to an interrupt per byte */
#endif
  if (hal_interrupt_attach_callback(BOARD_CONSOLE_UART_IRQ,
                                    console_uart_rx_isr) != HAL_OK)
    return 0;
  return hal_uart_enable_interrupt(BOARD_CONSOLE_UART, 1, 0) == HAL_OK;
#else
  /* QEMU semihosting has no async RX: the reader polls. */
  return 0;
#endif
}

void v_port_hw_system_reset(void) {
  /* What CMSIS NVIC_SystemReset does: keep the priority grouping, request the
     reset, and wait for it. The barriers make sure everything before the write
     has landed and nothing after it runs. */
  volatile uint32_t *aircr = (volatile uint32_t *)0xE000ED0CUL;
  __asm volatile("dsb" ::: "memory");
  *aircr = 0x05FA0000UL | (*aircr & 0x700UL) | (1UL << 2);
  __asm volatile("dsb" ::: "memory");
  for (;;)
    ;
}

#if defined(NAVHAL) && NAVHAL_CONFIG_DRV_RTC
/* The crystal: a marginal one takes ten seconds from cold (the NAVIXSM's
   measured 7-10). No LSI fallback -- an RC that drifts ~5% makes a clock that
   looks right and is not. */
#define RTC_LSE_TIMEOUT_MS 15000

int v_port_hw_rtc_start(void) {
  hal_rtc_config_t cfg = {.clock = HAL_RTC_CLOCK_LSE,
                          .lse_timeout_ms = RTC_LSE_TIMEOUT_MS};
  return hal_rtc_init(&cfg) == HAL_OK ? 0 : -1;
}

int v_port_hw_rtc_get(v_datetime_t *out) {
  hal_rtc_datetime_t dt;
  if (!out || hal_rtc_get_datetime(&dt) != HAL_OK)
    return -1;
  out->year = dt.year;
  out->month = dt.month;
  out->day = dt.day;
  out->hour = dt.hour;
  out->minute = dt.minute;
  out->second = dt.second;
  return hal_rtc_is_set() ? 0 : 1;
}

int v_port_hw_rtc_set(const v_datetime_t *in) {
  if (!in || in->year < 2000 || in->year > 2099)
    return -1;
  /* The calendar keeps a weekday too: Sakamoto's method, 1 = Monday. */
  static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = in->year - (in->month < 3);
  int w = (y + y / 4 - y / 100 + y / 400 + t[(in->month - 1) % 12] + in->day) % 7;
  hal_rtc_datetime_t dt = {.year = in->year,
                           .month = in->month,
                           .day = in->day,
                           .weekday = (uint8_t)(w == 0 ? 7 : w),
                           .hour = in->hour,
                           .minute = in->minute,
                           .second = in->second};
  return hal_rtc_set_datetime(&dt) == HAL_OK ? 0 : -1;
}
#else
int v_port_hw_rtc_start(void) { return -1; }
int v_port_hw_rtc_get(v_datetime_t *out) {
  (void)out;
  return -1;
}
int v_port_hw_rtc_set(const v_datetime_t *in) {
  (void)in;
  return -1;
}
#endif

uint32_t v_port_hw_cpu_hz(void) {
#ifdef NAVHAL
  return hal_clock_get_sysclk();
#else
  return 0; /* QEMU: the no-HAL clock is whatever the model runs at */
#endif
}

/* Non-blocking single-byte read. 1 if a byte was taken into *c, 0 if none was
 * waiting. This is the route-agnostic way in: an interrupt-driven console feeds
 * its reader from the ISR and needs nothing here, while CDC has no RX interrupt
 * of its own to hand over, so its reader polls. A caller that does both is
 * correct on either route without knowing which it has. */
int v_port_hw_console_try_read(char *c) {
  if (!c)
    return 0;
#if defined(NAVHAL) && NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
  uint8_t b = 0;
  if (hal_usb_cdc_read(&b, 1) == 0)
    return 0;
  *c = (char)b;
  return 1;
#elif defined(NAVHAL)
  if (!hal_uart_available(BOARD_CONSOLE_UART))
    return 0;
  *c = hal_uart_read_char(BOARD_CONSOLE_UART);
  return 1;
#else
  (void)c;
  return 0; /* semihosting: no non-blocking read */
#endif
}

/* ---------------------------------------------------------------------------
 * SD/MMC (SDIO) — used by the VFS init path
 *
 * Gated on NAVHAL_HAS_SDIO, not on NAVHAL alone: common/hal_sdio.h wraps its
 * whole body in that flag, so on a board whose config leaves the driver out
 * (nucleo_f767zi, say) the types and enums below simply do not exist and this
 * file will not compile. "A HAL is present" is not the same claim as "this
 * board has an SD slot". Without the driver these behave exactly as they do on
 * a backend with no SDIO at all — the VFS init path already treats a non-zero
 * return as "no card here" and carries on.
 * ------------------------------------------------------------------------- */

int v_port_hw_sdio_init(void) {
#if defined(NAVHAL) && NAVHAL_HAS_SDIO
  /* clock_div is auto-calculated from the system clock. The width is a named
   * value since NavHAL 0.3.8 — the peripheral's WIDBUS field is neither a lane
   * count nor a flag, so the enum keeps that mapping in one place. */
  hal_sdio_config_t sd_config = {.clock_div = 118,
                                 .bus_width = VAIOS_SDIO_4BIT
                                                  ? HAL_SDIO_BUS_WIDTH_4BIT
                                                  : HAL_SDIO_BUS_WIDTH_1BIT};
  return (hal_sdio_init(&sd_config) == HAL_SDIO_OK) ? 0 : -1;
#else
  return -1; /* No SDIO driver on this target. */
#endif
}

int v_port_hw_sdio_card_present(void) {
#if defined(NAVHAL) && NAVHAL_HAS_SDIO
  /* Reads the board's card-detect line (NavHAL 0.3.8). Before it existed, an
   * empty slot and a card behind a broken data line both looked like a timeout —
   * which is exactly the ambiguity that cost a day of debugging on this board. */
  return hal_sdio_card_present() ? 1 : 0;
#else
  return 0;
#endif
}

int v_port_hw_sdio_card_init(void) {
#if defined(NAVHAL) && NAVHAL_HAS_SDIO
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
#ifndef SCB_VTOR // NavHAL >= 0.3.2 defines it in its arch core_reg.h
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
#endif
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
      .base = V_PORT_USER_RO_BASE, .size = HAL_MPU_SIZE_512KB, // == _SIZE
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
#if defined(NAVHAL) && NAVHAL_HAS_CYCLE_COUNTER
  hal_cycle_counter_init();
#endif
}

uint32_t v_port_hw_cycle_counter_read(void) {
#if defined(NAVHAL) && NAVHAL_HAS_CYCLE_COUNTER
  return hal_cycle_counter_get();
#elif defined(NAVHAL)
  /* The core has a DWT — every ARMv7E-M part does — but this config left the
   * driver out (CONFIG_DRV_DWT), so there is nothing to read. hal_dwt.h still
   * declares the functions and only the bodies are gated, so this used to
   * compile and then fail at link. Zero means v_perf reports no cycles rather
   * than a wrong number; turn CONFIG_DRV_DWT on to get real ones. */
  return 0;
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
