/*
 * examples/perf_demo.c — full end-to-end exercise of the perf module.
 *
 * A self-contained workload that touches every observability surface, then a
 * reporter loops a v_perf_dump() over the console (USART2 @ 115200 on the
 * Nucleo-F401RE) so the snapshot can be captured and verified on real silicon:
 *
 *   - Scheduler / SysTick : 6 always-running tasks at three priorities
 *   - Per-task CPU + STACK : each task's cycles, switch-ins, and stack
 *                            high-water (the new [tasks] section). burn_stack
 *                            drives one task's stack deep so its high-water is
 *                            visibly larger than the idle tasks'.
 *   - IPC                  : a counting semaphore (contended) + a mutex
 *   - Heap                 : varied-size alloc/free churn
 *   - SPSC FIFO            : a producer outruns a slower consumer so the fifo
 *                            fills (peak rises) and DROP policy sheds items
 *                            (drops > 0) — the buffer-sizing/backpressure signal
 *
 * Build:  cmake -S . -B build -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=PERF_DEMO
 *         cmake --build build --target main
 * Flash:  st-flash --reset write build/main.bin 0x08000000
 * Watch:  /dev/ttyACM0 @ 115200
 */
#include "ipc.h"
#include "memory.h"
#include "perf.h"
#include "structure.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/* ---- shared resources ---------------------------------------------------- */
static SemaphoreHandle_t g_sem; /* counting(2) — workers contend, some block  */
static MutexHandle_t g_mtx;     /* mutual exclusion around the shared counter */
static volatile uint32_t g_shared;

static uint32_t g_fifo_buf[32];
static spsc_fifo_t g_fifo; /* uint32 ring, DROP policy                       */

static volatile uint32_t g_sink; /* defeats dead-store elimination           */

/* ---- workload tasks ------------------------------------------------------ */

/* Producer: bursts of 6 every 4 ms. The consumer drains 3 every 12 ms, so the
 * fifo fills toward capacity (peak climbs) and a full DROP fifo sheds the rest
 * (drops climb) — exactly the backpressure picture we want to read back. */
static void producer_task(void *arg) {
  (void)arg;
  uint32_t seq = 0;
  for (;;) {
    uint32_t burst[6];
    for (int i = 0; i < 6; i++)
      burst[i] = seq++;
    spsc_write(&g_fifo, burst, 6);
    v_delay(4);
  }
}

static void consumer_task(void *arg) {
  (void)arg;
  for (;;) {
    uint32_t out[3];
    size_t n = spsc_read(&g_fifo, out, 3);
    for (size_t i = 0; i < n; i++)
      g_sink += out[i];
    v_delay(12);
  }
}

/* Heap + IPC churn: varied alloc sizes (per-class buckets) and a contended
 * take/give plus a mutex-guarded shared increment. */
static void worker_task(void *arg) {
  uint32_t id = *(uint32_t *)arg;
  uint32_t i = 0;
  for (;;) {
    size_t sz = (size_t)(16u + id * 24u + (i++ & 7u) * 16u);
    void *p = v_malloc(sz);
    if (p)
      v_free(p);

    if (v_semaphore_take(g_sem, 25) == VA_PASS) {
      v_delay(2);
      v_semaphore_give(g_sem);
    }

    if (v_mutex_lock(g_mtx, 25) == VA_PASS) {
      g_shared++;
      v_mutex_unlock(g_mtx);
    }
    v_delay(6);
  }
}

/* Recurse to push the stack high-water mark well up: ~256 B per frame. */
static void burn_stack(uint32_t depth) {
  volatile uint8_t frame[256];
  for (int i = 0; i < 256; i++)
    frame[i] = (uint8_t)(depth + (uint32_t)i);
  if (depth)
    burn_stack(depth - 1);
  g_sink += frame[depth & 0xFFu]; /* touch after recursion: no tail-call */
}

static void deep_stack_task(void *arg) {
  (void)arg;
  burn_stack(5); /* ~1.5 KB peak — should dominate this task's high-water */
  for (;;)
    v_delay(500); /* stay alive; high-water is sticky */
}

/* ---- reporter ------------------------------------------------------------ */
static void reporter_task(void *arg) {
  (void)arg;
  v_delay(300); /* warm-up: let the workload move the counters */
  for (;;) {
    print_fmt("\r\n========== perf_demo snapshot ==========\r\n");
    v_perf_dump(); /* sched / isr / ipc / heap / [tasks]+stack */

    /* SPSC fifo is not in the kernel snapshot — read it explicitly. */
    print_fmt("[fifo g_fifo]\r\n");
    print_fmt("  fill/peak/cap: %u / %u / %u\r\n",
              (unsigned)spsc_available(&g_fifo), (unsigned)spsc_peak(&g_fifo),
              (unsigned)spsc_space(&g_fifo) + (unsigned)spsc_available(&g_fifo));
    print_fmt("  drops:         %u\r\n", (unsigned)spsc_drops(&g_fifo));
    print_fmt("  shared_ctr:    %u\r\n", (unsigned)g_shared);
    print_fmt("========================================\r\n");
    v_delay(2000);
  }
}

/* ---- setup --------------------------------------------------------------- */
static uint32_t w1 = 1, w2 = 2;

static void kernel_task(void *arg) {
  (void)arg;
  g_sem = v_semaphore_create_counting(2, 2);
  g_mtx = v_mutex_create();
  spsc_init(&g_fifo, g_fifo_buf, 32, sizeof(uint32_t));
  spsc_set_policy(&g_fifo, SPSC_POLICY_DROP);

  task_create(producer_task, NULL, 1024, 2);
  task_create(consumer_task, NULL, 1024, 1);
  task_create(worker_task, &w1, 2048, 1);
  task_create(worker_task, &w2, 2048, 1);
  task_create(deep_stack_task, NULL, 4096, 1); /* needs room for burn_stack */
  task_create(reporter_task, NULL, 2048, 3);   /* highest: owns the console */
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  task_create(kernel_task, NULL, 1024, 0);
  scheduler_start();
  for (;;) {
  }
  return 0;
}
