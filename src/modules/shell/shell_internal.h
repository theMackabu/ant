#ifndef ANT_MODULES_SHELL_INTERNAL_H
#define ANT_MODULES_SHELL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

typedef enum {
  SH_REDIR_STDIN = 0,
  SH_REDIR_STDOUT,
  SH_REDIR_STDOUT_APPEND,
  SH_REDIR_STDERR_TO_STDOUT,
} sh_redir_kind_t;

ant_value_t sh_runtime_begin(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_arg(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_command(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_redirect(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_submit(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_finish(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_context(ant_t *js, bool needs_accumulator);

#endif
