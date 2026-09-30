#ifndef VAIOS_SHELL_H
#define VAIOS_SHELL_H
#include "vaios_config.h"

typedef struct Command {
  void (*callback)(void *);
  const char *command;
} Command_t;
void shell_init(void);
int shell_register_command(void (*callback)(void *), const char *command);
void shell_run(void *args);
// Resolve the first whitespace-trimmed token of `cmd` to a registered command,
// or NULL if none. Public so the dispatch logic is unit-testable.
Command_t *shell_find_command(const char *cmd);

#endif // !VAIOS_SHELL_H