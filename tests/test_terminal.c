/**
 * @file test_terminal.c
 * @brief Unit tests for kernel/terminal.c.
 *
 * Most of terminal.c is UART/ISR-driven (terminal_init wires up
 * USART2_IRQn, _onRecieve reads chars from sh_readc/uart2_read_char,
 * terminal_run is the dispatcher loop) — none of that is host-testable
 * without simulating an interactive UART. The narrow part that IS host-
 * testable is the public register_command(): append-only into a fixed
 * MAX_CMD_NUMBER table, returns 0 on success and 1 when full.
 *
 * Note: terminal.c's _commands array and _cmd_count_idx are file-static
 * with no reset hook, so these tests share state. They are written
 * cumulatively (each registration is counted) and the order of
 * terminal_suite's cases matters.
 */
#include "framework.h"
#include "terminal.h"

static void dummy_cb(void *args) { (void)args; }

/* register_command returns 0 when there is room in the table. */
static void test_register_command_succeeds_with_room(void) {
  /* MAX_CMD_NUMBER is small (config default 6). Use distinct names so the
   * later test_register_command_returns_1_when_full assertion is reachable
   * without us needing every prior slot. */
  TEST_ASSERT_EQ(register_command(dummy_cb, "cmd_a"), 0);
  TEST_ASSERT_EQ(register_command(dummy_cb, "cmd_b"), 0);
}

/* register_command returns 1 once the table fills up. We fill the table by
 * calling register_command repeatedly; once an iteration returns 1, the
 * table was full at that point. */
static void test_register_command_returns_1_when_full(void) {
  int saw_full = 0;
  /* Call far more than MAX_CMD_NUMBER times; one of them must return 1. */
  for (int i = 0; i < 64 && !saw_full; i++) {
    if (register_command(dummy_cb, "filler") == 1)
      saw_full = 1;
  }
  TEST_ASSERT(saw_full);
}

static const test_case_t terminal_cases[] = {
    TEST_CASE(test_register_command_succeeds_with_room),
    TEST_CASE(test_register_command_returns_1_when_full),
};
const test_suite_t terminal_suite = {
    .name = "terminal (register_command)",
    .cases = terminal_cases,
    .count = TEST_COUNT(terminal_cases),
};
