/*
 * 57 — unprivileged tasks on the Bus IPC through the fd syscalls (v_bus_open /
 * v_bus_send / v_bus_recv), on target. Runs on the STM32F4 under Renode
 * (tools/renode_bus_user.sh) or real hardware.
 *
 * With VAIOS_MPU_USER_SEPARATION the tasks run unprivileged: they cannot touch
 * the bus structs or the block pool at all, so privileged main declares the
 * topics and each task only opens one by name.
 *
 *   sensor.q  default (drop) topic. The producer task sends a counter at
 *             ~1 kHz; two consumer tasks read it at their own pace, each
 *             checking order and that its own `missed` stays 0 (drop policy
 *             never overwrites what a reader still owes).
 *   cmd.pipe  a pipe topic (SPSC ring): the producer writes, consumer A reads,
 *             and consumer B's open must be refused — one reader only, checked
 *             through the syscall path too.
 *
 * Each task also checks that the kernel refuses user pointers into kernel
 * memory with -14 (EFAULT) instead of faulting: as a send payload, as the rx
 * block, and as the buffer the rx block points at (the two-level check). Flash
 * is readable by every task, so the topic names are string literals; flash as
 * an rx buffer is refused.
 *
 * Every task here is unprivileged (only idle is not), which also means no task
 * may touch a global: not the bus, not a shared progress flag, not even
 * current_task behind GET_CURRENT_TASK_ID(). So the tasks share nothing but the
 * topics — the message count is a compile-time constant and each side counts
 * its own — and v_bus_check stays in the privileged host suite. A leaked block
 * shows up here as the producer stalling, which fails the consumers' count.
 * The producer also reads the clock and runs a drift-free cadence
 * (v_get_ticks / task_delay_until), which are kernel globals behind SYS_ticks
 * and SYS_delay_until. Each task prints "[busu] <P|A|B> PASS" / "FAIL".
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "bus.h"
#include "navhal.h"
#include "port.h" // v_port_is_privileged
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"

#define MSGS 400
#define BLOCKS 24
#define KERNEL_SRAM ((void *)0x20000010u)
#define EFAULT_RC (-14)

V_BUS_POOL(pool, 32, BLOCKS);
static v_bus_t bus;
static v_bus_topic_t sensor, cmd;
static const v_bus_topic_cfg_t PIPE_CFG = {.pipe = 1, .reserve = 4};

// Console via fd 1: read-only syscall arguments may point into flash, so the
// format literal goes straight in and only the result lands in task memory.
static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

// The pointer checks, run against the producer's own handles. The sends go to
// the pipe, so the counted stream on sensor.q stays exactly 0..MSGS-1.
static int check_efault(int wfd, int rfd) {
  int bad = 0;
  uint32_t v = 1;

  // Kernel memory as the send payload.
  int rc = v_bus_send(wfd, KERNEL_SRAM, 4);
  if (rc != EFAULT_RC) {
    say("[busu] kernel payload rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // Kernel memory as the rx block itself.
  rc = v_bus_recv(rfd, (v_bus_rx_t *)KERNEL_SRAM);
  if (rc != EFAULT_RC) {
    say("[busu] kernel rx rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // An rx block of the task's own, pointing its buffer at kernel memory: the
  // second check is what stops a task writing where it pleases.
  v_bus_rx_t evil = {.buf = KERNEL_SRAM, .cap = 4};
  rc = v_bus_recv(rfd, &evil);
  if (rc != EFAULT_RC) {
    say("[busu] kernel rx.buf rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // Flash is readable, never writable: as an rx buffer it is refused too.
  v_bus_rx_t ro = {.buf = (void *)"flash!", .cap = 4};
  rc = v_bus_recv(rfd, &ro);
  if (rc != EFAULT_RC) {
    say("[busu] flash rx.buf rc=%d (want %d)\r\n", rc, EFAULT_RC);
    bad = 1;
  }
  // None of the refusals broke the handles: a real send still works.
  if (v_bus_send(wfd, &v, sizeof v) != VA_PASS) {
    say("[busu] send after refusals failed\r\n", 0, 0);
    bad = 1;
  }
  return bad;
}

// Producer: publishes a counter on sensor.q and mirrors it onto cmd.pipe.
static void producer(void *arg) {
  (void)arg;
  int bad = 0;
  say("[busu] P nPRIV=%d\r\n", v_port_is_privileged() ? 0 : 1, 0);

  int q = v_bus_open("sensor.q", V_BUS_WR); // names read straight from flash
  int p = v_bus_open("cmd.pipe", V_BUS_WR);
  if (q < 0 || p < 0) {
    say("[busu] P open q=%d p=%d\r\n", q, p);
    bad = 1;
  }
  // A reader's handle may not publish, and this one may not read.
  if (!bad) {
    v_bus_rx_t rx = {.buf = (void *)&bad, .cap = 0};
    if (v_bus_recv(q, &rx) != V_BUS_EINVAL) {
      say("[busu] P write handle read %d\r\n", 0, 0);
      bad = 1;
    }
  }
  if (!bad)
    bad = check_efault(p, q);

  for (uint32_t i = 0; i < MSGS && !bad; i++) {
    // Drop policy: a full pool refuses the send, so retry rather than skip —
    // the consumers expect an unbroken sequence. Bounded, so a wedged pool
    // fails the run instead of hanging it.
    int tries = 0;
    while (v_bus_send(q, &i, sizeof i) != VA_PASS) {
      v_delay(1);
      if (++tries > 2000) {
        say("[busu] P stuck at %d\r\n", (int)i, 0);
        bad = 1;
        break;
      }
    }
    v_bus_send(p, &i, sizeof i); // pipe: the ring may be full, that is fine
    if ((i & 7u) == 0)
      v_delay(1);
  }
  // The clock and the drift-free cadence, from unprivileged code: both are
  // kernel globals reached through SYS_ticks / SYS_delay_until, so before those
  // existed this faulted.
  uint32_t t0 = v_get_ticks(), last = t0;
  for (int k = 0; k < 5; k++)
    task_delay_until(&last, 2);
  uint32_t dt = v_get_ticks() - t0;
  if (last != t0 + 10 || dt < 10) {
    say("[busu] cadence last+%d dt=%d\r\n", (int)(last - t0), (int)dt);
    bad = 1;
  }

  say(bad ? "[busu] P FAIL\r\n" : "[busu] P PASS\r\n", 0, 0);
  for (;;)
    v_delay(1000);
}

// Consumers: id 1 also owns the pipe's single reader; id 2 must be refused it.
static void consumer(void *arg) {
  int id = (int)(uintptr_t)arg;
  int bad = 0;
  uint32_t expect = 0, got = 0, pipe_got = 0, last = 0;

  say(id == 1 ? "[busu] A nPRIV=%d\r\n" : "[busu] B nPRIV=%d\r\n",
      v_port_is_privileged() ? 0 : 1, 0);

  int q = v_bus_open("sensor.q", V_BUS_RD);
  if (q < 0) {
    say("[busu] open sensor.q %d\r\n", q, 0);
    bad = 1;
  }
  int p = v_bus_open("cmd.pipe", V_BUS_RD); // only one of the two gets it
  if (id == 2 && p != V_BUS_EBUSY) {
    say("[busu] second pipe reader p=%d (want %d)\r\n", p, V_BUS_EBUSY);
    bad = 1;
  }

  // No shared flag to watch: MSGS is what the producer will send, and an
  // unbroken sequence is exactly what this checks. The spin bound turns a
  // stalled producer into a FAIL rather than a hung run.
  for (uint32_t spins = 0; !bad && last + 1 < MSGS && spins < 400000u; spins++) {
    uint32_t v;
    v_bus_rx_t rx = {.buf = &v, .cap = sizeof v};
    if (v_bus_recv(q, &rx) == VA_PASS) {
      // A subscription starts empty: whatever this reader sees first is its
      // base (the producer may already have sent a few). From there the
      // sequence must be unbroken, and drop policy keeps `missed` at 0.
      if (rx.len != sizeof v || (got && v != expect) || rx.missed != 0) {
        say("[busu] got %d want %d\r\n", (int)v, (int)expect);
        bad = 1;
      }
      expect = v + 1;
      last = v;
      got++;
    } else {
      v_delay(1); // nothing queued for us yet
    }
    if (p >= 0) {
      v_bus_rx_t prx = {.buf = &v, .cap = sizeof v};
      if (v_bus_recv(p, &prx) == VA_PASS)
        pipe_got++;
    }
  }
  // It must have followed the stream to the end, and read most of it (a few
  // early messages predate its subscription).
  if (!bad && (last + 1 != MSGS || got < MSGS - 16)) {
    say("[busu] only got %d, last %d\r\n", (int)got, (int)last);
    bad = 1;
  }
  if (!bad && p >= 0 && pipe_got == 0) {
    say("[busu] pipe read nothing\r\n", 0, 0);
    bad = 1;
  }
  say(id == 1 ? (bad ? "[busu] A FAIL\r\n" : "[busu] A PASS\r\n")
              : (bad ? "[busu] B FAIL\r\n" : "[busu] B PASS\r\n"),
      0, 0);
  for (;;)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  v_bus_init(&bus, pool_blocks, pool_desc, 32, BLOCKS);
  v_bus_topic_declare(&bus, &sensor, "sensor.q", NULL);
  v_bus_topic_declare(&bus, &cmd, "cmd.pipe", &PIPE_CFG);
  v_log(LOG_INFO, "bus_user: start (privileged main)");

  // Consumers above the producer, so they subscribe before it publishes.
  task_create(producer, NULL, 2048, 2);
  task_create(consumer, (void *)1, 2048, 3);
  task_create(consumer, (void *)2, 2048, 3);
  scheduler_start();
  for (;;)
    ;
}
