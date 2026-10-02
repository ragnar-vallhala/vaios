/**
 * @file test_shell.c
 * @brief Unit tests for kernel/shell.c: registry, tokenizer, line editor.
 *
 * The editor is covered here, which the previous version of this file said was
 * impossible: "the UART/ISR parts aren't host-testable". They were not, because
 * the editing lived in the RX interrupt. Splitting the input path into
 * shell_feed() (one SPSC write, ISR-safe) and shell_poll() (thread mode, does
 * the editing) made the whole thing ordinary code that a test can drive by
 * feeding bytes.
 *
 * Every test resets one shell_t, the way shell_run keeps one on the shell
 * task's stack, so each starts clean. Nothing here touches shell.c statics:
 * there are none left.
 */
#include "framework.h"
#include "shell.h"
#include "utils.h" /* v_strcmp, v_strlen */
#include "task.h"  /* V_WAIT_FOREVER, MS_TO_TICKS */
#include "vfile.h" /* v_file_wait */

extern int g_stub_prompts;

static shell_t g_sh;
static void shell_reset(void) { shell_init(&g_sh, NULL, 0); }

static int dummy_cb(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  return 0;
}

/* Feed a NUL-terminated string as if it arrived byte by byte on the UART. */
static void feed(const char *s) {
  while (*s)
    shell_feed(&g_sh, *s++);
}

/* --- registry -------------------------------------------------------------- */

static void test_init_has_builtins(void) {
  shell_reset();
  TEST_ASSERT(shell_find_command(&g_sh, "help") != NULL);
  TEST_ASSERT(shell_find_command(&g_sh, "clear") != NULL);
  TEST_ASSERT(shell_find_command(&g_sh, "version") != NULL);
  TEST_ASSERT(shell_find_command(&g_sh, "history") != NULL);
}

static void test_register_then_find(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, "mine", "a help line"), 0);
  const Command_t *c = shell_find_command(&g_sh, "mine");
  TEST_ASSERT(c != NULL);
  TEST_ASSERT(c->fn == dummy_cb);
  TEST_ASSERT_EQ(v_strcmp(c->help, "a help line"), 0);
}

/* A name longer than the line buffer could never be matched, so it is refused
 * at registration rather than accepted and silently lost. */
static void test_register_rejects_overlong_name(void) {
  shell_reset();
  static char big[CMD_MAX_LEN + 8];
  for (unsigned i = 0; i < sizeof(big) - 1; i++)
    big[i] = 'x';
  big[sizeof(big) - 1] = '\0';
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, big, NULL), 1);
}

/* But a long-ish name that DOES fit must work: this is the case the old lookup
 * broke, because it sized its token buffer by the history depth. */
static void test_find_long_command_name(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, "diagnostics", NULL), 0);
  const Command_t *c = shell_find_command(&g_sh, "diagnostics");
  TEST_ASSERT(c != NULL);
  TEST_ASSERT_EQ(v_strcmp(c->name, "diagnostics"), 0);
}

static void test_register_full_returns_1(void) {
  shell_reset();
  static const char *names[MAX_CMD_NUMBER + 4] = {0};
  static char store[MAX_CMD_NUMBER + 4][8];
  int accepted = 0;
  for (int i = 0; i < MAX_CMD_NUMBER + 4; i++) {
    store[i][0] = 'c';
    store[i][1] = (char)('a' + (i % 26));
    store[i][2] = (char)('a' + (i / 26));
    store[i][3] = '\0';
    names[i] = store[i];
    if (shell_register_command(&g_sh, dummy_cb, names[i], NULL) == 0)
      accepted++;
    else
      break;
  }
  /* The built-ins are in flash now, so every runtime slot is the caller's. */
  TEST_ASSERT_EQ(accepted, MAX_CMD_NUMBER);
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, "zzz", NULL), 1);
}

/* Startup commands come from a const table -- in flash on target, readable by
 * an unprivileged shell -- and are found alongside the built-ins. */
