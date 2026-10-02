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

/* A string of any length: v_printf formats into 128 bytes, and the banner is
   longer than that. */
static void sh_puts(const char *s) {
  char chunk[100];
  while (*s) {
    int n = 0;
    while (s[n] && n < (int)sizeof chunk - 1) {
      chunk[n] = s[n];
      n++;
    }
    chunk[n] = '\0';
    v_printf("%s", chunk);
    s += n;
  }
}

/* The banner, as at boot, then what the shell itself adds. */
static void sh_banner(void) {
  sh_puts(v_banner);
  v_printf("  features:%s\r\n", v_features[0] ? v_features : " none");
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
#if VAIOS_MODULE_BUS
static int cmd_bus(shell_t *sh, int argc, char **argv);
#endif
#if VAIOS_MODULE_VFS
static int cmd_ls(shell_t *sh, int argc, char **argv);
static int cmd_cat(shell_t *sh, int argc, char **argv);
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
#if VAIOS_MODULE_BUS
    {cmd_bus, "bus", "topics and their counters", SHELL_CMD_PRIV},
#endif
#if VAIOS_MODULE_VFS
    /* Not privileged: the task file API goes through the VFS worker. */
    {cmd_ls, "ls", "[dir] list a directory (default /mnt/0:)", 0},
    {cmd_cat, "cat", "<file> print a file", 0},
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

    if (c == 0x05) { /* ENQ, "who are you": a host attaching (port.h) */
      /* On USB CDC the boot banner went out before anyone listened, so this
         is the first a terminal sees: the banner, then a prompt with any line
         already typed. ^E typed by hand asks the same. */
      v_printf("\r\n");
      sh_banner();
      v_printf(SHELL_PROMPT "%s", sh->line);
      continue;
    }

    if (c == 0x0C) { /* ^L: redraw */
      /* A fresh prompt with the line so far, not readline's clear-screen:
         `clear` clears. */
      v_printf("\r\n" SHELL_PROMPT "%s", sh->line);
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

#if VAIOS_MODULE_PERF
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

/* --- inspection ----------------------------------------------------------
 * ps, heap and bus read kernel memory (the task table, the allocator's
 * counters, the bus registry) and so are SHELL_CMD_PRIV; ls and cat use the
 * task file API and run in the shell. */

static int cmd_ps(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  static const char *const st[] = {"rdy", "run", "blk", "dly", "term"};
  TCB *t[16];
  int n = task_snapshot_list(t, 16);
  v_printf("  id pri state name\r\n");
  for (int i = 0; i < n; i++) {
    unsigned s = (unsigned)t[i]->status;
    v_printf("  %d  %d   %s   %s", (int)t[i]->task_id, (int)t[i]->priority,
             st[s <= 4u ? s : 0u], task_get_name(t[i]));
#if VAIOS_MODULE_PERF
    v_perf_task_t ps;
    v_perf_task_stats(t[i], &ps);
    v_printf("  stack %d/%d", (int)ps.stack_peak, (int)ps.stack_size);
#endif
    v_printf("\r\n");
  }
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

#if VAIOS_MODULE_BUS
static int cmd_bus(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
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

static int cmd_ls(shell_t *sh, int argc, char **argv) {
  (void)sh;
  const char *path = argc > 1 ? argv[1] : "/mnt/0:";
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
  (void)sh;
  if (argc < 2) {
    v_printf("usage: cat <file>\r\n");
    return 1;
  }
  int fd = v_vfs_open(argv[1], VFS_O_RDONLY, SHELL_IO_TICKS);
  if (fd < 0) {
    v_printf("cat: %s: error %d\r\n", argv[1], fd);
    return 1;
  }
  char buf[65];
  int n;
  while ((n = v_vfs_read(fd, buf, sizeof buf - 1, SHELL_IO_TICKS)) > 0) {
    buf[n] = '\0'; /* text: an embedded NUL ends the chunk early */
    v_printf("%s", buf);
  }
  v_vfs_close(fd, SHELL_IO_TICKS);
  v_printf("\r\n");
  return n < 0 ? 1 : 0;
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
  char line[CMD_MAX_LEN]; /* the arguments, re-joined: a copy, not a pointer */
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

void shell_helper_run(void *args) {
  const shell_config_t *cfg = (const shell_config_t *)args;
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
    int rc = (c && argc > 0) ? c->fn(NULL, argc, argv) : 1;
    v_queue_send(dfd, &rc, V_WAIT_FOREVER);
  }
}
#endif /* VAIOS_MPU_USER_SEPARATION */

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
  if (!(c->flags & SHELL_CMD_PRIV))
    rc = c->fn(sh, argc, argv);
#if VAIOS_MPU_USER_SEPARATION
  else if (!v_port_is_privileged())
    rc = via_helper(sh, c, argc, argv);
#endif
  else
    rc = c->fn(NULL, argc, argv); /* privileged already: run it here */
  if (rc != 0)
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
    (void)shell_exec(&sh, line);
    v_printf(SHELL_PROMPT);
  }
}
