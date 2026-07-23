#define _GNU_SOURCE
/**
 * @file port_hw.c
 * @brief Host (POSIX) port hardware facade.
 *
 * The "hardware" is the OS: a SIGALRM interval timer is the SysTick, stdin/stdout
 * is the console, and the rest (MPU, SDIO, FPU, clock) has no host analogue and
 * is a no-op. The tick handler calls v_host_on_tick() (port.c), which runs the
 * kernel tick and performs any pended context switch — the SysTick->PendSV chain.
 */

#include "port.h"
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// Defined in port.c: the SysTick-ISR body (kernel tick + deferred switch).
void v_host_on_tick(void);

// --- Clock / FPU: nothing to bring up on host. -------------------------------
void v_port_hw_clock_init(uint8_t internal_clock_setup) {
  (void)internal_clock_setup;
}
void v_port_hw_fpu_enable(void) {}

// --- SysTick == a SIGALRM interval timer. ------------------------------------
static void tick_signal_handler(int sig) {
  (void)sig;
  v_host_on_tick(); // may swapcontext to the next task (safe from this handler)
}

void v_port_hw_systick_init(uint32_t period_us) {
  struct sigaction sa;
  sa.sa_handler = tick_signal_handler;
  sigemptyset(&sa.sa_mask); // SIGALRM is auto-blocked in-handler regardless
  sa.sa_flags = 0;          // no SA_RESTART: a blocking syscall should see EINTR
  sigaction(SIGALRM, &sa, NULL);

  struct itimerval it;
  it.it_interval.tv_sec = period_us / 1000000u;
  it.it_interval.tv_usec = (long)(period_us % 1000000u);
  it.it_value = it.it_interval; // first fire one period from now
  setitimer(ITIMER_REAL, &it, NULL);
}

void v_port_hw_sched_irq_init(void) {} // no NVIC priorities to program

// Idle: sleep until the next signal (the tick) rather than burn a core.
void v_port_hw_cpu_idle(void) { pause(); }

// No external IRQs carry a maskable priority on host.
uint32_t v_port_hw_active_irq_priority(uint32_t *vectactive_out) {
  if (vectactive_out)
    *vectactive_out = 0u;
  return 0u;
}

// --- Console == stdio. -------------------------------------------------------
static void (*g_dma_done_cb)(void);

void v_port_hw_console_init(uint32_t baudrate, void (*dma_tx_done_cb)(void)) {
  (void)baudrate;
  g_dma_done_cb = dma_tx_done_cb;
}
void v_port_hw_console_write_dma(const uint8_t *bytes, uint32_t len) {
  fwrite(bytes, 1, len, stdout);
  fflush(stdout);
  if (g_dma_done_cb)
    g_dma_done_cb(); // the "transfer complete" IRQ fires synchronously here
}
void v_port_hw_console_write_string(const char *str) {
  fputs(str, stdout);
  fflush(stdout);
}
char v_port_hw_console_read_char(void) {
  int c = getchar();
  return (c == EOF) ? '\0' : (char)c;
}
void v_port_hw_console_rx_irq_init(void (*rx_cb)(void)) {
  (void)rx_cb; // no async stdin in this port
}

// --- SDIO: none. -------------------------------------------------------------
int v_port_hw_sdio_init(void) { return -1; }
int v_port_hw_sdio_card_init(void) { return -1; }

// --- Cycle counter == a monotonic clock (microseconds, 32-bit wrap). ---------
void v_port_hw_cycle_counter_init(void) {}
uint32_t v_port_hw_cycle_counter_read(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull);
}