static int other_cb(shell_t *sh, int argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  return 7;
}
static const Command_t startup_cmds[] = {
    {dummy_cb, "echo", "print the arguments", 0},
    {other_cb, "ticks", NULL, 0},
};
static void test_startup_table_found(void) {
  shell_init(&g_sh, startup_cmds, 2);
  const Command_t *c = shell_find_command(&g_sh, "ticks");
  TEST_ASSERT(c == &startup_cmds[1]); /* the flash entry itself, not a copy */
  TEST_ASSERT(shell_find_command(&g_sh, "echo") != NULL);
  TEST_ASSERT(shell_find_command(&g_sh, "help") != NULL); /* built-ins too */
  /* The runtime table is separate, and empty. */
  TEST_ASSERT_EQ(g_sh.extra_count, 0);
}

/* A command reaches its shell through its first argument, which is how one
 * can register another at runtime -- into the shell's own memory. */
static int adder_cb(shell_t *sh, int argc, char **argv) {
  (void)argc;
  (void)argv;
  return shell_register_command(sh, other_cb, "added", NULL);
}
static void test_command_registers_at_runtime(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_register_command(&g_sh, adder_cb, "add", NULL), 0);
  TEST_ASSERT(shell_find_command(&g_sh, "added") == NULL);
  const Command_t *c = shell_find_command(&g_sh, "add");
  TEST_ASSERT_EQ(c->fn(&g_sh, 0, NULL), 0);
  c = shell_find_command(&g_sh, "added");
  TEST_ASSERT(c != NULL);
  TEST_ASSERT_EQ(c->fn(&g_sh, 0, NULL), 7);
}

/* Two shells share nothing: the state is the shell_t, not file scope. */
static void test_shells_are_independent(void) {
  static shell_t other;
  shell_reset();
  shell_init(&other, NULL, 0);
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, "mine", NULL), 0);
  TEST_ASSERT(shell_find_command(&other, "mine") == NULL);
  char out[CMD_MAX_LEN];
  feed("half");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  shell_feed(&other, 'x');
  shell_feed(&other, '\n');
  TEST_ASSERT_EQ(shell_poll(&other, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "x"), 0); /* g_sh's half line did not leak */
}

/* shell_exec runs a line end to end. An ordinary command gets its shell; a
 * privileged one -- run here directly, since this build has no MPU separation
 * and so no helper -- gets NULL, exactly as it would in the helper, so a
 * command that works in one place works in the other. */
static shell_t *seen_sh;
static int seen_argc;
static char seen_arg1[16];
static int probe_cb(shell_t *sh, int argc, char **argv) {
  seen_sh = sh;
  seen_argc = argc;
  seen_arg1[0] = '\0';
  if (argc > 1)
    for (int i = 0; i < 15 && (seen_arg1[i] = argv[1][i]); i++)
      ;
  return 3;
}
static const Command_t exec_cmds[] = {
    {probe_cb, "plain", NULL, 0},
    {probe_cb, "priv", NULL, SHELL_CMD_PRIV},
};
static void test_exec_routes_by_privilege(void) {
  shell_init(&g_sh, exec_cmds, 2);
  char l1[] = "plain  one two";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l1), 3);
  TEST_ASSERT(seen_sh == &g_sh);
  TEST_ASSERT_EQ(seen_argc, 3);
  TEST_ASSERT_EQ(v_strcmp(seen_arg1, "one"), 0);

  char l2[] = "priv x";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l2), 3);
  TEST_ASSERT(seen_sh == NULL); /* the shell's state is not the helper's */
  TEST_ASSERT_EQ(v_strcmp(seen_arg1, "x"), 0);

  char l3[] = "nosuch";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l3), -1);
  char l4[] = "   ";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l4), 0);
}

/* A runtime registration can never be privileged: it is a function pointer an
 * unprivileged task chose, and the helper would call it with privilege. */
static void test_runtime_command_is_never_privileged(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_register_command(&g_sh, probe_cb, "late", NULL), 0);
  TEST_ASSERT_EQ(shell_find_command(&g_sh, "late")->flags, 0);
  char l[] = "late";
  shell_exec(&g_sh, l);
  TEST_ASSERT(seen_sh == &g_sh); /* ran as an ordinary command */
}

static void test_find_unknown_and_empty(void) {
  shell_reset();
  TEST_ASSERT(shell_find_command(&g_sh, "nope") == NULL);
  TEST_ASSERT(shell_find_command(&g_sh, "") == NULL);
  TEST_ASSERT(shell_find_command(&g_sh, NULL) == NULL);
}

