#include "arena.h"
#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

// Serialise the two tasks' log lines so they don't tear into each other.
static MutexHandle_t g_log;
#define LOG(...)                                                               \
  do {                                                                         \
    v_mutex_lock(g_log, V_WAIT_FOREVER);                                       \
    printk(__VA_ARGS__);                                                       \
    v_mutex_unlock(g_log);                                                     \
  } while (0)

/*
 * Stage-4 per-task arena test. Two tasks each carve a PRIVATE heap arena of a
 * different size and run the same malloc/free workload out of it. It shows:
 *   - v_task_arena_create + v_task_malloc/free work,
 *   - v_task_arena_used tracks live payload,
 *   - free coalesces: after freeing the middle block, a new alloc reuses it,
 *   - the two arenas are independent (different base addresses; each returns to
 *     used==0 regardless of the other).
 */
static void arena_task(const char *tag, uint32_t arena_sz) {
  if (v_task_arena_create(arena_sz) != 0) {
    LOG("[arena] %s: create(%u) FAILED\r\n", tag, (unsigned)arena_sz);
    for (;;)
      v_delay(1000);
  }
  for (int r = 0;; r++) {
    void *a = v_task_malloc(64);
    void *b = v_task_malloc(128);
    void *c = v_task_malloc(256);
    LOG("[arena] %s r%d: a=%x b=%x c=%x used=%u\r\n", tag, r, a, b, c,
           (unsigned)v_task_arena_used());
    v_task_free(b);               // free the middle block
    void *d = v_task_malloc(100); // should reuse the freed slot
    LOG("[arena] %s r%d: free b -> d=%x used=%u\r\n", tag, r, d,
           (unsigned)v_task_arena_used());
    v_task_free(a);
    v_task_free(c);
    v_task_free(d);
    LOG("[arena] %s r%d: all freed, used=%u (expect 0)\r\n", tag, r,
           (unsigned)v_task_arena_used());
    v_delay(700);
  }
}

void arena_a_task(void *arg) { (void)arg; arena_task("A", 1024); }
void arena_b_task(void *arg) { (void)arg; arena_task("B", 512); }

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  g_log = v_mutex_create();
  task_create(arena_a_task, NULL, 2048, 1);
  task_create(arena_b_task, NULL, 2048, 1);
  scheduler_start();
  while (1)
    ;
}
