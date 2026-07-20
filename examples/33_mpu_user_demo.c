#include "memory.h"
#include "port.h" // v_port_is_privileged
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * Stage-5 demo: the unprivileged flip (VAIOS_MPU_USER_SEPARATION).
 *
 * A user task runs UNPRIVILEGED (CONTROL.nPRIV=1) and confined to its own block
 * by the MPU. It cannot touch the kernel, peripherals, or another task — so it
 * does all I/O and allocation through SVC syscalls:
 *   - console output via v_file_write(1, ...) (fd 1 = /dev/console),
 *   - the heap via malloc/free (routed through SVC under this flag),
 *   - v_delay (SYS_delay).
 * It prints its own nPRIV bit to show the flip took, then deliberately reads a
 * kernel SRAM address to show the MPU catches the escape with a clean panic
 * (MemManage: "MPU fault in task N | fault addr 0x20000010").
 */

static uint32_t read_npriv(void) {
  return v_port_is_privileged() ? 0u : 1u; // 1 = unprivileged
}

static void uputs(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  v_file_write(1, s, n); // syscall: unprivileged-safe console write
}

void user_task(void *arg) {
  (void)arg;
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, "[user] running, nPRIV=%u (1=unpriv)\r\n",
                        (unsigned)read_npriv());
  v_file_write(1, buf, n);

  for (int r = 0; r < 3; r++) {
    void *p = malloc(64); // syscall -> privileged allocator
    n = print_fmt_buf(buf, sizeof buf,
                      "[user] r%d malloc=%x heap_used=%u (via syscall)\r\n", r,
                      p, (unsigned)v_task_heap_used());
    v_file_write(1, buf, n);
    free(p);
    v_delay(300);
  }

  uputs("[user] reading kernel SRAM 0x20000010 -> expect MPU fault\r\n");
  v_delay(100);
  volatile uint32_t *kern = (volatile uint32_t *)0x20000010; // kernel memory
  volatile uint32_t v = *kern;                               // MPU fault here
  (void)v;
  uputs("[user] BUG: no fault!\r\n"); // must never reach
  while (1)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  v_log(LOG_INFO, "mpu_user_demo: start (privileged main)");
  task_create(user_task, NULL, 4096, 1);
  scheduler_start();
  while (1)
    ;
}
