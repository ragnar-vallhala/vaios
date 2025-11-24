#ifndef NAVHAL
#warning "NAVHAL not found building for QEMU"
#define QEMU_BACKEND
#include "semihosting.h" // [TODO] QEMU impl missing for terminal
#include "qemu_irq.h"
#else
#ifndef CORTEX_M4
#define CORTEX_M4
#endif
#include "navhal.h"
#endif
#include "terminal.h"
#include "config.h"
#include <stddef.h>
#include "utils.h"
#include "vaios.h"
#include "task.h"

static char _term_history[CMD_BUFFER_SIZE][CMD_MAX_LEN]; // inclusive of escape seq
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
#define TERM_LOG(fmt, ...) \
    v_log(TERMINAL_LOG_LEVEL, "[TERM] " fmt, ##__VA_ARGS__)
#endif

static Command_t *_find_command(const char *cmd)
{
    for (int i = 0; i < _cmd_count_idx; i++)
    {
        if (v_strcmp(_commands[i].command, cmd) == 0)
            return &_commands[i];
    }
    return NULL;
}

static void vaios_self_check(void *args)
{
    TERM_LOG("VAIOS Version: %s", VERSION);
    TERM_LOG("Developer by NAVROBOTEC PVT. LTD.");
    TERM_LOG("Author: %s", AUTHOR);
    return;
}

static void clear_shell(void *args)
{
    TERM_LOG("\033[2J\033[H");
    return;
}

static void list_commands(void *args)
{

    for (int i = 0; i < MAX_CMD_NUMBER; i++)
    {
        if (_commands[i].callback)
        {
            TERM_LOG("%s", _commands[i].command);
        }
    }
}

static void _onRecieve(void)
{
    if (_initialized_terminal)
    {
        char c = 'a';
#ifdef NAVHAL
        c = uart2_read_char();
#elif defined(QEMU_BACKEND)
        c = sh_readc();
#endif

        _term_history[_current_command_count_idx][_current_command_buffer_idx++] = c;
        if (c == '\b')
        {
            if (_current_command_buffer_idx > 0)
                _current_command_buffer_idx--;
            _term_history[_current_command_count_idx][_current_command_buffer_idx] = '\0';
            if (_current_command_buffer_idx > 0)
                _current_command_buffer_idx--;
            _term_history[_current_command_count_idx][_current_command_buffer_idx] = '\0';
        }
        else if (c == 0x03)
        {
            _exit_requested = 1;
            _term_history[_current_command_count_idx][0] = '\0';
            _current_command_buffer_idx = 0;
            TERM_LOG("^C");
        }
        if (c == '\n')
        {
            _term_history[_current_command_count_idx][_current_command_buffer_idx] = '\0';
            const char *cmd = _term_history[_current_command_count_idx];
            if (v_strlen(cmd) >= ESCAPE_SEQ_LEN)
            {
                _term_history[_current_command_count_idx][_current_command_buffer_idx - ESCAPE_SEQ_LEN] = '\0';
                _command_exec_ready = 1;
            }

            _current_command_count_idx = (_current_command_count_idx + 1) % CMD_BUFFER_SIZE;
            _current_command_buffer_idx = 0;
        }
    }
    else
    {
#ifdef NAVHAL
        uart2_read_char();
#elif defined(QEMU_BACKEND)
        sh_readc();
#endif
    }
}
void terminal_init(void)
{
    for (int i = 0; i < CMD_BUFFER_SIZE; i++)
    {
        _term_history[i][0] = '\0';
    }
    for (int i = 0; i < MAX_CMD_NUMBER; i++)
    {
        _commands[i].callback = NULL;
    }
    _cmd_count_idx = 0;
    register_command(vaios_self_check, "vaios");
    register_command(clear_shell, "clear");
    register_command(list_commands, "ls");
#if defined(NAVHAL)
    hal_interrupt_attach_callback(USART2_IRQn, _onRecieve);
    hal_enable_interrupt(USART2_IRQn);
#elif defined(QEMU_BACKEND)
// [TODO] Implement for QEMU
// hal_interrupt_attach_callback(USART2_IRQn, _onRecieve);
// hal_enable_interrupt(USART2_IRQn);
#endif
    _initialized_terminal = 1;
}

int register_command(void (*callback)(void *), const char *command)
{
    if (_cmd_count_idx >= MAX_CMD_NUMBER)
        return 1; // or handle error
    _commands[_cmd_count_idx].callback = callback;
    _commands[_cmd_count_idx].command = command;
    _cmd_count_idx++;
    return 0;
}

void terminal_run(void *args)
{
    // Always run in task
    while (1)
    {
        if (_is_cmd_running && _exit_requested)
        {
            task_exit_request(_running_cmd_id);
            _is_cmd_running = 0;
            _exit_requested = 0;
        }
        if (_command_exec_ready)
        {
            const char *cmd_str = _term_history[(_current_command_count_idx - 1 + CMD_BUFFER_SIZE) % CMD_BUFFER_SIZE];
            Command_t *cmd = _find_command(cmd_str);
            if (cmd)
            {
                _running_cmd_id = task_create(cmd->callback, (void *)cmd_str, 1024, 0);
                if (_running_cmd_id != 0)
                    _is_cmd_running = 1;
            }
            else
                v_log(TERMINAL_LOG_LEVEL, "[TERM] %s command not found", cmd_str);
            _command_exec_ready = 0;
        }
        v_delay(10);
    }
}