static void test_register_rejects_null(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_register_command(&g_sh, NULL, "x", NULL), 1);
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, NULL, NULL), 1);
  TEST_ASSERT_EQ(shell_register_command(&g_sh, dummy_cb, "", NULL), 1);
}

/* --- tokenizer ------------------------------------------------------------- */

static void test_tokenize_splits(void) {
  char line[] = "perf save /mnt/0:p.csv";
  char *argv[8];
  int argc = shell_tokenize(line, argv, 8);
  TEST_ASSERT_EQ(argc, 3);
  TEST_ASSERT_EQ(v_strcmp(argv[0], "perf"), 0);
  TEST_ASSERT_EQ(v_strcmp(argv[1], "save"), 0);
  TEST_ASSERT_EQ(v_strcmp(argv[2], "/mnt/0:p.csv"), 0);
}

static void test_tokenize_collapses_whitespace(void) {
  char line[] = "   perf \t  show   ";
  char *argv[8];
  int argc = shell_tokenize(line, argv, 8);
  TEST_ASSERT_EQ(argc, 2);
  TEST_ASSERT_EQ(v_strcmp(argv[0], "perf"), 0);
  TEST_ASSERT_EQ(v_strcmp(argv[1], "show"), 0);
}

static void test_tokenize_empty_and_bounds(void) {
  char empty[] = "     ";
  char *argv[4];
  TEST_ASSERT_EQ(shell_tokenize(empty, argv, 4), 0);
  /* More tokens than argv can hold: stops at the bound, does not overrun. */
  char many[] = "a b c d e f";
  TEST_ASSERT_EQ(shell_tokenize(many, argv, 4), 4);
  TEST_ASSERT_EQ(shell_tokenize(NULL, argv, 4), 0);
}

/* --- line editor ----------------------------------------------------------- */

/* The bug this pins: dispatch used to require the line to be at least
 * ESCAPE_SEQ_LEN long, then chopped that many bytes off, so a short command
 * never ran and the line ending was assumed rather than handled. */
static void test_editor_short_line_submits(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("ls\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ls"), 0);
}

static void test_editor_accepts_cr_and_crlf(void) {
  char out[CMD_MAX_LEN];
  shell_reset();
  feed("help\r");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);

  shell_reset();
  feed("help\r\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
  /* The trailing LF must not submit a second, empty line -- nor reprompt as
     if one had been entered, which doubled the prompt on CRLF terminals. */
  g_stub_prompts = 0;
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  TEST_ASSERT_EQ(g_stub_prompts, 0);

  /* A bare LF on its own is still an empty line and reprompts. */
  feed("\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  TEST_ASSERT_EQ(g_stub_prompts, 1);
}

static void test_editor_incomplete_line_waits(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("vers");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  feed("ion\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "version"), 0);
}

static void test_editor_backspace(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("helq\b\bp\n"); /* "helq" -> backspace twice -> "he" -> 'p' -> "hep" */
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "hep"), 0);
}

static void test_editor_backspace_on_empty_is_harmless(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("\b\b\bok\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ok"), 0);
}

