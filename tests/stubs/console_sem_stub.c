/* The console's RX wake (devfs.c) for the binaries that build devfs without
 * kernel/ipc.c. A binary semaphore that banks one give, which is all the
 * console relies on; g_stub_sem_last_ticks records what a take asked for, so a
 * test can see the no-interrupt fallback clamp the wait to a poll interval. */
#include "ipc.h"

static int _banked;
uint32_t g_stub_sem_last_ticks;

SemaphoreHandle_t v_semaphore_create_binary_static(StaticSemaphore_t *b) {
  _banked = 0;
  return (SemaphoreHandle_t)b;
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem, int *woken) {
  (void)sem;
  (void)woken;
  _banked = 1;
  return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks) {
  (void)sem;
  g_stub_sem_last_ticks = ticks;
  if (!_banked)
    return VA_FAIL; /* nobody else runs here: an empty take is a timeout */
  _banked = 0;
  return VA_PASS;
}
