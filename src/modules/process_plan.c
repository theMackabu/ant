#include "process_plan.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#include <uv.h>

#include "ant.h"
#include "errors.h"
#include "gc/objects.h"
#include "internal.h"
#include "modules/process.h"
#include "process_stage.h"

typedef struct ant_process_run ant_process_run_t;

typedef struct {
  ant_process_stage_t process;
  ant_process_run_t *run;
  size_t index;
} ant_process_plan_stage_t;

typedef struct {
  uv_pipe_t handle;
  ant_process_run_t *run;
  bool stdout_stream;
  bool initialized;
  bool closed;
} ant_process_capture_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} ant_process_bytes_t;

struct ant_process_run {
  ant_t *js;
  ant_value_t promise;
  ant_process_plan_t plan;
  ant_process_plan_stage_t *stages;
  int *pipeline_fds;
  size_t pipeline_count;
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
  int capture_stdout_fds[2];
  int capture_stderr_fds[2];
  uv_fs_t redirect_open;
  size_t redirect_index;
  int open_error;
  int final_exit_code;
  int final_signal;
  size_t remaining_processes;
  ant_process_capture_t stdout_capture;
  ant_process_capture_t stderr_capture;
  ant_process_bytes_t stdout_bytes;
  ant_process_bytes_t stderr_bytes;
  bool spawned;
  bool settled;
};

static void process_run_try_finish(ant_process_run_t *run);
static void process_prepare_next_redirect(ant_process_run_t *run);

ant_value_t ant_process_plan_rejected_result(
  ant_t *js, ant_value_t error
) {
  error = js_take_thrown(js, error);
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, error);
  return promise;
}

void ant_process_plan_init(ant_process_plan_t *plan) {
  if (plan) *plan = (ant_process_plan_t){0};
}

void ant_process_plan_dispose(ant_process_plan_t *plan) {
  if (!plan) return;
  for (size_t i = 0; i < plan->command_count; i++) {
    for (size_t j = 0; j < plan->commands[i].argc; j++)
      free(plan->commands[i].argv[j]);
    free(plan->commands[i].argv);
  }
  free(plan->commands);
  for (size_t i = 0; i < plan->redirect_count; i++)
    free(plan->redirects[i].path);
  free(plan->redirects);
  free(plan->cwd);
  *plan = (ant_process_plan_t){0};
}

bool ant_process_plan_add_redirect(
  ant_process_plan_t *plan, ant_process_redirect_kind_t kind, const char *path
) {
  if (!plan || kind > ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT) return false;
  bool needs_path = kind != ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT;
  if (needs_path && !path) return false;
  if (plan->redirect_count == SIZE_MAX / sizeof(*plan->redirects)) return false;
  size_t next_count = plan->redirect_count + 1;
  ant_process_plan_redirect_t *redirects = realloc(
    plan->redirects, next_count * sizeof(*redirects)
  );
  if (!redirects) return false;
  plan->redirects = redirects;
  ant_process_plan_redirect_t *redirect = &redirects[plan->redirect_count];
  *redirect = (ant_process_plan_redirect_t){ .kind = kind };
  plan->redirect_count = next_count;
  if (needs_path && !(redirect->path = strdup(path))) return false;
  return true;
}

bool ant_process_plan_add_command(
  ant_process_plan_t *plan, const char *const *argv, size_t argc
) {
  if (!plan || !argv || argc == 0 || !argv[0] || argv[0][0] == '\0') return false;
  if (plan->command_count == SIZE_MAX / sizeof(*plan->commands)) return false;
  size_t next_count = plan->command_count + 1;
  ant_process_plan_command_t *commands = realloc(
    plan->commands, next_count * sizeof(*commands)
  );
  if (!commands) return false;
  plan->commands = commands;
  ant_process_plan_command_t *command = &commands[plan->command_count];
  *command = (ant_process_plan_command_t){0};
  command->argv = calloc(argc + 1, sizeof(*command->argv));
  if (!command->argv) return false;
  command->argc = argc;
  plan->command_count = next_count;
  for (size_t i = 0; i < argc; i++) {
    command->argv[i] = strdup(argv[i]);
    if (!command->argv[i]) return false;
  }
  return true;
}

