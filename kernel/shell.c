/**
 * @file shell.c
 * @brief The on-target command shell: input, line editing, dispatch.
 *
 * Three things changed shape here, and the reasons are worth keeping.
 *
 * 1. THE EDITOR IS NOT IN THE ISR. The console RX interrupt belongs to
 *    /dev/console (devfs.c): it queues bytes in kernel memory and wakes the
 *    reader. The shell reads fd 0, sleeps in v_file_wait when there is nothing,
 *    and edits in its own task. Before, the ISR walked a multi-line history
 *    ring, wrote NULs into it and set a "ready" flag -- interrupt context doing
 *    string work on shared state, and untestable besides.
 *
 * 2. COMMANDS TAKE argc/argv. They used to receive the raw line as `void *` and
 *    parse it themselves; `perf` carried its own subcommand splitter. One
 *    tokenizer serves all of them now.
 *
 * 3. A COMMAND RUNS IN THE SHELL TASK. It used to task_create() a task per
 *    invocation at priority 0 and pass a pointer INTO the history ring as the
 *    argument -- so the ISR could overwrite a running command's arguments, and
 *    every command competed with the scheduler at top priority. Commands are
 *    synchronous now, which is what a shell is; anything long-running should
 *    spawn its own task explicitly and say so.
 *
 * 4. NO STATE IN KERNEL MEMORY. Every mutable byte is in a shell_t that
 *    shell_run keeps on the shell task's own stack; the startup commands and the
 *    built-ins are const tables in flash. So the shell can run unprivileged
 *    under MPU separation, where file-scope statics -- kernel .bss -- fault on
 *    first touch (todo 164: MMFAR was sh->in_head, the input ring's own index).
 */
#include "perf.h"
#include "port.h" // v_port_is_privileged: which side of the helper we are
#include "shell.h"
#include "structure.h" // the helper's queues
#include "memory.h"    // heap
#include "periph_bus.h" // i2c scan, through the bus arbiter
#if VAIOS_MODULE_BUS
#include "bus.h"
#endif
#if VAIOS_MODULE_VFS
#include "vfs.h"
#endif
#include "task.h"
#include "utils.h"
#include "vfile.h" // v_file_read — console input through fd 0
#include "vaios.h"
#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>

/* vsh does all its I/O through file descriptors, so it needs devfs; Kconfig
   makes VAIOS_MODULE_SHELL depend on it. A build that has the CMake module on
   with devfs off (the no-HAL QEMU smoke) gets nothing from this file, rather
   than calls to a v_printf that was never declared. */
#if VAIOS_DEVFS

