/*
 * Host-port check of read-only syscall arguments (VAIOS_PORT=host,
 * VAIOS_MPU_USER_SEPARATION + DEVFS). Run with tools/run_host.sh HOST_USER_RO.
 *
 * An UNPRIVILEGED task makes real syscalls (host SVC path -> dispatch ->
 * v_access_ok / v_strnlen_user) whose pointers land in the executable's
 * read-only segments or its own stack, which the host port reports via
 * v_port_user_region:
 *   - a string literal is accepted for reads: console write, bus name;
 *   - a literal is refused as a write target (pbus rx) with -14;
 *   - a writable global is still refused: only read-only segments are opened;
 *   - a buffer on the task's own stack is a valid rx (host stacks are ucontext
 *     mappings outside mem_block, so this needs the port's region too).
 * Prints "HOST USER-RO PASS" and exits 0, or "... FAIL" and exits 1.
 */
#include "periph_bus.h"
#include "task.h"
#include "utils.h" // v_log
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>
#include <stdlib.h> // exit

#define EFAULT_RC (-14)

static v_pbus_t bus;
static char writable_global[16] = "not rodata"; // .data: must stay refused

static int instant_start(const v_pbus_xfer_t *x) {
  (void)x;
  v_pbus_done_isr(&bus, 0); // completes at once; fine for this check
  return 0;
}

static int check(const char *what, int got, int want) {
  int ok = got == want;
  v_log(ok ? LOG_INFO : LOG_ERROR, "%s: rc=%d (want %d) %s", what, got, want,
        ok ? "ok" : "WRONG");
  return ok;
}

static void user_task(void *arg) {
  (void)arg;
  // The host has no CONTROL.nPRIV; the dispatch reads this flag instead.
  extern TCB *current_task;
  int ok = check("task is unprivileged", current_task->privileged, 0);

  static const char msg[] = "[user] this text is read straight from rodata\n";
  ok &= check("write(rodata literal)", v_file_write(1, msg, sizeof msg - 1),
              (int)sizeof msg - 1);
  ok &= check("write(writable global)", v_file_write(1, writable_global, 4),
              EFAULT_RC);

  int fd = v_pbus_open("hostbus"); // name read from rodata
  ok &= fd >= 0;
  v_log(fd >= 0 ? LOG_INFO : LOG_ERROR, "pbus_open(rodata name): fd=%d", fd);

  uint8_t rx[4];
  v_pbus_xfer_t good = {.tx = "\x10", .tx_len = 1, .rx = rx, .rx_len = 4};
  ok &= check("xfer(tx in rodata)", v_pbus_xfer(fd, &good, 1, 10), 0);
  v_pbus_xfer_t bad = {.tx = "\x10", .tx_len = 1,
                       .rx = (void *)"rodata rx", .rx_len = 4};
  ok &= check("xfer(rx in rodata)", v_pbus_xfer(fd, &bad, 1, 10), EFAULT_RC);

  v_log(ok ? LOG_INFO : LOG_ERROR, "HOST USER-RO %s", ok ? "PASS" : "FAIL");
  v_log_flush(); // buffered logging: drain both halves of the double buffer
  v_log_flush(); //   before the process exits
  exit(ok ? 0 : 1);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg); // also registers /dev/console (fds 0-2) for DEVFS
  v_pbus_register("hostbus", &bus, instant_start);
  task_create(user_task, NULL, 4096, 2);
  scheduler_start();
  for (;;)
    ;
}