static bool process_bytes_append(
  ant_process_bytes_t *bytes, const char *data, size_t len
) {
  if (len > SIZE_MAX - bytes->len - 1) return false;
  size_t need = bytes->len + len + 1;
  if (need > bytes->cap) {
    size_t next = bytes->cap ? bytes->cap * 2 : 4096;
    while (next < need) {
      if (next > SIZE_MAX / 2) return false;
      next *= 2;
    }
    char *grown = realloc(bytes->data, next);
    if (!grown) return false;
    bytes->data = grown;
    bytes->cap = next;
  }
  if (len) memcpy(bytes->data + bytes->len, data, len);
  bytes->len += len;
  bytes->data[bytes->len] = '\0';
  return true;
}

static void process_append_error(
  ant_process_run_t *run, const char *file, int error
) {
  char message[512];
  int len = snprintf(
    message, sizeof(message), "ant:shell: %s: %s: %s\n",
    file ? file : "process", uv_err_name(error), uv_strerror(error)
  );
  if (len > 0) (void)process_bytes_append(
    &run->stderr_bytes, message,
    (size_t)len < sizeof(message) ? (size_t)len : sizeof(message) - 1
  );
}

static void process_close_fd(int *fd) {
  if (!fd || *fd < 0) return;
#ifdef _WIN32
  _close(*fd);
#else
  close(*fd);
#endif
  *fd = -1;
}

static void process_close_pipeline_fds(ant_process_run_t *run) {
  for (size_t i = 0; i < run->pipeline_count * 2; i++)
    process_close_fd(&run->pipeline_fds[i]);
  process_close_fd(&run->capture_stdout_fds[1]);
  process_close_fd(&run->capture_stderr_fds[1]);
  process_close_fd(&run->stdin_fd);
  process_close_fd(&run->stdout_fd);
  process_close_fd(&run->stderr_fd);
}

static void process_capture_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  (void)handle;
  size_t size = suggested > 65536 ? 65536 : suggested;
  if (size == 0) size = 4096;
  buf->base = malloc(size);
  buf->len = buf->base ? size : 0;
}

static void process_capture_closed(uv_handle_t *handle) {
  ant_process_capture_t *capture = handle->data;
  if (!capture) return;
  capture->closed = true;
  process_run_try_finish(capture->run);
}

static void process_capture_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  ant_process_capture_t *capture = stream->data;
  if (capture && nread > 0) {
    ant_process_bytes_t *bytes = capture->stdout_stream
      ? &capture->run->stdout_bytes : &capture->run->stderr_bytes;
    if (!process_bytes_append(bytes, buf->base, (size_t)nread)) {
      capture->run->open_error = UV_ENOMEM;
      capture->run->final_exit_code = 1;
      process_append_error(capture->run, "process capture", UV_ENOMEM);
    }
  }
  free(buf->base);
  if (!capture || nread >= 0 || uv_is_closing((uv_handle_t *)stream)) return;
  if (nread != UV_EOF) {
    capture->run->final_exit_code = 1;
    process_append_error(capture->run, "process capture", (int)nread);
  }
  uv_read_stop(stream);
  uv_close((uv_handle_t *)stream, process_capture_closed);
}

static int process_capture_start(
  ant_process_run_t *run, ant_process_capture_t *capture,
  int *fds, bool stdout_stream
) {
  capture->run = run;
  capture->stdout_stream = stdout_stream;
  int rc = uv_pipe_init(uv_default_loop(), &capture->handle, 0);
  if (rc != 0) return rc;
  capture->initialized = true;
  capture->handle.data = capture;
  rc = uv_pipe_open(&capture->handle, fds[0]);
  if (rc != 0) {
    uv_close((uv_handle_t *)&capture->handle, process_capture_closed);
    return rc;
  }
  fds[0] = -1;
  rc = uv_read_start(
    (uv_stream_t *)&capture->handle, process_capture_alloc, process_capture_read
  );
  if (rc != 0) uv_close((uv_handle_t *)&capture->handle, process_capture_closed);
  return rc;
}

