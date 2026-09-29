#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * Stage-2b test: printk/printf split + /dev/kmsg.
 *   - v_printf() (task) formats and routes through write(1) -> /dev/console.
 *   - printk() (kernel) writes directly to the console AND is mirrored into the
 *     kmsg ring.
 *   - The task drains kernel logs by read()ing /dev/kmsg and echoes them, so the
 *     kernel lines appear both directly and again via the drain.
 */
void kmsg_task(void *arg) {
  (void)arg;
  int kfd = v_file_open("/dev/kmsg", 0);
  v_printf("[task] printf->write; opened /dev/kmsg = fd %d\r\n", kfd);

  for (int c = 0;; c++) {
    v_printf("[task] tick %d (printf -> write)\r\n", c); // task output (not kmsg)

    if (c % 3 == 0)
      printk("[kernel] printk #%d (direct + kmsg)\r\n", c); // kernel log

    char kbuf[192];
    int n = v_file_read(kfd, kbuf, sizeof(kbuf) - 1); // drain kernel log ring
    if (n > 0) {
      v_printf("[task] --- drained %d bytes from /dev/kmsg ---\r\n", n);
      v_file_write(1, kbuf, (uint32_t)n);
    }
    v_delay(1000);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  printk("[kernel] boot printk (direct + kmsg)\r\n");
  task_create(kmsg_task, NULL, 2048, 1);
  scheduler_start();
  while (1)
    ;
}
