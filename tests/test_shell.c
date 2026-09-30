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
 * shell_init() clears the registry, re-registers the built-ins and resets the
 * editor, so each test calls it first for a deterministic start.
 */
#include "framework.h"
#include "shell.h"
#include "utils.h" /* v_strcmp, v_strlen */

static int dummy_cb(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}

/* Feed a NUL-terminated string as if it arrived byte by byte on the UART. */
static void feed(const char *s) {
  while (*s)
    shell_feed(*s++);
}

/* --- registry -------------------------------------------------------------- */

static void test_init_registers_builtins(void) {
  shell_init();
  TEST_ASSERT(shell_find_command("help") != NULL);
  TEST_ASSERT(shell_find_command("clear") != NULL);
  TEST_ASSERT(shell_find_command("version") != NULL);
  TEST_ASSERT(shell_find_command("history") != NULL);
}

static void test_register_then_find(void) {
  shell_init();
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, "mine", "a help line"), 0);
  Command_t *c = shell_find_command("mine");
  TEST_ASSERT(c != NULL);
  TEST_ASSERT(c->fn == dummy_cb);
  TEST_ASSERT_EQ(v_strcmp(c->help, "a help line"), 0);
}

/* A name longer than the line buffer could never be matched, so it is refused
 * at registration rather than accepted and silently lost. */
static void test_register_rejects_overlong_name(void) {
  shell_init();
  static char big[CMD_MAX_LEN + 8];
  for (unsigned i = 0; i < sizeof(big) - 1; i++)
    big[i] = 'x';
  big[sizeof(big) - 1] = '\0';
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, big, NULL), 1);
}

/* But a long-ish name that DOES fit must work: this is the case the old lookup
 * broke, because it sized its token buffer by the history depth. */
static void test_find_long_command_name(void) {
  shell_init();
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, "diagnostics", NULL), 0);
  Command_t *c = shell_find_command("diagnostics");
  TEST_ASSERT(c != NULL);
  TEST_ASSERT_EQ(v_strcmp(c->name, "diagnostics"), 0);
}

static void test_register_full_returns_1(void) {
  shell_init();
  static const char *names[MAX_CMD_NUMBER + 4] = {0};
  static char store[MAX_CMD_NUMBER + 4][8];
  int accepted = 0;
  for (int i = 0; i < MAX_CMD_NUMBER + 4; i++) {
    store[i][0] = 'c';
    store[i][1] = (char)('a' + (i % 26));
    store[i][2] = (char)('a' + (i / 26));
    store[i][3] = '\0';
    names[i] = store[i];
    if (shell_register_command(dummy_cb, names[i], NULL) == 0)
      accepted++;
    else
      break;
  }
  /* The built-ins already occupy slots, so this stops short of MAX. */
  TEST_ASSERT(accepted < MAX_CMD_NUMBER);
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, "zzz", NULL), 1);
}

static void test_find_unknown_and_empty(void) {
  shell_init();
  TEST_ASSERT(shell_find_command("nope") == NULL);
  TEST_ASSERT(shell_find_command("") == NULL);
  TEST_ASSERT(shell_find_command(NULL) == NULL);
}

static void test_register_rejects_null(void) {
  shell_init();
  TEST_ASSERT_EQ(shell_register_command(NULL, "x", NULL), 1);
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, NULL, NULL), 1);
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, "", NULL), 1);
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
  shell_init();
  char out[CMD_MAX_LEN];
  feed("ls\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ls"), 0);
}

static void test_editor_accepts_cr_and_crlf(void) {
  char out[CMD_MAX_LEN];
  shell_init();
  feed("help\r");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);

  shell_init();
  feed("help\r\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
  /* The trailing LF must not submit a second, empty line. */
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
}

static void test_editor_incomplete_line_waits(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("vers");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
  feed("ion\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "version"), 0);
}

static void test_editor_backspace(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("helq\b\bp\n"); /* "helq" -> backspace twice -> "he" -> 'p' -> "hep" */
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "hep"), 0);
}

static void test_editor_backspace_on_empty_is_harmless(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("\b\b\bok\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ok"), 0);
}

static void test_editor_ctrl_c_abandons(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("garbage\003");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

static void test_editor_empty_line_does_not_dispatch(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("\n\n\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
}

/* A line longer than CMD_MAX_LEN stops accepting characters rather than
 * overrunning, and still submits what it holds.
 *
 * Fed with a poll every few bytes, which is what the running shell does (it
 * polls on a 10 ms tick). Feeding it all at once instead overflows the input
 * FIFO and the '\n' is among what gets dropped -- see the next test, which is
 * about that on purpose. */
static void test_editor_overlong_line_is_truncated(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  for (int i = 0; i < CMD_MAX_LEN + 32; i++) {
    shell_feed('a');
    if ((i % 16) == 15)
      TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
  }
  shell_feed('\n');
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  /* Truncated to what the line can hold, NUL-terminated, never overrun. */
  TEST_ASSERT_EQ((int)v_strlen(out), CMD_MAX_LEN - 1);
}

/* Overflowing the FIFO loses bytes, and that is the deliberate trade: the
 * producer is an interrupt, so it drops rather than waits. What must NOT happen
 * is corruption -- the shell stays usable and the next line is clean. */
static void test_editor_survives_fifo_overflow(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  for (int i = 0; i < SHELL_INPUT_FIFO_SIZE * 3; i++)
    shell_feed('z');
  shell_feed('\n'); /* dropped with the rest of the tail */
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);

  /* The partial line survives, as it would on any terminal where you typed a
     lot and never pressed Enter -- and because it is now full, further
     characters are refused rather than overrunning. ^C is the way out, and
     after it the shell is clean. */
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT(v_strcmp(out, "help") != 0); /* the z-line, not "help" */

  shell_feed(0x03);
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

/* --- history -------------------------------------------------------------- */

static void test_history_up_recalls(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("first\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  feed("second\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);

  /* Up twice reaches "first"; submitting it yields it again. */
  feed("\033[A\033[A\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "first"), 0);
}

static void test_history_down_returns_to_fresh_line(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("only\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  /* Up to "only", then down past it: the line is empty again, so Enter does
     not dispatch. */
  feed("\033[A\033[B\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
}

static void test_history_up_at_start_is_harmless(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("\033[A\033[A");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 0);
  feed("help\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

/* An unknown escape must be swallowed, not typed into the line. */
static void test_editor_unknown_escape_swallowed(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("a\033[Cb\n"); /* ESC [ C is right-arrow, which we do not implement */
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "ab"), 0);
}

static void test_history_dedupes_immediate_repeat(void) {
  shell_init();
  char out[CMD_MAX_LEN];
  feed("same\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  feed("same\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  /* One entry, so a second Up cannot go further back. */
  feed("\033[A\033[A\n");
  TEST_ASSERT_EQ(shell_poll(out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "same"), 0);
}

static const test_case_t shell_cases[] = {
    TEST_CASE(test_init_registers_builtins),
    TEST_CASE(test_register_then_find),
    TEST_CASE(test_register_rejects_overlong_name),
    TEST_CASE(test_find_long_command_name),
    TEST_CASE(test_register_full_returns_1),
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