static void process_stage_closed(ant_process_stage_t *process) {
  ant_process_plan_stage_t *stage = process ? process->owner : NULL;
  if (!stage) return;
  process_run_try_finish(stage->run);
}

static void process_stage_exit(
  ant_process_stage_t *process, int64_t status, int signal
) {
  ant_process_plan_stage_t *stage = process ? process->owner : NULL;
  if (!stage) return;
  ant_process_run_t *run = stage->run;
  if (stage->index + 1 == run->plan.command_count) {
    int exit_code = signal ? 128 + signal : (int)status;
    if (exit_code != 0 || run->final_exit_code == 0)
      run->final_exit_code = exit_code;
    run->final_signal = signal;
  }
  if (run->remaining_processes > 0) run->remaining_processes--;
  process_run_try_finish(run);
}

static bool process_make_pipe(int fds[2]) {
#ifdef _WIN32
  return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) == 0;
#else
  if (pipe(fds) != 0) return false;
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
      fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0) return true;
  int saved_errno = errno;
  close(fds[0]);
  close(fds[1]);
  errno = saved_errno;
  return false;
#endif
}

static int process_stage_stdin_fd(ant_process_run_t *run, size_t index) {
  if (index > 0) return run->pipeline_fds[(index - 1) * 2];
  return run->stdin_fd;
}

static int process_stage_stdout_fd(ant_process_run_t *run, size_t index) {
  if (index + 1 < run->plan.command_count) return run->pipeline_fds[index * 2 + 1];
  if (run->stdout_fd >= 0) return run->stdout_fd;
  return run->capture_stdout_fds[1];
}

static int process_stage_stderr_fd(ant_process_run_t *run) {
  if (run->stderr_fd >= 0) return run->stderr_fd;
  if (run->plan.stderr_mode == ANT_PROCESS_STDERR_TO_STDOUT)
    return run->capture_stdout_fds[1];
  return run->capture_stderr_fds[1];
}

