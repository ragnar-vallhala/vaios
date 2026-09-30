/**
 * @file test_shell.c
 * @brief Unit tests for kernel/shell.c's command registry + dispatch logic.
 *
 * The UART/ISR parts (shell_run's loop, _onRecieve reading chars) aren't
 * host-testable. But the registry (shell_register_command), the lookup/parse
 * (shell_find_command: whitespace trimming, first-token extraction), and the
 * built-in command handlers ARE — the handlers are reached through the registry
 * function pointers (they just log via the host v_log stub).
 *
 * shell_init() clears the registry and re-registers the built-ins, so each
 * test calls it first for a deterministic starting state.
 */
#include "framework.h"
#include "shell.h"
#include "utils.h" /* v_strcmp */

static void dummy_cb(void *args) { (void)args; }

/* --- registry + init ------------------------------------------------------- */

static void test_init_registers_builtins(void) {
  shell_init();
  TEST_ASSERT_NOT_NULL(shell_find_command("vaios"));
  TEST_ASSERT_NOT_NULL(shell_find_command("clear"));
  TEST_ASSERT_NOT_NULL(shell_find_command("ls"));
}

static void test_register_then_find(void) {
  shell_init();
  TEST_ASSERT_EQ(shell_register_command(dummy_cb, "mycmd"), 0);
  Command_t *c = shell_find_command("mycmd");
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQ((void *)c->callback, (void *)dummy_cb);
}

static void test_register_full_returns_1(void) {
  shell_init(); /* built-ins take some slots; fill the rest */
  int saw_full = 0;
  for (int i = 0; i < 64 && !saw_full; i++)
    if (shell_register_command(dummy_cb, "filler") == 1)
      saw_full = 1;
  TEST_ASSERT(saw_full);
}

/* --- lookup / parse -------------------------------------------------------- */

static void test_find_unknown_returns_null(void) {
  shell_init();
  TEST_ASSERT_NULL(shell_find_command("definitely_not_a_command"));
}

static void test_find_skips_leading_spaces(void) {
  shell_init();
  TEST_ASSERT_NOT_NULL(shell_find_command("   ls"));
}

static void test_find_resolves_first_token(void) {
  shell_init();
  Command_t *c = shell_find_command("ls -l /tmp");
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT(v_strcmp(c->command, "ls") == 0); /* resolved to "ls", args dropped */
}

static void test_find_empty_string(void) {
  shell_init();
  TEST_ASSERT_NULL(shell_find_command(""));
}

/* --- dispatch: invoking a resolved handler exercises its body -------------- */

static void test_dispatch_builtin_handlers(void) {
  shell_init();
  Command_t *ls = shell_find_command("ls");
  Command_t *v = shell_find_command("vaios");
  Command_t *cl = shell_find_command("clear");
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
  shell_init();
  Command_t *p = shell_find_command("perf");
  TEST_ASSERT_NOT_NULL(p);
  p->callback((void *)"perf show");  /* _perf_cmd_subarg -> "show" */
  p->callback((void *)"perf reset"); /* -> "reset" */
  p->callback((void *)"perf");       /* no-arg -> treated as show */
  p->callback((void *)"perf bogus"); /* unknown -> usage hint */
  TEST_ASSERT(1);
}
#endif

static const test_case_t shell_cases[] = {
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
const test_suite_t shell_suite = {
    .name = "shell (registry + dispatch)",
    .cases = shell_cases,
    .count = TEST_COUNT(shell_cases),
};
