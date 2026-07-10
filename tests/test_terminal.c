/**
 * @file test_terminal.c
 * @brief Unit tests for kernel/terminal.c's command registry + dispatch logic.
 *
 * The UART/ISR parts (terminal_run's loop, _onRecieve reading chars) aren't
 * host-testable. But the registry (register_command), the lookup/parse
 * (terminal_find_command: whitespace trimming, first-token extraction), and the
 * built-in command handlers ARE — the handlers are reached through the registry
 * function pointers (they just log via the host v_log stub).
 *
 * terminal_init() clears the registry and re-registers the built-ins, so each
 * test calls it first for a deterministic starting state.
 */
#include "framework.h"
#include "terminal.h"
#include "utils.h" /* v_strcmp */

static void dummy_cb(void *args) { (void)args; }

/* --- registry + init ------------------------------------------------------- */

static void test_init_registers_builtins(void) {
  terminal_init();
  TEST_ASSERT_NOT_NULL(terminal_find_command("vaios"));
  TEST_ASSERT_NOT_NULL(terminal_find_command("clear"));
  TEST_ASSERT_NOT_NULL(terminal_find_command("ls"));
}

static void test_register_then_find(void) {
  terminal_init();
  TEST_ASSERT_EQ(register_command(dummy_cb, "mycmd"), 0);
  Command_t *c = terminal_find_command("mycmd");
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQ((void *)c->callback, (void *)dummy_cb);
}

static void test_register_full_returns_1(void) {
  terminal_init(); /* built-ins take some slots; fill the rest */
  int saw_full = 0;
  for (int i = 0; i < 64 && !saw_full; i++)
    if (register_command(dummy_cb, "filler") == 1)
      saw_full = 1;
  TEST_ASSERT(saw_full);
}

/* --- lookup / parse -------------------------------------------------------- */

static void test_find_unknown_returns_null(void) {
  terminal_init();
  TEST_ASSERT_NULL(terminal_find_command("definitely_not_a_command"));
}

static void test_find_skips_leading_spaces(void) {
  terminal_init();
  TEST_ASSERT_NOT_NULL(terminal_find_command("   ls"));
}

static void test_find_resolves_first_token(void) {
  terminal_init();
  Command_t *c = terminal_find_command("ls -l /tmp");
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT(v_strcmp(c->command, "ls") == 0); /* resolved to "ls", args dropped */
}

static void test_find_empty_string(void) {
  terminal_init();
  TEST_ASSERT_NULL(terminal_find_command(""));
}

/* --- dispatch: invoking a resolved handler exercises its body -------------- */

static void test_dispatch_builtin_handlers(void) {
  terminal_init();
  Command_t *ls = terminal_find_command("ls");
  Command_t *v = terminal_find_command("vaios");
  Command_t *cl = terminal_find_command("clear");
  TEST_ASSERT_NOT_NULL(ls);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_NOT_NULL(cl);
  ls->callback((void *)"ls");      /* list_commands */
  v->callback((void *)"vaios");    /* vaios_self_check */
  cl->callback((void *)"clear");   /* clear_shell */
  TEST_ASSERT(1);                  /* reached without crashing */
}

#if VAIOS_MODULE_PERF
static void test_dispatch_perf_subcommands(void) {
  terminal_init();
  Command_t *p = terminal_find_command("perf");
  TEST_ASSERT_NOT_NULL(p);
  p->callback((void *)"perf show");  /* _perf_cmd_subarg -> "show" */
  p->callback((void *)"perf reset"); /* -> "reset" */
  p->callback((void *)"perf");       /* no-arg -> treated as show */
  p->callback((void *)"perf bogus"); /* unknown -> usage hint */
  TEST_ASSERT(1);
}
#endif

static const test_case_t terminal_cases[] = {
    TEST_CASE(test_init_registers_builtins),
    TEST_CASE(test_register_then_find),
    TEST_CASE(test_register_full_returns_1),
    TEST_CASE(test_find_unknown_returns_null),
    TEST_CASE(test_find_skips_leading_spaces),
    TEST_CASE(test_find_resolves_first_token),
    TEST_CASE(test_find_empty_string),
    TEST_CASE(test_dispatch_builtin_handlers),
#if VAIOS_MODULE_PERF
    TEST_CASE(test_dispatch_perf_subcommands),
#endif
};
const test_suite_t terminal_suite = {
    .name = "terminal (registry + dispatch)",
    .cases = terminal_cases,
    .count = TEST_COUNT(terminal_cases),
};
