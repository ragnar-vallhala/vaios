#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Stage-5 fd-typed IPC regression, on-target (processor-in-the-loop).
 *
 * Runs the named-object / fd-mutex fixes under the REAL scheduler + SVC path on
 * ARM/Renode (the host suite covers them with a frozen scheduler):
 *
 *   #7   a name that doesn't fit the 16-byte slot is rejected, not silently
 *        truncated into a second, unreachable object.
 *   #12  a non-recursive fd mutex re-locked by its own owner returns instead of
 *        self-deadlocking (with V_WAIT_FOREVER a regressed build hangs here).
 *   #4   a task that opens named semaphores and exits without closing them has
 *        its fds closed by teardown, so the table slots are reclaimed — repeated
 *        open+exit past the table size keeps succeeding.
 *
 * A correct build prints PASS #7 / PASS #12 / PASS #4 then DONE.
 */

extern void v_print(const char *s);
extern int print_fmt_buf(char *out, uint32_t out_size, const char *fmt, ...);

#define NAMED_SEM_SLOTS 8 /* mirror MAX_NAMED_SEMS in kernel/ipc.c */

/* #4 worker: open a uniquely-named sem and EXIT without closing it. */
static void leak_worker(void *arg) {
  char name[8];
  (void)print_fmt_buf(name, sizeof name, "lk%d", (int)(uintptr_t)arg);
  v_sem_open(name, V_IPC_CREATE); /* leaked fd — teardown must reclaim the slot */
  task_exit();
}

static void orchestrator(void *arg) {
  (void)arg;
  char buf[80];

  /* ---- #7: over-long name rejected, exact-fit name accepted ---- */
  int too_long = v_sem_open("sensor_bus_north", V_IPC_CREATE); /* 16 chars */
  int ok_name = v_sem_open("fifteenchars_ok", V_IPC_CREATE);   /* 15 chars */
  if (too_long < 0 && ok_name >= 0) {
    v_print("[fdipc] PASS #7: over-long name rejected\r\n");
  } else {
    (void)print_fmt_buf(buf, sizeof buf,
                        "[fdipc] FAIL #7: too_long=%d ok=%d\r\n", too_long,
                        ok_name);
    v_print(buf);
  }

  /* ---- #12: owner re-lock must return, not self-deadlock ---- */
  int m = v_mtx_open("m12", V_IPC_CREATE);
  v_mtx_lock(m, V_WAIT_FOREVER); /* acquire */
  v_print("[fdipc] #12 re-locking owned mutex (must return)...\r\n");
  v_mtx_lock(m, V_WAIT_FOREVER); /* owner re-lock: fixed returns; buggy hangs */
  v_print("[fdipc] PASS #12: owner re-lock returned (no self-deadlock)\r\n");
  v_mtx_unlock(m);

  /* ---- #4: named-sem slots freed when a task exits without closing ---- */
  for (int i = 0; i < NAMED_SEM_SLOTS + 2; i++) {
    task_create(leak_worker, (void *)(uintptr_t)i, 1024, 3); /* higher prio */
    task_delay(20); /* let it open+exit and the idle GC reclaim it */
  }
  int after = v_sem_open("post_leak", V_IPC_CREATE);
  if (after >= 0)
    v_print("[fdipc] PASS #4: named-sem slots freed on exit\r\n");
  else
    v_print("[fdipc] FAIL #4: named-sem table exhausted (leak on exit)\r\n");

  v_print("[fdipc] DONE\r\n");
  while (1)
    task_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg); /* clocks + heap + scheduler_init (like fd_ipc_test) */
  task_create(orchestrator, NULL, 2048, 2);
  scheduler_start();
  while (1)
    ;
}
