/*
 * 32 — the on-target shell.
 *
 * Shows the two things worth knowing about it:
 *
 *   argv.  A command is `int (*)(int argc, char **argv)`, so `echo one two`
 *          arrives already split. Commands used to receive the raw line as a
 *          void* and parse it themselves.
 *
 *   Commands run IN THE SHELL TASK, synchronously. That is what a shell is, and
 *          it is why the argument pointers are safe to use for the duration of
 *          the call. The cost is that a command which never returns wedges the
 *          shell, so anything long-running spawns a task and returns — `spin`
 *          below is the pattern.
 *
 * Talk to it with any serial terminal -- `tio /dev/ttyACM0` is the least
 * trouble. Two settings matter whatever you use: LOCAL ECHO OFF, because the
 * shell echoes what you type and you would otherwise see it twice; and Ctrl+C
 * passed through rather than trapped, because the shell uses it to abandon a
 * line. tio and screen do both by default; minicom needs its local echo turned
 * off. Over USB CDC the baud is cosmetic (USB negotiates its own rate); it is
 * CONSOLE_BAUDRATE that matters on the UART route. Type `help`.
 */
#include "memory.h"
#include "shell.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vaios_config.h"

static int cmd_echo(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    print_fmt(i + 1 < argc ? "%s " : "%s", argv[i]);
  print_fmt("\r\n");
  return 0;
}

static int cmd_ticks(int argc, char **argv) {
  (void)argc;
  (void)argv;
  print_fmt("%d\r\n", (int)v_get_ticks());
  return 0;
}

/* The long-work pattern: the body runs in a task of its own so the shell stays
   responsive, and the command itself returns immediately. */
static void spinner(void *arg) {
  (void)arg;
  for (int i = 0; i < 5; i++) {
    v_log(LOG_INFO, "spinner: %d", i);
    v_delay(500);
  }
  task_exit();
}

static int cmd_spin(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (task_create(spinner, NULL, 512, 1) == 0) {
    print_fmt("spin: could not create the task\r\n");
    return 1;
  }
  print_fmt("spin: started\r\n");
  return 0;
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  /* v_system_init, not v_init: it does the same clock/heap/scheduler bring-up
     and then two things this example cannot do without.

     v_devfs_init registers /dev/console, which v_fd_table_init needs in order
     to pre-open fd 0/1/2 when a task is created -- and the shell's whole I/O
     path is fd 1 for output and a non-blocking fd 0 for input. Without it the
     shell runs and prints into a void: no node, no descriptors, every write
     refused. That is exactly what happened before this line changed.

     v_perf_init arms the DWT cycle counter, so `perf show` reports real numbers
     instead of zeros. It read real numbers on the bench anyway, but only
     because a debugger had been attached and left DEMCR.TRCENA set -- which is
     not a thing to depend on. */
  v_system_init(&cfg);

  shell_init();
  shell_register_command(cmd_echo, "echo", "print the arguments");
  shell_register_command(cmd_ticks, "ticks", "kernel tick count");
  shell_register_command(cmd_spin, "spin", "start a background task");

  task_create(shell_run, NULL, SHELL_TASK_STACK_SIZE, 1);
  scheduler_start();
  while (1)
    ;
}
