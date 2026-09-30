#ifndef VAIOS_SHELL_H
#define VAIOS_SHELL_H
#include "vaios_config.h"

/**
 * @file shell.h
 * @brief The on-target command shell.
 *
 * Not to be confused with tools/terminal.c, which is the host-side TTY emulator
 * you talk to it through. A terminal carries characters; a shell reads a line
 * and runs something.
 *
 * The input path is split in two on purpose. shell_feed() is all the console RX
 * interrupt does — hand over one byte and return. Everything else, the line
 * editing included, happens in shell_poll() in thread mode. The editor used to
 * live in the ISR, mutating a multi-line ring and a ready flag from interrupt
 * context, which is both against the kernel's own rule about where work belongs
 * and the reason none of it could be unit-tested.
 */

/* Sizes live here, not private to shell.c, so a caller can size its own buffers
   against the same bounds -- a test that feeds the input FIFO needs to know how
   big it is. Fallbacks cover a build whose Kconfig predates these symbols. */
#ifndef SHELL_INPUT_FIFO_SIZE
#define SHELL_INPUT_FIFO_SIZE 64
#endif
#ifndef SHELL_MAX_ARGV
#define SHELL_MAX_ARGV 8
#endif

/**
 * @brief A command body: argc/argv, like any shell.
 *
 * argv[0] is the command name as typed. Returns 0 on success, non-zero to
 * report failure (the shell prints it).
 *
 * This used to be `void (*)(void *)` receiving the raw line, so every command
 * that took an argument parsed its own — `perf` hand-rolled subcommand matching.
 * One tokenizer for everybody instead.
 */
typedef int (*shell_fn_t)(int argc, char **argv);

typedef struct Command {
  shell_fn_t fn;
  const char *name;
  const char *help; /* one line, shown by `help`; may be NULL */
} Command_t;

/** @brief Clear the registry, register the built-ins, arm the input path. */
void shell_init(void);

/**
 * @brief Register a command. 0 on success, 1 when the table is full
 *        (MAX_CMD_NUMBER) or the name is unusable.
 *
 * A name longer than CMD_MAX_LEN-1 is REFUSED rather than accepted and then
 * never matched, which is what happened while the lookup bounded its token
 * buffer by the history depth.
 */
int shell_register_command(shell_fn_t fn, const char *name, const char *help);

/** @brief The shell task body. Runs commands in its own context. */
void shell_run(void *args);

/** @brief Exact lookup by name. NULL if unknown. */
Command_t *shell_find_command(const char *name);

/* --- input path ----------------------------------------------------------- */

/**
 * @brief Hand one received byte to the shell. Safe from an ISR: it does a single
 *        lock-free SPSC write and nothing else. Bytes are dropped if the shell
 *        is not draining, which is better than an ISR that edits.
 */
void shell_feed(char c);

/**
 * @brief Drain fed bytes, editing the current line. Returns 1 when a complete
 *        line has been placed in @p out (NUL-terminated, at most @p cap bytes),
 *        0 when there is nothing complete yet.
 *
 * Exposed because the editor is worth testing: CR, LF and CRLF all submit,
 * backspace erases one character at any length, ^C abandons the line, and the
 * up/down arrows walk the history.
 */
int shell_poll(char *out, int cap);

/**
 * @brief Split @p line in place into at most @p max_argv tokens on whitespace.
 *        Returns argc. @p line is modified (NULs written at separators).
 */
int shell_tokenize(char *line, char **argv, int max_argv);

#endif // !VAIOS_SHELL_H
