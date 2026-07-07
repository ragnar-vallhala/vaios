#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Stage-3 fd mutex + v_wait test. Two producers each signal a distinct named
 * semaphore at different rates; a consumer v_wait()s on BOTH sem fds at once and
 * services whichever fires. A shared named *mutex* serialises the log so lines
 * never interleave — exercising v_mtx_open/lock/unlock across three tasks.
 *
 * fds are per-task, so every task opens the names it needs itself (opening the
 * same name yields a private fd to the same shared kernel object).
 *
 * Expected: "/sem/a" lines about twice as often as "/sem/b", every log line
 * intact (mutex working), and the consumer never spins on an empty wait.
 */
static void logline(int mtx, const char *who, int idx) {
  v_mtx_lock(mtx, V_WAIT_FOREVER);
  printk("[fd-wait] %s idx=%d (%s)\r\n", who, idx, idx == 0 ? "/sem/a" : "/sem/b");
  v_mtx_unlock(mtx);
}

static void producer(const char *name, int idx, uint32_t period_ms) {
  int fd = v_sem_open(name, V_IPC_CREATE);
  int mtx = v_mtx_open("/mtx/log", V_IPC_CREATE);
  for (;;) {
    v_sem_give(fd);
    logline(mtx, "produced", idx);
    v_delay(period_ms);
  }
}

void prod_a_task(void *arg) { (void)arg; producer("/sem/a", 0, 300); }
void prod_b_task(void *arg) { (void)arg; producer("/sem/b", 1, 700); }

void consumer_task(void *arg) {
  (void)arg;
  int fds[2];
  fds[0] = v_sem_open("/sem/a", V_IPC_CREATE);
  fds[1] = v_sem_open("/sem/b", V_IPC_CREATE);
  int mtx = v_mtx_open("/mtx/log", V_IPC_CREATE);
  for (;;) {
    int idx = v_wait(fds, 2, V_WAIT_FOREVER); // block until either is ready
    if (idx < 0)
      continue;
    v_sem_take(fds[idx], 0); // consume the ready one (non-blocking)
    logline(mtx, "consumed", idx);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  task_create(prod_a_task, NULL, 1024, 1);
  task_create(prod_b_task, NULL, 1024, 1);
  task_create(consumer_task, NULL, 2048, 2); // higher priority
  scheduler_start();
  while (1)
    ;
}
