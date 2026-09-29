#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * Stage-2 devfs / fd-table test. A task writes to /dev/console entirely through
 * the file syscalls: fd 1 (stdout, pre-opened to /dev/console at task creation)
 * and an explicit open()/write()/close(). If the fd table + object model +
 * console device + open/write/close syscalls work, this text appears on the
 * UART. printk() (kernel) still uses the separate direct path.
 */
static uint32_t slen(const char *s) {
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

void devfs_task(void *arg) {
  (void)arg;
  const char *hello = "[devfs] hello via write(1) -> /dev/console\r\n";
  v_file_write(1, hello, slen(hello)); // fd 1 = stdout

  // Explicit open/write/close of /dev/console.
  int fd = v_file_open("/dev/console", 0);
  char buf[64];
  int n = print_fmt_buf(buf, sizeof(buf), "[devfs] open(/dev/console) = fd %d\r\n", fd);
  v_file_write(fd, buf, n);
  v_file_close(fd);

  for (int c = 0;; c++) {
    n = print_fmt_buf(buf, sizeof(buf), "[devfs] tick %d via write()\r\n", c);
    v_file_write(1, buf, n);
    v_delay(500);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  task_create(devfs_task, NULL, 2048, 1);
  scheduler_start();
  while (1)
    ;
}
