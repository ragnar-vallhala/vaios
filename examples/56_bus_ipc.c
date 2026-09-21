/*
 * 56 — Bus IPC (include/bus.h) on target: an ISR producer, readers at
 * different speeds, overwrite and drop topics. Runs on the STM32F4 under Renode
 * (tools/renode_bus_ipc.sh) or real hardware.
 *
 *   imu.raw  V_BUS_OVERWRITE, published at 2 kHz from the TIM2 IRQ (the
 *            single-producer ISR path). Each sample carries a checksum.
 *     fast   reads everything it can (1 ms polling) and forwards every 10th
 *            sample onto "log".
 *     slow   reads one sample every 20 ms, so overwrite evicts under it: it
 *            skips ahead and must see the exact gap as `missed` — including
 *            when the IRQ evicts the very sample it is copying.
 *   log      V_BUS_DROP: fast publishes, logger drains slowly, so some are
 *            refused; none may vanish silently.
 *
 * Checks: every pop is in order with seq == expected + missed; no checksum
 * failure (no torn copy); after the IRQ stops and everyone drains, every
 * imu seq is either read or reported missed by each reader, log received +
 * dropped == sent, and the pool is whole again (v_bus_check). The verdict
 * prints as "BUS-IPC PASS" / "BUS-IPC FAIL".
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "bus.h"
#include "memory.h"
#include "navhal.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"

#define IMU_HZ 2000
#define RUN_MS 2000
#define BLOCKS 48

typedef struct {
  uint32_t seq;
  uint32_t tick;
  int16_t xyz[3];
  uint16_t sum; // over the fields above: a torn copy breaks it
} imu_sample_t;

V_BUS_POOL(pool, 32, BLOCKS);
static v_bus_t bus;
static v_bus_topic_t imu, logt;
static v_bus_sub_t fast_sub, slow_sub, log_sub;
static const v_bus_topic_cfg_t OVERWRITE = {.overflow = V_BUS_OVERWRITE};

static volatile uint32_t imu_sent, running = 1;

static uint16_t checksum(const imu_sample_t *s) {
  uint32_t x = s->seq ^ s->tick ^ (uint16_t)s->xyz[0] ^
               ((uint32_t)(uint16_t)s->xyz[1] << 8) ^ (uint16_t)s->xyz[2];
  return (uint16_t)(x ^ (x >> 16));
}

// The producer: 2 kHz, from interrupt context.
static void tim2_isr(void) {
  imu_sample_t s = {.seq = imu_sent,
                    .tick = v_get_ticks(),
                    .xyz = {(int16_t)imu_sent, (int16_t)(imu_sent * 3),
                            (int16_t)-imu_sent}};
  s.sum = checksum(&s);
  if (v_bus_publish(&imu, &s, sizeof s) == VA_PASS)
    imu_sent++;
}

// One reader's view: next seq it expects, and its counters.
typedef struct {
  const char *name;
  v_bus_sub_t *sub;
  uint32_t expect, got, missed, bad_order, bad_sum;
} reader_t;

static reader_t fast = {.name = "fast", .sub = &fast_sub};
static reader_t slow = {.name = "slow", .sub = &slow_sub};
static uint32_t log_sent, log_dropped, log_got, log_bad, log_expect;

static int read_one(reader_t *r) {
  imu_sample_t s;
  uint16_t len;
  uint32_t missed;
  if (v_bus_pop(r->sub, &s, sizeof s, &len, &missed) != VA_PASS)
    return 0;
  if (len != sizeof s || s.sum != checksum(&s))
    r->bad_sum++;
  if (s.seq != r->expect + missed)
    r->bad_order++;
  r->expect = s.seq + 1;
  r->got++;
  r->missed += missed;
  return 1;
}

static void fast_task(void *arg) {
  (void)arg;
  while (running) {
    while (read_one(&fast)) {
      if (fast.got % 10 == 0) { // forward a sample to the drop topic
        int r = v_bus_publish(&logt, &log_sent, sizeof log_sent);
        if (r == VA_PASS)
          log_sent++;
        else
          log_dropped++;
      }
    }
    v_delay(1);
  }
  for (;;)
    v_delay(1000);
}

static void slow_task(void *arg) {
  (void)arg;
  while (running) {
    read_one(&slow);
    v_delay(20);
  }
  for (;;)
    v_delay(1000);
}

static void logger_task(void *arg) {
  (void)arg;
  while (running) {
    uint32_t v;
    uint16_t len;
    if (v_bus_pop(&log_sub, &v, sizeof v, &len, NULL) == VA_PASS) {
      log_bad += v != log_expect; // drop never loses a queued message
      log_expect = v + 1;
      log_got++;
    }
    v_delay(30); // slower than fast forwards: the queue fills, drops happen
  }
  for (;;)
    v_delay(1000);
}

static void drain(reader_t *r) {
  while (read_one(r))
    ;
}

static void monitor_task(void *arg) {
  (void)arg;
  v_delay(RUN_MS);
  // Stop the producer, let the readers park, then drain everything.
  hal_timer_disable_interrupt(TIM2);
  hal_timer_stop(TIM2);
  running = 0;
  v_delay(50);
  drain(&fast);
  drain(&slow);
  uint32_t v;
  uint16_t len;
  while (v_bus_pop(&log_sub, &v, sizeof v, &len, NULL) == VA_PASS) {
    log_bad += v != log_expect;
    log_expect = v + 1;
    log_got++;
  }

  reader_t *rs[2] = {&fast, &slow};
  int pass = imu_sent > 1000;
  for (int i = 0; i < 2; i++) {
    reader_t *r = rs[i];
    v_log(LOG_INFO, "bus-ipc %s: got=%u missed=%u order_err=%u sum_err=%u",
          r->name, r->got, r->missed, r->bad_order, r->bad_sum);
    // every seq since it subscribed (at 0) was either read or reported lost
    pass = pass && r->bad_order == 0 && r->bad_sum == 0 &&
           r->got + r->missed == imu_sent;
  }
  pass = pass && slow.missed > 0; // overwrite really happened under it
  v_log(LOG_INFO, "bus-ipc imu sent=%u; log sent=%u dropped=%u got=%u bad=%u",
        imu_sent, log_sent, log_dropped, log_got, log_bad);
  pass = pass && log_got == log_sent && log_bad == 0 && log_dropped > 0;
  int pool_ok = v_bus_free_blocks(&bus) == BLOCKS && v_bus_check(&bus) == VA_PASS;
  v_log(LOG_INFO, "bus-ipc pool: free=%u/%u check=%s", v_bus_free_blocks(&bus),
        BLOCKS, pool_ok ? "ok" : "BROKEN");
  pass = pass && pool_ok;
  v_log(LOG_INFO, pass ? "BUS-IPC PASS" : "BUS-IPC FAIL");
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

  v_bus_init(&bus, pool_blocks, pool_desc, 32, BLOCKS);
  v_bus_topic_declare(&bus, &imu, "imu.raw", &OVERWRITE);
  v_bus_topic_declare(&bus, &logt, "log", NULL); // drop when full
  v_bus_subscribe(&imu, &fast_sub);
  v_bus_subscribe(&imu, &slow_sub);
  v_bus_subscribe(&logt, &log_sub);

  task_create_named(monitor_task, NULL, 1024, 6, "monitor");
  task_create_named(fast_task, NULL, 1024, 5, "fast");
  task_create_named(logger_task, NULL, 1024, 3, "logger");
  task_create_named(slow_task, NULL, 1024, 2, "slow");

  // TIM2 at 2 kHz; NavHAL's default IRQ priority (8) is inside the
  // kernel-masked band, as the ISR publish path requires.
  hal_timer_init_freq(TIM2, IMU_HZ);
  hal_timer_attach_callback(TIM2, tim2_isr);
  hal_timer_enable_interrupt(TIM2);
  hal_timer_start(TIM2);

  v_log(LOG_INFO, "bus-ipc: starting");
  scheduler_start();
  for (;;)
    ;
}