#ifndef SHELL_LOG
#define SHELL_LOG(fmt, ...)                                                    \
  v_log(SHELL_LOG_LEVEL, "[SHELL] " fmt, ##__VA_ARGS__)
#endif

/* The prompt: the shell's name, vsh, and -- with a filesystem -- where it is.
   "vsh:/logs> ". The format is a constant so the tests can find a prompt among
   everything else printed. */
#define SHELL_PROMPT_FMT "vsh%s%s> "

/* The longest name a directory listing can show (todo 174). */
#if VAIOS_MODULE_VFS
#define VFS_NAME_MAX_SHOWN (VFS_NAME_MAX - 1)
#else
#define VFS_NAME_MAX_SHOWN 0
#endif

/* --- input staging ---------------------------------------------------------
 * A plain byte ring, not the kernel's general-purpose FIFO. It was that FIFO while
 * the producer was the RX interrupt and reuse cost nothing -- shell.c sits in
 * the same library as structure.c. Now that input arrives batched from
 * v_file_read(0, ...) the ring only exists so shell_feed() and the editor share
 * one path, and structure.c would drag mutexes, semaphores and the tick into
 * any test binary that wants the shell. Twelve lines beats that.
 *
 * Single producer, single consumer, and both are the shell task -- shell_feed
 * is no longer called from an interrupt -- so there is nothing to lock. */

static int in_push(shell_t *sh, uint8_t b) {
  unsigned next = (sh->in_head + 1u) % SHELL_INPUT_FIFO_SIZE;
  if (next == sh->in_tail)
    return 0; /* full: drop, as the fifo did */
  sh->in_buf[sh->in_head] = b;
  sh->in_head = next;
  return 1;
}

static int in_pop(shell_t *sh, uint8_t *out) {
  if (sh->in_tail == sh->in_head)
    return 0;
  *out = sh->in_buf[sh->in_tail];
  sh->in_tail = (sh->in_tail + 1u) % SHELL_INPUT_FIFO_SIZE;
  return 1;
}

/* Bounded copy. The kernel's string set has no v_strncpy, and this is the only
   place that wants one; always NUL-terminates within `cap`. */
static void sh_copy(char *dst, const char *src, int cap) {
  int i = 0;
  if (cap <= 0)
    return;
  while (src[i] && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void sh_putc(char c) {
  char b[2] = {c, '\0'};
  v_printf("%s", b);
}

/* A string, for the banner's line callback: v_printf has no length limit, so
   this is only the callback's shape. */
static void sh_puts(const char *s) { v_printf("%s", s); }

/* The boot banner, rendered again for whoever is looking now -- with uptime,
   which the boot banner cannot know. The clock stays out: reading it is
   privileged, and this may be an unprivileged shell. */
static void sh_banner(void) {
  uint32_t s = (uint32_t)((uint64_t)v_get_ticks() * TICK_PERIOD_US / 1000000u);
  v_banner_print(sh_puts, 0, (int32_t)s);
  v_printf("\r\n");
}

#if VAIOS_MODULE_VFS
/* A path as a person reads it: the drive prefix FatFs needs ("0:") is not
   shown for the first drive, the only one there usually is. */
static const char *sh_display_path(const char *p) {
  return (p[0] == '0' && p[1] == ':') ? p + 2 : p;
}
#endif

static void sh_prompt(const shell_t *sh) {
#if VAIOS_MODULE_VFS
  v_printf(SHELL_PROMPT_FMT, ":", sh_display_path(sh->cwd));
#else
  (void)sh;
  v_printf(SHELL_PROMPT_FMT, "", "");
#endif
}

/* --- ^C while a command runs ----------------------------------------------
 * Cooperative, as a tty's is: a command that can run long (cat, hexdump, cp,
 * bus sub) calls shell_interrupted between chunks. A ^C stops it, and -- as a
 * tty flushes on interrupt -- drops whatever was typed ahead with it; anything
 * else that arrived stays queued for the editor. A command that loops without
 * asking still cannot be stopped: those spawn a task (see `spin`). sh may be
 * NULL for a privileged command in the helper, which has no editor to keep
 * input for. */
int shell_interrupted(shell_t *sh) {
  char rx[16];
  int n, hit = 0;
  while ((n = v_file_read(0, rx, (uint32_t)sizeof rx)) > 0)
    for (int i = 0; i < n; i++) {
      if (rx[i] == 0x03)
        hit = 1;
      else if (sh && !hit)
        shell_feed(sh, rx[i]);
    }
  if (hit) {
    if (sh)
      sh->in_head = sh->in_tail = 0;
    v_printf("^C\r\n");
  }
  return hit;
}

/* A number from a command line: decimal, or hex with 0x. -1 if it is not one. */
static long sh_num(const char *s) {
  long v = 0;
  int base = 10;
  if (!s || !*s)
    return -1;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
    if (!*s)
      return -1;
  }
  for (; *s; s++) {
    int d = (*s >= '0' && *s <= '9')   ? *s - '0'
            : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
            : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10
                                       : 99;
    if (d >= base)
      return -1;
    v = v * base + d;
    if (v > 0x7FFFFFFFL)
      return -1;
  }
  return v;
}

/* --- tokenizing ----------------------------------------------------------- */

int shell_tokenize(char *line, char **argv, int max_argv) {
  int argc = 0;
  if (!line || !argv || max_argv <= 0)
    return 0;
  char *p = line;
  while (*p && argc < max_argv) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0')
      break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = '\0';
  }
  return argc;
}

/* The registry is three tables read as one: the built-ins and the startup
   table are in flash, the runtime registrations in the shell's own memory. */
static int cmd_help(shell_t *sh, int argc, char **argv);
static int cmd_clear(shell_t *sh, int argc, char **argv);
static int cmd_version(shell_t *sh, int argc, char **argv);
static int cmd_history(shell_t *sh, int argc, char **argv);
static int cmd_dmesg(shell_t *sh, int argc, char **argv);
#if VAIOS_MODULE_PERF
static int cmd_perf(shell_t *sh, int argc, char **argv);
#endif
static int cmd_ps(shell_t *sh, int argc, char **argv);
static int cmd_heap(shell_t *sh, int argc, char **argv);
static int cmd_uptime(shell_t *sh, int argc, char **argv);
static int cmd_reboot(shell_t *sh, int argc, char **argv);
static int cmd_loglevel(shell_t *sh, int argc, char **argv);
#if VAIOS_MODULE_PERF
static int cmd_top(shell_t *sh, int argc, char **argv);
#endif
static int cmd_kill(shell_t *sh, int argc, char **argv);
static int cmd_i2c(shell_t *sh, int argc, char **argv);
#if VAIOS_MODULE_BUS
static int cmd_bus(shell_t *sh, int argc, char **argv);
#endif
#if VAIOS_MODULE_VFS
static int cmd_ls(shell_t *sh, int argc, char **argv);
static int cmd_cat(shell_t *sh, int argc, char **argv);
static int cmd_cd(shell_t *sh, int argc, char **argv);
static int cmd_pwd(shell_t *sh, int argc, char **argv);
static int cmd_mkdir(shell_t *sh, int argc, char **argv);
static int cmd_rm(shell_t *sh, int argc, char **argv);
static int cmd_stat(shell_t *sh, int argc, char **argv);
static int cmd_head(shell_t *sh, int argc, char **argv);
static int cmd_tail(shell_t *sh, int argc, char **argv);
static int cmd_hexdump(shell_t *sh, int argc, char **argv);
static int cmd_touch(shell_t *sh, int argc, char **argv);
static int cmd_write(shell_t *sh, int argc, char **argv);
static int cmd_cp(shell_t *sh, int argc, char **argv);
static int cmd_mv(shell_t *sh, int argc, char **argv);
static int cmd_df(shell_t *sh, int argc, char **argv);
#endif

/* In flash. `perf` is privileged: it dumps through the kernel's own print path,
   which writes the kmsg ring in kernel memory. Run from an unprivileged shell
   that was an MPU fault at kmsg_head and -- a task's MPU fault being a kernel
   panic -- a halted board; through the helper it is fine. */
static const Command_t builtins[] = {
    {cmd_help, "help", "list commands", 0},
    {cmd_clear, "clear", "clear the screen", 0},
    {cmd_version, "version", "kernel version, build and target", 0},
    {cmd_history, "history", "recent command lines", 0},
    {cmd_dmesg, "dmesg", "kernel log since boot (or since last dmesg)", 0},
#if VAIOS_MODULE_PERF
    {cmd_perf, "perf", "[show|reset] kernel counters", SHELL_CMD_PRIV},
#endif
    {cmd_ps, "ps", "tasks", SHELL_CMD_PRIV},
    {cmd_heap, "heap", "kernel heap in use", SHELL_CMD_PRIV},
    {cmd_uptime, "uptime", "time since boot", 0},
    {cmd_reboot, "reboot", "reset the board", SHELL_CMD_PRIV},
    {cmd_loglevel, "loglevel", "[trace|debug|info|warn|error|fatal] log threshold",
     SHELL_CMD_PRIV},
#if VAIOS_MODULE_PERF
    {cmd_top, "top", "[frames] CPU per task, every second (until ^C)",
     SHELL_CMD_PRIV},
#endif
    {cmd_kill, "kill", "<pid> end a task this shell started", 0},
    {cmd_i2c, "i2c", "scan <bus> -y: which addresses answer", 0},
#if VAIOS_MODULE_BUS
    {cmd_bus, "bus", "[sub <topic> [n] | pub <topic> <text>] topics",
     SHELL_CMD_PRIV},
#endif
#if VAIOS_MODULE_VFS
    /* Not privileged: the task file API goes through the VFS worker. */
    {cmd_ls, "ls", "[dir] list a directory (default: here)", 0},
    {cmd_cat, "cat", "<file> print a file", 0},
    {cmd_cd, "cd", "[dir] change directory (default: the card's root)", 0},
    {cmd_pwd, "pwd", "where you are", 0},
    {cmd_mkdir, "mkdir", "<dir> make a directory", 0},
    {cmd_rm, "rm", "<path> remove a file or an empty directory", 0},
    {cmd_stat, "stat", "<path> size, type and modified time", 0},
    {cmd_head, "head", "[-n N] <file> the first N lines (10)", 0},
    {cmd_tail, "tail", "[-n N] <file> the last N lines (10)", 0},
    {cmd_hexdump, "hexdump", "<file> [offset] [len] bytes as hex (256)", 0},
    {cmd_touch, "touch", "<file> create it if it is not there", 0},
    {cmd_write, "write", "<file> <text...> append a line", 0},
    {cmd_cp, "cp", "<src> <dst> copy a file", 0},
    {cmd_mv, "mv", "<src> <dst> rename, or move into a directory", 0},
    {cmd_df, "df", "the card's size and free space", 0},
#endif
};
static const int n_builtins = (int)(sizeof builtins / sizeof builtins[0]);

static const Command_t *cmd_at(const shell_t *sh, int i) {
  if (i < n_builtins)
    return &builtins[i];
  i -= n_builtins;
  if (i < sh->table_count)
    return &sh->table[i];
  i -= sh->table_count;
  return i < sh->extra_count ? &sh->extra[i] : NULL;
}

const Command_t *shell_find_command(const shell_t *sh, const char *name) {
  if (!sh || !name || *name == '\0')
    return NULL;
  const Command_t *c;
  for (int i = 0; (c = cmd_at(sh, i)) != NULL; i++)
    if (c->fn && c->name && v_strcmp(c->name, name) == 0)
      return c;
  return NULL;
}

int shell_register_command(shell_t *sh, shell_fn_t fn, const char *name,
                           const char *help) {
  if (!sh || !fn || !name || *name == '\0')
    return 1;
  /* Refuse a name the lookup could never match, rather than accept it and
     silently never find it. */
  if (v_strlen(name) >= CMD_MAX_LEN)
    return 1;
  if (sh->extra_count >= MAX_CMD_NUMBER)
    return 1;
  sh->extra[sh->extra_count].fn = fn;
  sh->extra[sh->extra_count].name = name;
  sh->extra[sh->extra_count].help = help;
  sh->extra[sh->extra_count].flags = 0; /* never privileged: see SHELL_CMD_PRIV */
  sh->extra_count++;
  return 0;
}

/* --- history -------------------------------------------------------------- */

static void hist_push(shell_t *sh, const char *line) {
  if (!line || *line == '\0')
    return;
  /* Don't store a repeat of the newest entry: holding a key on the up arrow
     should walk back through what was typed, not through duplicates. */
  if (sh->hist_count > 0) {
    int newest = (sh->hist_head - 1 + CMD_BUFFER_SIZE) % CMD_BUFFER_SIZE;
    if (v_strcmp(sh->hist[newest], line) == 0)
      return;
  }
  sh_copy(sh->hist[sh->hist_head], line, CMD_MAX_LEN);
  sh->hist_head = (sh->hist_head + 1) % CMD_BUFFER_SIZE;
  if (sh->hist_count < CMD_BUFFER_SIZE)
    sh->hist_count++;
}

/* steps back from newest: 0 = newest, sh->hist_count-1 = oldest. */
static const char *hist_at(const shell_t *sh, int back) {
  if (back < 0 || back >= sh->hist_count)
    return NULL;
  int idx = (sh->hist_head - 1 - back + 2 * CMD_BUFFER_SIZE) % CMD_BUFFER_SIZE;
  return sh->hist[idx];
}

/* Wipe the visible line and put `s` in its place, on screen and in sh->line. */
static void line_replace(shell_t *sh, const char *s) {
  for (int i = 0; i < sh->len; i++)
    v_printf("\b \b");
  sh->len = 0;
  sh->line[0] = '\0';
  if (!s)
    return;
  while (*s && sh->len < CMD_MAX_LEN - 1) {
    sh->line[sh->len++] = *s;
    sh_putc(*s);
    s++;
  }
  sh->line[sh->len] = '\0';
}

static void sh_complete(shell_t *sh); /* tab: below, beside the path helpers */

/* --- input path ----------------------------------------------------------- */

void shell_feed(shell_t *sh, char c) {
  /* The single entry to the editor: shell_poll pushes what it read from fd 0
     through here, and the tests inject at the same point. A full ring drops
     the byte rather than blocking. */
  (void)in_push(sh, (uint8_t)c);
}

int shell_poll(shell_t *sh, char *out, int cap) {
  uint8_t b;
  if (!sh || !out || cap <= 0)
    return 0;
  /* Input comes through fd 0, like any other task's would: /dev/console's read
     is non-blocking and returns what has arrived. Nothing here touches a
     peripheral, which is what lets the shell task run unprivileged.
     shell_feed() stays the single entry to the editor, so the tests inject at
     exactly the point the console does. */
  {
    char rx[16];
    int n = v_file_read(0, rx, (uint32_t)sizeof rx);
    for (int i = 0; i < n; i++)
      shell_feed(sh, rx[i]);
  }
  while (in_pop(sh, &b)) {
    char c = (char)b;

    /* The LF of a CRLF pair: the CR already submitted the line. Swallowed
       here, before anything else sees it, or it reads as an empty line and
       prints a second prompt after every command. */
    if (c == '\n' && sh->prev_cr) {
      sh->prev_cr = 0;
      continue;
    }
    sh->prev_cr = (c == '\r');

    /* Arrow keys arrive as ESC '[' 'A'/'B'. Two states is the whole parser;
       anything else after ESC is swallowed rather than typed into the line. */
    if (sh->esc == 1) {
      sh->esc = (c == '[') ? 2 : 0;
      continue;
    }
    if (sh->esc == 2) {
      sh->esc = 0;
      if (c == 'A') { /* up: older */
        if (sh->hist_pos + 1 < sh->hist_count) {
          sh->hist_pos++;
          line_replace(sh, hist_at(sh, sh->hist_pos));
        }
      } else if (c == 'B') { /* down: newer, then back to a fresh line */
        if (sh->hist_pos > 0) {
          sh->hist_pos--;
          line_replace(sh, hist_at(sh, sh->hist_pos));
        } else if (sh->hist_pos == 0) {
          sh->hist_pos = -1;
          line_replace(sh, NULL);
        }
      }
      continue;
    }
    if (c == 0x1B) {
      sh->esc = 1;
      continue;
    }

    if (c == 0x03) { /* ^C: abandon the line */
      v_printf("^C\r\n");
      sh_prompt(sh);
      sh->len = 0;
      sh->line[0] = '\0';
      sh->hist_pos = -1;
      continue;
    }

    if (c == 0x05) { /* ENQ, "who are you": a host attaching (port.h) */
      /* On USB CDC the boot banner went out before anyone listened, so this
         is the first a terminal sees: the banner, then a prompt with any line
         already typed. ^E typed by hand asks the same. */
      v_printf("\r\n");
      sh_banner();
      sh_prompt(sh);
      v_printf("%s", sh->line);
      continue;
    }

    if (c == '\t') {
      sh_complete(sh);
      continue;
    }

    if (c == 0x0C) { /* ^L: redraw */
      /* A fresh prompt with the line so far, not readline's clear-screen:
         `clear` clears. */
      v_printf("\r\n");
      sh_prompt(sh);
      v_printf("%s", sh->line);
      continue;
    }

    if (c == '\b' || c == 0x7F) { /* backspace / DEL */
      if (sh->len > 0) {
        sh->len--;
        sh->line[sh->len] = '\0';
        v_printf("\b \b");
      }
      continue;
    }

    /* CR, LF or CRLF all submit. The old code required the line to be at least
       ESCAPE_SEQ_LEN long before it would dispatch at all, and then chopped that
       many bytes off the end -- so short commands never ran and the line ending
       was assumed rather than handled. An empty line just reprompts. */
    if (c == '\r' || c == '\n') {
      v_printf("\r\n");
      sh->line[sh->len] = '\0';
      if (sh->len == 0) {
        sh_prompt(sh);
        sh->hist_pos = -1;
        continue;
      }
      sh_copy(out, sh->line, cap);
      hist_push(sh, sh->line);
      sh->len = 0;
      sh->line[0] = '\0';
      sh->hist_pos = -1;
      return 1;
    }

    /* Anything else printable joins the line. A full line stops accepting
       rather than wrapping or overflowing. */
    if (c >= 0x20 && c < 0x7F && sh->len < CMD_MAX_LEN - 1) {
      sh->line[sh->len++] = c;
      sh->line[sh->len] = '\0';
      sh_putc(c);
    }
  }
  return 0;
}

/* --- built-ins ------------------------------------------------------------- */

static int cmd_help(shell_t *sh, int argc, char **argv) {
  (void)argc;
  (void)argv;
  const Command_t *c;
  for (int i = 0; (c = cmd_at(sh, i)) != NULL; i++) {
    if (!c->fn)
      continue;
    /* No %-10s: the formatter parses a '0' flag and a width, not '-', so a
       left-justify would fall through to the default arm and emit "%-". */
    v_printf("  %s", c->name);
    if (c->help)
      v_printf("\t%s", c->help);
    v_printf("\r\n");
  }
  return 0;
}

static int cmd_clear(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  /* Straight to the console, not through v_log: a screen-clear with an
     "[INFO 42]" prefix in front of it is not a screen clear. */
  v_printf("\033[2J\033[H");
  return 0;
}

static int cmd_version(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  sh_banner();
  v_printf("  NAVROBOTEC PVT. LTD. -- %s\r\n", AUTHOR);
  return 0;
}

/* The kernel log, from /dev/kmsg: the boot banner and messages included, which
   is how a terminal that attached after boot sees them. The ring is drained by
   reading, so a second dmesg shows only what came since. */
static int cmd_dmesg(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  int fd = v_file_open("/dev/kmsg", 0);
  if (fd < 0) {
    v_printf("dmesg: no /dev/kmsg\r\n");
    return 1;
  }
  char buf[100];
  int n;
  while ((n = v_file_read(fd, buf, sizeof buf - 1)) > 0) {
    buf[n] = '\0';
    v_printf("%s", buf);
  }
  v_file_close(fd);
  return 0;
}

static int cmd_history(shell_t *sh, int argc, char **argv) {
  (void)argc;
  (void)argv;
  /* Oldest first, so the numbering reads like a log. */
  for (int back = sh->hist_count - 1; back >= 0; back--)
    v_printf("  %d  %s\r\n", sh->hist_count - back, hist_at(sh, back));
  return 0;
}

#if VAIOS_MODULE_PERF && VAIOS_MODULE_VFS
static int sh_arg_path(const shell_t *sh, const char *cmd, const char *arg,
                       char *out, int cap);
#endif
#if VAIOS_MODULE_PERF
/* `perf [show|reset|save <path>]`. No hand-rolled argument splitting: argv[1]
   is the subcommand and argv[2] is the path, because the shell tokenized. */
static int cmd_perf(shell_t *sh, int argc, char **argv) {
  (void)sh;
  const char *sub = (argc > 1) ? argv[1] : "show";
  if (v_strcmp(sub, "show") == 0) {
    v_perf_dump_to(v_printf); /* through fd 1, so > can catch it */
    return 0;
  }
  if (v_strcmp(sub, "reset") == 0) {
    v_perf_reset();
    v_printf("perf: counters reset\r\n");
    return 0;
  }
#if VAIOS_MODULE_VFS
  if (v_strcmp(sub, "save") == 0) {
    if (argc < 3) {
      v_printf("usage: perf save <path>\r\n");
      return 1;
    }
    char path[CMD_MAX_LEN];
    if (sh_arg_path(sh, "perf", argv[2], path, sizeof path))
      return 1; /* relative to the user's working directory, like ls */
    int rc = v_perf_dump_to_file(path);
    v_printf(rc == 0 ? "perf: saved to %s\r\n" : "perf: save failed (is VFS "
                                                  "mounted?) %s\r\n",
              argv[2]);
    return rc == 0 ? 0 : 1;
  }
  v_printf("usage: perf [show|reset|save <path>]\r\n");
#else
  v_printf("usage: perf [show|reset]\r\n");
#endif
  return 1;
}
#endif /* VAIOS_MODULE_PERF */

/* --- inspection ----------------------------------------------------------
 * ps, heap and bus read kernel memory (the task table, the allocator's
 * counters, the bus registry) and so are SHELL_CMD_PRIV; ls and cat use the
 * task file API and run in the shell. */

/* One table cell: `s` padded to `w` columns, right-aligned for numbers. The
   formatter has widths but no left-justify, so cells are built by hand. */
extern uint32_t task_count; /* kernel/task.c: tasks that exist */

static void cell(char **p, char *end, const char *s, int w, int right) {
  int n = (int)v_strlen(s);
  for (int k = 0; right && k < w - n && *p < end; k++)
    *(*p)++ = ' ';
  for (int k = 0; s[k] && *p < end; k++)
    *(*p)++ = s[k];
  for (int k = 0; !right && k < w - n && *p < end; k++)
    *(*p)++ = ' ';
  **p = '\0';
}

static int cmd_ps(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  static const char *const st[] = {"ready", "running", "blocked", "sleeping",
                                   "exited"};
  /* As many as there are, plus room for a few started meanwhile: the kernel
     has no task limit, so neither does the list. Privileged, so the kernel
     heap rather than a stack sized for a guess. */
  int cap = (int)task_count + 8;
  TCB **t = (TCB **)v_malloc((size_t)cap * sizeof *t);
  if (!t) {
    v_printf("ps: out of memory\r\n");
    return 1;
  }
  int n = task_snapshot_list(t, cap);
  for (int a = 1; a < n; a++) /* by pid: the snapshot is in scheduler order */
    for (int b = a; b > 0 && t[b - 1]->task_id > t[b]->task_id; b--) {
      TCB *x = t[b];
      t[b] = t[b - 1];
      t[b - 1] = x;
    }
#if VAIOS_MODULE_PERF
  v_printf("    PID  PRI  STATE     NAME            STACK  USE\r\n");
#else
  v_printf("    PID  PRI  STATE     NAME\r\n");
#endif
  for (int i = 0; i < n; i++) {
    char row[96], num[24], *p = row, *end = row + sizeof row - 1;
    unsigned s = (unsigned)t[i]->status;
    const char *name = task_get_name(t[i]);
    print_fmt_buf(num, sizeof num, "%d", (int)t[i]->task_id);
    cell(&p, end, num, 7, 1);
    print_fmt_buf(num, sizeof num, "%d", (int)t[i]->priority);
    cell(&p, end, num, 5, 1);
    cell(&p, end, "  ", 2, 0);
    cell(&p, end, st[s <= 4u ? s : 0u], 10, 0);
    cell(&p, end, name[0] ? name : "-", 10, 0);
#if VAIOS_MODULE_PERF
    v_perf_task_t ps;
    v_perf_task_stats(t[i], &ps);
    print_fmt_buf(num, sizeof num, "%d/%d", (int)ps.stack_peak,
                  (int)ps.stack_size);
    cell(&p, end, num, 11, 1);
    print_fmt_buf(num, sizeof num, "%d%%",
                  ps.stack_size ? (int)(ps.stack_peak * 100u / ps.stack_size)
                                : 0);
    cell(&p, end, num, 5, 1);
#endif
    v_printf("%s\r\n", row);
  }
  v_free(t);
  return 0;
}

static int cmd_heap(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  v_printf("  %d of %d bytes in %d blocks\r\n",
           (int)v_get_heap_allocation_size(), (int)v_get_heap_size(),
           (int)v_get_heap_allocation_count());
  return 0;
}

static int cmd_uptime(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  uint32_t t = (uint32_t)((uint64_t)v_get_ticks() * TICK_PERIOD_US / 1000000u);
  if (t >= 86400u)
    v_printf("up %dd %dh %dm %ds\r\n", (int)(t / 86400u), (int)(t / 3600u % 24u),
             (int)(t / 60u % 60u), (int)(t % 60u));
  else
    v_printf("up %dh %dm %ds\r\n", (int)(t / 3600u), (int)(t / 60u % 60u),
             (int)(t % 60u));
  return 0;
}

static int cmd_reboot(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  v_printf("rebooting\r\n");
  /* Let the console writer, which outranks us, put that on the wire, and the
     logger empty its buffer, before the chip goes. */
  task_delay(MS_TO_TICKS(50));
  v_log_drain();
  v_port_hw_system_reset();
  return 0; /* only on a port whose reset returns (the host tests) */
}

static const char *const _levels[] = {"trace", "debug", "info",
                                      "warn",  "error", "fatal"};

static int cmd_loglevel(shell_t *sh, int argc, char **argv) {
  (void)sh;
  if (argc > 1) {
    int lv = -1;
    for (int i = 0; i < 6; i++)
      if (v_strcmp(argv[1], _levels[i]) == 0)
        lv = i;
    if (lv < 0) {
      v_printf("usage: loglevel [trace|debug|info|warn|error|fatal]\r\n");
      return 1;
    }
    v_log_set_level(lv);
  }
  int cur = v_log_get_level();
  v_printf("loglevel: %s\r\n", (cur >= 0 && cur < 6) ? _levels[cur] : "?");
  return 0;
}

#if VAIOS_MODULE_PERF
/* Each task's share of the CPU over the last second, busiest first, redrawn
   in place like top. From the per-task cycle counters, so it needs the perf
   module and a cycle counter that runs (v_perf_init). */
static int cmd_top(shell_t *sh, int argc, char **argv) {
  long frames = argc > 1 ? sh_num(argv[1]) : 0; /* 0: until ^C */
  if (frames < 0) {
    v_printf("usage: top [frames]\r\n");
    return 1;
  }
  /* Sized from the kernel's task count, with room for a few started while it
     runs; on the kernel heap, since five arrays of every task would not fit a
     2 KiB helper stack. */
  int cap = (int)task_count + 8;
  TCB **t = (TCB **)v_malloc((size_t)cap * sizeof *t);
  uint32_t *ids = (uint32_t *)v_malloc((size_t)cap * sizeof *ids);
  uint32_t *pct10 = (uint32_t *)v_malloc((size_t)cap * sizeof *pct10);
  uint64_t *was = (uint64_t *)v_malloc((size_t)cap * sizeof *was);
  uint64_t *now = (uint64_t *)v_malloc((size_t)cap * sizeof *now);
  int rc = 0;
  if (!t || !ids || !pct10 || !was || !now) {
    v_printf("top: out of memory\r\n");
    rc = 1;
    goto out;
  }
  int n = task_snapshot_list(t, cap);
  for (int i = 0; i < n; i++) {
    v_perf_task_t ps;
    v_perf_task_stats(t[i], &ps);
    ids[i] = t[i]->task_id;
    was[i] = ps.cycles_run;
  }
  int nwas = n;
  uint64_t c0 = v_perf_cycles();
  for (long f = 0; frames == 0 || f < frames; f++) {
    for (int step = 0; step < 10; step++) { /* a second, asking about ^C */
      task_delay(MS_TO_TICKS(100));
      if (shell_interrupted(sh)) {
        rc = SHELL_INTERRUPTED;
        goto out;
      }
    }
    uint64_t c1 = v_perf_cycles(), total = c1 - c0;
    c0 = c1;
    n = task_snapshot_list(t, cap);
    for (int i = 0; i < n; i++) {
      v_perf_task_t ps;
      v_perf_task_stats(t[i], &ps);
      now[i] = ps.cycles_run;
      uint64_t prev = now[i]; /* a task new this second counts from here */
      for (int k = 0; k < nwas; k++)
        if (ids[k] == t[i]->task_id)
          prev = was[k];
      pct10[i] = total ? (uint32_t)((now[i] - prev) * 1000u / total) : 0;
    }
    for (int a = 1; a < n; a++) /* busiest first */
      for (int b = a; b > 0 && pct10[b - 1] < pct10[b]; b--) {
        uint32_t x = pct10[b];
        pct10[b] = pct10[b - 1];
        pct10[b - 1] = x;
        TCB *y = t[b];
        t[b] = t[b - 1];
        t[b - 1] = y;
        uint64_t z = now[b];
        now[b] = now[b - 1];
        now[b - 1] = z;
      }
    uint32_t up = (uint32_t)((uint64_t)v_get_ticks() * TICK_PERIOD_US / 1000000u);
    v_printf("\033[H\033[2Jtop - up %dh %dm %ds, %d tasks   (^C to stop)\r\n\r\n",
             (int)(up / 3600u), (int)(up / 60u % 60u), (int)(up % 60u), n);
    v_printf("    PID  NAME          CPU\r\n");
    for (int i = 0; i < n; i++) {
      char row[64], num[16], *p = row, *end = row + sizeof row - 1;
      const char *name = task_get_name(t[i]);
      print_fmt_buf(num, sizeof num, "%d", (int)t[i]->task_id);
      cell(&p, end, num, 7, 1);
      cell(&p, end, "  ", 2, 0);
      cell(&p, end, name[0] ? name : "-", 10, 0);
      print_fmt_buf(num, sizeof num, "%d.%d%%", (int)(pct10[i] / 10u),
                    (int)(pct10[i] % 10u));
      cell(&p, end, num, 7, 1);
      v_printf("%s\r\n", row);
    }
    for (int i = 0; i < n; i++) {
      ids[i] = t[i]->task_id;
      was[i] = now[i];
    }
    nwas = n;
  }
out: /* v_free logs a NULL, and a failed allocation leaves some NULL */
  if (now)
    v_free(now);
  if (was)
    v_free(was);
  if (pct10)
    v_free(pct10);
  if (ids)
    v_free(ids);
  if (t)
    v_free(t);
  return rc;
}
#endif

/* The kernel lets a task end only the tasks it spawned -- the right limit for
   a shell, which is not every task's owner -- and says which refusal it was. */
static int cmd_kill(shell_t *sh, int argc, char **argv) {
  (void)sh;
  long pid = argc > 1 ? sh_num(argv[1]) : -1;
  if (pid <= 0) {
    v_printf("usage: kill <pid>\r\n");
    return 1;
  }
  int rc = v_task_kill((uint32_t)pid);
  if (rc == VA_PASS)
    return 0;
  v_printf(rc == V_TASK_EPERM ? "kill: %d: not started by this shell\r\n"
                              : "kill: %d: no such task\r\n",
           (int)pid);
  return 1;
}

/* i2c scan <bus> -y: i2cdetect -r's grid, through the bus arbiter -- the bus
   must be one the board registered (v_pbus_register), and the probe is
   whatever its hook makes of a one-byte read at each address. Never raw, and
   never without -y: some devices act on a read (a FIFO pops, a status flag
   clears), so a scan is not something to run on a bus that is flying. */
static int cmd_i2c(shell_t *sh, int argc, char **argv) {
  if (argc < 3 || v_strcmp(argv[1], "scan") != 0) {
    v_printf("usage: i2c scan <bus> -y\r\n");
    return 1;
  }
  if (argc < 4 || v_strcmp(argv[3], "-y") != 0) {
    v_printf("i2c: a scan reads one byte from every address on %s,\r\n"
             "  and some devices act on a read. Not on a bus in flight.\r\n"
             "  Add -y to go.\r\n",
             argv[2]);
    return 1;
  }
  int fd = v_pbus_open(argv[2]);
  if (fd < 0) {
    v_printf("i2c: %s: no such bus (the board registers them)\r\n", argv[2]);
    return 1;
  }
  static const char hx[] = "0123456789abcdef";
  v_printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
  int rc = 0;
  for (int row = 0; row < 8 && !rc; row++) {
    char line[64];
    int o = 0;
    line[o++] = hx[row];
    line[o++] = '0';
    line[o++] = ':';
    for (int col = 0; col < 16; col++) {
      int a = row * 16 + col;
      line[o++] = ' ';
      if (a < 0x08 || a > 0x77) { /* reserved addresses: not probed */
        line[o++] = ' ';
        line[o++] = ' ';
        continue;
      }
      uint8_t b;
      v_pbus_xfer_t x = {.addr = (uint16_t)a, .rx = &b, .rx_len = 1};
      int r = v_pbus_xfer(fd, &x, 0, MS_TO_TICKS(20));
      line[o++] = r >= 0 ? hx[a >> 4] : '-';
      line[o++] = r >= 0 ? hx[a & 15] : '-';
    }
    line[o] = '\0';
    v_printf("%s\r\n", line);
    if (shell_interrupted(sh))
      rc = SHELL_INTERRUPTED;
  }
  v_file_close(fd);
  return rc;
}

#if VAIOS_MODULE_BUS
static int bus_sub(shell_t *sh, const char *topic, long count) {
  int fd = v_bus_open(topic, V_BUS_RD);
  if (fd < 0) {
    v_printf("bus: %s: no such topic\r\n", topic);
    return 1;
  }
  uint8_t buf[64];
  int rc = 0;
  for (long got = 0; got < count;) {
    v_bus_rx_t rx = {buf, sizeof buf, 0, 0};
    int r = v_bus_recv_wait(fd, &rx, MS_TO_TICKS(100));
    if (shell_interrupted(sh)) {
      rc = SHELL_INTERRUPTED;
      break;
    }
    if (r == V_BUS_EMSGSIZE) {
      v_printf("  (a message larger than %d bytes)\r\n", (int)sizeof buf);
      got++;
      continue;
    }
    if (r != VA_PASS)
      continue; /* nothing within 100 ms: ask about ^C again */
    if (rx.missed)
      v_printf("  (%d missed)\r\n", (int)rx.missed);
    int text = 1;
    for (int k = 0; k < rx.len; k++)
      if (buf[k] < 0x20 || buf[k] > 0x7E)
        text = 0;
    char line[3 * sizeof buf + 8];
    int o = 0;
    for (int k = 0; k < rx.len; k++) {
      if (text) {
        line[o++] = (char)buf[k];
      } else {
        static const char hx[] = "0123456789abcdef";
        line[o++] = hx[buf[k] >> 4];
        line[o++] = hx[buf[k] & 15];
        line[o++] = ' ';
      }
    }
    line[o] = '\0';
    v_printf("  [%d] ", (int)rx.len);
    sh_puts(line);
    v_printf("\r\n");
    got++;
  }
  v_file_close(fd);
  return rc;
}

static int cmd_bus(shell_t *sh, int argc, char **argv) {
  if (argc >= 3 && v_strcmp(argv[1], "sub") == 0)
    return bus_sub(sh, argv[2], argc > 3 ? sh_num(argv[3]) : 10);
  if (argc >= 4 && v_strcmp(argv[1], "pub") == 0) {
    int fd = v_bus_open(argv[2], V_BUS_WR);
    if (fd < 0) {
      v_printf("bus: %s: no such topic\r\n", argv[2]);
      return 1;
    }
    char msg[CMD_MAX_LEN];
    int o = 0;
    for (int a = 3; a < argc; a++) {
      if (a > 3 && o < (int)sizeof msg - 1)
        msg[o++] = ' ';
      for (const char *q = argv[a]; *q && o < (int)sizeof msg - 1; q++)
        msg[o++] = *q;
    }
    int r = v_bus_send(fd, msg, (uint16_t)o);
    v_file_close(fd);
    if (r != VA_PASS) {
      v_printf("bus: %s: not published (%d)\r\n", argv[2], r);
      return 1;
    }
    return 0;
  }
  if (argc > 1) {
    v_printf("usage: bus [sub <topic> [n] | pub <topic> <text>]\r\n");
    return 1;
  }
  const v_bus_topic_t *t;
  int i = 0;
  for (; (t = v_bus_topic_nth(i)) != NULL; i++) {
    v_bus_topic_stats_t st;
    if (v_bus_topic_stats(t, &st) != VA_PASS)
      continue;
    v_printf("  %s  pub=%d drop=%d evict=%d queued=%d subs=%d\r\n", t->name,
             (int)st.published, (int)st.dropped, (int)st.evicted,
             (int)st.queued, (int)st.subs);
  }
  if (i == 0)
    v_printf("  no topics\r\n");
  return 0;
}
#endif

#if VAIOS_MODULE_VFS
#define SHELL_IO_TICKS 500 /* how long a file call waits for the VFS worker */

/* `arg` against the working directory, folded into `out` as a FatFs path.
   Drive-qualified ("0:/a") or under the mount ("/mnt/0:a") is absolute; a
   leading '/' is the current drive's root; anything else is relative. "." and
   ".." fold, never climbing above the drive's root. 0, or -1 if it does not
   fit. */
static int sh_path(const shell_t *sh, const char *arg, char *out, int cap) {
  char full[2 * CMD_MAX_LEN];
  int n = 0;
  if (!arg)
    arg = "";
  if (arg[0] == '/' && arg[1] == 'm' && arg[2] == 'n' && arg[3] == 't' &&
      arg[4] == '/')
    arg += 5; /* the VFS would strip it too */
  const char *colon = arg;
  while (*colon && *colon != ':' && *colon != '/')
    colon++;
  if (*colon == ':') { /* drive-qualified: absolute */
    sh_copy(full, arg, (int)sizeof full);
  } else {
    const char *c = sh->cwd;
    if (arg[0] == '/') /* the drive's root: just the cwd's drive */
      while (*c && *c != ':' && n < (int)sizeof full - 1)
        full[n++] = *c++;
    else
      while (*c && n < (int)sizeof full - 1)
        full[n++] = *c++;
    if (arg[0] == '/')
      full[n++] = ':';
    if (n < (int)sizeof full - 1)
      full[n++] = '/';
    sh_copy(full + n, arg, (int)sizeof full - n);
  }

  /* fold: out = drive, then "/component" per surviving component */
  const char *p = full;
  int o = 0;
  while (*p && *p != ':' && o < cap - 1)
    out[o++] = *p++;
  if (*p != ':')
    return -1; /* no drive: nothing to resolve against */
  if (o >= cap - 1)
    return -1;
  out[o++] = *p++;
  int root = o;
  while (*p) {
    while (*p == '/')
      p++;
    const char *b = p;
    while (*p && *p != '/')
      p++;
    int len = (int)(p - b);
    if (len == 0 || (len == 1 && b[0] == '.'))
      continue;
    if (len == 2 && b[0] == '.' && b[1] == '.') {
      while (o > root && out[o - 1] != '/')
        o--;
      if (o > root)
        o--; /* and the '/' before it */
      continue;
    }
    if (o + 1 + len >= cap)
      return -1;
    out[o++] = '/';
    for (int k = 0; k < len; k++)
      out[o++] = b[k];
  }
  if (o == root) /* the root itself */
    out[o++] = '/';
  out[o] = '\0';
  return 0;
}

/* Resolve argv's path or say why not. */
static int sh_arg_path(const shell_t *sh, const char *cmd, const char *arg,
                       char *out, int cap) {
  if (sh_path(sh, arg, out, cap) == 0)
    return 0;
  v_printf("%s: %s: path too long\r\n", cmd, arg);
  return -1;
}

static int cmd_ls(shell_t *sh, int argc, char **argv) {
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "ls", argc > 1 ? argv[1] : ".", path, sizeof path))
    return 1;
  int d = v_vfs_diropen(path, SHELL_IO_TICKS);
  if (d < 0) {
    v_printf("ls: %s: error %d\r\n", path, d);
    return 1;
  }
  vfs_dirent_t e;
  while (v_vfs_dirnext(d, &e, SHELL_IO_TICKS) == 1)
    v_printf(e.is_dir ? "  %s/\r\n" : "  %s  %d\r\n", e.name, (int)e.size);
  v_vfs_close(d, SHELL_IO_TICKS);
  return 0;
}

