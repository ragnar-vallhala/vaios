/*
 * 58 — the release gate for unprivileged operation (roadmap M7): a
 * flight-shaped application in which EVERY application task is unprivileged.
 * Runs on the STM32F4 under Renode (tools/renode_flight_user.sh) or hardware.
 *
 * Privileged code is only what a board must do: clocks, the bus and its topics,
 * the telemetry queue, and the sensor ISR. Everything after scheduler_start is
 * unprivileged and reaches the kernel exclusively through syscalls.
 *
 *   TIM2 ISR (privileged)  publishes imu.raw at 1 kHz — the single-producer
 *                          lock-free path, as a real sensor ISR would.
 *   estimator (unpriv)     BLOCKS on imu.raw (v_bus_recv_wait), checks each
 *                          sample's checksum, and publishes att.est.
 *   controller (unpriv)    runs a fixed 5 ms cadence with task_delay_until,
 *                          reads the newest att.est, and sends telemetry to an
 *                          MPMC queue. Checks its own drift with v_get_ticks.
 *   logger (unpriv)        drains the telemetry queue with a blocking receive.
 *
 * Each task also: names itself (v_task_info), reads its own perf counters
 * (v_perf_self_stats), and spawns a worker of its own (v_task_spawn) which it
 * later ends (v_task_kill) — so task creation, ownership and teardown are all
 * exercised by the tasks themselves, not by init.
 *
 * Nothing here touches a global: an unprivileged task may read and write only
 * its own stack and heap block, plus read-only flash. Tasks therefore share
 * nothing but topics and the queue, and each verdict is printed by the task that
 * reached it: "[flight] <name> PASS" / "FAIL".
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "bus.h"
#include "navhal.h"
#include "perf.h"
#include "port.h" // v_port_is_privileged
#include "structure.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"

#define IMU_HZ 200 // Renode emulates ~10x slower than the board; 1 kHz there
                    // saturates the emulated CPU and measures the emulator
#define CTL_PERIOD_TICKS 5 // 5 ms control cadence
#define CTL_CYCLES 200     // 1 s of flight
#define TELEM_CAP 8
#define BLOCKS 40
#define BLOCK_SZ 32
#define TIM_CLK_HZ 84000000u

typedef struct {
  uint32_t seq;
  int16_t gyro[3];
  uint16_t sum;
} imu_sample_t;

typedef struct {
  uint32_t seq;
  int16_t att[3];
  uint16_t sum;
} att_est_t;

typedef struct {
  uint32_t cycle;
  uint32_t tick;
  int16_t att0;
} telem_t;

V_BUS_POOL(pool, BLOCK_SZ, BLOCKS);
static v_bus_t bus;
static v_bus_topic_t imu, att;
static mpmc_queue_t telem_q;
static telem_t telem_buf[TELEM_CAP];
static const v_bus_topic_cfg_t OVERWRITE = {.overflow = V_BUS_OVERWRITE};
static const v_bus_topic_cfg_t EST_CFG = {.reserve = 4};

// --- privileged: the sensor ISR ----------------------------------------------
static volatile uint32_t imu_seq;

static uint16_t imu_checksum(const imu_sample_t *s) {
  uint32_t x = s->seq ^ (uint16_t)s->gyro[0] ^ ((uint32_t)(uint16_t)s->gyro[1] << 8) ^
               (uint16_t)s->gyro[2];
  return (uint16_t)(x ^ (x >> 16));
}
static uint16_t att_checksum(const att_est_t *s) {
  uint32_t x = s->seq ^ (uint16_t)s->att[0] ^ ((uint32_t)(uint16_t)s->att[1] << 8) ^
               (uint16_t)s->att[2];
  return (uint16_t)(x ^ (x >> 16));
}

static void tim2_isr(void) {
  hal_timer_clear_interrupt_flag(TIM2);
  imu_sample_t s = {.seq = imu_seq,
                    .gyro = {(int16_t)imu_seq, (int16_t)(imu_seq * 3),
                             (int16_t)-(int32_t)imu_seq}};
  s.sum = imu_checksum(&s);
  if (v_bus_publish(&imu, &s, sizeof s) == VA_PASS)
    imu_seq++;
}

// --- unprivileged side -------------------------------------------------------
// Console via fd 1. Read-only syscall arguments may point into flash, so the
// format literal goes straight in and only the result lands in task memory.
static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

// Who am I, what have I done, and can I run a worker of my own? All kernel-side
// state, all through syscalls.
static int identify_and_spawn(const char *expect, void (*worker)(void *)) {
  v_task_info_t me;
  if (v_task_info(&me) != VA_PASS || v_strcmp(me.name, expect) != 0) {
    say("[flight] bad self-info\r\n", 0, 0);
    return -1;
  }
  v_perf_task_t mine;
  v_perf_self_stats(&mine);
  if (mine.switches_in == 0) {
    say("[flight] no perf counters\r\n", 0, 0);
    return -1;
  }
  say("[flight] %s id=%d nPRIV=1\r\n", (int)(uintptr_t)expect, (int)me.id);

  // A worker below our own priority: asking for more is refused by the kernel.
  v_task_spawn_t cfg = {.entry = worker, .arg = 0, .stack_size = 1024,
                        .priority = me.priority > 1 ? me.priority - 1 : 1,
                        .name = "worker"};
  int wid = v_task_spawn(&cfg);
  if (wid <= 0) {
    say("[flight] spawn failed %d\r\n", wid, 0);
    return -1;
  }
  return wid;
}

// What every task's worker does: nothing but prove it is alive and unprivileged,
// then wait to be ended by its owner.
static void worker_task(void *arg) {
  (void)arg;
  say("[flight] worker nPRIV=%d\r\n", v_port_is_privileged() ? 0 : 1, 0);
  for (;;)
    v_delay(50);
}

// The estimator: sleeps on the sensor topic instead of polling, and verifies
// every sample it is handed.
static void estimator_task(void *arg) {
  (void)arg;
  int bad = 0;
  int wid = identify_and_spawn("estimator", worker_task);
  bad |= wid < 0;

  int in = v_bus_open("imu.raw", V_BUS_RD);
  int out = v_bus_open("att.est", V_BUS_WR);
  if (in < 0 || out < 0) {
    say("[flight] estimator open %d %d\r\n", in, out);
    bad = 1;
  }

  uint32_t got = 0, torn = 0, missed_total = 0;
  // Same horizon as the controller: the scenario ends when it does.
  while (!bad && got < CTL_CYCLES) {
    imu_sample_t s;
    v_bus_rx_t rx = {.buf = &s, .cap = sizeof s};
    if (v_bus_recv_wait(in, &rx, 100) != VA_PASS) {
      say("[flight] estimator starved at %d\r\n", (int)got, 0);
      bad = 1;
      break;
    }
    if (rx.len != sizeof s || s.sum != imu_checksum(&s))
      torn++;
    missed_total += rx.missed;
    got++;

    att_est_t e = {.seq = s.seq,
                   .att = {s.gyro[0] / 2, s.gyro[1] / 2, s.gyro[2] / 2}};
    e.sum = att_checksum(&e);
    v_bus_send(out, &e, sizeof e); // att.est is an overwrite topic: never blocks
  }
  if (!bad && torn) {
    say("[flight] estimator torn=%d\r\n", (int)torn, 0);
    bad = 1;
  }
  if (wid > 0 && v_task_kill((uint32_t)wid) != VA_PASS) {
    say("[flight] estimator kill failed\r\n", 0, 0);
    bad = 1;
  }
  say(bad ? "[flight] estimator FAIL\r\n" : "[flight] estimator PASS\r\n", 0, 0);
  for (;;)
    v_delay(1000);
}

// The controller: a fixed cadence that must not drift, which is what
// task_delay_until and v_get_ticks are for.
static void controller_task(void *arg) {
  (void)arg;
  int bad = 0;
  int wid = identify_and_spawn("controller", worker_task);
  bad |= wid < 0;

  int est = v_bus_open("att.est", V_BUS_RD);
  int tq = v_queue_open("/q/telem", V_Q_WR);
  if (est < 0 || tq < 0) {
    say("[flight] controller open %d %d\r\n", est, tq);
    bad = 1;
  }

  uint32_t start = v_get_ticks(), last = start, cycles = 0, sent = 0, torn = 0;
  while (!bad && cycles < CTL_CYCLES) {
    // Take the newest estimate available; an overwrite topic means we skip
    // stale ones rather than falling behind.
    att_est_t e;
    v_bus_rx_t rx = {.buf = &e, .cap = sizeof e};
    int16_t att0 = 0;
    while (v_bus_recv(est, &rx) == VA_PASS) {
      if (rx.len != sizeof e || e.sum != att_checksum(&e))
        torn++;
      att0 = e.att[0];
    }
    telem_t t = {.cycle = cycles, .tick = v_get_ticks(), .att0 = att0};
    if (v_queue_send(tq, &t, 20) == VA_PASS)
      sent++;
    cycles++;
    task_delay_until(&last, CTL_PERIOD_TICKS);
  }

  // The cadence is the point: CTL_CYCLES periods must have taken about
  // CTL_CYCLES * CTL_PERIOD_TICKS ticks, with no accumulated drift.
  uint32_t elapsed = v_get_ticks() - start;
  uint32_t want = (uint32_t)CTL_CYCLES * CTL_PERIOD_TICKS;
  uint32_t slack = want / 10u; // 10%: the logger's work must not push us out
  if (!bad && (elapsed + slack < want || elapsed > want + slack)) {
    say("[flight] cadence %d ticks, wanted %d\r\n", (int)elapsed, (int)want);
    bad = 1;
  }
  if (!bad && (torn || sent < CTL_CYCLES - 4u)) {
    say("[flight] controller torn=%d sent=%d\r\n", (int)torn, (int)sent);
    bad = 1;
  }
  if (wid > 0 && v_task_kill((uint32_t)wid) != VA_PASS)
    bad = 1;
  say(bad ? "[flight] controller FAIL\r\n" : "[flight] controller PASS\r\n", 0,
      0);
  for (;;)
    v_delay(1000);
}

// The logger: drains telemetry with a blocking receive, in order.
static void logger_task(void *arg) {
  (void)arg;
  int bad = 0;
  int wid = identify_and_spawn("logger", worker_task);
  bad |= wid < 0;

  int tq = v_queue_open("/q/telem", V_Q_RD);
  if (tq < 0) {
    say("[flight] logger open %d\r\n", tq, 0);
    bad = 1;
  }

  uint32_t got = 0, expect = 0, disorder = 0;
  while (!bad && got < CTL_CYCLES - 8u) {
    telem_t t;
    if (v_queue_recv(tq, &t, 200) != VA_PASS)
      break; // the controller has finished
    if (t.cycle < expect) {
      disorder++;
    }
    expect = t.cycle + 1;
    got++;
  }
  if (!bad && (disorder || got < CTL_CYCLES / 2u)) {
    say("[flight] logger got=%d disorder=%d\r\n", (int)got, (int)disorder);
    bad = 1;
  }
  if (wid > 0 && v_task_kill((uint32_t)wid) != VA_PASS)
    bad = 1;
  say(bad ? "[flight] logger FAIL\r\n" : "[flight] logger PASS\r\n", 0, 0);
  for (;;)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  v_perf_init(); // so the tasks' own counters are real

  // Everything a task cannot do for itself: the bus, its topics, the queue.
  v_bus_init(&bus, pool_blocks, pool_desc, BLOCK_SZ, BLOCKS);
  v_bus_topic_declare(&bus, &imu, "imu.raw", &OVERWRITE);
  v_bus_topic_declare(&bus, &att, "att.est", &EST_CFG);
  mpmc_init(&telem_q, telem_buf, TELEM_CAP, sizeof(telem_t));
  v_queue_register("/q/telem", &telem_q);

  // The sensor: TIM2 at 1 kHz, NavHAL's default IRQ priority sits inside the
  // kernel-maskable band.
  hal_timer_config_t tick = {.prescaler = TIM_CLK_HZ / 1000000u - 1,
                             .auto_reload = 1000000u / IMU_HZ};
  hal_timer_init(TIM2, &tick);
  hal_timer_attach_callback(TIM2, tim2_isr);
  hal_timer_enable_interrupt(TIM2);
  hal_timer_start(TIM2);

  v_log(LOG_INFO, "flight_user: start (privileged init only)");
  task_create_named(estimator_task, NULL, 2048, 3, "estimator");
  task_create_named(controller_task, NULL, 2048, 2, "controller");
  task_create_named(logger_task, NULL, 2048, 1, "logger");
  scheduler_start();
  for (;;)
    ;
}
