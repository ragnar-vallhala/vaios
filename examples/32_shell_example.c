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
 * Talk to it with tools/terminal (the host TTY side) or any serial terminal at
 * CONSOLE_BAUDRATE. Type `help`.
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
  v_init(&cfg); /* console first: shell_init arms the RX interrupt */
  shell_init();
  shell_register_command(cmd_echo, "echo", "print the arguments");
  shell_register_command(cmd_ticks, "ticks", "kernel tick count");
  shell_register_command(cmd_spin, "spin", "start a background task");

  v_heap_memory_init();
  scheduler_init();
  task_create(shell_run, NULL, SHELL_TASK_STACK_SIZE, 1);
  scheduler_start();
  while (1)
    ;
}
