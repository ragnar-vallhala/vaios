/* Minimal externs for the vaios_devfs_tests binary (built with VAIOS_DEVFS=1,
 * VAIOS_SYSCALL_SVC=0 so devfs runs its bodies directly). devfs.c needs the
 * current_task global and the /dev/kmsg backing read; everything else (console
 * I/O) comes from port_hw_stub. */
#include "task.h"
#include <stdint.h>

TCB *current_task = 0;

int v_kmsg_read(char *out, uint32_t len) {
  (void)out;
  (void)len;
  return 0;
}
