#include "bus.h"
#include "task.h"
#include "upipe.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * v_upipe (VAIOS_UPIPE): two unprivileged tasks move N messages of 8 B and of
 * 256 B through a zero-copy pipe, then the same through a bus pipe topic (two
 * syscalls and two copies a message), and print the time per message. The
 * consumer runs above the producer, so every message takes the slow path: the
 * consumer is asleep, the commit kicks it awake.
 *
 * A third unprivileged task is handed the pipe's address and reads it: it is
 * not an end of the pipe, so it faults and is killed (KILL policy); the
 * benchmark goes on.
 */

#define N 20000u
#define SLOT 256u
#define STK 2048

/* -DSTREAM=1: producer and consumer share a priority and the ring has 64
 * slots, so the producer fills it and the consumer drains it in batches: the
 * sleep/kick syscalls are amortised over many messages. */
#ifdef STREAM
#define PRIO_PROD 2
#define PRIO_CONS 2
#define RING 64
#define TAG "stream "
#else
#define PRIO_PROD 2
#define PRIO_CONS 3
#define RING 4
#define TAG ""
#endif

#define BLOCK (SLOT + V_BUS_HDR_SIZE) // a pipe topic's message is one block
V_BUS_POOL(pool, BLOCK, 6); // any bigger and the heap no longer fits the F401
static v_bus_t bus;
static v_bus_topic_t topic;
static const v_bus_topic_cfg_t PIPE_CFG = {.pipe = 1, .reserve = 4};

extern void v_print(const char *s); /* polling console write (privileged) */

static v_upipe_t *pipe_addr; // kernel memory: only privileged code reads it

static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

static v_upipe_t *my_pipe(void) {
  v_upipe_t *p;
  while (!(p = v_upipe_self()))
    v_delay(1);
  return p;
}

static void fill(uint32_t *w, uint32_t words, uint32_t seq) {
  for (uint32_t i = 0; i < words; i++)
    w[i] = seq + i;
}

// Sum of the payload, and whether it is the one fill() wrote for `seq`.
static int check(const uint32_t *w, uint32_t words, uint32_t seq) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < words; i++)
    sum += w[i];
  return sum == seq * words + words * (words - 1u) / 2u;
}

static void producer(void *arg) {
  (void)arg;
  v_upipe_t *p = my_pipe();
  int wfd = v_bus_open("bench.pipe", V_BUS_WR);
  static const uint32_t sizes[2] = {8, SLOT};
  uint32_t buf[SLOT / 4];
  for (int s = 0; s < 2; s++) {
    uint32_t words = sizes[s] / 4u;
    uint32_t waits = 0; // v_upipe_wait calls: a task's statics are kernel RAM
    for (uint32_t i = 0; i < N; i++) {
      uint32_t *slot;
      while (!(slot = v_upipe_reserve(p))) {
        waits++;
        v_upipe_wait(p, 1, 10);
      }
      fill(slot, words, i);
      v_upipe_commit(p);
    }
    say("[up] " TAG "%u B: upipe prod waits/1000msg %u\r\n", (int)sizes[s],
        (int)(waits * 1000u / N));
    for (uint32_t i = 0; i < N; i++) {
      fill(buf, words, i);
      int rc;
      while ((rc = v_bus_send(wfd, buf, (uint16_t)sizes[s])) == VA_FAIL)
        v_delay(1); // full
      if (rc != VA_PASS) {
        say("[up] bus send %d\r\n", rc, 0);
        break;
      }
    }
  }
  for (;;)
    v_delay(1000);
}

static void consumer(void *arg) {
  (void)arg;
  v_upipe_t *p = my_pipe();
  int rfd = v_bus_open("bench.pipe", V_BUS_RD);
  static const uint32_t sizes[2] = {8, SLOT};
  uint32_t buf[SLOT / 4];
  int bad = rfd < 0;
  for (int s = 0; s < 2; s++) {
    uint32_t words = sizes[s] / 4u;
    uint32_t waits = 0;
    uint32_t t0 = v_get_ticks();
    for (uint32_t i = 0; i < N; i++) {
      const uint32_t *slot;
      while (!(slot = v_upipe_peek(p))) {
        waits++;
        v_upipe_wait(p, 0, 10);
      }
      if (!check(slot, words, i) && !bad) {
        say("[up] upipe data bad at %u\r\n", (int)i, 0);
        bad = 1;
      }
      v_upipe_release(p);
    }
    uint32_t t1 = v_get_ticks();
    for (uint32_t i = 0; i < N; i++) {
      v_bus_rx_t rx = {.buf = buf, .cap = sizeof buf};
      int rc = v_bus_recv_wait(rfd, &rx, 100);
      if (rc != VA_PASS) {
        say("[up] bus recv %d at %u\r\n", rc, (int)i);
        bad = 1;
        break;
      }
      if (!check(buf, words, i) && !bad) {
        say("[up] bus data bad at %u len %u\r\n", (int)i, (int)rx.len);
        bad = 1;
      }
    }
    uint32_t t2 = v_get_ticks();
    // ms for N messages -> ns per message (1 tick = 1 ms)
    say("[up] " TAG "%u B: upipe %u ns/msg", (int)sizes[s],
        (int)((t1 - t0) * 1000000u / N));
    say(", bus %u ns/msg\r\n", (int)((t2 - t1) * 1000000u / N), 0);
    say("[up] " TAG "%u B: upipe cons waits/1000msg %u\r\n", (int)sizes[s],
        (int)(waits * 1000u / N));
  }
  say(bad ? "[up] data FAIL\r\n" : "[up] data ok\r\n", 0, 0);
  for (;;)
    v_delay(1000);
}

static void intruder(void *arg) {
  v_upipe_t *p = arg;
  say("[up] intruder reads the pipe\r\n", 0, 0);
  say("[up] BUG: intruder read wr=%u\r\n", (int)p->wr, 0);
  for (;;)
    v_delay(1000);
}

static void monitor(void *arg) {
  (void)arg;
  for (;;) {
    task_delay(2000);
    char buf[96];
    print_fmt_buf(buf, sizeof buf,
                  "[up] faults=%u last addr 0x%x pipe 0x%x\r\n",
                  (unsigned)v_mpu_fault_stats.faults,
                  (unsigned)v_mpu_fault_stats.last_addr, (unsigned)pipe_addr);
    v_print(buf);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  v_bus_init(&bus, pool_blocks, pool_desc, BLOCK, 6);
  v_bus_topic_declare(&bus, &topic, "bench.pipe", &PIPE_CFG);
  v_print("[up] upipe: start\r\n");
  uint32_t prod = task_create_named(producer, NULL, STK, PRIO_PROD, "prod");
  uint32_t cons = task_create_named(consumer, NULL, STK, PRIO_CONS, "cons");
  pipe_addr = v_upipe_create(SLOT, RING, prod, cons);
  if (!pipe_addr)
    v_print("[up] BUG: v_upipe_create failed\r\n");
  uint32_t in = task_create_named(intruder, pipe_addr, STK, 4, "intruder");
  task_set_fault_policy(in, V_FAULT_KILL, 0);
  task_create_privileged(monitor, NULL, STK, 1, "monitor");
  scheduler_start();
  for (;;)
    ;
}