static void test_editor_ctrl_c_abandons(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("garbage\003");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

static void test_editor_empty_line_does_not_dispatch(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("\n\n\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
}

/* A line longer than CMD_MAX_LEN stops accepting characters rather than
 * overrunning, and still submits what it holds.
 *
 * Fed with a poll every few bytes, which is what the running shell does (it
 * polls on a 10 ms tick). Feeding it all at once instead overflows the input
 * FIFO and the '\n' is among what gets dropped -- see the next test, which is
 * about that on purpose. */
static void test_editor_overlong_line_is_truncated(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  for (int i = 0; i < CMD_MAX_LEN + 32; i++) {
    shell_feed(&g_sh, 'a');
    if ((i % 16) == 15)
      TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  }
  shell_feed(&g_sh, '\n');
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  /* Truncated to what the line can hold, NUL-terminated, never overrun. */
  TEST_ASSERT_EQ((int)v_strlen(out), CMD_MAX_LEN - 1);
}

/* Overflowing the FIFO loses bytes, and that is the deliberate trade: the
 * producer is an interrupt, so it drops rather than waits. What must NOT happen
 * is corruption -- the shell stays usable and the next line is clean. */
static void test_editor_survives_fifo_overflow(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  for (int i = 0; i < SHELL_INPUT_FIFO_SIZE * 3; i++)
    shell_feed(&g_sh, 'z');
  shell_feed(&g_sh, '\n'); /* dropped with the rest of the tail */
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);

  /* The partial line survives, as it would on any terminal where you typed a
     lot and never pressed Enter -- and because it is now full, further
     characters are refused rather than overrunning. ^C is the way out, and
     after it the shell is clean. */
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT(v_strcmp(out, "help") != 0); /* the z-line, not "help" */

  shell_feed(&g_sh, 0x03);
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

/* --- history -------------------------------------------------------------- */

static void test_history_up_recalls(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("first\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  feed("second\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);

  /* Up twice reaches "first"; submitting it yields it again. */
  feed("\033[A\033[A\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "first"), 0);
}

static void test_history_down_returns_to_fresh_line(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("only\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  /* Up to "only", then down past it: the line is empty again, so Enter does
     not dispatch. */
  feed("\033[A\033[B\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
}

static void test_history_up_at_start_is_harmless(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("\033[A\033[A");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

/* An unknown escape must be swallowed, not typed into the line. */
static void test_editor_unknown_escape_swallowed(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("a\033[Cb\n"); /* ESC [ C is right-arrow, which we do not implement */
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ab"), 0);
}

static void test_history_dedupes_immediate_repeat(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("same\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  feed("same\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  /* One entry, so a second Up cannot go further back. */
  feed("\033[A\033[A\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "same"), 0);
}

/* This binary's port has no console RX interrupt, so nothing will ever signal
 * the shell's wait on fd 0. The wait must then come back after one poll
 * interval rather than park forever -- the shell's V_WAIT_FOREVER would
 * otherwise wedge it on semihosting and the host port. */
extern uint32_t g_stub_sem_last_ticks;
static void test_wait_without_rx_irq_polls(void) {
  shell_reset();
  g_stub_sem_last_ticks = 0;
  TEST_ASSERT_EQ(v_file_wait(0, V_WAIT_FOREVER), -1);
  TEST_ASSERT_EQ(g_stub_sem_last_ticks, MS_TO_TICKS(10));
}

static const test_case_t shell_cases[] = {
    TEST_CASE(test_wait_without_rx_irq_polls),
    TEST_CASE(test_init_has_builtins),
    TEST_CASE(test_register_then_find),
    TEST_CASE(test_register_rejects_overlong_name),
    TEST_CASE(test_find_long_command_name),
    TEST_CASE(test_register_full_returns_1),
    TEST_CASE(test_startup_table_found),
    TEST_CASE(test_command_registers_at_runtime),
    TEST_CASE(test_shells_are_independent),
    TEST_CASE(test_exec_routes_by_privilege),
    TEST_CASE(test_runtime_command_is_never_privileged),
    TEST_CASE(test_find_unknown_and_empty),
    TEST_CASE(test_register_rejects_null),
    TEST_CASE(test_tokenize_splits),
    TEST_CASE(test_tokenize_collapses_whitespace),
    TEST_CASE(test_tokenize_empty_and_bounds),
    TEST_CASE(test_editor_short_line_submits),
    TEST_CASE(test_editor_accepts_cr_and_crlf),
    TEST_CASE(test_editor_incomplete_line_waits),
    TEST_CASE(test_editor_backspace),
    TEST_CASE(test_editor_backspace_on_empty_is_harmless),
    TEST_CASE(test_editor_ctrl_c_abandons),
    TEST_CASE(test_editor_empty_line_does_not_dispatch),
    TEST_CASE(test_editor_overlong_line_is_truncated),
    TEST_CASE(test_editor_survives_fifo_overflow),
    TEST_CASE(test_history_up_recalls),
    TEST_CASE(test_history_down_returns_to_fresh_line),
    TEST_CASE(test_history_up_at_start_is_harmless),
    TEST_CASE(test_editor_unknown_escape_swallowed),
    TEST_CASE(test_history_dedupes_immediate_repeat),
};

const test_suite_t shell_suite = {
    .name = "shell (registry, tokenizer, line editor)",
    .cases = shell_cases,
    .count = TEST_COUNT(shell_cases),
};
