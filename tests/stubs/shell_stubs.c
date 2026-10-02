/* Externs for the vaios_shell_tests binary (VAIOS_DEVFS=1, VAIOS_SYSCALL_SVC=0,
 * so devfs and the shell run their bodies directly).
 *
 * The shell does its I/O through fds now, so this binary links the real
 * kernel/devfs.c: the tests exercise the actual v_file_read(0, ...) path rather
 * than a stand-in, and /dev/console's read returns 0 here because
 * port_hw_stub's try_read has nothing to offer. That is what the tests want --
 * bytes enter through shell_feed(), which is the same entry the console uses.
 *
 * Output is discarded. The tests assert on the LINE the editor returns, never on
 * what was printed, so a real formatter would only drag utils.c and the logging
 * path in for nothing.
 */
#include "task.h"
#include "vfile.h"
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

/* A real synthetic task with a real fd table, not a NULL current_task: the
   shell reads fd 0 now, so v_file_read walks current_task->fds[] and the tests
   should go through that rather than round a fake. v_devfs_init registers
   /dev/console, v_fd_table_init pre-opens 0/1/2 onto it, and the console's read
   returns 0 bytes here because port_hw_stub's try_read has nothing to give --
   which is what the tests want: input arrives through shell_feed(). */
static TCB _shell_test_task;
TCB *current_task = &_shell_test_task;

void shell_test_env_init(void) {
  v_devfs_init();
  v_fd_table_init(&_shell_test_task);
}

int v_kmsg_read(char *out, uint32_t len) {
  (void)out;
  (void)len;
  return 0;
}

/* Counts prompts printed, which is the one piece of output a test needs: a
   CRLF terminal must get one prompt per line, not two. */
int g_stub_prompts;
int v_printf(const char *fmt, ...) {
  if (strstr(fmt, "vaios> "))
    g_stub_prompts++;
  return 0;
}

/* kernel/shell.c's perf command is compiled out here (VAIOS_MODULE_PERF=0), and
 * v_strcmp/v_strlen come from the utils stub alongside this. */

/* The three the shell leans on from the kernel's own helpers. Real string
   semantics matter here -- the registry compares names with v_strcmp and the
   tests assert on v_strlen -- so these are libc-backed rather than faked. */
uint32_t v_strlen(const char *s) {
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

int v_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* What ps and heap read. One task -- the synthetic one above -- and a heap
   with fixed numbers, so the commands run end to end here. */
int task_snapshot_list(TCB **out, int max) {
  if (max < 1)
    return 0;
  out[0] = &_shell_test_task;
  return 1;
}
const char *task_get_name(const TCB *t) {
  (void)t;
  return "shell";
}
uint32_t v_get_heap_size(void) { return 4096; }
uint32_t v_get_heap_allocation_count(void) { return 2; }
uint32_t v_get_heap_allocation_size(void) { return 100; }

/* The banner the shell prints (kernel/vaios.c, not linked here). */
const char v_banner[] = "vaios test (unknown)\r\n";
const char v_features[] = " devfs shell";