static int cmd_cat(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: cat <file>\r\n");
    return 1;
  }
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "cat", argv[1], path, sizeof path))
    return 1;
  int fd = v_vfs_open(path, VFS_O_RDONLY, SHELL_IO_TICKS);
  if (fd < 0) {
    v_printf("cat: %s: error %d\r\n", path, fd);
    return 1;
  }
  char buf[65];
  int n;
  while ((n = v_vfs_read(fd, buf, sizeof buf - 1, SHELL_IO_TICKS)) > 0) {
    buf[n] = '\0'; /* text: an embedded NUL ends the chunk early */
    v_printf("%s", buf);
    if (shell_interrupted(sh)) {
      v_vfs_close(fd, SHELL_IO_TICKS);
      return SHELL_INTERRUPTED;
    }
  }
  v_vfs_close(fd, SHELL_IO_TICKS);
  v_printf("\r\n");
  return n < 0 ? 1 : 0;
}

static int cmd_cd(shell_t *sh, int argc, char **argv) {
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "cd", argc > 1 ? argv[1] : "/", path, sizeof path))
    return 1;
  /* It has to be a directory that is there: one that opens as one. */
  int d = v_vfs_diropen(path, SHELL_IO_TICKS);
  if (d < 0) {
    v_printf("cd: %s: no such directory\r\n", path);
    return 1;
  }
  v_vfs_close(d, SHELL_IO_TICKS);
  sh_copy(sh->cwd, path, CMD_MAX_LEN);
  return 0;
}

