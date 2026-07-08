#include "ipc.h"
#include "memory.h"
#include "perf.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Stage-4 per-task heap test. Two tasks run the same malloc/free workload out of
 * THEIR OWN region (the low end of each task's stack block). It shows:
 *   - standard malloc/free/realloc work from a task, backed by its own block,
 *   - v_task_heap_used tracks live payload,
 *   - free coalesces: after freeing the middle block, a new alloc reuses it,
 *   - the two heaps are independent (each task's blocks sit in its own region;
 *     each returns to used==0 regardless of the other).
 * The kernel keeps using v_malloc/v_free for its own allocations.
 */
static MutexHandle_t g_log;
#define LOG(...)                                                               \
  do {                                                                         \
    v_mutex_lock(g_log, V_WAIT_FOREVER);                                       \
    printk(__VA_ARGS__);                                                       \
    v_mutex_unlock(g_log);                                                     \
  } while (0)

static void heap_task(const char *tag, int dump) {
  for (int r = 0;; r++) {
    void *a = malloc(64);
    void *b = malloc(128);
    void *c = malloc(256);
    LOG("[heap] %s r%d: a=%x b=%x c=%x used=%u\r\n", tag, r, a, b, c,
        (unsigned)v_task_heap_used());
    free(b);               // free the middle block
    void *d = malloc(100); // should reuse the freed slot
    LOG("[heap] %s r%d: free b -> d=%x used=%u\r\n", tag, r, d,
        (unsigned)v_task_heap_used());
    free(a);
    free(c);
    free(d);
    LOG("[heap] %s r%d: all freed, used=%u (expect 0)\r\n", tag, r,
        (unsigned)v_task_heap_used());
    // Dump perf once the heaps have been exercised: the [tasks] lines now carry
    // stack=/heap=/total= (peak footprint, heap included).
    if (dump && r == 2) {
      v_mutex_lock(g_log, V_WAIT_FOREVER);
      v_perf_dump();
      v_mutex_unlock(g_log);
    }
    v_delay(700);
  }
}

void heap_a_task(void *arg) { (void)arg; heap_task("A", 1); }
void heap_b_task(void *arg) { (void)arg; heap_task("B", 0); }

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  g_log = v_mutex_create();
  task_create(heap_a_task, NULL, 4096, 1);
  task_create(heap_b_task, NULL, 4096, 1);
  scheduler_start();
  while (1)
    ;
}