static void process_spawn(ant_process_run_t *run) {
  run->spawned = true;
  size_t count = run->plan.command_count;
  run->pipeline_count = count > 0 ? count - 1 : 0;
  if (run->pipeline_count) {
    run->pipeline_fds = malloc(run->pipeline_count * 2 * sizeof(int));
    if (!run->pipeline_fds) run->open_error = UV_ENOMEM;
    else for (size_t i = 0; i < run->pipeline_count * 2; i++) run->pipeline_fds[i] = -1;
  }
  for (size_t i = 0; !run->open_error && i < run->pipeline_count; i++)
    if (!process_make_pipe(&run->pipeline_fds[i * 2])) run->open_error = uv_translate_sys_error(errno);

  if (!run->open_error && (run->stdout_fd < 0 ||
      run->plan.stderr_mode == ANT_PROCESS_STDERR_TO_STDOUT) &&
      !process_make_pipe(run->capture_stdout_fds))
    run->open_error = uv_translate_sys_error(errno);
  if (!run->open_error && run->stderr_fd < 0 &&
      run->plan.stderr_mode == ANT_PROCESS_STDERR_CAPTURE &&
      !process_make_pipe(run->capture_stderr_fds))
    run->open_error = uv_translate_sys_error(errno);

  if (run->open_error) {
    process_append_error(run, "process plan", run->open_error);
    run->final_exit_code = 1;
    process_close_pipeline_fds(run);
    process_run_try_finish(run);
    return;
  }

  if (run->capture_stdout_fds[0] >= 0) {
    int rc = process_capture_start(
      run, &run->stdout_capture, run->capture_stdout_fds, true
    );
    if (rc != 0) run->open_error = rc;
  }
  if (!run->open_error && run->capture_stderr_fds[0] >= 0) {
    int rc = process_capture_start(
      run, &run->stderr_capture, run->capture_stderr_fds, false
    );
    if (rc != 0) run->open_error = rc;
  }

  if (run->open_error) {
    process_append_error(run, "process capture", run->open_error);
    run->final_exit_code = 1;
    process_close_pipeline_fds(run);
    process_run_try_finish(run);
    return;
  }

  run->stages = calloc(count, sizeof(*run->stages));
  if (!run->stages) {
    run->open_error = UV_ENOMEM;
    process_append_error(run, "process plan", run->open_error);
    run->final_exit_code = 1;
    process_close_pipeline_fds(run);
    process_run_try_finish(run);
    return;
  }
  run->remaining_processes = 0;
  for (size_t i = 0; !run->open_error && i < count; i++) {
    ant_process_plan_stage_t *stage = &run->stages[i];
    stage->run = run;
    stage->index = i;
    ant_process_stage_init(
      &stage->process, stage, process_stage_exit, process_stage_closed
    );
    uv_stdio_container_t stdio[3] = {0};
    int stdin_fd = process_stage_stdin_fd(run, i);
    int stdout_fd = process_stage_stdout_fd(run, i);
    int stderr_fd = process_stage_stderr_fd(run);
    if (stdin_fd >= 0) ant_process_stdio_inherit_fd(&stdio[0], stdin_fd);
    else ant_process_stdio_ignore(&stdio[0]);
    if (stdout_fd >= 0) ant_process_stdio_inherit_fd(&stdio[1], stdout_fd);
    else ant_process_stdio_ignore(&stdio[1]);
    if (stderr_fd >= 0) ant_process_stdio_inherit_fd(&stdio[2], stderr_fd);
    else ant_process_stdio_ignore(&stdio[2]);
    ant_process_spawn_spec_t spec = {
      .file = run->plan.commands[i].argv[0],
      .args = run->plan.commands[i].argv,
      .cwd = run->plan.cwd,
      .stdio = stdio,
      .stdio_count = 3,
    };
    int rc = ant_process_stage_spawn(&stage->process, &spec);
    if (rc < 0) {
      process_append_error(run, spec.file, rc);
      if (i + 1 == count) run->final_exit_code = 127;
      continue;
    }
    run->remaining_processes++;
  }
  process_close_pipeline_fds(run);
  process_run_try_finish(run);
}

static void process_redirect_open_done(uv_fs_t *request) {
  ant_process_run_t *run = request->data;
  ant_process_plan_redirect_t *redirect =
    &run->plan.redirects[run->redirect_index];
  int fd = (int)request->result;
  uv_fs_req_cleanup(request);
  if (fd < 0) {
    process_append_error(run, redirect->path, fd);
    run->final_exit_code = 1;
    run->spawned = true;
    process_close_pipeline_fds(run);
    process_run_try_finish(run);
    return;
  }
  if (redirect->kind == ANT_PROCESS_REDIRECT_STDIN) {
    process_close_fd(&run->stdin_fd);
    run->stdin_fd = fd;
  } else {
    process_close_fd(&run->stdout_fd);
    run->stdout_fd = fd;
  }
  run->redirect_index++;
  process_prepare_next_redirect(run);
}

// TODO: reduce nesting
static void process_prepare_next_redirect(ant_process_run_t *run) {
  while (run->redirect_index < run->plan.redirect_count) {
    ant_process_plan_redirect_t *redirect =
      &run->plan.redirects[run->redirect_index];
    if (redirect->kind == ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT) {
      process_close_fd(&run->stderr_fd);
      if (run->stdout_fd >= 0) {
      #ifdef _WIN32
        run->stderr_fd = _dup(run->stdout_fd);
      #else
        run->stderr_fd = dup(run->stdout_fd);
      #endif
        if (run->stderr_fd < 0) {
          int error = uv_translate_sys_error(errno);
          process_append_error(run, "stderr redirection", error);
          run->final_exit_code = 1;
          run->spawned = true;
          process_close_pipeline_fds(run);
          process_run_try_finish(run);
          return;
        }
        run->plan.stderr_mode = ANT_PROCESS_STDERR_CAPTURE;
      } else run->plan.stderr_mode = ANT_PROCESS_STDERR_TO_STDOUT;
      run->redirect_index++;
      continue;
    }

    int flags = O_RDONLY;
    if (redirect->kind != ANT_PROCESS_REDIRECT_STDIN) {
      flags = O_CREAT | O_WRONLY |
        (redirect->kind == ANT_PROCESS_REDIRECT_STDOUT_APPEND
          ? O_APPEND : O_TRUNC);
    }
    run->redirect_open.data = run;
    int rc = uv_fs_open(
      uv_default_loop(), &run->redirect_open, redirect->path,
      flags, 0666, process_redirect_open_done
    );
    if (rc >= 0) return;
    uv_fs_req_cleanup(&run->redirect_open);
    process_append_error(run, redirect->path, rc);
    run->final_exit_code = 1;
    run->spawned = true;
    process_close_pipeline_fds(run);
    process_run_try_finish(run);
    return;
  }
  process_spawn(run);
}