static int cmd_pwd(shell_t *sh, int argc, char **argv) {
  (void)argc;
  (void)argv;
  v_printf("%s\r\n", sh_display_path(sh->cwd));
  return 0;
}

static int cmd_mkdir(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: mkdir <dir>\r\n");
    return 1;
  }
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "mkdir", argv[1], path, sizeof path))
    return 1;
  int rc = v_vfs_makedir(path, SHELL_IO_TICKS);
  if (rc < 0) {
    v_printf("mkdir: %s: error %d\r\n", path, rc);
    return 1;
  }
  return 0;
}

/* Open argv's file through the working directory, or say why not. */
static int sh_open(shell_t *sh, const char *cmd, const char *arg, int flags) {
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, cmd, arg, path, sizeof path))
    return -1;
  int fd = v_vfs_open(path, flags, SHELL_IO_TICKS);
  if (fd < 0)
    v_printf("%s: %s: error %d\r\n", cmd, sh_display_path(path), fd);
  return fd;
}

/* "-n N" before the file, for head and tail. The index of the file argument,
   or 0 when the line is not usable. */
static int opt_n(int argc, char **argv, long *n) {
  *n = 10;
  int a = 1;
  if (a < argc && v_strcmp(argv[a], "-n") == 0) {
    *n = a + 1 < argc ? sh_num(argv[a + 1]) : -1;
    a += 2;
  }
  return (*n >= 0 && a < argc) ? a : 0;
}

