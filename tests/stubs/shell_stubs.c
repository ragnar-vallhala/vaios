/* Externs for the vaios_shell_tests binary (VAIOS_DEVFS=1, VAIOS_SYSCALL_SVC=0,
 * so devfs and the shell run their bodies directly).
 *
 * The shell does its I/O through fds now, so this binary links the real
 * kernel/devfs.c: the tests exercise the actual v_file_read(0, ...) path rather
 * than a stand-in, and /dev/console's read returns 0 here because
 * port_hw_stub's try_read has nothing to offer. That is what the tests want --
 * bytes enter through shell_feed(), which is the same entry the console uses.
 *
 * Output is discarded. The tests assert on the LINE the editor returns, never on
 * what was printed, so a real formatter would only drag utils.c and the logging
 * path in for nothing.
 */
#include "task.h"
#include "vfile.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* A real synthetic task with a real fd table, not a NULL current_task: the
   shell reads fd 0 now, so v_file_read walks current_task->fds[] and the tests
   should go through that rather than round a fake. v_devfs_init registers
   /dev/console, v_fd_table_init pre-opens 0/1/2 onto it, and the console's read
   returns 0 bytes here because port_hw_stub's try_read has nothing to give --
   which is what the tests want: input arrives through shell_feed(). */
static TCB _shell_test_task;
TCB *current_task = &_shell_test_task;

void shell_test_env_init(void) {
  v_devfs_init();
  v_fd_table_init(&_shell_test_task);
}

int v_kmsg_read(char *out, uint32_t len) {
  (void)out;
  (void)len;
  return 0;
}

/* Counts prompts printed, which is the one piece of output a test needs: a
   CRLF terminal must get one prompt per line, not two. */
int g_stub_prompts;
char g_stub_last_prompt[80]; /* the last prompt, as it would print */
/* Everything printed, formatted by libc's vsnprintf -- the same result as the
   kernel formatter for what the shell uses (%d %s %02d %%) -- so a test can
   check what a command said. stub_out_reset() between checks. */
char g_stub_out[4096];
int g_stub_out_len;
void stub_out_reset(void) {
  g_stub_out_len = 0;
  g_stub_out[0] = '\0';
}
int v_printf(const char *fmt, ...) {
  va_list ap;
  if (strcmp(fmt, "vsh%s%s> ") == 0) { /* SHELL_PROMPT_FMT */
    va_start(ap, fmt);
    const char *a = va_arg(ap, const char *), *b = va_arg(ap, const char *);
    va_end(ap);
    snprintf(g_stub_last_prompt, sizeof g_stub_last_prompt, "vsh%s%s> ", a, b);
    g_stub_prompts++;
  }
  va_start(ap, fmt);
  int room = (int)sizeof g_stub_out - g_stub_out_len;
  int n = vsnprintf(g_stub_out + g_stub_out_len, (size_t)room, fmt, ap);
  va_end(ap);
  if (n > 0)
    g_stub_out_len += n < room ? n : room - 1;
  return n;
}

/* kernel/shell.c's perf command is compiled out here (VAIOS_MODULE_PERF=0), and
 * v_strcmp/v_strlen come from the utils stub alongside this. */

/* The three the shell leans on from the kernel's own helpers. Real string
   semantics matter here -- the registry compares names with v_strcmp and the
   tests assert on v_strlen -- so these are libc-backed rather than faked. */
uint32_t v_strlen(const char *s) {
  uint32_t n = 0;
  while (s[n])
    n++;
  return n;
}

int v_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* What ps and heap read. One task -- the synthetic one above -- and a heap
   with fixed numbers, so the commands run end to end here. */
int task_snapshot_list(TCB **out, int max) {
  if (max < 1)
    return 0;
  out[0] = &_shell_test_task;
  return 1;
}
const char *task_get_name(const TCB *t) {
  (void)t;
  return "shell";
}
uint32_t v_get_heap_size(void) { return 4096; }
uint32_t v_get_heap_allocation_count(void) { return 2; }
uint32_t v_get_heap_allocation_size(void) { return 100; }

/* The banner the shell prints (kernel/vaios.c, not linked here): one line, and
   the uptime it was asked for, so a test can see the shell passes one. */
int32_t g_stub_banner_uptime = -2;
void v_banner_print(void (*put)(const char *line), uint32_t cpu_hz,
                    int32_t uptime_s) {
  (void)cpu_hz;
  g_stub_banner_uptime = uptime_s;
  put("vaios test\r\n");
}
uint32_t v_get_ticks(void) { return 0; }

/* The shell formats numbers with it (ps, the banner); libc does the same. */
int print_fmt_buf(char *out, uint32_t out_size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out, out_size, fmt, ap);
  va_end(ap);
  return n;
}

/* A fake card: three directories that exist, and a handful of files held in
   memory, enough for the file commands to read, write, seek and remove. */
#include "vfs.h"
#include "ipc.h" /* VA_PASS */
static const char *const _dirs[] = {"0:/", "0:/LOGS", "0:/LOGS/A"};
char g_stub_mkdir_path[64];

