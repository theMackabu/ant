#include "process_stage.h"

#include <string.h>

static void process_stage_close(ant_process_stage_t *stage);

static void process_stage_closed(uv_handle_t *handle) {
  ant_process_stage_t *stage = handle ? handle->data : NULL;
  if (!stage) return;
  stage->closed = true;
  if (stage->close_cb) stage->close_cb(stage);
}

static void process_stage_exited(
  uv_process_t *handle, int64_t exit_status, int term_signal
) {
  ant_process_stage_t *stage = handle ? handle->data : NULL;
  if (!stage) return;
  if (stage->exit_cb)
    stage->exit_cb(stage, exit_status, term_signal);
  process_stage_close(stage);
}

void ant_process_stage_init(
  ant_process_stage_t *stage,
  void *owner,
  ant_process_stage_exit_cb exit_cb,
  ant_process_stage_close_cb close_cb
) {
  if (!stage) return;
  *stage = (ant_process_stage_t){
    .owner = owner,
    .exit_cb = exit_cb,
    .close_cb = close_cb,
  };
  stage->handle.data = stage;
}

int ant_process_stage_spawn(
  ant_process_stage_t *stage, const ant_process_spawn_spec_t *spec
) {
  if (!stage || !spec || !spec->file || !spec->args)
    return UV_EINVAL;

  uv_process_options_t options;
  memset(&options, 0, sizeof(options));
  options.exit_cb = process_stage_exited;
  options.file = spec->file;
  options.args = spec->args;
  options.env = spec->env;
  options.cwd = spec->cwd;
  options.flags = spec->flags;
  options.stdio = spec->stdio;
  options.stdio_count = spec->stdio_count;

  stage->handle.data = stage;
  int result = uv_spawn(
    spec->loop ? spec->loop : uv_default_loop(),
    &stage->handle,
    &options
  );
  stage->handle_open = true;
  if (result < 0) {
    process_stage_close(stage);
    return result;
  }
  stage->started = true;
  return 0;
}

static void process_stage_close(ant_process_stage_t *stage) {
  if (!stage || !stage->handle_open || stage->closed) return;
  uv_handle_t *handle = (uv_handle_t *)&stage->handle;
  if (!uv_is_closing(handle)) uv_close(handle, process_stage_closed);
}

int ant_process_stage_kill(ant_process_stage_t *stage, int signal) {
  if (!stage || !stage->started || stage->closed) return UV_ESRCH;
  return uv_process_kill(&stage->handle, signal);
}

void ant_process_stage_ref(ant_process_stage_t *stage) {
  if (!stage || !stage->handle_open || stage->closed) return;
  uv_handle_t *handle = (uv_handle_t *)&stage->handle;
  if (!uv_is_closing(handle)) uv_ref(handle);
}

void ant_process_stage_unref(ant_process_stage_t *stage) {
  if (!stage || !stage->handle_open || stage->closed) return;
  uv_handle_t *handle = (uv_handle_t *)&stage->handle;
  if (!uv_is_closing(handle)) uv_unref(handle);
}

uv_pid_t ant_process_stage_pid(const ant_process_stage_t *stage) {
  return stage && stage->started ? stage->handle.pid : 0;
}

void ant_process_stdio_ignore(uv_stdio_container_t *stdio) {
  if (!stdio) return;
  *stdio = (uv_stdio_container_t){ .flags = UV_IGNORE };
}

void ant_process_stdio_inherit_fd(uv_stdio_container_t *stdio, int fd) {
  if (!stdio) return;
  *stdio = (uv_stdio_container_t){ .flags = UV_INHERIT_FD };
  stdio->data.fd = fd;
}

void ant_process_stdio_create_pipe(
  uv_stdio_container_t *stdio, uv_stream_t *stream, bool child_reads
) {
  if (!stdio) return;
  *stdio = (uv_stdio_container_t){
    .flags = UV_CREATE_PIPE |
      (child_reads ? UV_READABLE_PIPE : UV_WRITABLE_PIPE),
  };
  stdio->data.stream = stream;
}