static int cmd_rm(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: rm <path>\r\n");
    return 1;
  }
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "rm", argv[1], path, sizeof path))
    return 1;
  int rc = v_vfs_remove(path, SHELL_IO_TICKS);
  if (rc < 0) {
    /* FatFs refuses a directory that still has something in it */
    v_printf("rm: %s: error %d (a directory must be empty)\r\n",
             sh_display_path(path), rc);
    return 1;
  }
  return 0;
}

static int cmd_stat(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: stat <path>\r\n");
    return 1;
  }
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "stat", argv[1], path, sizeof path))
    return 1;
  vfs_stat_t st;
  int rc = v_vfs_info(path, &st, SHELL_IO_TICKS);
  if (rc < 0 || !st.exists) {
    v_printf("stat: %s: no such file or directory\r\n", sh_display_path(path));
    return 1;
  }
  /* FAT packs the date and time: (fdate << 16) | ftime, fdate = year-1980:7
     month:4 day:5, ftime = hour:5 minute:6 second/2:5. */
  uint32_t d = st.mtime >> 16, t = st.mtime & 0xFFFFu;
  v_printf("  path      %s\r\n", sh_display_path(path));
  v_printf("  type      %s\r\n", st.is_dir ? "directory" : "file");
  if (!st.is_dir)
    v_printf("  size      %d bytes\r\n", (int)st.size);
  if (d)
    v_printf("  modified  %d-%02d-%02d %02d:%02d\r\n", (int)(1980u + (d >> 9)),
             (int)(d >> 5 & 15u), (int)(d & 31u), (int)(t >> 11),
             (int)(t >> 5 & 63u));
  return 0;
}