static void process_run_free(ant_process_run_t *run) {
  if (!run) return;
  process_close_pipeline_fds(run);
  process_close_fd(&run->capture_stdout_fds[0]);
  process_close_fd(&run->capture_stderr_fds[0]);
  ant_process_plan_dispose(&run->plan);
  free(run->pipeline_fds);
  free(run->stages);
  free(run->stdout_bytes.data);
  free(run->stderr_bytes.data);
  free(run);
}

static void process_run_try_finish(ant_process_run_t *run) {
  if (!run || run->settled || !run->spawned || run->remaining_processes != 0)
    return;
  for (size_t i = 0; i < run->plan.command_count; i++)
    if (run->stages && run->stages[i].process.handle_open &&
        !run->stages[i].process.closed)
      return;
  if (run->stdout_capture.initialized && !run->stdout_capture.closed) return;
  if (run->stderr_capture.initialized && !run->stderr_capture.closed) return;
  run->settled = true;
  ant_value_t result = js_mkobj(run->js);
  js_set(run->js, result, "stdout", js_mkstr(
    run->js, run->stdout_bytes.data ? run->stdout_bytes.data : "",
    run->stdout_bytes.len
  ));
  js_set(run->js, result, "stderr", js_mkstr(
    run->js, run->stderr_bytes.data ? run->stderr_bytes.data : "",
    run->stderr_bytes.len
  ));
  js_set(run->js, result, "exitCode", js_mknum((double)run->final_exit_code));
  const char *signal_name = process_signal_name(run->final_signal);
  js_set(run->js, result, "signalCode", signal_name
    ? js_mkstr(run->js, signal_name, strlen(signal_name)) : js_mknull());
  js_set(run->js, result, "exited", js_false);
  js_resolve_promise(run->js, run->promise, result);
  gc_unroot_pending_promise(run->js, js_obj_ptr(run->promise));
  process_run_free(run);
}

ant_value_t ant_process_plan_submit(ant_t *js, ant_process_plan_t *plan) {
  if (!js || !plan || plan->command_count == 0)
    return ant_process_plan_rejected_result(js, js_mkerr(js, "invalid process plan"));
  
  ant_process_run_t *run = calloc(1, sizeof(*run));
  if (!run) return ant_process_plan_rejected_result(js, js_mkerr(js, "out of memory"));
  
  run->js = js;
  run->plan = *plan;
  *plan = (ant_process_plan_t){0};
  
  run->stdin_fd = -1;
  run->stdout_fd = -1;
  run->stderr_fd = -1;
  run->capture_stdout_fds[0] = run->capture_stdout_fds[1] = -1;
  run->capture_stderr_fds[0] = run->capture_stderr_fds[1] = -1;
  run->promise = js_mkpromise(js);
  
  if (is_err(run->promise)) {
    ant_value_t error = run->promise;
    process_run_free(run);
    return ant_process_plan_rejected_result(js, error);
  }
  
  ant_value_t promise = run->promise;
  gc_root_pending_promise(js, js_obj_ptr(promise));

  process_prepare_next_redirect(run);
  return promise;
}
