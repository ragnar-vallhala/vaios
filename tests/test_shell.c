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
#include <string.h>
#include "shell.h"
#include "utils.h" /* v_strcmp, v_strlen */
#include "task.h"  /* V_WAIT_FOREVER, MS_TO_TICKS */
#include "vfile.h" /* v_file_wait */

extern int g_stub_prompts;
extern int32_t g_stub_banner_uptime;

static shell_t g_sh;

static void sh_copy_test(char *dst, const char *src) {
  while ((*dst++ = *src++))
    ;
}
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
 * and so no helper -- gets it too: its working directory is the user's. */
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
  /* run directly (no MPU separation here, so no helper): the shell itself,
     so a relative path means what it means to the user */
  TEST_ASSERT(seen_sh == &g_sh);
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

/* The inspection built-ins exist, ps and heap are privileged (they read kernel
 * memory, so an unprivileged shell sends them to the helper), and both run. */
static void test_inspection_builtins(void) {
  shell_reset();
  TEST_ASSERT_EQ(shell_find_command(&g_sh, "ps")->flags, SHELL_CMD_PRIV);
  TEST_ASSERT_EQ(shell_find_command(&g_sh, "heap")->flags, SHELL_CMD_PRIV);
  char l1[] = "ps", l2[] = "heap";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l1), 0);
  TEST_ASSERT_EQ(shell_exec(&g_sh, l2), 0);
}

/* cd and pwd: a working directory per shell, paths resolved against it, "."
 * and ".." folded, never above the drive's root, and a cd to somewhere that
 * is not a directory refused with the old one kept. */
static void cd_to(const char *where) {
  char l[CMD_MAX_LEN] = "cd ";
  sh_copy_test(l + 3, where);
  shell_exec(&g_sh, l);
}
static void test_cd_and_paths(void) {
  shell_reset();
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/"), 0);
  cd_to("LOGS");
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/LOGS"), 0);
  cd_to("A");
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/LOGS/A"), 0);
  cd_to("../.././LOGS/./A/..");
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/LOGS"), 0);
  cd_to("../../../..");                  /* never above the root */
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/"), 0);
  cd_to("/mnt/0:LOGS");                  /* the mount prefix, drive-qualified */
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/LOGS"), 0);
  cd_to("nope");                         /* refused: stays where it was */
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/LOGS"), 0);
  char l[] = "cd";                       /* no argument: the card's root */
  shell_exec(&g_sh, l);
  TEST_ASSERT_EQ(v_strcmp(g_sh.cwd, "0:/"), 0);
}

/* The prompt is vsh's and carries where it is, without FatFs's "0:". */
extern char g_stub_last_prompt[80];
static void test_prompt_shows_cwd(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("\n");
  shell_poll(&g_sh, out, sizeof out);
  TEST_ASSERT_EQ(v_strcmp(g_stub_last_prompt, "vsh:/> "), 0);
  cd_to("LOGS");
  feed("\n");
  shell_poll(&g_sh, out, sizeof out);
  TEST_ASSERT_EQ(v_strcmp(g_stub_last_prompt, "vsh:/LOGS> "), 0);
}

/* mkdir resolves against the working directory too. */
extern char g_stub_mkdir_path[64];
static void test_mkdir_is_relative(void) {
  shell_reset();
  cd_to("LOGS");
  char l[] = "mkdir run1";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l), 0);
  TEST_ASSERT_EQ(v_strcmp(g_stub_mkdir_path, "0:/LOGS/run1"), 0);
  char l2[] = "mkdir /top";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l2), 0);
  TEST_ASSERT_EQ(v_strcmp(g_stub_mkdir_path, "0:/top"), 0);
}

/* --- the file commands, over the fake card (shell_stubs.c) ---------------- */
extern char g_stub_out[4096];
void stub_out_reset(void);
void stub_fs_reset(void);
void stub_fs_put(const char *path, const char *data, int len);
const char *stub_fs_get(const char *path, int *len);

static int run(const char *line) {
  char l[CMD_MAX_LEN];
  sh_copy_test(l, line);
  stub_out_reset();
  return shell_exec(&g_sh, l);
}
static void put(const char *path, const char *text) {
  stub_fs_put(path, text, (int)v_strlen(text));
}
static int out_is(const char *want) { return v_strcmp(g_stub_out, want) == 0; }