static int cmd_head(shell_t *sh, int argc, char **argv) {
  long want;
  int a = opt_n(argc, argv, &want);
  if (!a) {
    v_printf("usage: head [-n N] <file>\r\n");
    return 1;
  }
  int fd = sh_open(sh, "head", argv[a], VFS_O_RDONLY);
  if (fd < 0)
    return 1;
  char buf[65];
  int n, rc = 0;
  long lines = 0;
  while (lines < want && (n = v_vfs_read(fd, buf, sizeof buf - 1,
                                         SHELL_IO_TICKS)) > 0) {
    int k = 0;
    while (k < n && lines < want)
      if (buf[k++] == '\n')
        lines++;
    buf[k] = '\0'; /* up to and including the last wanted newline */
    v_printf("%s", buf);
    if (shell_interrupted(sh)) {
      rc = SHELL_INTERRUPTED;
      break;
    }
  }
  v_vfs_close(fd, SHELL_IO_TICKS);
  return rc;
}

/* The last N lines: read back from the end a block at a time, counting
   newlines, until there are N of them -- however long the lines -- then print
   from there to the end. A final newline ends the last line rather than
   starting another. */
static int cmd_tail(shell_t *sh, int argc, char **argv) {
  long want;
  int a = opt_n(argc, argv, &want);
  if (!a) {
    v_printf("usage: tail [-n N] <file>\r\n");
    return 1;
  }
  int fd = sh_open(sh, "tail", argv[a], VFS_O_RDONLY);
  if (fd < 0)
    return 1;
  long size = v_vfs_size(fd, SHELL_IO_TICKS);
  long start = want == 0 ? size : 0, pos = size, lines = 0;
  char buf[129];
  int rc = 0;
  while (want > 0 && pos > 0 && !start) {
    long blk = pos > 128 ? 128 : pos;
    pos -= blk;
    if (v_vfs_seek(fd, pos, VFS_SEEK_SET, SHELL_IO_TICKS) < 0 ||
        v_vfs_read(fd, buf, (uint32_t)blk, SHELL_IO_TICKS) != blk) {
      rc = 1;
      break;
    }
    for (long k = blk - 1; k >= 0; k--)
      if (buf[k] == '\n' && pos + k != size - 1 && ++lines == want) {
        start = pos + k + 1;
        break;
      }
  }
  if (!rc && start < size &&
      v_vfs_seek(fd, start, VFS_SEEK_SET, SHELL_IO_TICKS) >= 0) {
    int n;
    while ((n = v_vfs_read(fd, buf, 128, SHELL_IO_TICKS)) > 0) {
      buf[n] = '\0'; /* text: an embedded NUL ends the chunk early */
      sh_puts(buf);
      if (shell_interrupted(sh)) {
        rc = SHELL_INTERRUPTED;
        break;
      }
    }
  }
  v_vfs_close(fd, SHELL_IO_TICKS);
  return rc;
}

/* Offset, sixteen bytes in hex, the same as text: hexdump -C's layout. */
static int cmd_hexdump(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: hexdump <file> [offset] [len]\r\n");
    return 1;
  }
  long off = argc > 2 ? sh_num(argv[2]) : 0;
  long len = argc > 3 ? sh_num(argv[3]) : 256;
  if (off < 0 || len < 0) {
    v_printf("hexdump: offset and len are numbers (0x for hex)\r\n");
    return 1;
  }
  int fd = sh_open(sh, "hexdump", argv[1], VFS_O_RDONLY);
  if (fd < 0)
    return 1;
  int rc = 0;
  if (off && v_vfs_seek(fd, off, VFS_SEEK_SET, SHELL_IO_TICKS) < 0)
    rc = 1;
  static const char hx[] = "0123456789abcdef";
  uint8_t b[16];
  while (!rc && len > 0) {
    int n = v_vfs_read(fd, b, len < 16 ? (uint32_t)len : 16u, SHELL_IO_TICKS);
    if (n <= 0)
      break;
    char row[80];
    int o = 0;
    for (int sft = 28; sft >= 0; sft -= 4)
      row[o++] = hx[((uint32_t)off >> sft) & 15u];
    row[o++] = ' ';
    for (int k = 0; k < 16; k++) {
      row[o++] = ' ';
      if (k == 8)
        row[o++] = ' '; /* a gap between the two halves */
      row[o++] = k < n ? hx[b[k] >> 4] : ' ';
      row[o++] = k < n ? hx[b[k] & 15] : ' ';
    }
    row[o++] = ' ';
    row[o++] = ' ';
    row[o++] = '|';
    for (int k = 0; k < n; k++)
      row[o++] = (b[k] >= 0x20 && b[k] < 0x7F) ? (char)b[k] : '.';
    row[o++] = '|';
    row[o] = '\0';
    v_printf("%s\r\n", row);
    off += n;
    len -= n;
    if (shell_interrupted(sh))
      rc = SHELL_INTERRUPTED;
  }
  v_vfs_close(fd, SHELL_IO_TICKS);
  return rc;
}

static int cmd_touch(shell_t *sh, int argc, char **argv) {
  if (argc < 2) {
    v_printf("usage: touch <file>\r\n");
    return 1;
  }
  int fd = sh_open(sh, "touch", argv[1], VFS_O_WRONLY | VFS_O_CREAT);
  if (fd < 0)
    return 1;
  v_vfs_close(fd, SHELL_IO_TICKS);
  return 0;
}

/* Append argv[2..] as one line. Until there is > redirection. */
static int cmd_write(shell_t *sh, int argc, char **argv) {
  if (argc < 3) {
    v_printf("usage: write <file> <text...>\r\n");
    return 1;
  }
  int fd = sh_open(sh, "write", argv[1],
                   VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND);
  if (fd < 0)
    return 1;
  char line[CMD_MAX_LEN + 1];
  int o = 0;
  for (int a = 2; a < argc; a++) {
    if (a > 2 && o < CMD_MAX_LEN)
      line[o++] = ' ';
    for (const char *q = argv[a]; *q && o < CMD_MAX_LEN; q++)
      line[o++] = *q;
  }
  line[o++] = '\n';
  int w = v_vfs_write(fd, line, (uint32_t)o, SHELL_IO_TICKS);
  v_vfs_close(fd, SHELL_IO_TICKS);
  if (w != o) {
    v_printf("write: error %d\r\n", w);
    return 1;
  }
  return 0;
}

static int cmd_cp(shell_t *sh, int argc, char **argv) {
  if (argc < 3) {
    v_printf("usage: cp <src> <dst>\r\n");
    return 1;
  }
  char src[CMD_MAX_LEN], dst[CMD_MAX_LEN];
  if (sh_arg_path(sh, "cp", argv[1], src, sizeof src) ||
      sh_arg_path(sh, "cp", argv[2], dst, sizeof dst))
    return 1;
  if (v_strcmp(src, dst) == 0) {
    v_printf("cp: %s and %s are the same file\r\n", argv[1], argv[2]);
    return 1;
  }
  int in = v_vfs_open(src, VFS_O_RDONLY, SHELL_IO_TICKS);
  if (in < 0) {
    v_printf("cp: %s: error %d\r\n", sh_display_path(src), in);
    return 1;
  }
  int out = v_vfs_open(dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC,
                       SHELL_IO_TICKS);
  if (out < 0) {
    v_vfs_close(in, SHELL_IO_TICKS);
    v_printf("cp: %s: error %d\r\n", sh_display_path(dst), out);
    return 1;
  }
  uint8_t buf[128];
  int n, rc = 0;
  while ((n = v_vfs_read(in, buf, sizeof buf, SHELL_IO_TICKS)) > 0) {
    if (v_vfs_write(out, buf, (uint32_t)n, SHELL_IO_TICKS) != n) {
      v_printf("cp: %s: write failed\r\n", sh_display_path(dst));
      rc = 1;
      break;
    }
    if (shell_interrupted(sh)) {
      rc = SHELL_INTERRUPTED;
      break;
    }
  }
  if (n < 0)
    rc = 1;
  v_vfs_close(out, SHELL_IO_TICKS);
  v_vfs_close(in, SHELL_IO_TICKS);
  return rc;
}

/* As Unix mv: into `dst` when it is a directory that is there, keeping the
   name; otherwise to `dst`. Same drive only (FatFs renames, never copies). */
