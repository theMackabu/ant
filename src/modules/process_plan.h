#ifndef ANT_MODULES_PROCESS_PLAN_H
#define ANT_MODULES_PROCESS_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

#define ANT_PROCESS_FD_LIMIT 10

typedef enum {
  ANT_PROCESS_FD_OPEN_READ = 0,
  ANT_PROCESS_FD_OPEN_WRITE,
  ANT_PROCESS_FD_OPEN_APPEND,
  ANT_PROCESS_FD_OPEN_READ_WRITE,
  ANT_PROCESS_FD_OPEN_EXCLUSIVE,
  ANT_PROCESS_FD_DUP,
  ANT_PROCESS_FD_CLOSE,
  ANT_PROCESS_FD_BYTES,
  ANT_PROCESS_FD_HOST,
  ANT_PROCESS_FD_CAPTURE,
  ANT_PROCESS_FD_ADOPT,
} ant_process_fd_kind_t;

typedef struct {
  ant_process_fd_kind_t kind;
  int fd;
  int source;
  char *path;
  char *data;
  size_t len;
  int opened;
  int error;
} ant_process_fd_op_t;

typedef struct {
  ant_process_fd_op_t *items;
  size_t count;
} ant_process_fd_ops_t;

typedef enum {
  ANT_PROCESS_PLAN_COMMAND_EXTERNAL = 0,
  ANT_PROCESS_PLAN_COMMAND_NATIVE,
} ant_process_plan_command_kind_t;

typedef struct {
  ant_process_plan_command_kind_t kind;
  union {
    struct {
      char *file;
      char **argv;
      size_t argc;
      char **env;
    } external;
    struct {
      char *stdout_data;
      size_t stdout_len;
      char *stderr_data;
      size_t stderr_len;
      int exit_code;
    } native;
  } as;
  ant_process_fd_ops_t fds;
} ant_process_plan_command_t;

typedef struct {
  ant_process_plan_command_t *commands;
  size_t command_count;
  ant_process_fd_ops_t base;
  size_t capture_count;
  char *cwd;
  ant_value_t spawned;
  int umask;
  int limit_count;
  int limit_resources[ANT_PROCESS_FD_LIMIT];
  double limit_values[ANT_PROCESS_FD_LIMIT];
  bool has_spawned;
} ant_process_plan_t;

int ant_process_umask_push(int mask);
void ant_process_umask_pop(int previous);

int ant_process_open_flags(ant_process_fd_kind_t kind);
int ant_process_open_sync(const char *path, int flags, int mask);

void ant_process_plan_init(ant_process_plan_t *plan);
void ant_process_plan_dispose(ant_process_plan_t *plan);
void ant_process_plan_release(ant_t *js, ant_process_plan_t *plan);

bool ant_process_plan_add_capture(ant_process_plan_t *plan, size_t *index);
bool ant_process_plan_take_external(ant_process_plan_t *plan, char *file, char **argv, size_t argc, char **env);

bool ant_process_plan_add_native_stage(
  ant_process_plan_t *plan,
  const char *stdout_data, size_t stdout_len,
  const char *stderr_data, size_t stderr_len,
  int exit_code
);

bool ant_process_plan_add_fd(
  ant_process_plan_t *plan, ant_process_plan_command_t *command,
  ant_process_fd_kind_t kind, int fd, int source,
  const char *path, const char *data, size_t len
);

ant_value_t ant_process_plan_rejected_result(ant_t *js, ant_value_t error);
ant_value_t ant_process_plan_submit(ant_t *js, ant_process_plan_t *plan);
ant_value_t ant_process_plan_spawned(ant_t *js, ant_process_plan_t *plan);

#endif