static void test_head_and_tail(void) {
  shell_reset();
  stub_fs_reset();
  put("0:/f.txt", "one\ntwo\nthree\nfour\n");
  TEST_ASSERT_EQ(run("head -n 2 f.txt"), 0);
  TEST_ASSERT(out_is("one\ntwo\n"));
  TEST_ASSERT_EQ(run("tail -n 2 f.txt"), 0);
  TEST_ASSERT(out_is("three\nfour\n"));
  TEST_ASSERT_EQ(run("tail f.txt"), 0); /* fewer lines than 10: all of them */
  TEST_ASSERT(out_is("one\ntwo\nthree\nfour\n"));
  put("0:/g.txt", "a\nb\nlast"); /* no final newline */
  TEST_ASSERT_EQ(run("tail -n 1 g.txt"), 0);
  TEST_ASSERT(out_is("last"));
  /* lines longer in total than one v_printf (128 bytes) come out whole */
  put("0:/long.txt", "a-line-that-is-sixty-characters-long-to-fill-up-space-xxxxx\n"
                      "b-line-that-is-sixty-characters-long-to-fill-up-space-xxxxx\n"
                      "c-line-that-is-sixty-characters-long-to-fill-up-space-xxxxx\n");
  TEST_ASSERT_EQ(run("tail -n 3 long.txt"), 0);
  TEST_ASSERT_EQ((int)v_strlen(g_stub_out), 180);
  /* lines longer in total than any window: 3 x 300 characters come out whole */
  {
    static char longf[903];
    for (int l = 0; l < 3; l++) {
      for (int k = 0; k < 300; k++)
        longf[l * 301 + k] = (char)('a' + l);
      longf[l * 301 + 300] = '\n';
    }
    stub_fs_put("0:/wide.txt", longf, 903);
    TEST_ASSERT_EQ(run("tail -n 2 wide.txt"), 0);
    TEST_ASSERT_EQ((int)v_strlen(g_stub_out), 602);
    TEST_ASSERT(g_stub_out[0] == 'b' && g_stub_out[301] == 'c');
    TEST_ASSERT_EQ(run("tail -n 0 wide.txt"), 0);
    TEST_ASSERT_EQ((int)v_strlen(g_stub_out), 0);
  }
  TEST_ASSERT_EQ(run("head -n x f.txt"), 1); /* not a number */
  TEST_ASSERT_EQ(run("head nope.txt"), 1);
}

static void test_write_cp_rm_touch(void) {
  int len;
  shell_reset();
  stub_fs_reset();
  TEST_ASSERT_EQ(run("write log.txt hello there"), 0);
  TEST_ASSERT_EQ(run("write log.txt again"), 0); /* appends */
  const char *d = stub_fs_get("0:/log.txt", &len);
  TEST_ASSERT(d && len == 18 && memcmp(d, "hello there\nagain\n", 18) == 0);
  TEST_ASSERT_EQ(run("cp log.txt copy.txt"), 0);
  d = stub_fs_get("0:/copy.txt", &len);
  TEST_ASSERT(d && len == 18 && memcmp(d, "hello there\nagain\n", 18) == 0);
  TEST_ASSERT_EQ(run("cp log.txt ./log.txt"), 1); /* the same file */
  TEST_ASSERT_EQ(run("touch empty.txt"), 0);
  TEST_ASSERT(stub_fs_get("0:/empty.txt", &len) != NULL && len == 0);
  TEST_ASSERT_EQ(run("touch log.txt"), 0); /* an existing file is left alone */
  stub_fs_get("0:/log.txt", &len);
  TEST_ASSERT_EQ(len, 18);
  TEST_ASSERT_EQ(run("rm copy.txt"), 0);
  TEST_ASSERT(stub_fs_get("0:/copy.txt", &len) == NULL);
  TEST_ASSERT_EQ(run("rm copy.txt"), 1);
}

static void test_stat_and_hexdump(void) {
  shell_reset();
  stub_fs_reset();
  put("0:/LOGS/h.bin", "ABC\001");
  cd_to("LOGS");
  TEST_ASSERT_EQ(run("stat h.bin"), 0);
  TEST_ASSERT(strstr(g_stub_out, "size      4 bytes") != NULL);
  TEST_ASSERT(strstr(g_stub_out, "modified  2026-10-03 14:05") != NULL);
  TEST_ASSERT(strstr(g_stub_out, "path      /LOGS/h.bin") != NULL);
  TEST_ASSERT_EQ(run("stat /LOGS"), 0);
  TEST_ASSERT(strstr(g_stub_out, "type      directory") != NULL);
  TEST_ASSERT_EQ(run("stat nope"), 1);
  TEST_ASSERT_EQ(run("hexdump h.bin"), 0);
  /* byte for byte what `hexdump -C` prints for the same bytes */
  TEST_ASSERT(out_is("00000000  41 42 43 01                                       |ABC.|\r\n"));
  TEST_ASSERT_EQ(run("hexdump h.bin 0x1 2"), 0);
  TEST_ASSERT(out_is("00000001  42 43                                             |BC|\r\n"));
}