static int cmd_mv(shell_t *sh, int argc, char **argv) {
  if (argc < 3) {
    v_printf("usage: mv <src> <dst>\r\n");
    return 1;
  }
  char src[CMD_MAX_LEN], dst[CMD_MAX_LEN];
  if (sh_arg_path(sh, "mv", argv[1], src, sizeof src) ||
      sh_arg_path(sh, "mv", argv[2], dst, sizeof dst))
    return 1;
  vfs_stat_t st;
  if (v_vfs_info(dst, &st, SHELL_IO_TICKS) == 0 && st.exists && st.is_dir) {
    const char *base = src;
    for (const char *q = src; *q; q++)
      if (*q == '/')
        base = q + 1;
    int n = (int)v_strlen(dst);
    if (n + 1 + (int)v_strlen(base) >= (int)sizeof dst) {
      v_printf("mv: %s/%s: path too long\r\n", argv[2], base);
      return 1;
    }
    if (dst[n - 1] != '/')
      dst[n++] = '/';
    sh_copy(dst + n, base, (int)sizeof dst - n);
  }
  int rc = v_vfs_rename(src, dst, SHELL_IO_TICKS);
  if (rc < 0) {
    v_printf(rc == -8 ? "mv: %s: already exists\r\n" /* FR_EXIST */
                      : "mv: %s: error %d\r\n",
             sh_display_path(rc == -8 ? dst : src), rc);
    return 1;
  }
  return 0;
}

/* KiB as a person reads it: 512K, 3.4M, 29.7G. */
static void human(char *out, int cap, uint32_t kib) {
  static const char units[] = "KMGT";
  int u = 0;
  uint32_t whole = kib, tenth = 0;
  while (whole >= 1024 && u < 3) {
    tenth = (whole % 1024u) * 10u / 1024u;
    whole /= 1024u;
    u++;
  }
  if (u == 0 || whole >= 100)
    print_fmt_buf(out, (uint32_t)cap, "%d%c", (int)whole, units[u]);
  else
    print_fmt_buf(out, (uint32_t)cap, "%d.%d%c", (int)whole, (int)tenth,
                  units[u]);
}

static int cmd_df(shell_t *sh, int argc, char **argv) {
  (void)argc;
  (void)argv;
  char drive[4] = {sh->cwd[0], ':', '\0', '\0'};
  vfs_space_t sp;
  /* the first call after a mount may scan the whole FAT: allow for it */
  int rc = v_vfs_space(drive, &sp, MS_TO_TICKS(30000));
  if (rc < 0) {
    v_printf("df: %s: error %d\r\n", drive, rc);
    return 1;
  }
  uint32_t used = sp.total_kib - sp.free_kib;
  char size[12], usd[12], avail[12], pct[8];
  human(size, sizeof size, sp.total_kib);
  human(usd, sizeof usd, used);
  human(avail, sizeof avail, sp.free_kib);
  print_fmt_buf(pct, sizeof pct, "%d%%",
                sp.total_kib ? (int)((uint64_t)used * 100u / sp.total_kib) : 0);
  char row[80], *p = row, *end = row + sizeof row - 1;
  cell(&p, end, drive, 6, 0);
  cell(&p, end, size, 8, 1);
  cell(&p, end, usd, 8, 1);
  cell(&p, end, avail, 8, 1);
  cell(&p, end, pct, 6, 1);
  v_printf("  Drive    Size    Used   Avail  Use%%\r\n  %s\r\n", row);
  return 0;
}
#endif

/* --- lifecycle ------------------------------------------------------------ */

void shell_init(shell_t *sh, const Command_t *cmds, int count) {
  if (!sh)
    return;
  sh->table = cmds;
  sh->table_count = (cmds && count > 0) ? count : 0;
  sh->extra_count = 0;
  sh->in_head = sh->in_tail = 0;
  sh->len = 0;
  sh->line[0] = '\0';
  sh->hist_count = 0;
  sh->hist_head = 0;
  sh->hist_pos = -1;
  sh->esc = 0;
  sh->prev_cr = 0;
  sh->req_fd = sh->done_fd = -1;
  sh->capture = 0;
#if VAIOS_MODULE_VFS
  sh_copy(sh->cwd, "0:/", CMD_MAX_LEN);
#endif
}

/* --- tab completion --------------------------------------------------------
 * The word under the cursor (the line's last): the first word against command
 * names, any later word against the card's paths -- case-insensitively, as FAT
 * names are. One match fills in, with '/' after a directory and a space after
 * anything else; several fill in what they share, and when that is nothing
 * more, they are listed and the line is redrawn. */
typedef struct {
  const char *word; /* what has been typed of it */
  int wlen;
  int nocase;
  int dirs_only;            /* cd: a file is no answer */
  int count;                /* matches so far */
  char first[CMD_MAX_LEN];  /* the first match */
  int common;               /* how much every match shares with it */
  int is_dir;               /* the first match is a directory */
} comp_t;

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int comp_eq(const comp_t *k, char a, char b) {
  return k->nocase ? lc(a) == lc(b) : a == b;
}

/* Whether `name` matches, and what it does to the common part. */
static int comp_offer(comp_t *k, const char *name, int is_dir) {
  if (k->dirs_only && !is_dir)
    return 0;
  for (int i = 0; i < k->wlen; i++)
    if (!name[i] || !comp_eq(k, name[i], k->word[i]))
      return 0;
  if (k->count++ == 0) {
    sh_copy(k->first, name, CMD_MAX_LEN);
    k->common = (int)v_strlen(k->first);
    k->is_dir = is_dir;
  } else {
    int i = 0;
    while (i < k->common && name[i] && comp_eq(k, name[i], k->first[i]))
      i++;
    k->common = i;
  }
  return 1;
}

static void sh_insert(shell_t *sh, const char *p, int n) {
  for (int i = 0; i < n && sh->len < CMD_MAX_LEN - 1; i++) {
    sh->line[sh->len++] = p[i];
    sh_putc(p[i]);
  }
  sh->line[sh->len] = '\0';
}

#if VAIOS_MODULE_VFS
/* Offer each entry of `dir` (a word's directory part, as typed) -- and, when
   `list` is set, print the matching ones instead. 0, or -1 if the directory
   does not open. */
static int comp_dir(shell_t *sh, comp_t *k, const char *dir, int list) {
  char path[CMD_MAX_LEN];
  if (sh_path(sh, *dir ? dir : ".", path, sizeof path))
    return -1;
  int d = v_vfs_diropen(path, SHELL_IO_TICKS);
  if (d < 0)
    return -1;
  vfs_dirent_t e;
  while (v_vfs_dirnext(d, &e, SHELL_IO_TICKS) == 1) {
    comp_t probe = *k;
    if (!comp_offer(list ? &probe : k, e.name, e.is_dir))
      continue;
    if (list)
      v_printf(e.is_dir ? "%s/  " : "%s  ", e.name);
  }
  v_vfs_close(d, SHELL_IO_TICKS);
  return 0;
}
#endif

static void sh_complete(shell_t *sh) {
  int start = sh->len;
  while (start > 0 && sh->line[start - 1] != ' ')
    start--;
  int first_word = 1;
  for (int i = 0; i < start; i++)
    if (sh->line[i] != ' ')
      first_word = 0;
  comp_t k = {0};
  k.word = sh->line + start;
  k.wlen = sh->len - start;

#if VAIOS_MODULE_VFS
  char dir[CMD_MAX_LEN] = "";
#endif
  if (first_word) {
    const Command_t *c;
    for (int i = 0; (c = cmd_at(sh, i)) != NULL; i++)
      if (c->fn && c->name)
        comp_offer(&k, c->name, 0);
  } else {
#if VAIOS_MODULE_VFS
    /* split the word at its last '/': the directory, then the name in it */
    int slash = -1;
    for (int i = 0; i < k.wlen; i++)
      if (k.word[i] == '/')
        slash = i;
    if (slash >= 0) {
      sh_copy(dir, k.word, slash + 2 < CMD_MAX_LEN ? slash + 2 : CMD_MAX_LEN);
      if (slash == 0)
        dir[1] = '\0'; /* "/x": the root */
      else
        dir[slash] = '\0';
      k.word += slash + 1;
      k.wlen -= slash + 1;
    }
    k.nocase = 1;
    k.dirs_only = sh->line[0] == 'c' && sh->line[1] == 'd' && sh->line[2] == ' ';
    if (comp_dir(sh, &k, dir, 0) < 0)
      return;
#else
    return;
#endif
  }

  if (k.count == 0)
    return;
  if (k.common > k.wlen) /* what every match shares, past what was typed */
    sh_insert(sh, k.first + k.wlen, k.common - k.wlen);
  if (k.count == 1) {
    /* A name exactly as long as a listing can hold may have been cut there
       (todo 174): fill it in, but leave it open rather than end it. */
    if (!first_word && (int)v_strlen(k.first) == VFS_NAME_MAX_SHOWN)
      return;
    sh_insert(sh, k.is_dir ? "/" : " ", 1);
    return;
  }
  if (k.common > k.wlen)
    return; /* it moved: another tab lists what is left */
  v_printf("\r\n");
  if (first_word) {
    const Command_t *c;
    for (int i = 0; (c = cmd_at(sh, i)) != NULL; i++) {
      comp_t probe = k;
      if (c->fn && c->name && comp_offer(&probe, c->name, 0))
        v_printf("%s  ", c->name);
    }
  }
#if VAIOS_MODULE_VFS
  else
    comp_dir(sh, &k, dir, 1);
#endif
  v_printf("\r\n");
  sh_prompt(sh);
  v_printf("%s", sh->line);
}

/* --- privileged commands --------------------------------------------------
 * Numbered across the two const tables, built-ins first. The number is all that
 * crosses to the helper: it resolves it against its own copy of the same flash
 * tables, so there is nothing for the shell to forge. */
