/*
 * 32 — the on-target shell.
 *
 * Shows the two things worth knowing about it:
 *
 *   argv.  A command is `int (*)(int argc, char **argv)`, so `echo one two`
 *          arrives already split. Commands used to receive the raw line as a
 *          void* and parse it themselves.
 *
 *   Commands are a const table, in flash, handed to the shell task. With the
 *          shell's mutable state on its own stack, that is what lets it run
 *          unprivileged under VAIOS_MPU_USER_SEPARATION.
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
#include "vfile.h"
#if VAIOS_MODULE_VFS
#include "vfs.h"
#endif
#if VAIOS_MODULE_BUS
#include "bus.h"
#endif

#if VAIOS_MODULE_BUS
/* Something for `bus` to show, and `bus sub demo.ticks` to watch: a task that
   publishes the uptime once a second, through an fd like any task would. With
   nobody subscribed the bus frees each message rather than counting it; with
   a `bus sub` running, they arrive. */
V_BUS_POOL(demo, 32, 8);
static v_bus_t demo_bus;
static v_bus_topic_t demo_topic;

static void ticker(void *arg) {
  (void)arg;
  int fd = v_bus_open("demo.ticks", V_BUS_WR);
  for (;;) {
    char msg[24];
    int n = print_fmt_buf(msg, sizeof msg, "uptime %ds",
                          (int)(v_get_ticks() / MS_TO_TICKS(1000)));
    if (fd >= 0 && n > 0)
      v_bus_send(fd, msg, (uint16_t)n);
    v_delay(1000);
  }
}
#endif

static int cmd_echo(shell_t *sh, int argc, char **argv) {
  (void)sh;
  for (int i = 1; i < argc; i++)
    v_printf(i + 1 < argc ? "%s " : "%s", argv[i]);
  v_printf("\r\n");
  return 0;
}

static int cmd_ticks(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  v_printf("%d\r\n", (int)v_get_ticks());
  return 0;
}

/* The long-work pattern: the body runs in a task of its own so the shell stays
   responsive, and the command itself returns immediately. The child is spawned,
   not task_create()d: task_create is not a syscall, and the shell may be
   unprivileged. A spawned child is unprivileged too and cannot outrank us. */
static void spinner(void *arg) {
  (void)arg;
  for (int i = 0; i < 5; i++) {
    v_printf("spinner: %d\r\n", i);
    v_delay(500);
  }
  task_exit();
}

static int cmd_spin(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  v_task_spawn_t d = {spinner, NULL, 512, 1, "spinner"};
  if (v_task_spawn(&d) <= 0) {
    v_printf("spin: could not create the task\r\n");
    return 1;
  }
  v_printf("spin: started\r\n");
  return 0;
}

/* The startup commands, and what the shell task is started with. Both const,
   so both are in flash: the shell reads them however unprivileged it is. A
   command can still add more at runtime with shell_register_command(sh, ...);
   those go into the shell task's own memory. Mark a command SHELL_CMD_PRIV
   when it needs kernel state or hardware: an unprivileged shell then has the
   helper run it (the built-in `perf` is one). */
static const Command_t commands[] = {
    {cmd_echo, "echo", "print the arguments", 0},
    {cmd_ticks, "ticks", "kernel tick count", 0},
    {cmd_spin, "spin", "start a background task", 0},
};
static const shell_config_t shell_cfg = {
    commands, (int)(sizeof commands / sizeof commands[0])};

/* Console output from a task is queued by the write syscall and put on the wire
   here, in thread mode: inside the syscall the USB interrupt that completes a
   CDC transfer is masked. Privileged, because it drives the hardware; above the
   shell's priority, so a write is drained before the shell runs on and output
   stays in order. 1024: it peaked at 348 bytes on CDC, and under MPU
   separation a task block must be a power of two. */
static void console_writer(void *arg) {
  (void)arg;
  for (;;)
    v_console_writer_step(V_WAIT_FOREVER);
}

#if VAIOS_MODULE_VFS
/* The filesystem's worker, for `ls` and `cat`: privileged, because it drives
   the SD card, and in thread mode, where an SDIO completion can arrive. */
static void vfsio(void *arg) {
  (void)arg;
  for (;;)
    v_vfs_worker_step(100);
}
#endif

int main(void) {
  /* The SD card only when the filesystem is built in (CONFIG_VAIOS_MODULE_VFS),
     which is what gives the shell `ls` and `cat`. */
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = VAIOS_MODULE_VFS};
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

#if VAIOS_MPU_USER_SEPARATION
  task_create_privileged(console_writer, NULL, 1024, 2, "conout");
#else
  task_create_named(console_writer, NULL, 1024, 2, "conout");
#endif
#if VAIOS_MODULE_BUS
  v_bus_init(&demo_bus, demo_blocks, demo_desc, 32, 8);
  v_bus_topic_declare(&demo_bus, &demo_topic, "demo.ticks", NULL);
#endif
#if VAIOS_MODULE_VFS
  if (v_vfs_mount("/mnt/") != VA_PASS)
    v_log(LOG_ERROR, "shell: no SD card mounted; ls and cat will fail");
#if VAIOS_MPU_USER_SEPARATION
  task_create_privileged(vfsio, NULL, 2048, 1, "vfsio");
#else
  task_create_named(vfsio, NULL, 2048, 1, "vfsio");
#endif
#endif
  task_create_named(shell_run, (void *)&shell_cfg, SHELL_TASK_STACK_SIZE, 1,
                    "shell");
#if VAIOS_MODULE_BUS
  task_create_named(ticker, NULL, 1024, 1, "ticker");
#endif
#if VAIOS_MPU_USER_SEPARATION
  /* Runs the SHELL_CMD_PRIV commands for the now-unprivileged shell, from the
     same flash config. The shell's priority: the shell only waits while it
     runs, and the writer (above both) keeps their output in order. */
  task_create_privileged(shell_helper_run, (void *)&shell_cfg, 2048, 1,
                         "shpriv");
#endif
  scheduler_start();
  while (1)
    ;
}
