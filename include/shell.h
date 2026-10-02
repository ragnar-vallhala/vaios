#ifndef VAIOS_SHELL_H
#define VAIOS_SHELL_H
#include "vaios_config.h"
#include <stdint.h>

/**
 * @file shell.h
 * @brief The on-target command shell.
 *
 * Not to be confused with a terminal, which is the host-side program you talk to
 * it through (tio, screen, minicom). A terminal carries characters; a shell
 * reads a line and runs something. vaios used to ship its own under tools/; a
 * TTY emulator is an ordinary host tool and not this repo's job.
 *
 * Input comes through fd 0 like any task's. The console's RX interrupt is
 * /dev/console's (devfs.c), not the shell's: it queues bytes in kernel memory
 * and wakes the reader. shell_run() reads, edits in thread mode through
 * shell_poll(), and sleeps in v_file_wait() when nothing is pending. The editor
 * used to live in the ISR, mutating a multi-line ring and a ready flag from
 * interrupt context, which is both against the kernel's own rule about where
 * work belongs and the reason none of it could be unit-tested.
 *
 * WHERE THE STATE LIVES. Nothing the shell changes is in kernel memory, so the
 * shell task can run unprivileged. Commands given at startup are a const table
 * in FLASH, which every task may read. Everything mutable -- the line, the
 * history, the input ring and the commands registered at runtime -- is a
 * shell_t in the SHELL TASK's own memory: shell_run() keeps it on its stack and
 * passes it to every command. It used to be file-scope statics in kernel .bss,
 * and turning on MPU_USER_SEPARATION faulted on the first keystroke (todo 164).
 */

/* Sizes live here, not private to shell.c, so a caller can size its own buffers
   against the same bounds -- a test that feeds the input FIFO needs to know how
   big it is. The fallbacks matter more than they look: VAIOS_MODULE_SHELL
   depends on VAIOS_DEVFS, so a config with DEVFS off emits none of the shell
   menu's symbols and these are all a build has. Kconfig wins where it speaks. */
#ifndef CMD_MAX_LEN
#define CMD_MAX_LEN 64 /* the whole line, command and arguments */
#endif
#ifndef CMD_BUFFER_SIZE
#define CMD_BUFFER_SIZE 10 /* history DEPTH in lines, not a byte size */
#endif
#ifndef MAX_CMD_NUMBER
#define MAX_CMD_NUMBER 16 /* RUNTIME registry slots; the flash table is apart */
#endif
#ifndef SHELL_INPUT_FIFO_SIZE
#define SHELL_INPUT_FIFO_SIZE 64
#endif
#ifndef SHELL_MAX_ARGV
#define SHELL_MAX_ARGV 8
#endif

typedef struct shell shell_t;

/**
 * @brief A command body: the shell it runs in, then argc/argv like any shell.
 *
 * argv[0] is the command name as typed. Returns 0 on success, non-zero to
 * report failure (the shell prints it). @p sh is how a command reaches the
 * shell's own state -- `history` reads it, and a command may register another.
 */
typedef int (*shell_fn_t)(shell_t *sh, int argc, char **argv);

typedef struct Command {
  shell_fn_t fn;
  const char *name;
  const char *help; /* one line, shown by `help`; may be NULL */
  uint8_t flags;    /* SHELL_CMD_PRIV, or 0 */
} Command_t;

/**
 * Run this command PRIVILEGED. Only honoured in a const table (the built-ins and
 * the startup table): shell_register_command never sets it, or an unprivileged
 * task could hand the helper any function to call.
 *
 * When the shell itself is unprivileged (VAIOS_MPU_USER_SEPARATION), such a
 * command runs in the helper task (shell_helper_run) instead: the shell sends it
 * the command's index and a copy of the line, never a pointer, and waits for the
 * return code. Either way a privileged command gets sh == NULL -- the shell's
 * state is not the helper's to touch -- and argv from its own copy of the line.
 */
#define SHELL_CMD_PRIV 0x1

/**
 * @brief What shell_run() is started with: the application's startup commands.
 *
 * Declare both this and the table `static const`, so they are in flash and the
 * shell task can read them however unprivileged it is:
 *
 *     static const Command_t cmds[] = {{cmd_echo, "echo", "print the args"}};
 *     static const shell_config_t cfg = {cmds, 1};
 *     task_create(shell_run, (void *)&cfg, SHELL_TASK_STACK_SIZE, 1);
 */
