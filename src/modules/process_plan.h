#ifndef ANT_MODULES_PROCESS_PLAN_H
#define ANT_MODULES_PROCESS_PLAN_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

typedef enum {
  ANT_PROCESS_STDERR_CAPTURE = 0,
  ANT_PROCESS_STDERR_TO_STDOUT,
} ant_process_stderr_mode_t;

typedef enum {
  ANT_PROCESS_REDIRECT_STDIN = 0,
  ANT_PROCESS_REDIRECT_STDOUT_TRUNCATE,
  ANT_PROCESS_REDIRECT_STDOUT_APPEND,
  ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT,
} ant_process_redirect_kind_t;

typedef struct {
  ant_process_redirect_kind_t kind;
  char *path;
} ant_process_plan_redirect_t;

typedef enum {
  ANT_PROCESS_PLAN_COMMAND_EXTERNAL = 0,
  ANT_PROCESS_PLAN_COMMAND_NATIVE,
} ant_process_plan_command_kind_t;

typedef struct {
  ant_process_plan_command_kind_t kind;
  union {
    struct {
      char **argv;
      size_t argc;
    } external;
    struct {
      char *stdout_data;
      size_t stdout_len;
      char *stderr_data;
      size_t stderr_len;
      int exit_code;
    } native;
  } as;
} ant_process_plan_command_t;

typedef struct {
  ant_process_plan_command_t *commands;
  size_t command_count;
  ant_process_plan_redirect_t *redirects;
  size_t redirect_count;
  char *cwd;
  ant_process_stderr_mode_t stderr_mode;
} ant_process_plan_t;

bool ant_process_plan_add_native_stage(
  ant_process_plan_t *plan,
  const char *stdout_data, size_t stdout_len,
  const char *stderr_data, size_t stderr_len,
  int exit_code
);

void ant_process_plan_init(ant_process_plan_t *plan);
void ant_process_plan_dispose(ant_process_plan_t *plan);

bool ant_process_plan_add_command(ant_process_plan_t *plan, const char *const *argv, size_t argc);
bool ant_process_plan_add_redirect(ant_process_plan_t *plan, ant_process_redirect_kind_t kind, const char *path);

ant_value_t ant_process_plan_rejected_result(ant_t *js, ant_value_t error);
ant_value_t ant_process_plan_submit(ant_t *js, ant_process_plan_t *plan);

#endif
