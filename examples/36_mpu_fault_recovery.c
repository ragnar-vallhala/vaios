#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * MPU fault recovery, on-target (VAIOS_MPU_FAULT_RECOVERY, default policy
 * RESTART with a cap of 3), one task per fault policy.
 *
 * Unprivileged tasks that fault on every run, each a different way:
 *   - escaper reads kernel SRAM (a data-access violation, frame stacked fine);
 *   - overflow recurses off the bottom of its stack, so PSP is left where the
 *     task may not write (the switch-out must not push there). On the F401 the
 *     guard stops it, with MSTKERR: the frame itself could not be stacked.
 *     Renode lets it run through the guard (its MPU model does not give the
 *     higher-numbered region the overlap) to the end of the block region --
 *     contained either way.
 *   - child, spawned by survivor with RESTART and a cap of 1, reads kernel
 *     SRAM too.
 * Policies: escaper keeps the default (restart, cap 3: runs 4 times), overflow
 * is set to KILL by main (runs once), child runs twice. So faults = 4 + 1 + 2
 * = 7 and restarts = 3 + 0 + 1 = 4. survivor also asks for a PANIC child,
 * which an unprivileged parent may not have, then ticks throughout and must
 * still be ticking after a privileged monitor has read the kernel's counters.
 *
 * Built with -DFR_PANIC=1, main gives escaper the PANIC policy instead: its
 * first fault must stop the system.
 */

extern void v_print(const char *s); /* polling console write (privileged) */

static void uputs(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  v_file_write(1, s, n); /* syscall: unprivileged-safe */
}

static void escaper(void *arg) {
  (void)arg;
  uputs("[fr] escaper start\r\n");
  volatile uint32_t v = *(volatile uint32_t *)0x20000010; /* kernel SRAM */
  (void)v;
  uputs("[fr] BUG: escaper not stopped\r\n");
  for (;;)
    v_delay(1000);
}

static int dive(int depth) {
  volatile uint8_t pad[8]; /* small frames, so the stores walk into the guard */
  pad[0] = (uint8_t)depth;
  if (depth > 1000000) /* never: the guard stops it far sooner */
    return 0;
  return dive(depth + 1) + pad[0]; /* not a tail call */
}

static void overflow(void *arg) {
  (void)arg;
  uputs("[fr] overflow start\r\n");
  dive(0);
  uputs("[fr] BUG: overflow not stopped\r\n");
  for (;;)
    v_delay(1000);
}

static void child(void *arg) {
  (void)arg;
  uputs("[fr] child start\r\n");
  volatile uint32_t v = *(volatile uint32_t *)0x20000010; /* kernel SRAM */
  (void)v;
  uputs("[fr] BUG: child not stopped\r\n");
  for (;;)
    v_delay(1000);
}

static void survivor(void *arg) {
  (void)arg;
  v_task_spawn_t cfg = {.entry = child, .stack_size = 1024, .priority = 1,
                        .name = "child", .fault_policy = V_FAULT_PANIC};
  uputs(v_task_spawn(&cfg) == V_TASK_EPERM ? "[fr] panic child refused\r\n"
                                           : "[fr] BUG: panic child allowed\r\n");
  cfg.fault_policy = V_FAULT_RESTART;
  cfg.fault_restart_max = 1;
  if (v_task_spawn(&cfg) <= 0)
    uputs("[fr] BUG: child spawn failed\r\n");
  for (;;) {
    uputs("[fr] survivor tick\r\n");
    v_delay(250);
  }
}

static void monitor(void *arg) {
  (void)arg;
  task_delay(1500); /* every restart is long done by now */
  /* Repeated, so a USB CDC console attached after boot still sees it. */
  for (;;) {
    char buf[112];
    print_fmt_buf(buf, sizeof buf,
                  "[fr] faults=%u restarts=%u max=%u | last: task %u addr 0x%x "
                  "MMFSR 0x%x\r\n",
                  (unsigned)v_mpu_fault_stats.faults,
                  (unsigned)v_mpu_fault_stats.restarts,
                  (unsigned)VAIOS_MPU_FAULT_RESTART_MAX,
                  (unsigned)v_mpu_fault_stats.last_task,
                  (unsigned)v_mpu_fault_stats.last_addr,
                  (unsigned)v_mpu_fault_stats.last_mmfsr);
    v_print(buf);
    v_print("[fr] DONE\r\n");
    task_delay(2000);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  v_print("[fr] mpu_fault_recovery: start\r\n");
  uint32_t esc = task_create_named(escaper, NULL, 2048, 2, "escaper");
#ifdef FR_PANIC
  task_set_fault_policy(esc, V_FAULT_PANIC, 0);
#else
  (void)esc;
#endif
  uint32_t ov = task_create_named(overflow, NULL, 2048, 2, "overflow");
  task_set_fault_policy(ov, V_FAULT_KILL, 0);
  task_create_named(survivor, NULL, 1024, 1, "survivor");
  task_create_privileged(monitor, NULL, 2048, 3, "monitor");
  scheduler_start();
  for (;;)
    ;
}
