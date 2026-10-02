/* The console's wakes (devfs.c: one semaphore for RX, one for TX) for the
 * binaries that build devfs without kernel/ipc.c. Binary semaphores that bank
 * one give each, which is all the console relies on; the banked flag lives in
 * the caller's StaticSemaphore_t, so each handle has its own.
 * g_stub_sem_last_ticks records what a take asked for, so a test can see the
 * no-interrupt fallback clamp the wait to a poll interval. */
#include "ipc.h"
#include "task.h"
#include <string.h>

uint32_t g_stub_sem_last_ticks;

static uint8_t *banked(SemaphoreHandle_t sem) { return (uint8_t *)sem; }

SemaphoreHandle_t v_semaphore_create_binary_static(StaticSemaphore_t *b) {
  memset(b, 0, sizeof *b);
  return (SemaphoreHandle_t)b;
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem, int *woken) {
  *banked(sem) = 1;
  if (woken)
    *woken = 1; /* say a waiter outranks us, so the preempt path runs */
  return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks) {
  g_stub_sem_last_ticks = ticks;
  if (!*banked(sem))
    return VA_FAIL; /* nobody else runs here: an empty take is a timeout */
  *banked(sem) = 0;
  return VA_PASS;
}

/* The wake's preempt request: nothing to switch to in these binaries. */
void v_port_trigger_pendsv(void) {}