typedef struct {
  char path[64];
  char data[700];
  int len, used;
} sfile_t;
static sfile_t _files[6];
static struct {
  int file, pos, used;
} _fds[4];

void stub_fs_reset(void) {
  memset(_files, 0, sizeof _files);
  memset(_fds, 0, sizeof _fds);
}
static sfile_t *ffind(const char *path) {
  for (int i = 0; i < 6; i++)
    if (_files[i].used && strcmp(_files[i].path, path) == 0)
      return &_files[i];
  return NULL;
}
void stub_fs_put(const char *path, const char *data, int len) {
  sfile_t *f = ffind(path);
  for (int i = 0; !f && i < 6; i++)
    if (!_files[i].used)
      f = &_files[i];
  if (!f)
    return; /* the fake card is full */
  strncpy(f->path, path, sizeof f->path - 1);
  memcpy(f->data, data, (size_t)len);
  f->len = len;
  f->used = 1;
}
const char *stub_fs_get(const char *path, int *len) {
  sfile_t *f = ffind(path);
  if (!f)
    return NULL;
  *len = f->len;
  return f->data;
}

int v_vfs_diropen(const char *path, uint32_t ticks) {
  (void)ticks;
  for (unsigned i = 0; i < sizeof _dirs / sizeof _dirs[0]; i++)
    if (strcmp(path, _dirs[i]) == 0)
      return 3;
  return -5; /* FR_NO_PATH */
}
int v_vfs_dirnext(int fd, vfs_dirent_t *ent, uint32_t ticks) {
  (void)fd;
  (void)ent;
  (void)ticks;
  return 0;
}
int v_vfs_open(const char *path, int flags, uint32_t ticks) {
  (void)ticks;
  sfile_t *f = ffind(path);
  if (!f && !(flags & VFS_O_CREAT))
    return -4; /* FR_NO_FILE */
  if (!f) {
    stub_fs_put(path, "", 0);
    f = ffind(path);
    if (!f)
      return -16; /* full */
  }
  if (flags & VFS_O_TRUNC)
    f->len = 0;
  for (int i = 0; i < 4; i++)
    if (!_fds[i].used) {
      _fds[i].used = 1;
      _fds[i].file = (int)(f - _files);
      _fds[i].pos = (flags & VFS_O_APPEND) ? f->len : 0;
      return 10 + i;
    }
  return -16;
}
int v_vfs_close(int fd, uint32_t ticks) {
  (void)ticks;
  if (fd >= 10 && fd < 14)
    _fds[fd - 10].used = 0;
  return 0;
}
int v_vfs_read(int fd, void *buf, uint32_t len, uint32_t ticks) {
  (void)ticks;
  sfile_t *f = &_files[_fds[fd - 10].file];
  int *pos = &_fds[fd - 10].pos;
  int n = f->len - *pos < (int)len ? f->len - *pos : (int)len;
  memcpy(buf, f->data + *pos, (size_t)n);
  *pos += n;
  return n;
}
int v_vfs_write(int fd, const void *buf, uint32_t len, uint32_t ticks) {
  (void)ticks;
  sfile_t *f = &_files[_fds[fd - 10].file];
  int *pos = &_fds[fd - 10].pos;
  memcpy(f->data + *pos, buf, len);
  *pos += (int)len;
  if (*pos > f->len)
    f->len = *pos;
  return (int)len;
}
long v_vfs_seek(int fd, long off, int whence, uint32_t ticks) {
  (void)whence;
  (void)ticks;
  _fds[fd - 10].pos = (int)off;
  return off;
}
long v_vfs_size(int fd, uint32_t ticks) {
  (void)ticks;
  return _files[_fds[fd - 10].file].len;
}
int v_vfs_remove(const char *path, uint32_t ticks) {
  (void)ticks;
  sfile_t *f = ffind(path);
  if (!f)
    return -4;
  f->used = 0;
  return 0;
}
int v_vfs_info(const char *path, vfs_stat_t *st, uint32_t ticks) {
  (void)ticks;
  memset(st, 0, sizeof *st);
  sfile_t *f = ffind(path);
  if (f) {
    st->exists = 1;
    st->size = (uint32_t)f->len;
    /* 2026-10-03 14:05: (46 << 9 | 10 << 5 | 3) << 16 | (14 << 11 | 5 << 5) */
    st->mtime = ((46u << 9 | 10u << 5 | 3u) << 16) | (14u << 11 | 5u << 5);
    return 0;
  }
  if (v_vfs_diropen(path, 0) > 0) {
    st->exists = st->is_dir = 1;
    return 0;
  }
  return -4;
}
int v_vfs_makedir(const char *path, uint32_t ticks) {
  (void)ticks;
  strncpy(g_stub_mkdir_path, path, sizeof g_stub_mkdir_path - 1);
  return 0;
}

/* kill: pid 5 is this shell's child, pid 2 is not. */
int v_task_kill(uint32_t id) { return id == 5 ? VA_PASS : id == 2 ? V_TASK_EPERM : V_TASK_EINVAL; }
