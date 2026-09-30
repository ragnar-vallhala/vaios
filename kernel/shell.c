/**
 * @file shell.c
 * @brief The on-target command shell: input, line editing, dispatch.
 *
 * Three things changed shape here, and the reasons are worth keeping.
 *
 * 1. THE EDITOR IS NOT IN THE ISR. The console RX interrupt calls shell_feed(),
 *    which does one lock-free SPSC write and returns. shell_poll() drains that
 *    in thread mode and does the editing. Before, the ISR walked a multi-line
 *    history ring, wrote NULs into it and set a "ready" flag -- interrupt
 *    context doing string work on shared state, and untestable besides.
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
 */
#include "port.h" // v_port_hw_console_* — all hardware access goes through here
#include "perf.h"
#include "shell.h"
#include "structure.h" // spsc_* — the ISR -> task byte channel
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vaios_config.h"
#include <stddef.h>
#include <stdint.h>

#ifndef SHELL_LOG
#define SHELL_LOG(fmt, ...)                                                    \
  v_log(SHELL_LOG_LEVEL, "[SHELL] " fmt, ##__VA_ARGS__)
#endif

#define SHELL_PROMPT "vaios> "

/* --- registry -------------------------------------------------------------- */

static Command_t _commands[MAX_CMD_NUMBER];
static int _cmd_count = 0;

/* --- input: one producer (the RX ISR), one consumer (the shell task) ------- */

static spsc_fifo_t _in;
static uint8_t _in_store[SHELL_INPUT_FIFO_SIZE];

/* --- the line being edited, and the history ------------------------------- */

static char _line[CMD_MAX_LEN];
static int _len = 0;
static char _hist[CMD_BUFFER_SIZE][CMD_MAX_LEN];
static int _hist_count = 0; /* lines stored, saturating at CMD_BUFFER_SIZE */
static int _hist_head = 0;  /* next slot to write */
static int _hist_pos = -1;  /* -1 = editing a fresh line; else steps back */
static int _esc = 0;        /* 0 normal, 1 saw ESC, 2 saw ESC '[' */
static int _initialized_shell = 0;

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
  print_fmt("%s", b);
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

Command_t *shell_find_command(const char *name) {
  if (!name || *name == '\0')
    return NULL;
  for (int i = 0; i < _cmd_count; i++) {
    if (_commands[i].fn && v_strcmp(_commands[i].name, name) == 0)
      return &_commands[i];
  }
  return NULL;
}

int shell_register_command(shell_fn_t fn, const char *name, const char *help) {
  if (!fn || !name || *name == '\0')
    return 1;
  /* Refuse a name the lookup could never match, rather than accept it and
     silently never find it. */
  if (v_strlen(name) >= CMD_MAX_LEN)
    return 1;
  if (_cmd_count >= MAX_CMD_NUMBER)
    return 1;
  _commands[_cmd_count].fn = fn;
  _commands[_cmd_count].name = name;
  _commands[_cmd_count].help = help;
  _cmd_count++;
  return 0;
}

/* --- history -------------------------------------------------------------- */

static void hist_push(const char *line) {
  if (!line || *line == '\0')
    return;
  /* Don't store a repeat of the newest entry: holding a key on the up arrow
     should walk back through what was typed, not through duplicates. */
  if (_hist_count > 0) {
    int newest = (_hist_head - 1 + CMD_BUFFER_SIZE) % CMD_BUFFER_SIZE;
    if (v_strcmp(_hist[newest], line) == 0)
      return;
  }
  sh_copy(_hist[_hist_head], line, CMD_MAX_LEN);
  _hist_head = (_hist_head + 1) % CMD_BUFFER_SIZE;
  if (_hist_count < CMD_BUFFER_SIZE)
    _hist_count++;
}

/* steps back from newest: 0 = newest, _hist_count-1 = oldest. */
static const char *hist_at(int back) {
  if (back < 0 || back >= _hist_count)
    return NULL;
  int idx = (_hist_head - 1 - back + 2 * CMD_BUFFER_SIZE) % CMD_BUFFER_SIZE;
  return _hist[idx];
}

/* Wipe the visible line and put `s` in its place, on screen and in _line. */
static void line_replace(const char *s) {
  for (int i = 0; i < _len; i++)
    print_fmt("\b \b");
  _len = 0;
  _line[0] = '\0';
  if (!s)
    return;
  while (*s && _len < CMD_MAX_LEN - 1) {
    _line[_len++] = *s;
    sh_putc(*s);
    s++;
  }
  _line[_len] = '\0';
}

/* --- input path ----------------------------------------------------------- */

void shell_feed(char c) {
  uint8_t b = (uint8_t)c;
  /* One SPSC write, nothing else: this is called from the RX interrupt. A full
     fifo drops the byte, which is the right trade against an ISR that edits. */
  (void)spsc_write(&_in, &b, 1);
}

int shell_poll(char *out, int cap) {
  uint8_t b;
  if (!out || cap <= 0)
    return 0;
  while (spsc_read(&_in, &b, 1) == 1) {
    char c = (char)b;

    /* Arrow keys arrive as ESC '[' 'A'/'B'. Two states is the whole parser;
       anything else after ESC is swallowed rather than typed into the line. */
    if (_esc == 1) {
      _esc = (c == '[') ? 2 : 0;
      continue;
    }
    if (_esc == 2) {
      _esc = 0;
      if (c == 'A') { /* up: older */
        if (_hist_pos + 1 < _hist_count) {
          _hist_pos++;
          line_replace(hist_at(_hist_pos));
        }
      } else if (c == 'B') { /* down: newer, then back to a fresh line */
        if (_hist_pos > 0) {
          _hist_pos--;
          line_replace(hist_at(_hist_pos));
        } else if (_hist_pos == 0) {
          _hist_pos = -1;
          line_replace(NULL);
        }
      }
      continue;
    }
    if (c == 0x1B) {
      _esc = 1;
      continue;
    }

    if (c == 0x03) { /* ^C: abandon the line */
      print_fmt("^C\r\n" SHELL_PROMPT);
      _len = 0;
      _line[0] = '\0';
      _hist_pos = -1;
      continue;
    }

    if (c == '\b' || c == 0x7F) { /* backspace / DEL */
      if (_len > 0) {
        _len--;
        _line[_len] = '\0';
        print_fmt("\b \b");
      }
      continue;
    }

    /* CR, LF or CRLF all submit. The old code required the line to be at least
       ESCAPE_SEQ_LEN long before it would dispatch at all, and then chopped that
       many bytes off the end -- so short commands never ran and the line ending
       was assumed rather than handled. An empty line just reprompts. */
    if (c == '\r' || c == '\n') {
      if (c == '\n' && _len == 0 && _line[0] == '\0') {
        /* swallow the LF of a CRLF pair: the CR already submitted */
      }
      print_fmt("\r\n");
      _line[_len] = '\0';
      if (_len == 0) {
        print_fmt(SHELL_PROMPT);
        _hist_pos = -1;
        continue;
      }
      sh_copy(out, _line, cap);
      hist_push(_line);
      _len = 0;
      _line[0] = '\0';
      _hist_pos = -1;
      return 1;
    }

    /* Anything else printable joins the line. A full line stops accepting
       rather than wrapping or overflowing. */
    if (c >= 0x20 && c < 0x7F && _len < CMD_MAX_LEN - 1) {
      _line[_len++] = c;
      _line[_len] = '\0';
      sh_putc(c);
    }
  }
  return 0;
}

/* --- built-ins ------------------------------------------------------------- */

static int cmd_help(int argc, char **argv) {
  (void)argc;
  (void)argv;
  for (int i = 0; i < _cmd_count; i++) {
    if (!_commands[i].fn)
      continue;
    /* No %-10s: print_fmt parses a '0' flag and a width, not '-', so a
       left-justify would fall through to the default arm and emit "%-". */
    print_fmt("  %s", _commands[i].name);
    if (_commands[i].help)
      print_fmt("\t%s", _commands[i].help);
    print_fmt("\r\n");
  }
  return 0;
}

static int cmd_clear(int argc, char **argv) {
  (void)argc;
  (void)argv;
  /* Straight to the console, not through v_log: a screen-clear with an
     "[INFO 42]" prefix in front of it is not a screen clear. */
  print_fmt("\033[2J\033[H");
  return 0;
}

static int cmd_version(int argc, char **argv) {
  (void)argc;
  (void)argv;
  print_fmt("vaios %s\r\n", VAIOS_VERSION);
  print_fmt("NAVROBOTEC PVT. LTD. -- %s\r\n", AUTHOR);
  return 0;
}

static int cmd_history(int argc, char **argv) {
  (void)argc;
  (void)argv;
  /* Oldest first, so the numbering reads like a log. */
  for (int back = _hist_count - 1; back >= 0; back--)
    print_fmt("  %d  %s\r\n", _hist_count - back, hist_at(back));
  return 0;
}

#if VAIOS_MODULE_PERF
/* `perf [show|reset|save <path>]`. No hand-rolled argument splitting: argv[1]
   is the subcommand and argv[2] is the path, because the shell tokenized. */
static int cmd_perf(int argc, char **argv) {
  const char *sub = (argc > 1) ? argv[1] : "show";
  if (v_strcmp(sub, "show") == 0) {
    v_perf_dump();
    return 0;
  }
  if (v_strcmp(sub, "reset") == 0) {
    v_perf_reset();
    print_fmt("perf: counters reset\r\n");
    return 0;
  }
#if VAIOS_MODULE_VFS
  if (v_strcmp(sub, "save") == 0) {
    if (argc < 3) {
      print_fmt("usage: perf save <path>\r\n");
      return 1;
    }
    int rc = v_perf_dump_to_file(argv[2]);
    print_fmt(rc == 0 ? "perf: saved to %s\r\n" : "perf: save failed (is VFS "
                                                  "mounted?) %s\r\n",
              argv[2]);
    return rc == 0 ? 0 : 1;
  }
  print_fmt("usage: perf [show|reset|save <path>]\r\n");
#else
  print_fmt("usage: perf [show|reset]\r\n");
#endif
  return 1;
}
#endif /* VAIOS_MODULE_PERF */

/* --- lifecycle ------------------------------------------------------------ */

/* The console RX interrupt's whole job. v_port_hw_console_rx_irq_init takes a
   void(void), so the read happens here and the byte goes straight into the
   fifo -- two calls, no state, no string work in interrupt context. */
static void shell_feed_isr(void) {
  shell_feed(v_port_hw_console_read_char());
}

void shell_init(void) {
  _cmd_count = 0;
  for (int i = 0; i < MAX_CMD_NUMBER; i++)
    _commands[i].fn = NULL;
  _len = 0;
  _line[0] = '\0';
  _hist_count = 0;
  _hist_head = 0;
  _hist_pos = -1;
  _esc = 0;
  spsc_init(&_in, _in_store, SHELL_INPUT_FIFO_SIZE, 1);

  shell_register_command(cmd_help, "help", "list commands");
  shell_register_command(cmd_clear, "clear", "clear the screen");
  shell_register_command(cmd_version, "version", "kernel version and author");
  shell_register_command(cmd_history, "history", "recent command lines");
#if VAIOS_MODULE_PERF
  shell_register_command(cmd_perf, "perf", "[show|reset] kernel counters");
#endif

  _initialized_shell = 1;
  v_port_hw_console_rx_irq_init(shell_feed_isr);
}

void shell_run(void *args) {
  (void)args;
  char line[CMD_MAX_LEN];
  char *argv[SHELL_MAX_ARGV];
  print_fmt("\r\n" SHELL_PROMPT);
  for (;;) {
    if (shell_poll(line, (int)sizeof line)) {
      int argc = shell_tokenize(line, argv, SHELL_MAX_ARGV);
      if (argc > 0) {
        Command_t *c = shell_find_command(argv[0]);
        if (c) {
          int rc = c->fn(argc, argv);
          if (rc != 0)
            print_fmt("%s: exit %d\r\n", argv[0], rc);
        } else {
          print_fmt("%s: not found\r\n", argv[0]);
        }
      }
      print_fmt(SHELL_PROMPT);
    }
    /* Nothing to do but wait for input. 10 ms is imperceptible to a person and
       keeps the shell off the CPU; the fifo absorbs bursts in between. */
    v_delay(10);
  }
}
