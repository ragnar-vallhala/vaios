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
#include "shell.h"
#include "task.h"
#include "utils.h"
#include "vfile.h" // v_file_read — console input through fd 0
#include "vaios.h"
#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>

#ifndef SHELL_LOG
#define SHELL_LOG(fmt, ...)                                                    \
  v_log(SHELL_LOG_LEVEL, "[SHELL] " fmt, ##__VA_ARGS__)
#endif

#define SHELL_PROMPT "vaios> "

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
#if VAIOS_MODULE_PERF && !VAIOS_MPU_USER_SEPARATION
static int cmd_perf(shell_t *sh, int argc, char **argv);
#endif

/* In flash. `perf` dumps through the kernel's own print path, which writes the
   kmsg ring in kernel memory -- from an unprivileged shell that is an MPU fault,
   and a task's MPU fault is a kernel panic. Seen on the F401: fault address
   kmsg_head, board halted. So under MPU separation it is not offered at all
   until the stage-3 privileged helper can run it. */
static const Command_t builtins[] = {
    {cmd_help, "help", "list commands"},
    {cmd_clear, "clear", "clear the screen"},
    {cmd_version, "version", "kernel version and author"},
    {cmd_history, "history", "recent command lines"},
#if VAIOS_MODULE_PERF && !VAIOS_MPU_USER_SEPARATION
    {cmd_perf, "perf", "[show|reset] kernel counters"},
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
      v_printf("^C\r\n" SHELL_PROMPT);
      sh->len = 0;
      sh->line[0] = '\0';
      sh->hist_pos = -1;
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
        v_printf(SHELL_PROMPT);
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
  v_printf("vaios %s\r\n", VAIOS_VERSION);
  v_printf("NAVROBOTEC PVT. LTD. -- %s\r\n", AUTHOR);
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

#if VAIOS_MODULE_PERF && !VAIOS_MPU_USER_SEPARATION
/* `perf [show|reset|save <path>]`. No hand-rolled argument splitting: argv[1]
   is the subcommand and argv[2] is the path, because the shell tokenized. */
static int cmd_perf(shell_t *sh, int argc, char **argv) {
  (void)sh;
  const char *sub = (argc > 1) ? argv[1] : "show";
  if (v_strcmp(sub, "show") == 0) {
    v_perf_dump();
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
    int rc = v_perf_dump_to_file(argv[2]);
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
}

void shell_run(void *args) {
  /* The shell's whole state, on this task's own stack: nothing it changes is
     in kernel memory. The config it was started with is in flash. */
  const shell_config_t *cfg = (const shell_config_t *)args;
  shell_t sh;
  shell_init(&sh, cfg ? cfg->cmds : NULL, cfg ? cfg->count : 0);
  char line[CMD_MAX_LEN];
  char *argv[SHELL_MAX_ARGV];
  v_printf("\r\n" SHELL_PROMPT);
  for (;;) {
    if (!shell_poll(&sh, line, (int)sizeof line)) {
      /* Everything that arrived has been edited and no line is complete:
         sleep until the console's RX interrupt says more came. A line DOES
         return straight to shell_poll, since a paste can leave the next line
         already queued with nothing left to signal it. */
      (void)v_file_wait(0, V_WAIT_FOREVER);
      continue;
    }
    int argc = shell_tokenize(line, argv, SHELL_MAX_ARGV);
    if (argc > 0) {
      const Command_t *c = shell_find_command(&sh, argv[0]);
      if (c) {
        int rc = c->fn(&sh, argc, argv);
        if (rc != 0)
          v_printf("%s: exit %d\r\n", argv[0], rc);
      } else {
        v_printf("%s: not found\r\n", argv[0]);
      }
    }
    v_printf(SHELL_PROMPT);
  }
}
