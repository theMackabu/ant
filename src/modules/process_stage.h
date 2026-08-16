#ifndef ANT_MODULES_PROCESS_STAGE_H
#define ANT_MODULES_PROCESS_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <uv.h>

typedef struct ant_process_stage ant_process_stage_t;
typedef void (*ant_process_stage_close_cb)(ant_process_stage_t *stage);

typedef void (*ant_process_stage_exit_cb)(
  ant_process_stage_t *stage,
  int64_t exit_status, 
  int term_signal
);

typedef struct {
  uv_loop_t *loop;
  const char *file;
  char **args;
  char **env;
  const char *cwd;
  unsigned int flags;
  uv_stdio_container_t *stdio;
  int stdio_count;
} ant_process_spawn_spec_t;

struct ant_process_stage {
  uv_process_t handle;
  void *owner;
  ant_process_stage_exit_cb exit_cb;
  ant_process_stage_close_cb close_cb;
  bool handle_open;
  bool started;
  bool closed;
};

void ant_process_stage_init(
  ant_process_stage_t *stage,
  void *owner,
  ant_process_stage_exit_cb exit_cb,
  ant_process_stage_close_cb close_cb
);

int ant_process_stage_spawn(ant_process_stage_t *stage, const ant_process_spawn_spec_t *spec);
int ant_process_stage_kill(ant_process_stage_t *stage, int signal);

void ant_process_stage_ref(ant_process_stage_t *stage);
void ant_process_stage_unref(ant_process_stage_t *stage);
uv_pid_t ant_process_stage_pid(const ant_process_stage_t *stage);

void ant_process_stdio_ignore(uv_stdio_container_t *stdio);
void ant_process_stdio_inherit_fd(uv_stdio_container_t *stdio, int fd);
void ant_process_stdio_create_pipe(uv_stdio_container_t *stdio, uv_stream_t *stream, bool child_reads);

#endif