/* kill reports which refusal the kernel gave. */
static void test_kill(void) {
  shell_reset();
  TEST_ASSERT_EQ(run("kill 5"), 0);
  TEST_ASSERT_EQ(run("kill 2"), 1);
  TEST_ASSERT(strstr(g_stub_out, "not started by this shell") != NULL);
  TEST_ASSERT_EQ(run("kill 9"), 1);
  TEST_ASSERT(strstr(g_stub_out, "no such task") != NULL);
  TEST_ASSERT_EQ(run("kill"), 1);
}

/* ^C: the console delivers it like any byte, and a long command stops. Bytes
 * typed AHEAD of it are dropped with it; a byte that is not ^C stays for the
 * editor. */
extern void (*g_stub_console_rx_sink)(const uint8_t *, uint16_t);
static void test_ctrl_c_interrupts(void) {
  shell_reset();
  stub_fs_reset();
  TEST_ASSERT_EQ(shell_interrupted(&g_sh), 0);
  g_stub_console_rx_sink((const uint8_t *)"x", 1);
  TEST_ASSERT_EQ(shell_interrupted(&g_sh), 0); /* kept for the editor */
  char out[CMD_MAX_LEN];
  feed("\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "x"), 0);

  static char big[650];
  for (int i = 0; i < 649; i++)
    big[i] = (char)('a' + i % 26);
  stub_fs_put("0:/big.txt", big, 649);
  g_stub_console_rx_sink((const uint8_t *)"ls\003", 3);
  TEST_ASSERT_EQ(run("cat big.txt"), SHELL_INTERRUPTED);
  TEST_ASSERT((int)v_strlen(g_stub_out) < 200); /* stopped after a chunk */
  TEST_ASSERT(strstr(g_stub_out, "exit") == NULL); /* not a failure */
  feed("\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0); /* "ls" was dropped */
}

static void test_loglevel(void) {
  shell_reset();
  TEST_ASSERT_EQ(run("loglevel"), 0);
  TEST_ASSERT(out_is("loglevel: info\r\n"));
  TEST_ASSERT_EQ(run("loglevel debug"), 0);
  TEST_ASSERT(out_is("loglevel: debug\r\n"));
  TEST_ASSERT_EQ(run("loglevel loud"), 1); /* not a level: unchanged */
  TEST_ASSERT_EQ(run("loglevel"), 0);
  TEST_ASSERT(out_is("loglevel: debug\r\n"));
  run("loglevel info");
}

/* reboot asks the port to reset (the stub counts instead of resetting). */
extern int g_stub_resets;
static void test_reboot(void) {
  shell_reset();
  g_stub_resets = 0;
  TEST_ASSERT_EQ(run("reboot"), 0);
  TEST_ASSERT_EQ(g_stub_resets, 1);
  TEST_ASSERT_EQ(shell_find_command(&g_sh, "reboot")->flags, SHELL_CMD_PRIV);
}

/* > and >>: the command's output lands in the file, with Unix line endings;
 * >> appends; the console has it back afterwards; a > with no file, or not
 * last, is refused before anything runs. */
static void test_redirect(void) {
  int len;
  shell_reset();
  stub_fs_reset();
  TEST_ASSERT_EQ(run("uptime > up.txt"), 0);
  const char *d = stub_fs_get("0:/up.txt", &len);
  TEST_ASSERT(d && len == 12 && memcmp(d, "up 0h 0m 0s\n", 12) == 0);
  TEST_ASSERT_EQ(run("uptime >> up.txt"), 0);
  d = stub_fs_get("0:/up.txt", &len);
  TEST_ASSERT_EQ(len, 24);
  TEST_ASSERT_EQ(run("uptime > up.txt"), 0); /* > truncates */
  stub_fs_get("0:/up.txt", &len);
  TEST_ASSERT_EQ(len, 12);
  TEST_ASSERT_EQ(run("uptime >"), 1);
  TEST_ASSERT_EQ(run("uptime > a.txt b"), 1);
  TEST_ASSERT(stub_fs_get("0:/a.txt", &len) == NULL); /* nothing ran */
  /* fd 1 is the console again: a plain command prints, nothing is caught */
  TEST_ASSERT_EQ(run("uptime"), 0);
  stub_fs_get("0:/up.txt", &len);
  TEST_ASSERT_EQ(len, 12);
}

/* More output than /dev/capture holds: the file says where it was cut. */
static void test_redirect_overflow(void) {
  int len;
  shell_reset();
  stub_fs_reset();
  static char big[650];
  for (int i = 0; i < 650; i++)
    big[i] = 'x';
  stub_fs_put("0:/b.bin", big, 650);
  TEST_ASSERT_EQ(run("hexdump b.bin 0 650 > dump.txt"), 0); /* ~3 KB */
  const char *d = stub_fs_get("0:/dump.txt", &len);
  TEST_ASSERT(d != NULL && len > 1900 && len <= 2048);
  int found = 0;
  for (int i = 0; i + 15 <= len && !found; i++)
    found = memcmp(d + i, "output cut here", 15) == 0;
  TEST_ASSERT(found);
}

/* Tab: what the line reads after typing `typed` and pressing it. */
static const char *after_tab(const char *typed) {
  static char out[CMD_MAX_LEN];
  char ignore[CMD_MAX_LEN];
  feed(typed);
  feed("\t");
  shell_poll(&g_sh, ignore, sizeof ignore);
  sh_copy_test(out, g_sh.line);
  feed("\003"); /* ^C: a fresh line for the next one */
  shell_poll(&g_sh, ignore, sizeof ignore);
  return out;
}

static void test_tab_completes_commands(void) {
  shell_reset();
  TEST_ASSERT_EQ(v_strcmp(after_tab("upt"), "uptime "), 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("hexd"), "hexdump "), 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("zz"), "zz"), 0); /* nothing matches */
  /* several, nothing more in common: listed, the line unchanged */
  stub_out_reset();
  TEST_ASSERT_EQ(v_strcmp(after_tab("h"), "h"), 0);
  TEST_ASSERT(strstr(g_stub_out, "help") && strstr(g_stub_out, "history") &&
              strstr(g_stub_out, "hexdump"));
}

