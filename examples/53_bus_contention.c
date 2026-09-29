/*
 * 53 — bus arbiter (include/periph_bus.h) under heavy contention, on target.
 * Runs on the STM32F4 under Renode (tools/renode_bus.sh) or real hardware.
 *
 * The "bus" is a real DMA2 stream plus TIM3 as the wire:
 *   start()   programs a DMA2 Stream0 memory-to-memory copy (the payload) and
 *             arms TIM3 for the transfer's wire time (Renode finishes the DMA
 *             copy at once, so the wire time gives transfers a real duration).
 *   TIM3 IRQ  = the transfer-complete interrupt: checks the DMA flag and calls
 *             v_pbus_done_isr(), which chains the next job from the ISR.
 *   TIM2 IRQ  = the cyclic hardware timer (2 kHz) -> v_pbus_tick_isr(), plus a
 *             watchdog that recovers wedged transfers with v_pbus_abort_isr().
 *
 * Load (> 100% of the bus on purpose):
 *   cyclic  imu  prio 7  every tick (2 kHz), 120 us   -> 24%
 *           baro prio 4  every 4    (500 Hz), 200 us  -> 10%
 *           mag  prio 2  every 8    (250 Hz), 150 us  ->  4%
 *   async   log0 prio 5  back-to-back one-shots, 300 us
 *           log1 prio 1  back-to-back one-shots, 400 us
 *   sync    3 tasks, lock at bus prio 6/3/0, hold ~100 us, ~1 kHz each
 * Every WEDGE_EVERY-th imu transfer "loses" its IRQ (a slave holding SDA); the
 * watchdog must abort it and the bus must carry on.
 *
 * Checks: at most one transfer ever in flight, no DMA in flight while a sync
 * task holds the bus, every completed payload arrives intact, every wedge is
 * recovered, and the bus keeps moving. The verdict prints as
 * "BUS PASS" / "BUS FAIL". Starvation of low-priority users is reported, not
 * failed: the arbiter is strict priority by design.
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "periph_bus.h"
#include "memory.h"
#include "navhal.h"
#include "port.h" // v_port_trigger_pendsv
#include "task.h"
#include "utils.h"
#include "vaios.h"

#define WORDS 16
#define TIMER_HZ 2000
#define WEDGE_EVERY 500
#define STUCK_TICKS 4     // 2 ms with no new start while a job is active
#define RUN_MS 3000
#define TIM_CLK_HZ 84000000u // APB1 timer clock at the default 84 MHz setup

typedef struct {
  const char *name;
  uint16_t wire_us;
  v_pbus_job_t job;
  SemaphoreHandle_t done_sem; // async one-shot users block on this
  uint32_t src[WORDS], dst[WORDS];
  volatile uint32_t starts, dones, errors, bad_data;
} user_t;

static v_pbus_t bus;
static hal_dma_config_t dma = {
    .controller = HAL_DMA_CONTROLLER_2, // only DMA2 can do memory-to-memory
    .stream = 0,
    .direction = HAL_DMA_DIR_M2M,
    .data_count = WORDS,
    .src_inc = 1,
    .dst_inc = 1,
    .data_width = HAL_DMA_DATA_WIDTH_32,
    .priority = HAL_DMA_PRIORITY_HIGH,
    .fifo_mode = 1, // M2M requires FIFO (non-direct) mode
    .fifo_threshold = HAL_DMA_FIFO_THRESHOLD_FULL,
};

static volatile uint32_t in_flight, wire_armed, start_seq;
static volatile uint32_t violations, wedges, aborts, spurious;
static volatile uint32_t lock_ok[3], lock_timeouts[3];

static int bus_start(v_pbus_job_t *j);
static void bus_done(v_pbus_job_t *j, int rc);

#define USER(n, p, us, per)                                                   \
  {.name = n, .wire_us = us,                                                  \
   .job = {.start = bus_start, .done = bus_done, .prio = p, .period = per}}

static user_t imu = USER("imu", 7, 120, 1), baro = USER("baro", 4, 200, 4),
              mag = USER("mag", 2, 150, 8), log0 = USER("log0", 5, 300, 0),
              log1 = USER("log1", 1, 400, 0);
static user_t *const users[] = {&imu, &baro, &mag, &log0, &log1};

// ---- "hardware" -------------------------------------------------------------

static int bus_start(v_pbus_job_t *j) {
  user_t *u = j->arg;
  if (in_flight++ != 0)
    violations++; // arbiter let two transfers onto the bus
  u->starts++;
  start_seq++;
  for (int i = 0; i < WORDS; i++) {
    u->src[i] = start_seq * 131u + (uint32_t)i;
    u->dst[i] = 0;
  }
  if (u == &imu && u->starts % WEDGE_EVERY == 0) {
    wedges++;
    return 0; // "started" but no IRQ will ever come
  }
  dma.src_addr = (uint32_t)u->src;
  dma.dst_addr = (uint32_t)u->dst;
  hal_dma_init(&dma);
  hal_dma_start(&dma);
  hal_timer_stop(TIM3);
  hal_timer_set_auto_reload(TIM3, u->wire_us);
  hal_timer_reset(TIM3);
  hal_timer_clear_interrupt_flag(TIM3);
  wire_armed = 1;
  hal_timer_start(TIM3);
  return 0;
}

static void bus_done(v_pbus_job_t *j, int rc) {
  user_t *u = j->arg;
  in_flight--;
  if (rc) {
    u->errors++;
  } else {
    u->dones++;
    for (int i = 0; i < WORDS; i++)
      if (u->dst[i] != u->src[i]) {
        u->bad_data++;
        break;
      }
  }
  if (u->done_sem) {
    int woken = 0;
    v_semaphore_give_from_isr(u->done_sem, &woken);
    if (woken)
      v_port_trigger_pendsv();
  }
}

// Transfer-complete IRQ (end of wire time).
static void tim3_isr(void) {
  hal_timer_stop(TIM3);
  if (!wire_armed) {
    spurious++; // update event from timer setup, not a transfer
    return;
  }
  wire_armed = 0;
  int ok = hal_dma_transfer_complete(&dma);
  hal_dma_clear_flags(&dma);
  v_pbus_done_isr(&bus, ok ? 0 : -5);
}

// Cyclic timer + wedge watchdog.
static void tim2_isr(void) {
  static uint32_t last_seq, stuck;
  v_pbus_tick_isr(&bus);
  if (bus.active && start_seq == last_seq) {
    if (++stuck >= STUCK_TICKS) {
      stuck = 0;
      hal_timer_stop(TIM3); // silence the wire and DMA first, then abort
      wire_armed = 0;
      hal_dma_stop(&dma);
      hal_dma_clear_flags(&dma);
      aborts++;
      v_pbus_abort_isr(&bus, -110);
    }
  } else {
    stuck = 0;
  }
  last_seq = start_seq;
}

// ---- tasks ----------------------------------------------------------------

static void burn_us(uint32_t us) {
  volatile uint32_t n = us * 20; // ~84 MHz, a few cycles per iteration
  while (n--)
    ;
}

static void async_task(void *arg) {
  user_t *u = arg;
  for (;;) {
    if (v_pbus_submit(&bus, &u->job) != VA_PASS)
      violations++; // our own job can't be busy: we wait for each one
    v_semaphore_take(u->done_sem, V_WAIT_FOREVER);
  }
}

static const uint8_t sync_prio[3] = {6, 3, 0};

static void sync_task(void *arg) {
  int id = (int)(long)arg;
  for (;;) {
    if (v_pbus_lock(&bus, sync_prio[id], 3) == VA_PASS) {
      if (in_flight || wire_armed)
        violations++; // DMA traffic while a blocking user owns the bus
      burn_us(100);   // the blocking hal_* transfer
      if (in_flight || wire_armed)
        violations++;
      v_pbus_unlock(&bus);
      lock_ok[id]++;
    } else {
      lock_timeouts[id]++;
    }
    v_delay(1);
  }
}

static void report(const char *tag) {
  v_log(LOG_INFO, "bus %s: overruns=%u wedges=%u aborts=%u viol=%u spur=%u",
        tag, bus.overruns, wedges, aborts, violations, spurious);
  for (unsigned i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
    user_t *u = users[i];
    v_log(LOG_INFO, "  %s: starts=%u done=%u err=%u bad=%u", u->name,
          u->starts, u->dones, u->errors, u->bad_data);
  }
  for (int i = 0; i < 3; i++)
    v_log(LOG_INFO, "  sync%d(prio %u): locked=%u timeouts=%u", i,
          sync_prio[i], lock_ok[i], lock_timeouts[i]);
}

static void monitor_task(void *arg) {
  for (uint32_t t = 500; t < RUN_MS; t += 500) {
    v_delay(500);
    report("progress");
  }
  v_delay(500);
  // Freeze the load so the counters stop moving under the verdict.
  hal_timer_disable_interrupt(TIM2);
  report("final");

  uint32_t bad = 0;
  for (unsigned i = 0; i < sizeof(users) / sizeof(users[0]); i++)
    bad += users[i]->bad_data;
  int pass = violations == 0 && bad == 0 && wedges > 0 &&
             aborts == wedges && imu.errors == aborts && imu.dones > 1000 &&
             log0.dones > 0 && lock_ok[0] > 0;
  if (log1.dones == 0 || lock_ok[2] == 0)
    v_log(LOG_WARN, "low-priority users starved (strict priority, expected)");
  v_log(LOG_INFO, pass ? "BUS PASS" : "BUS FAIL");
  v_log_flush();
  for (;;)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();

  for (unsigned i = 0; i < sizeof(users) / sizeof(users[0]); i++)
    users[i]->job.arg = users[i];
  log0.done_sem = v_semaphore_create_binary();
  log1.done_sem = v_semaphore_create_binary();

  // TIM3: 1 MHz counter, ARR set per transfer. Both IRQs sit at NavHAL's
  // default priority (8), inside the kernel-masked band.
  hal_timer_config_t wire = {.prescaler = TIM_CLK_HZ / 1000000u - 1,
                             .auto_reload = 1000};
  hal_timer_init(TIM3, &wire);
  hal_timer_stop(TIM3);
  hal_timer_attach_callback(TIM3, tim3_isr);
  hal_timer_enable_interrupt(TIM3);

  v_pbus_cyclic_add(&bus, &imu.job);
  v_pbus_cyclic_add(&bus, &baro.job);
  v_pbus_cyclic_add(&bus, &mag.job);
  hal_timer_init_freq(TIM2, TIMER_HZ);
  hal_timer_attach_callback(TIM2, tim2_isr);
  hal_timer_enable_interrupt(TIM2);
  hal_timer_start(TIM2);

  task_create_named(monitor_task, NULL, 1024, 6, "monitor");
  task_create_named(sync_task, (void *)0, 512, 5, "sync0");
  task_create_named(async_task, &log0, 512, 4, "log0");
  task_create_named(sync_task, (void *)1, 512, 3, "sync1");
  task_create_named(async_task, &log1, 512, 2, "log1");
  task_create_named(sync_task, (void *)2, 512, 1, "sync2");
  v_log(LOG_INFO, "bus contention: starting");
  scheduler_start();
  for (;;)
    ;
}
