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
  /* Through fd 1 as the real one goes, so a redirect (fd 1 -> /dev/capture)
     catches it; and into g_stub_out, for the tests that read the output. */
  char tmp[1024]; /* the real v_printf streams; nothing a test prints is longer */
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof tmp - 1)
    n = (int)sizeof tmp - 1;
  if (n > 0) {
    v_file_write(1, tmp, (uint32_t)n);
    int room = (int)sizeof g_stub_out - g_stub_out_len - 1;
    int k = n < room ? n : room;
    memcpy(g_stub_out + g_stub_out_len, tmp, (size_t)(k > 0 ? k : 0));
    g_stub_out_len += k > 0 ? k : 0;
    g_stub_out[g_stub_out_len] = '\0';
  }
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
uint32_t task_count = 1;
int g_stub_tasks = 1; /* how many tasks the snapshot reports */
#include <stdlib.h>
void *v_malloc(size_t n) { return malloc(n); }
void v_free(void *p) { free(p); }
int task_snapshot_list(TCB **out, int max) {
  int n = 0;
  for (; n < g_stub_tasks && n < max; n++)
    out[n] = &_shell_test_task;
  return n;
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
  char data[4096];
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

/* A directory listing: the fake card's subdirectories and files directly in
   `path`, in that order. One open listing at a time is all the shell needs. */
static char _ls_dir[64];
static int _ls_pos;
int v_vfs_diropen(const char *path, uint32_t ticks) {
  (void)ticks;
  for (unsigned i = 0; i < sizeof _dirs / sizeof _dirs[0]; i++)
    if (strcmp(path, _dirs[i]) == 0) {
      strncpy(_ls_dir, path, sizeof _ls_dir - 1);
      _ls_pos = 0;
      return 3;
    }
  return -5; /* FR_NO_PATH */
}
/* `full` directly inside `dir`: the rest after "dir/" (or "0:/") has no '/'. */
static const char *child_of(const char *dir, const char *full) {
  size_t n = strlen(dir);
  if (strncmp(full, dir, n) != 0)
    return NULL;
  const char *rest = full + n;
  if (dir[n - 1] != '/') {
    if (*rest != '/')
      return NULL;
    rest++;
  }
  return (*rest && !strchr(rest, '/')) ? rest : NULL;
}
int v_vfs_dirnext(int fd, vfs_dirent_t *ent, uint32_t ticks) {
  (void)fd;
  (void)ticks;
  int k = 0;
  for (unsigned i = 0; i < sizeof _dirs / sizeof _dirs[0]; i++) {
    const char *c = child_of(_ls_dir, _dirs[i]);
    if (c && k++ == _ls_pos) {
      memset(ent, 0, sizeof *ent);
      strncpy(ent->name, c, VFS_NAME_MAX - 1);
      ent->is_dir = 1;
      _ls_pos++;
      return 1;
    }
  }
  for (int i = 0; i < 6; i++) {
    const char *c = _files[i].used ? child_of(_ls_dir, _files[i].path) : NULL;
    if (c && k++ == _ls_pos) {
      memset(ent, 0, sizeof *ent);
      strncpy(ent->name, c, VFS_NAME_MAX - 1);
      ent->size = (uint32_t)_files[i].len;
      _ls_pos++;
      return 1;
    }
  }
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
  if (*pos + (int)len > (int)sizeof f->data)
    len = (uint32_t)((int)sizeof f->data - *pos); /* the fake card is small */
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

/* loglevel and reboot: the kernel side they touch. */
static int _level = 2; /* LOG_INFO */
void v_log_set_level(int level) {
  if (level >= 0 && level <= 5)
    _level = level;
}
int v_log_get_level(void) { return _level; }
void v_log_drain(void) {}
void task_delay(uint32_t ticks) { (void)ticks; }

/* i2c scan: one bus, "i2c1", where 0x68 and 0x76 answer. */
#include "periph_bus.h"
int v_pbus_open(const char *name) { return strcmp(name, "i2c1") == 0 ? 6 : -22; }
int v_pbus_xfer(int fd, const v_pbus_xfer_t *x, uint8_t prio, uint32_t ticks) {
  (void)fd;
  (void)prio;
  (void)ticks;
  return (x->addr == 0x68 || x->addr == 0x76) ? 0 : -5;
}

/* mv / df on the fake card: a rename moves the entry; the card is 30 GiB with
   1.5 GiB in use. */
int v_vfs_rename(const char *from, const char *to, uint32_t ticks) {
  (void)ticks;
  sfile_t *f = ffind(from);
  if (!f)
    return -4; /* FR_NO_FILE */
  if (ffind(to))
    return -8; /* FR_EXIST */
  strncpy(f->path, to, sizeof f->path - 1);
  return 0;
}
int v_vfs_space(const char *drive, vfs_space_t *out, uint32_t ticks) {
  (void)ticks;
  if (strcmp(drive, "0:") != 0)
    return -11; /* FR_INVALID_DRIVE */
  out->total_kib = 30u * 1024u * 1024u;
  out->free_kib = out->total_kib - 1536u * 1024u;
  return 0;
}