static void test_tab_completes_paths(void) {
  shell_reset();
  stub_fs_reset();
  put("0:/LOGS/run1.csv", "1");
  put("0:/LOGS/run2.csv", "2");
  put("0:/abcdefghijkl", "x"); /* 12: as long as a listing can show */
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat LO"), "cat LOGS/"), 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat lo"), "cat loGS/"), 0); /* any case */
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat LOGS/r"), "cat LOGS/run"), 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat LOGS/run1"), "cat LOGS/run1.csv "),
                 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat /LO"), "cat /LOGS/"), 0);
  /* cd completes directories only: A/ is the one among two files */
  TEST_ASSERT_EQ(v_strcmp(after_tab("cd LOGS/"), "cd LOGS/A/"), 0);
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat LOGS/"), "cat LOGS/"), 0); /* three */
  /* may have been cut at 12: filled in, but left open */
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat abc"), "cat abcdefghijkl"), 0);
  cd_to("LOGS");
  TEST_ASSERT_EQ(v_strcmp(after_tab("cat ../LO"), "cat ../LOGS/"), 0);
}

static void test_i2c_scan(void) {
  shell_reset();
  TEST_ASSERT_EQ(run("i2c scan i2c1"), 1); /* not without -y */
  TEST_ASSERT(strstr(g_stub_out, "Add -y to go.\r\n") != NULL); /* whole */
  TEST_ASSERT_EQ(run("i2c scan nope -y"), 1);
  TEST_ASSERT_EQ(run("i2c scan i2c1 -y"), 0);
  /* the rows i2cdetect would print: 0x68 and 0x76 answer, reserved blank */
  TEST_ASSERT(strstr(g_stub_out, "\n60: -- -- -- -- -- -- -- -- 68 -- -- -- -- -- -- --\r") != NULL);
  TEST_ASSERT(strstr(g_stub_out, "\n70: -- -- -- -- -- -- 76 --                        \r") != NULL);
  TEST_ASSERT(strstr(g_stub_out, "\n00:                         -- -- -- -- -- -- -- --\r") != NULL);
}

