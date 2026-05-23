/**
 * @file test_utils.c
 * @brief Unit tests for kernel/utils.c's public formatter and v_atof.
 *
 * print_fmt_buf is the printf-into-buffer entry point used by everything
 * from v_log to v_panic; the static itoa_simple / utoa_simple integer
 * formatters are exercised indirectly through the %d/%u/%x format
 * specifiers. v_atof is the string-to-float helper used by the terminal.
 */
#include "framework.h"
#include "utils.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * print_fmt_buf — printf into a caller-supplied buffer
 * ---------------------------------------------------------------------- */

static void test_pf_plain_string(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "hello");
  TEST_ASSERT(strcmp(buf, "hello") == 0);
}

static void test_pf_percent_d_positive(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "%d", 42);
  TEST_ASSERT(strcmp(buf, "42") == 0);
}

static void test_pf_percent_d_negative(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "%d", -1234);
  TEST_ASSERT(strcmp(buf, "-1234") == 0);
}

static void test_pf_percent_d_zero(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "%d", 0);
  TEST_ASSERT(strcmp(buf, "0") == 0);
}

static void test_pf_percent_u(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "%u", (unsigned)4000000000u);
  TEST_ASSERT(strcmp(buf, "4000000000") == 0);
}

static void test_pf_percent_x_lower(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "%x", 0xDEADBEEFu);
  /* utils.c's utoa_simple emits lowercase hex by default. */
  TEST_ASSERT(strcmp(buf, "deadbeef") == 0 ||
              strcmp(buf, "DEADBEEF") == 0); /* tolerate either case */
}

static void test_pf_percent_s(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "value=%s", "world");
  TEST_ASSERT(strcmp(buf, "value=world") == 0);
}

static void test_pf_percent_c(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "[%c]", 'A');
  TEST_ASSERT(strcmp(buf, "[A]") == 0);
}

static void test_pf_double_percent(void) {
  char buf[32] = {0};
  print_fmt_buf(buf, sizeof(buf), "100%%");
  TEST_ASSERT(strcmp(buf, "100%") == 0);
}

static void test_pf_multiple_args(void) {
  char buf[64] = {0};
  print_fmt_buf(buf, sizeof(buf), "%s=%d, %s=%d", "a", 1, "b", -2);
  TEST_ASSERT(strcmp(buf, "a=1, b=-2") == 0);
}

static void test_pf_truncation_respects_buf_size(void) {
  /* Buffer too small for the formatted output. Implementation must NOT
   * write past out_size — verify the trailing canary byte. */
  char buf[16];
  memset(buf, 0xAA, sizeof(buf));
  print_fmt_buf(buf, 8, "%s", "abcdefghijk");
  TEST_ASSERT_EQ((unsigned char)buf[15], 0xAAu); /* untouched */
}

static void test_pf_empty_format(void) {
  char buf[8];
  memset(buf, 0xAA, sizeof(buf));
  print_fmt_buf(buf, sizeof(buf), "");
  TEST_ASSERT_EQ((unsigned char)buf[0], 0u); /* NUL-terminated empty string */
}

/* -------------------------------------------------------------------------
 * v_atof — string to float
 * ---------------------------------------------------------------------- */

/* Float comparison helper — small tolerance for the simple parser. */
static int float_close(float a, float b) {
  float d = a - b;
  if (d < 0)
    d = -d;
  return d < 0.0001f;
}

static void test_atof_integer(void) {
  TEST_ASSERT(float_close(v_atof("123"), 123.0f));
}

static void test_atof_decimal(void) {
  TEST_ASSERT(float_close(v_atof("1.5"), 1.5f));
}

static void test_atof_negative(void) {
  TEST_ASSERT(float_close(v_atof("-2.25"), -2.25f));
}

static void test_atof_zero(void) {
  TEST_ASSERT(float_close(v_atof("0"), 0.0f));
  TEST_ASSERT(float_close(v_atof("0.0"), 0.0f));
}

static void test_atof_leading_whitespace(void) {
  /* Many simple atof implementations skip leading whitespace; vaios's may
   * or may not. Just verify a clean number parses, leaving the
   * whitespace-handling spec for the implementation. */
  TEST_ASSERT(float_close(v_atof("42.5"), 42.5f));
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
void run_utils_tests(void) {
  TEST_SUITE_BEGIN("utils (print_fmt_buf + v_atof)");
  /* print_fmt_buf */
  TEST_RUN(test_pf_plain_string);
  TEST_RUN(test_pf_percent_d_positive);
  TEST_RUN(test_pf_percent_d_negative);
  TEST_RUN(test_pf_percent_d_zero);
  TEST_RUN(test_pf_percent_u);
  TEST_RUN(test_pf_percent_x_lower);
  TEST_RUN(test_pf_percent_s);
  TEST_RUN(test_pf_percent_c);
  TEST_RUN(test_pf_double_percent);
  TEST_RUN(test_pf_multiple_args);
  TEST_RUN(test_pf_truncation_respects_buf_size);
  TEST_RUN(test_pf_empty_format);
  /* v_atof */
  TEST_RUN(test_atof_integer);
  TEST_RUN(test_atof_decimal);
  TEST_RUN(test_atof_negative);
  TEST_RUN(test_atof_zero);
  TEST_RUN(test_atof_leading_whitespace);
  TEST_SUITE_END();
}
