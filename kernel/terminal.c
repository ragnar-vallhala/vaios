#include <stdint.h>
#include "port.h" // v_port_hw_console_* — all hardware access goes through here
#include "perf.h"
#include "task.h"
#include "terminal.h"
#include "utils.h"
#include "vaios.h"
#include "vaios_config.h"
#include <stddef.h>

static char _term_history[CMD_BUFFER_SIZE]
                         [CMD_MAX_LEN]; // inclusive of escape seq
static Command_t _commands[MAX_CMD_NUMBER];
static int _cmd_count_idx = 0;
static int _current_command_count_idx = 0;  // default keep 0
static int _current_command_buffer_idx = 0; // default keep -1
static int _command_exec_ready = 0;
static int _initialized_terminal = 0;
static int _exit_requested = 0;
static int _running_cmd_id = 0;
static int _is_cmd_running = 0;

#ifndef TERM_LOG
#define TERM_LOG(fmt, ...)                                                     \
  v_log(TERMINAL_LOG_LEVEL, "[TERM] " fmt, ##__VA_ARGS__)
#endif

static Command_t *_find_command(const char *cmd) {
  char buf[CMD_BUFFER_SIZE];
  uint8_t pre_flag = 1;
  int i = 0;
  for (i = 0; i < CMD_BUFFER_SIZE; i++) {
    if (cmd[i] == ' ' && pre_flag)
      continue;
    pre_flag = 0;
    if (cmd[i] == '\0' || cmd[i] == ' ')
      break;
    buf[i] = cmd[i];
  }
  if (i == CMD_BUFFER_SIZE)
    return NULL;
  buf[i] = '\0';
  for (int i = 0; i < _cmd_count_idx; i++) {
    if (v_strcmp(_commands[i].command, buf) == 0)
      return &_commands[i];
  }
  return NULL;
}

static void vaios_self_check(void *args) {
  TERM_LOG("VAIOS Version: %s", VAIOS_VERSION);
  TERM_LOG("Developer by NAVROBOTEC PVT. LTD.");
  TERM_LOG("Author: %s", AUTHOR);
  return;
}

static void clear_shell(void *args) {
  TERM_LOG("\033[2J\033[H");
  return;
}

static void list_commands(void *args) {

  for (int i = 0; i < MAX_CMD_NUMBER; i++) {
    if (_commands[i].callback) {
      TERM_LOG("%s", _commands[i].command);
    }
  }
}

#if VAIOS_MODULE_PERF
/* `perf [show|reset]` — display or zero kernel perf counters.
 *
 * No-arg `perf` is treated as `perf show`. Unknown sub-commands fall
 * through to a usage hint. Phase 6b adds `perf save <path>` gated on
 * VAIOS_MODULE_VFS and a runtime mount check. */
static const char *_perf_cmd_subarg(const char *cmd_str) {
  if (!cmd_str) return NULL;
  /* Skip leading whitespace (terminal already trims, but be defensive). */
  while (*cmd_str == ' ') cmd_str++;
  /* Skip the command name itself. */
  while (*cmd_str && *cmd_str != ' ') cmd_str++;
  /* Skip space(s) before the sub-command. */
  while (*cmd_str == ' ') cmd_str++;
  return cmd_str;
}

static void perf_command(void *args) {
  const char *sub = _perf_cmd_subarg((const char *)args);
  if (!sub || *sub == '\0' || v_strcmp(sub, "show") == 0) {
    v_perf_dump();
    return;
  }
  if (v_strcmp(sub, "reset") == 0) {
    v_perf_reset();
    print_fmt("perf: counters reset\r\n");
    return;
  }
#if VAIOS_MODULE_VFS
  /* `perf save <path>` — write a CSV snapshot to the VFS. */
  if (v_strncmp(sub, "save", 4) == 0 && (sub[4] == ' ' || sub[4] == '\0')) {
    const char *path = sub + 4;
    while (*path == ' ') path++;
    if (*path == '\0') {
      print_fmt("usage: perf save <path>\r\n");
      return;
    }
    int rc = v_perf_dump_to_file(path);
    if (rc == 0) {
      print_fmt("perf: saved to %s\r\n", path);
    } else {
      print_fmt("perf: save failed (is VFS mounted?)\r\n");
    }
    return;
  }
  print_fmt("usage: perf [show|reset|save <path>]\r\n");
#else
  print_fmt("usage: perf [show|reset]\r\n");
#endif
}
#endif /* VAIOS_MODULE_PERF */

static void _onRecieve(void) {
  if (_initialized_terminal) {
    char c = v_port_hw_console_read_char();

    _term_history[_current_command_count_idx][_current_command_buffer_idx++] =
        c;
    if (c == '\b') {
      if (_current_command_buffer_idx > 0)
        _current_command_buffer_idx--;
      _term_history[_current_command_count_idx][_current_command_buffer_idx] =
          '\0';
      if (_current_command_buffer_idx > 0)
        _current_command_buffer_idx--;
      _term_history[_current_command_count_idx][_current_command_buffer_idx] =
          '\0';
    } else if (c == 0x03) {
      _exit_requested = 1;
      _term_history[_current_command_count_idx][0] = '\0';
      _current_command_buffer_idx = 0;
      TERM_LOG("^C");
    }
    if (c == '\n') {
      _term_history[_current_command_count_idx][_current_command_buffer_idx] =
          '\0';
      const char *cmd = _term_history[_current_command_count_idx];
      if (v_strlen(cmd) >= ESCAPE_SEQ_LEN) {
        _term_history[_current_command_count_idx]
                     [_current_command_buffer_idx - ESCAPE_SEQ_LEN] = '\0';
        _command_exec_ready = 1;
      }

      _current_command_count_idx =
          (_current_command_count_idx + 1) % CMD_BUFFER_SIZE;
      _current_command_buffer_idx = 0;
    }
  } else {
    /* Drain the incoming character even when the terminal isn't ready. */
    (void)v_port_hw_console_read_char();
  }
}

void terminal_init(void) {
  for (int i = 0; i < CMD_BUFFER_SIZE; i++) {
    _term_history[i][0] = '\0';
  }
  for (int i = 0; i < MAX_CMD_NUMBER; i++) {
    _commands[i].callback = NULL;
  }
  _cmd_count_idx = 0;
  register_command(vaios_self_check, "vaios");
  register_command(clear_shell, "clear");
  register_command(list_commands, "ls");
#if VAIOS_MODULE_PERF
  register_command(perf_command, "perf");
#endif
  v_port_hw_console_rx_irq_init(_onRecieve);
  _initialized_terminal = 1;
}

int register_command(void (*callback)(void *), const char *command) {
  if (_cmd_count_idx >= MAX_CMD_NUMBER)
    return 1; // or handle error
  _commands[_cmd_count_idx].callback = callback;
  _commands[_cmd_count_idx].command = command;
  _cmd_count_idx++;
  return 0;
}

void terminal_run(void *args) {
  // Always run in task
  while (1) {
    if (_exit_requested) {
      if (_is_cmd_running)
        task_exit_request(_running_cmd_id);
      _is_cmd_running = 0;
      _exit_requested = 0;
    }
    if (_command_exec_ready) {
      const char *cmd_str =
          _term_history[(_current_command_count_idx - 1 + CMD_BUFFER_SIZE) %
                        CMD_BUFFER_SIZE];
      Command_t *cmd = _find_command(cmd_str);
      if (cmd) {
        _running_cmd_id = task_create(cmd->callback, (void *)cmd_str, 1024, 0);
        if (_running_cmd_id != 0)
          _is_cmd_running = 1;
      } else
        v_log(TERMINAL_LOG_LEVEL, "[TERM] %s command not found", cmd_str);
      _command_exec_ready = 0;
    }
    v_delay(10);
  }
}
