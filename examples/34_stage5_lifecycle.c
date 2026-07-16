#include "ipc.h"
#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Stage-5 task-lifecycle regression, on-target (processor-in-the-loop).
 *
 * The host suite validates the task-teardown fixes with a frozen scheduler and
 * synthetic TCBs; this runs them under the REAL running scheduler on ARM/Renode,
 * so the actual block/exit/wake/context-switch paths are exercised:
 *
 *   #6  a task that exits while holding a mutex must release it — another task
 *       can then acquire it (v_ipc_task_teardown hands it off / frees it).
 *   #5  a task force-terminated while BLOCKED on a semaphore must be unlinked
 *       from that wait queue — a later give must not wake its freed TCB.
 *
 * A correct build prints "PASS #6", "PASS #5", "DONE". A regressed one either
 * prints "FAIL #6" (mutex leaked) or crashes/hangs before "PASS #5" (the give
 * dereferences the dead blocked task).
 */

extern void v_print(const char *s); /* polling console write (Renode-safe) */

static MutexHandle_t g_mtx;
static SemaphoreHandle_t g_sem;
static uint32_t g_blocker_id;

/* #6: lock a mutex and EXIT while still holding it. */
static void holder_task(void *arg) {
  (void)arg;
  v_mutex_lock(g_mtx, V_WAIT_FOREVER);
  v_print("[stage5] holder locked mutex, exiting while held\r\n");
  task_exit(); /* noreturn — terminates while owning g_mtx */
}

/* #5: block forever on a semaphore; the orchestrator terminates us mid-wait. */
static void blocker_task(void *arg) {
  (void)arg;
  v_semaphore_take(g_sem, V_WAIT_FOREVER);
  v_print("[stage5] blocker woke UNEXPECTEDLY\r\n"); /* must not happen */
  task_exit();
}

static void orchestrator(void *arg) {
  (void)arg;
  /* By now the higher-priority tasks have run: holder locked g_mtx and exited;
   * blocker is parked on g_sem. */

  /* #6: the mutex must be reclaimable now that its owner is gone. */
  if (v_mutex_lock(g_mtx, 200) == VA_PASS)
    v_print("[stage5] PASS #6: mutex reclaimed after owner exit\r\n");
  else
    v_print("[stage5] FAIL #6: mutex leaked on owner exit\r\n");

  /* #5: terminate the blocked task, then drive the semaphore it was parked on.
   * If teardown left its dead node on the wait queue, this give would wake a
   * freed TCB and corrupt the run — reaching the next line proves it did not. */
  task_exit_request(g_blocker_id);
  v_semaphore_give(g_sem);
  v_print("[stage5] PASS #5: blocked-task termination stable\r\n");

  v_print("[stage5] DONE\r\n");
  while (1)
    task_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();

  g_mtx = v_mutex_create();
  g_sem = v_semaphore_create_binary(); /* starts empty -> take blocks */

  scheduler_init();
  /* Priority-3 tasks run before the priority-2 orchestrator. Whichever prio-3
   * task runs first, by the time the orchestrator runs the holder has exited
   * (releasing the mutex) and the blocker is parked — so the checks are order
   * independent. */
  task_create(holder_task, NULL, 1024, 3);
  g_blocker_id = task_create(blocker_task, NULL, 1024, 3);
  task_create(orchestrator, NULL, 1024, 2);

  scheduler_start();
  while (1)
    ;
}
