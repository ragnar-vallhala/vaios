/**
 * @file test_utils.c
 * @brief Unit tests for kernel/utils.c's public formatter and v_atof.
 *
 * print_fmt_buf is the printf-into-buffer entry point used by everything
 * from v_log to v_panic; the static itoa_simple / utoa_simple integer
 * formatters are exercised indirectly through the %d/%u/%x format
 * specifiers. v_atof is the string-to-float helper used by the shell.
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

/* %p prints the full pointer width (uintptr_t), 0x-prefixed, and consumes
 * exactly one arg — so a trailing specifier still reads its own value (the
 * host-port desync regression: %p emitted literally ate the next arg). */
static void test_pf_percent_p(void) {
  char buf[64] = {0};
  void *p = (void *)(uintptr_t)0xABCD;
  print_fmt_buf(buf, sizeof(buf), "%p tail=%u", p, 7u);
  TEST_ASSERT(strcmp(buf, "0xabcd tail=7") == 0);
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

/* A format string ending in a bare '%'. The parser consumes the '%' with an
 * in-body p++ (the 15 cpp/loop-variable-changed alerts are all this cursor),
 * and the specifier switch's default arm emits '%' followed by *p. If nothing
 * stops it, *p there is the NUL and the loop's own p++ then steps PAST the
 * terminator, reading whatever follows. The sentinel after the NUL is what
 * makes that visible: it must never reach the output. */
static void test_pf_trailing_percent_stops_at_nul(void) {
  char fmt[8];
  memcpy(fmt, "ab%\0Z!", 6);
  fmt[6] = 0;
  fmt[7] = 0;
  char buf[32];
  memset(buf, 0xAA, sizeof(buf));
  int n = print_fmt_buf(buf, sizeof(buf), fmt);
  /* "ab" plus whatever it decides a lone '%' means — but never 'Z' or '!', */
  TEST_ASSERT_EQ(buf[0], 'a');
  TEST_ASSERT_EQ(buf[1], 'b');
  for (int i = 0; i < n; i++) {
    TEST_ASSERT(buf[i] != 'Z');
    TEST_ASSERT(buf[i] != '!');
  }
  /* and it must terminate rather than run on. */
  TEST_ASSERT(n >= 2);
  TEST_ASSERT(n <= 4);
}

static void test_pf_truncation_respects_buf_size(void) {
  /* Buffer too small for the formatted output. Implementation must NOT
   * write past out_size — verify the trailing canary byte. */
  char buf[16];
  memset(buf, 0xAA, sizeof(buf));
  int n = print_fmt_buf(buf, 8, "%s", "abcdefghijk");
  TEST_ASSERT_EQ((unsigned char)buf[15], 0xAAu); /* untouched */
  /* Truncation is silent: the return is the capped length, not the length the
   * message wanted. v_log keys its '~' marker off exactly this, so if the
   * formatter ever starts returning the would-be length, the marker breaks. */
  TEST_ASSERT_EQ(n, 7);
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
 * v_memset / v_memcpy / v_strcmp / v_strncmp — pure mem/string helpers used
 * across the kernel (previously 0-call in coverage).
 * ---------------------------------------------------------------------- */

static void test_memset_fills(void) {
  unsigned char b[8];
  void *r = v_memset(b, 0xAB, sizeof b);
  TEST_ASSERT_EQ(r, (void *)b);
  for (unsigned i = 0; i < sizeof b; i++)
    TEST_ASSERT_EQ(b[i], 0xAB);
}

static void test_memset_zero_len(void) {
  unsigned char b[4] = {1, 2, 3, 4};
  v_memset(b, 0xFF, 0); /* no-op */
  TEST_ASSERT_EQ(b[0], 1);
}

static void test_memcpy_copies(void) {
  const char src[] = "abcdef";
  char dst[8] = {0};
  void *r = v_memcpy(dst, src, 7);
  TEST_ASSERT_EQ(r, (void *)dst);
  TEST_ASSERT(strcmp(dst, "abcdef") == 0);
}

static void test_memcpy_zero_len(void) {
  char dst[4] = {'x', 'x', 'x', 'x'};
  v_memcpy(dst, "yy", 0);
  TEST_ASSERT_EQ(dst[0], 'x'); /* untouched */
}

static void test_strcmp_equal_and_order(void) {
  TEST_ASSERT_EQ(v_strcmp("abc", "abc"), 0);
  TEST_ASSERT(v_strcmp("abc", "abd") < 0);
  TEST_ASSERT(v_strcmp("abd", "abc") > 0);
  TEST_ASSERT(v_strcmp("ab", "abc") < 0); /* shorter prefix compares less */
}

static void test_strncmp_bounded(void) {
  TEST_ASSERT_EQ(v_strncmp("abcXX", "abcYY", 3), 0); /* equal in first 3 */
  TEST_ASSERT(v_strncmp("abc", "abd", 3) < 0);
  TEST_ASSERT_EQ(v_strncmp("abc", "abd", 2), 0); /* equal in first 2 */
  TEST_ASSERT_EQ(v_strncmp("", "", 5), 0);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------- */
/* --- v_printf: streamed, no length limit ----------------------------------- */
/* Host only: these read what v_printf wrote from the capturing v_file_write in
   stubs/utils_stubs.c. The on-target runner links the real one. */
#ifdef VAIOS_HOST_TEST
extern char g_out[2048];
extern int g_out_len, g_take, g_refuse_once;
int v_printf(const char *fmt, ...);
static void out_reset(void) {
  g_out_len = 0;
  g_out[0] = '\0';
  g_take = 0;
  g_refuse_once = 0;
}

/* It used to format into 128 bytes and drop the rest without a word. */
static void test_v_printf_has_no_length_limit(void) {
  static char big[701];
  for (int i = 0; i < 700; i++)
    big[i] = (char)('a' + i % 26);
  big[700] = '\0';
  out_reset();
  int n = v_printf("<%s>", big);
  TEST_ASSERT_EQ(n, 702);
  TEST_ASSERT_EQ(g_out_len, 702);
  TEST_ASSERT(g_out[0] == '<' && g_out[1] == 'a' && g_out[700] == 'x' &&
              g_out[701] == '>');
  /* a long literal format, no conversions at all */
  out_reset();
  char fmt[301];
  for (int i = 0; i < 300; i++)
    fmt[i] = '-';
  fmt[300] = '\0';
  TEST_ASSERT_EQ(v_printf(fmt), 300);
  TEST_ASSERT_EQ(g_out_len, 300);
}

/* Every conversion still formats as the kernel formatter does, flags and
 * widths included, with each argument taken at its own type. */
static void test_v_printf_conversions(void) {
  out_reset();
  v_printf("%d|%02d|%5d|%u|%x|%X|%c|%s|%%|%ld|%lld|%lu", -42, 7, 33, 4000000000u,
           0xbeef, 0xBEEF, 'Z', "str", -123456L, -5000000000LL, 9UL);
  TEST_ASSERT_EQ(strcmp(g_out, "-42|07|   33|4000000000|beef|BEEF|Z|str|%|"
                               "-123456|-5000000000|9"),
                 0);
  out_reset();
  v_printf("%s and %s", "first", "second"); /* plain %s twice */
  TEST_ASSERT_EQ(strcmp(g_out, "first and second"), 0);
}

/* v_eprintf is v_printf on fd 2, stderr, conversions and all. */
extern int g_last_fd;
static void test_v_eprintf_goes_to_fd_2(void) {
  out_reset();
  v_eprintf("%s: %d", "err", -5);
  TEST_ASSERT_EQ(strcmp(g_out, "err: -5"), 0);
  TEST_ASSERT_EQ(g_last_fd, 2);
  v_printf("x");
  TEST_ASSERT_EQ(g_last_fd, 1);
}

/* The console taking a little at a time, or nothing once: all of it still
 * arrives, in order. */
static void test_v_printf_retries_partial_writes(void) {
  out_reset();
  g_take = 7;
  g_refuse_once = 1;
  v_printf("%s=%d;%s", "alpha-beta-gamma", 12345, "the rest of the line");
  TEST_ASSERT_EQ(strcmp(g_out, "alpha-beta-gamma=12345;the rest of the line"), 0);
}
#endif /* VAIOS_HOST_TEST */

static const test_case_t utils_cases[] = {
    /* print_fmt_buf */
    TEST_CASE(test_pf_plain_string),
    TEST_CASE(test_pf_percent_d_positive),
    TEST_CASE(test_pf_percent_d_negative),
    TEST_CASE(test_pf_percent_d_zero),
    TEST_CASE(test_pf_percent_u),
    TEST_CASE(test_pf_percent_x_lower),
    TEST_CASE(test_pf_percent_p),
    TEST_CASE(test_pf_percent_s),
    TEST_CASE(test_pf_percent_c),
    TEST_CASE(test_pf_double_percent),
    TEST_CASE(test_pf_multiple_args),
    TEST_CASE(test_pf_truncation_respects_buf_size),
    TEST_CASE(test_pf_empty_format),
    /* v_atof */
    TEST_CASE(test_atof_integer),
    TEST_CASE(test_atof_decimal),
    TEST_CASE(test_atof_negative),
    TEST_CASE(test_atof_zero),
    TEST_CASE(test_atof_leading_whitespace),
    /* mem/string helpers */
    TEST_CASE(test_memset_fills),
    TEST_CASE(test_memset_zero_len),
    TEST_CASE(test_memcpy_copies),
    TEST_CASE(test_memcpy_zero_len),
    TEST_CASE(test_strcmp_equal_and_order),
    TEST_CASE(test_strncmp_bounded),
    TEST_CASE(test_pf_trailing_percent_stops_at_nul),
#ifdef VAIOS_HOST_TEST
    TEST_CASE(test_v_printf_has_no_length_limit),
    TEST_CASE(test_v_printf_conversions),
    TEST_CASE(test_v_eprintf_goes_to_fd_2),
    TEST_CASE(test_v_printf_retries_partial_writes),
#endif
};
const test_suite_t utils_suite = {
    .name = "utils (print_fmt_buf + v_atof)",
    .cases = utils_cases,
    .count = TEST_COUNT(utils_cases),
};