static void test_mv_and_df(void) {
  int len;
  shell_reset();
  stub_fs_reset();
  put("0:/a.txt", "A");
  put("0:/b.txt", "B");
  TEST_ASSERT_EQ(run("mv a.txt c.txt"), 0);
  TEST_ASSERT(stub_fs_get("0:/a.txt", &len) == NULL);
  TEST_ASSERT(stub_fs_get("0:/c.txt", &len) != NULL);
  TEST_ASSERT_EQ(run("mv c.txt LOGS"), 0); /* into a directory, same name */
  TEST_ASSERT(stub_fs_get("0:/LOGS/c.txt", &len) != NULL);
  TEST_ASSERT_EQ(run("mv b.txt LOGS/c.txt"), 1); /* would replace */
  TEST_ASSERT(strstr(g_stub_out, "already exists") != NULL);
  TEST_ASSERT_EQ(run("mv nope.txt x"), 1);
  TEST_ASSERT_EQ(run("df"), 0);
  TEST_ASSERT(strstr(g_stub_out, "0:       30.0G    1.5G   28.5G    5%") != NULL);
}

static void test_uptime(void) {
  shell_reset();
  TEST_ASSERT_EQ(run("uptime"), 0);
  TEST_ASSERT(out_is("up 0h 0m 0s\r\n"));
}

/* ps lists every task, however many: the list is sized from the kernel's
 * count, not a fixed 16. The stub snapshot returns one task per slot asked for
 * up to g_stub_tasks. */
extern int g_stub_tasks;
extern uint32_t task_count;
static void test_ps_has_no_fixed_limit(void) {
  shell_reset();
  g_stub_tasks = 40;
  task_count = 40; /* what the kernel would count for 40 tasks */
  TEST_ASSERT_EQ(run("ps"), 0);
  int rows = 0;
  for (const char *q = g_stub_out; (q = strstr(q, "shell")) != NULL; q++)
    rows++;
  TEST_ASSERT_EQ(rows, 40);
  g_stub_tasks = 1;
  task_count = 1;
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

/* ^L redraws: one fresh prompt, and the line being typed survives it. */
static void test_editor_ctrl_l_redraws(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("he\014");
  g_stub_prompts = 0;
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  TEST_ASSERT_EQ(g_stub_prompts, 1);
  feed("lp\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "help"), 0);
}

/* ENQ is what the CDC console delivers when a host attaches: the shell answers
 * with its banner and one prompt, and a line already typed survives it. */
static void test_editor_enq_greets(void) {
  shell_reset();
  char out[CMD_MAX_LEN];
  feed("ve\005");
  g_stub_prompts = 0;
  g_stub_banner_uptime = -2;
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 0);
  TEST_ASSERT_EQ(g_stub_prompts, 1);
  TEST_ASSERT_EQ(g_stub_banner_uptime, 0); /* the shell knows its uptime */
  feed("rsion\n");
  TEST_ASSERT_EQ(shell_poll(&g_sh, out, sizeof out), 1);
  TEST_ASSERT_EQ(v_strcmp(out, "version"), 0);
  char l1[] = "version", l2[] = "dmesg";
  TEST_ASSERT_EQ(shell_exec(&g_sh, l1), 0);
  TEST_ASSERT_EQ(shell_exec(&g_sh, l2), 0); /* /dev/kmsg is empty here */
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
    TEST_CASE(test_inspection_builtins),
    TEST_CASE(test_cd_and_paths),
    TEST_CASE(test_mkdir_is_relative),
    TEST_CASE(test_prompt_shows_cwd),
    TEST_CASE(test_head_and_tail),
    TEST_CASE(test_write_cp_rm_touch),
    TEST_CASE(test_stat_and_hexdump),
    TEST_CASE(test_kill),
    TEST_CASE(test_ctrl_c_interrupts),
    TEST_CASE(test_uptime),
    TEST_CASE(test_mv_and_df),
    TEST_CASE(test_i2c_scan),
    TEST_CASE(test_tab_completes_commands),
    TEST_CASE(test_tab_completes_paths),
    TEST_CASE(test_redirect),
    TEST_CASE(test_redirect_overflow),
    TEST_CASE(test_loglevel),
    TEST_CASE(test_reboot),
    TEST_CASE(test_ps_has_no_fixed_limit),
    TEST_CASE(test_find_unknown_and_empty),
    TEST_CASE(test_register_rejects_null),
    TEST_CASE(test_tokenize_splits),
    TEST_CASE(test_tokenize_collapses_whitespace),
    TEST_CASE(test_tokenize_empty_and_bounds),
    TEST_CASE(test_editor_short_line_submits),
    TEST_CASE(test_editor_accepts_cr_and_crlf),
    TEST_CASE(test_editor_ctrl_l_redraws),
    TEST_CASE(test_editor_enq_greets),
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