#if VAIOS_MPU_USER_SEPARATION
static const Command_t *priv_nth(const Command_t *table, int count, int n) {
  for (int i = 0; i < n_builtins; i++)
    if ((builtins[i].flags & SHELL_CMD_PRIV) && n-- == 0)
      return &builtins[i];
  for (int i = 0; table && i < count; i++)
    if ((table[i].flags & SHELL_CMD_PRIV) && n-- == 0)
      return &table[i];
  return NULL;
}

typedef struct {
  int16_t index;          /* which privileged command */
  uint8_t capture;        /* its output is redirected: write to /dev/capture */
  char line[CMD_MAX_LEN]; /* the arguments, re-joined: a copy, not a pointer */
#if VAIOS_MODULE_VFS
  char cwd[CMD_MAX_LEN]; /* where the user is: relative paths resolve here */
#endif
} shell_req_t;

static int priv_index(const shell_t *sh, const Command_t *c) {
  const Command_t *p;
  for (int n = 0; (p = priv_nth(sh->table, sh->table_count, n)) != NULL; n++)
    if (p == c)
      return n;
  return -1;
}

static int via_helper(shell_t *sh, const Command_t *c, int argc, char **argv) {
  if (sh->req_fd < 0)
    sh->req_fd = v_queue_open("shell.req", V_Q_WR);
  if (sh->done_fd < 0)
    sh->done_fd = v_queue_open("shell.done", V_Q_RD);
  if (sh->req_fd < 0 || sh->done_fd < 0) {
    v_printf("%s: needs the privileged helper (shell_helper_run)\r\n",
             argv[0]);
    return 1;
  }
  shell_req_t r;
  r.index = (int16_t)priv_index(sh, c);
  r.capture = sh->capture;
#if VAIOS_MODULE_VFS
  sh_copy(r.cwd, sh->cwd, CMD_MAX_LEN);
#endif
  int n = 0;
  for (int a = 0; a < argc; a++) {
    if (a && n < CMD_MAX_LEN - 1)
      r.line[n++] = ' ';
    for (const char *p = argv[a]; *p && n < CMD_MAX_LEN - 1; p++)
      r.line[n++] = *p;
  }
  r.line[n] = '\0';
  int rc = 1;
  if (v_queue_send(sh->req_fd, &r, V_WAIT_FOREVER) != VA_PASS ||
      v_queue_recv(sh->done_fd, &rc, V_WAIT_FOREVER) != VA_PASS)
    return 1;
  return rc;
}

/* The helper's queues are kernel memory, which is fine: only it, privileged,
   touches them directly; the shell reaches them through fds. One slot each --
   the shell waits for every answer before it asks again. */
static mpmc_queue_t _req_q, _done_q;
static shell_req_t _req_buf[1];
static int _done_buf[1];

static shell_t _view; /* what a privileged command sees as its shell */

void shell_helper_run(void *args) {
  const shell_config_t *cfg = (const shell_config_t *)args;
  shell_init(&_view, cfg ? cfg->cmds : NULL, cfg ? cfg->count : 0);
  mpmc_init(&_req_q, _req_buf, 1, sizeof(shell_req_t));
  mpmc_init(&_done_q, _done_buf, 1, sizeof(int));
  v_queue_register("shell.req", &_req_q);
  v_queue_register("shell.done", &_done_q);
  int rfd = v_queue_open("shell.req", V_Q_RD);
  int dfd = v_queue_open("shell.done", V_Q_WR);
  for (;;) {
    shell_req_t r;
    if (v_queue_recv(rfd, &r, V_WAIT_FOREVER) != VA_PASS)
      continue;
    r.line[CMD_MAX_LEN - 1] = '\0'; /* our copy: terminate it ourselves */
    const Command_t *c =
        priv_nth(cfg ? cfg->cmds : NULL, cfg ? cfg->count : 0, r.index);
    char *argv[SHELL_MAX_ARGV];
    int argc = shell_tokenize(r.line, argv, SHELL_MAX_ARGV);
    /* A redirected command's output goes where the shell's would: point our
       fd 1 at the capture device for the call, as the shell did its own. */
    int cap = -1, saved = -1;
    if (r.capture) {
      cap = v_file_open("/dev/capture", 0);
      saved = v_file_dup(1);
      if (cap >= 0 && saved >= 0)
        v_file_dup2(cap, 1);
    }
    /* The command's shell: the helper's own context, in kernel memory it may
       touch, carrying the user's working directory and nothing else of theirs. */
#if VAIOS_MODULE_VFS
    sh_copy(_view.cwd, r.cwd, CMD_MAX_LEN);
    _view.cwd[CMD_MAX_LEN - 1] = '\0';
#endif
    int rc = (c && argc > 0) ? c->fn(&_view, argc, argv) : 1;
    if (saved >= 0) {
      v_file_dup2(saved, 1);
      v_file_close(saved);
    }
    if (cap >= 0)
      v_file_close(cap);
    v_queue_send(dfd, &rc, V_WAIT_FOREVER);
  }
}
#endif /* VAIOS_MPU_USER_SEPARATION */

static int run_one(shell_t *sh, const Command_t *c, int argc, char **argv) {
  if (!(c->flags & SHELL_CMD_PRIV))
    return c->fn(sh, argc, argv);
#if VAIOS_MPU_USER_SEPARATION
  if (!v_port_is_privileged())
    return via_helper(sh, c, argc, argv);
#endif
  return c->fn(sh, argc, argv); /* privileged already: run it here */
}

#if VAIOS_MODULE_VFS
/* `cmd ... > file` / `>> file`, the redirect as the last two words. Output
   cannot be written to a file while the command runs -- a write to fd 1 is a
   syscall, and the filesystem cannot be used inside one -- so fd 1 points at
   /dev/capture for the command, and what it caught goes into the file after.
   The file is opened FIRST, so a bad path fails before anything runs. */
static int run_redirected(shell_t *sh, const Command_t *c, int argc,
                          char **argv, const char *target, int append) {
  char path[CMD_MAX_LEN];
  if (sh_arg_path(sh, "vsh", target, path, sizeof path))
    return 1;
  int vf = v_vfs_open(path,
                      VFS_O_WRONLY | VFS_O_CREAT |
                          (append ? VFS_O_APPEND : VFS_O_TRUNC),
                      SHELL_IO_TICKS);
  if (vf < 0) {
    v_printf("vsh: %s: error %d\r\n", sh_display_path(path), vf);
    return 1;
  }
  int cap = v_file_open("/dev/capture", 0);
  int saved = v_file_dup(1);
  if (cap < 0 || saved < 0 || v_file_dup2(cap, 1) < 0) {
    v_printf("vsh: cannot redirect (no free descriptor)\r\n");
    if (cap >= 0)
      v_file_close(cap);
    if (saved >= 0)
      v_file_close(saved);
    v_vfs_close(vf, SHELL_IO_TICKS);
    return 1;
  }
  sh->capture = 1;
  int rc = run_one(sh, c, argc, argv);
  sh->capture = 0;
  v_file_dup2(saved, 1); /* the console again, before anything is said */
  v_file_close(saved);
  char buf[128];
  int n;
  while ((n = v_file_read(cap, buf, sizeof buf)) > 0)
    if (v_vfs_write(vf, buf, (uint32_t)n, SHELL_IO_TICKS) != n) {
      v_printf("vsh: %s: write failed\r\n", sh_display_path(path));
      rc = 1;
      while (v_file_read(cap, buf, sizeof buf) > 0)
        ; /* empty it for the next command */
      break;
    }
  v_file_close(cap);
  v_vfs_close(vf, SHELL_IO_TICKS);
  return rc;
}
#endif

int shell_exec(shell_t *sh, char *line) {
  char *argv[SHELL_MAX_ARGV];
  int argc = shell_tokenize(line, argv, SHELL_MAX_ARGV);
  if (argc == 0)
    return 0;
  const Command_t *c = shell_find_command(sh, argv[0]);
  if (!c) {
    v_printf("%s: not found\r\n", argv[0]);
    return -1;
  }
  int rc;
#if VAIOS_MODULE_VFS
  int redir = 0;
  for (int a = 1; a < argc; a++)
    if (v_strcmp(argv[a], ">") == 0 || v_strcmp(argv[a], ">>") == 0)
      redir = a;
  if (redir) {
    if (redir != argc - 2) {
      v_printf("vsh: > and >> take one file, last: cmd ... > file\r\n");
      return 1;
    }
    rc = run_redirected(sh, c, redir, argv, argv[redir + 1],
                        argv[redir][1] == '>');
  } else
#endif
    rc = run_one(sh, c, argc, argv);
  if (rc != 0 && rc != SHELL_INTERRUPTED)
    v_printf("%s: exit %d\r\n", argv[0], rc);
  return rc;
}

void shell_run(void *args) {
  /* The shell's whole state, on this task's own stack: nothing it changes is
     in kernel memory. The config it was started with is in flash. */
  const shell_config_t *cfg = (const shell_config_t *)args;
  shell_t sh;
  shell_init(&sh, cfg ? cfg->cmds : NULL, cfg ? cfg->count : 0);
  char line[CMD_MAX_LEN];
  v_printf("\r\n");
  sh_prompt(&sh);
  for (;;) {
    if (!shell_poll(&sh, line, (int)sizeof line)) {
      /* Everything that arrived has been edited and no line is complete:
         sleep until the console's RX interrupt says more came. A line DOES
         return straight to shell_poll, since a paste can leave the next line
         already queued with nothing left to signal it. */
      (void)v_file_wait(0, V_WAIT_FOREVER);
      continue;
    }
    (void)shell_exec(&sh, line);
    sh_prompt(&sh);
  }
}

#endif /* VAIOS_DEVFS */
