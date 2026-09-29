#ifndef VAIOS_TERMINAL_H
#define VAIOS_TERMINAL_H
#include "vaios_config.h"

typedef struct Command {
  void (*callback)(void *);
  const char *command;
} Command_t;
void terminal_init(void);
int register_command(void (*callback)(void *), const char *command);
void terminal_run(void *args);
// Resolve the first whitespace-trimmed token of `cmd` to a registered command,
// or NULL if none. Public so the dispatch logic is unit-testable.
Command_t *terminal_find_command(const char *cmd);

#endif // !VAIOS_TERMINAL_H