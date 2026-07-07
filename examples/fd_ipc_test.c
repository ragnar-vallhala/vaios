#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Stage-3 fd-typed IPC test. Two tasks synchronise through *named* semaphores
 * referenced by per-task file descriptors — no shared handle is passed. Each
 * task v_sem_open()s the same names ("/sem/ping", "/sem/pong") and gets its own
 * fd to the same kernel object. They ping-pong: pinger gives ping and waits on
 * pong; ponger waits on ping and gives pong. Binary sems start at 0, so the
 * first v_sem_give kicks the handshake off.
 *
 * If the named registry, fd allocation, fd->object resolution, and the blocking
 * deferred-result path all work, you get an alternating ping/pong on the UART.
 *
 * Note on the timeout: the kernel has no "wait forever" sentinel — ticks_to_wait
 * is a finite deadline (now + ticks), so 0xFFFFFFFF would overflow into the past
 * and time out immediately. The partner always replies promptly, so a generous
 * finite timeout blocks cleanly until the handshake completes.
 */
#define WAIT_TICKS 5000u

void pinger_task(void *arg) {
  (void)arg;
  int ping = v_sem_open("/sem/ping", V_IPC_CREATE);
  int pong = v_sem_open("/sem/pong", V_IPC_CREATE);
  printk("[fd-ipc] pinger: ping=fd%d pong=fd%d\r\n", ping, pong);

  for (int i = 0;; i++) {
    printk("[fd-ipc] ping %d\r\n", i);
    v_sem_give(ping);            // wake the ponger
    v_sem_take(pong, WAIT_TICKS); // wait for its reply
    v_delay(500);
  }
}

void ponger_task(void *arg) {
  (void)arg;
  int ping = v_sem_open("/sem/ping", V_IPC_CREATE);
  int pong = v_sem_open("/sem/pong", V_IPC_CREATE);
  printk("[fd-ipc] ponger: ping=fd%d pong=fd%d\r\n", ping, pong);

  for (;;) {
    v_sem_take(ping, WAIT_TICKS); // wait for a ping
    printk("[fd-ipc]   pong\r\n");
    v_sem_give(pong);               // reply
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  task_create(pinger_task, NULL, 2048, 1);
  task_create(ponger_task, NULL, 2048, 1);
  scheduler_start();
  while (1)
    ;
}