typedef struct {
  const Command_t *cmds;
  int count;
} shell_config_t;

/** @brief All of the shell's mutable state. Lives in the shell task's memory. */
struct shell {
  const Command_t *table; /* startup commands, in flash */
  int table_count;
  Command_t extra[MAX_CMD_NUMBER]; /* registered at runtime */
  int extra_count;

  uint8_t in_buf[SHELL_INPUT_FIFO_SIZE]; /* bytes read from fd 0, not edited */
  unsigned in_head, in_tail;

  char line[CMD_MAX_LEN]; /* the line being edited */
  int len;
  char hist[CMD_BUFFER_SIZE][CMD_MAX_LEN];
  int hist_count; /* lines stored, saturating at CMD_BUFFER_SIZE */
  int hist_head;  /* next slot to write */
  int hist_pos;   /* -1 = editing a fresh line; else steps back */
  int esc;        /* 0 normal, 1 saw ESC, 2 saw ESC '[' */
  int prev_cr;    /* last byte was CR: an LF now ends the same line */
  int req_fd, done_fd; /* the helper's queues, opened on first use; -1 until */
#if VAIOS_MODULE_VFS
  /* This shell's working directory, as a FatFs path ("0:/logs"). The shell's
     own, not FatFs's f_chdir, which is one global for every task. */
  char cwd[CMD_MAX_LEN];
#endif
};

/** @brief Reset @p sh: empty line, history and runtime registry, with
 *         @p cmds (@p count of them, may be NULL/0) as the startup table. */
void shell_init(shell_t *sh, const Command_t *cmds, int count);

/**
 * @brief Register a command at runtime, into @p sh. 0 on success, 1 when the
 *        runtime table is full (MAX_CMD_NUMBER) or the name is unusable.
 *
 * @p name and @p help are kept as pointers, so they must outlive the shell --
 * a string literal is the usual answer. A name longer than CMD_MAX_LEN-1 is
 * REFUSED rather than accepted and then never matched, which is what happened
 * while the lookup bounded its token buffer by the history depth.
 */
int shell_register_command(shell_t *sh, shell_fn_t fn, const char *name,
                           const char *help);

/**
 * @brief The shell task body. @p args is a `const shell_config_t *` (or NULL
 *        for the built-ins alone). Runs commands in its own context.
 */
void shell_run(void *args);

/**
 * @brief The privileged helper's task body (VAIOS_MPU_USER_SEPARATION). Start
 *        it PRIVILEGED (task_create_privileged) with the same config as the
 *        shell, so both resolve a command index against the same flash tables.
 */
#if VAIOS_MPU_USER_SEPARATION
void shell_helper_run(void *args);
#endif

/**
 * @brief Run one line: tokenize @p line (in place), look the command up, run it
 *        -- through the helper if it is privileged and the shell is not.
 *        Returns its result (printing a non-zero one), or -1 when the line
 *        names no command (0 for an empty line). shell_run() is this in a loop.
 */
int shell_exec(shell_t *sh, char *line);

/** @brief Exact lookup: built-ins, then the startup table, then the runtime
 *         registry. NULL if unknown. */
const Command_t *shell_find_command(const shell_t *sh, const char *name);

/* --- input path ----------------------------------------------------------- */

/**
 * @brief Hand one received byte to the line editor. Task context: shell_poll()
 *        pushes what it read from fd 0 through here, and the tests inject at
 *        the same point. A full input ring drops the byte.
 */
void shell_feed(shell_t *sh, char c);

/**
 * @brief Drain fed bytes, editing the current line. Returns 1 when a complete
 *        line has been placed in @p out (NUL-terminated, at most @p cap bytes),
 *        0 when there is nothing complete yet.
 *
 * Exposed because the editor is worth testing: CR, LF and CRLF all submit,
 * backspace erases one character at any length, ^C abandons the line, and the
 * up/down arrows walk the history.
 */
int shell_poll(shell_t *sh, char *out, int cap);

/**
 * @brief Split @p line in place into at most @p max_argv tokens on whitespace.
 *        Returns argc. @p line is modified (NULs written at separators).
 */
int shell_tokenize(char *line, char **argv, int max_argv);

#endif // !VAIOS_SHELL_H
