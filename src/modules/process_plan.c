#include "process_plan.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <uv.h>

#include "ant.h"
#include "errors.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/buffer.h"
#include "modules/process.h"
#include "process_stage.h"

#define PROCESS_CHUNK_SIZE 65536

typedef struct ant_process_run ant_process_run_t;
typedef struct ant_process_native_stage ant_process_native_stage_t;
typedef struct ant_process_feeder ant_process_feeder_t;

typedef struct {
  ant_process_stage_t process;
  ant_process_run_t *run;
  ant_process_native_stage_t *native;
  size_t index;
} ant_process_plan_stage_t;

struct ant_process_native_stage {
  uv_fs_t write_request;
  ant_process_plan_stage_t *stage;
  int stdout_fd;
  int stderr_fd;
  size_t offset;
  bool writing_stderr;
  bool write_failed;
};

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} ant_process_bytes_t;

typedef struct {
  uv_pipe_t handle;
  ant_process_run_t *run;
  ant_process_bytes_t bytes;
  int fds[2];
  bool initialized;
  bool closed;
} ant_process_capture_t;

struct ant_process_feeder {
  uv_pipe_t handle;
  uv_write_t write;
  uv_fs_t read;
  ant_process_run_t *run;
  ant_process_feeder_t *next;
  char *chunk;
  int source;
  bool closed;
};

typedef struct {
#ifndef _WIN32
  struct rlimit saved[ANT_PROCESS_FD_LIMIT];
  bool applied[ANT_PROCESS_FD_LIMIT];
#endif
  int umask;
} ant_process_limits_t;

struct ant_process_run {
  ant_t *js;
  ant_value_t promise;
  ant_process_plan_t plan;
  ant_process_plan_stage_t *stages;
  int *statuses;
  int *pipeline_fds;
  size_t pipeline_count;
  ant_process_capture_t *captures;
  ant_process_feeder_t *feeders;
  ant_process_bytes_t diagnostics;
  uv_fs_t open_request;
  size_t open_list;
  size_t open_index;
  int final_signal;
  size_t remaining_stages;
  bool spawned;
  bool spawning;
  bool settled;
};

static void process_run_try_finish(ant_process_run_t *run);
static void process_open_next(ant_process_run_t *run);
static void process_feeder_pump(ant_process_feeder_t *feeder);
static void process_native_stage_advance(ant_process_native_stage_t *native);

ant_value_t ant_process_plan_rejected_result(ant_t *js, ant_value_t error) {
  GC_ROOT_SAVE(root_mark, js);
  error = js_take_thrown(js, error);
  GC_ROOT_PIN(js, error);

  ant_value_t promise = js_mkpromise(js);
  if (!is_err(promise)) {
    GC_ROOT_PIN(js, promise);
    js_reject_promise(js, promise, error);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

void ant_process_plan_init(ant_process_plan_t *plan) {
  if (plan) *plan = (ant_process_plan_t){ .umask = -1 };
}

int ant_process_umask_push(int mask) {
#ifndef _WIN32
  if (mask >= 0) return (int)umask((mode_t)mask);
#else
  (void)mask;
#endif
  return -1;
}

void ant_process_umask_pop(int previous) {
#ifndef _WIN32
  if (previous >= 0) umask((mode_t)previous);
#else
  (void)previous;
#endif
}

int ant_process_open_flags(ant_process_fd_kind_t kind) {
  int flags = O_RDONLY;
  switch (kind) {
    case ANT_PROCESS_FD_OPEN_WRITE: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case ANT_PROCESS_FD_OPEN_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
    case ANT_PROCESS_FD_OPEN_READ_WRITE: flags = O_RDWR | O_CREAT; break;
    case ANT_PROCESS_FD_OPEN_EXCLUSIVE: flags = O_WRONLY | O_CREAT | O_EXCL; break;
    default: break;
  }
#ifdef _WIN32
  flags |= _O_BINARY;
#endif
  return flags;
}

int ant_process_open_sync(const char *path, int flags, int mask) {
  uv_fs_t request;
  int previous = ant_process_umask_push(mask);
  int fd = uv_fs_open(NULL, &request, path, flags, 0666, NULL);
  ant_process_umask_pop(previous);
  uv_fs_req_cleanup(&request);
  return fd;
}

static void process_plan_resolve_spawned(ant_t *js, ant_process_plan_t *plan, const int *pids, size_t count) {
  if (!plan->has_spawned) return;
  plan->has_spawned = false;
  ant_value_t promise = plan->spawned;
  plan->spawned = Ant_Value_Undefined;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);

  ant_value_t list = Ant_Array_Create(js);
  if (is_err(list)) js_resolve_promise(js, promise, Ant_Value_Undefined);
  else {
    GC_ROOT_PIN(js, list);
    for (size_t i = 0; i < count; i++) js_arr_push(js, list, Ant_Value_FromNumber((double)pids[i]));
    js_resolve_promise(js, promise, list);
  }

  GC_ROOT_RESTORE(js, root_mark);
  gc_unroot_pending_promise(js, js_obj_ptr(promise));
}

ant_value_t ant_process_plan_spawned(ant_t *js, ant_process_plan_t *plan) {
  if (plan->has_spawned) return plan->spawned;

  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  plan->spawned = promise;
  plan->has_spawned = true;
  gc_root_pending_promise(js, js_obj_ptr(promise));

  return promise;
}

void ant_process_plan_release(ant_t *js, ant_process_plan_t *plan) {
  if (plan) process_plan_resolve_spawned(js, plan, NULL, 0);
  ant_process_plan_dispose(plan);
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

static int process_dup_fd(int fd) {
  if (fd < 0) return -1;
#ifdef _WIN32
  return _dup(fd);
#else
  return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#endif
}

static bool process_make_pipe(int fds[2]) {
#ifdef _WIN32
  return _pipe(fds, PROCESS_CHUNK_SIZE, _O_BINARY | _O_NOINHERIT) == 0;
#else
  if (pipe(fds) != 0) return false;
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0) return true;

  int saved_errno = errno;
  close(fds[0]);
  close(fds[1]);
  errno = saved_errno;

  return false;
#endif
}

static void process_fd_ops_dispose(ant_process_fd_ops_t *ops) {
  for (size_t i = 0; i < ops->count; i++) {
    ant_process_fd_op_t *op = &ops->items[i];
    if (op->kind == ANT_PROCESS_FD_ADOPT) process_close_fd(&op->opened);
    free(op->path);
    free(op->data);
  }
  free(ops->items);
  *ops = (ant_process_fd_ops_t){0};
}

static void process_free_strings(char **strings) {
  if (!strings) return;
  for (char **cursor = strings; *cursor; cursor++) free(*cursor);
  free(strings);
}

static void process_command_dispose(ant_process_plan_command_t *command) {
  if (command->kind == ANT_PROCESS_PLAN_COMMAND_EXTERNAL) {
    free(command->as.external.file);
    process_free_strings(command->as.external.argv);
    process_free_strings(command->as.external.env);
  } else {
    free(command->as.native.stdout_data);
    free(command->as.native.stderr_data);
  }
  process_fd_ops_dispose(&command->fds);
}

void ant_process_plan_dispose(ant_process_plan_t *plan) {
  if (!plan) return;
  for (size_t i = 0; i < plan->command_count; i++) process_command_dispose(&plan->commands[i]);
  free(plan->commands);
  process_fd_ops_dispose(&plan->base);
  free(plan->cwd);
  *plan = (ant_process_plan_t){ .umask = -1 };
}

static ant_process_plan_command_t *process_plan_push(ant_process_plan_t *plan) {
  if (plan->command_count == SIZE_MAX / sizeof(*plan->commands)) return NULL;

  size_t next_count = plan->command_count + 1;
  ant_process_plan_command_t *commands = realloc(plan->commands, next_count * sizeof(*commands));
  if (!commands) return NULL;

  plan->commands = commands;
  ant_process_plan_command_t *command = &commands[plan->command_count];
  *command = (ant_process_plan_command_t){0};
  plan->command_count = next_count;

  return command;
}

bool ant_process_plan_take_external(ant_process_plan_t *plan, char *file, char **argv, size_t argc, char **env) {
  if (!plan || !file || !argv || argc == 0 || !argv[0]) return false;

  ant_process_plan_command_t *command = process_plan_push(plan);
  if (!command) return false;

  command->kind = ANT_PROCESS_PLAN_COMMAND_EXTERNAL;
  command->as.external.file = file;
  command->as.external.argv = argv;
  command->as.external.argc = argc;
  command->as.external.env = env;

  return true;
}

static bool process_plan_copy_bytes(char **out, const char *data, size_t len) {
  *out = NULL;
  if (len == 0) return true;
  if (len > SIZE_MAX - 1) return false;

  char *copy = malloc(len + 1);
  if (!copy) return false;

  memcpy(copy, data, len);
  copy[len] = '\0';
  *out = copy;

  return true;
}

bool ant_process_plan_add_native_stage(
  ant_process_plan_t *plan,
  const char *stdout_data, size_t stdout_len,
  const char *stderr_data, size_t stderr_len,
  int exit_code
) {
  if (!plan || (!stdout_data && stdout_len) || (!stderr_data && stderr_len)) return false;

  ant_process_plan_command_t *command = process_plan_push(plan);
  if (!command) return false;
  command->kind = ANT_PROCESS_PLAN_COMMAND_NATIVE;

  if (!process_plan_copy_bytes(&command->as.native.stdout_data, stdout_data, stdout_len)) return false;
  if (!process_plan_copy_bytes(&command->as.native.stderr_data, stderr_data, stderr_len)) return false;

  command->as.native.stdout_len = stdout_len;
  command->as.native.stderr_len = stderr_len;
  command->as.native.exit_code = exit_code;

  return true;
}

static bool process_fd_op_valid(const ant_process_plan_t *plan, ant_process_fd_kind_t kind, int fd, int source, const char *path) {
  if (fd < 0 || fd >= ANT_PROCESS_FD_LIMIT || kind > ANT_PROCESS_FD_ADOPT) return false;
  switch (kind) {
    case ANT_PROCESS_FD_DUP: return source >= 0 && source < ANT_PROCESS_FD_LIMIT;
    case ANT_PROCESS_FD_CAPTURE: return source >= 0 && (size_t)source < plan->capture_count;
    case ANT_PROCESS_FD_HOST:
    case ANT_PROCESS_FD_ADOPT: return source >= 0;
    default: return kind > ANT_PROCESS_FD_OPEN_EXCLUSIVE || path != NULL;
  }
}

bool ant_process_plan_add_fd(
  ant_process_plan_t *plan, ant_process_plan_command_t *command,
  ant_process_fd_kind_t kind, int fd, int source,
  const char *path, const char *data, size_t len
) {
  if (!plan || !process_fd_op_valid(plan, kind, fd, source, path)) return false;

  ant_process_fd_ops_t *ops = command ? &command->fds : &plan->base;
  if (ops->count == SIZE_MAX / sizeof(*ops->items)) return false;

  ant_process_fd_op_t *items = realloc(ops->items, (ops->count + 1) * sizeof(*items));
  if (!items) return false;
  ops->items = items;

  ant_process_fd_op_t *op = &items[ops->count];
  *op = (ant_process_fd_op_t){
    .kind = kind,
    .fd = fd,
    .source = source,
    .opened = kind == ANT_PROCESS_FD_ADOPT ? source : -1,
  };

  if (path && !(op->path = strdup(path))) return false;
  if (kind == ANT_PROCESS_FD_BYTES) {
    if (!process_plan_copy_bytes(&op->data, data, len)) {
      free(op->path);
      return false;
    }
    op->len = len;
  }

  ops->count++;
  return true;
}

bool ant_process_plan_add_capture(ant_process_plan_t *plan, size_t *index) {
  if (!plan || plan->capture_count >= ANT_PROCESS_FD_LIMIT * 2) return false;
  *index = plan->capture_count++;
  return true;
}

static bool process_bytes_reserve(ant_process_bytes_t *bytes, size_t need) {
  if (need <= bytes->cap) return true;

  size_t next = bytes->cap ? bytes->cap * 2 : 4096;
  while (next < need) {
    if (next > SIZE_MAX / 2) return false;
    next *= 2;
  }

  char *grown = realloc(bytes->data, next);
  if (!grown) return false;

  bytes->data = grown;
  bytes->cap = next;

  return true;
}

static bool process_bytes_append(ant_process_bytes_t *bytes, const char *data, size_t len) {
  if (len > SIZE_MAX - bytes->len - 1) return false;
  if (!process_bytes_reserve(bytes, bytes->len + len + 1)) return false;

  if (len) memcpy(bytes->data + bytes->len, data, len);
  bytes->len += len;
  bytes->data[bytes->len] = '\0';

  return true;
}

static void process_diagnostic(ant_process_run_t *run, const char *subject, int error) {
  char reason[256];
  snprintf(reason, sizeof(reason), "%s", uv_strerror(error));
  if (reason[0] >= 'a' && reason[0] <= 'z') reason[0] = (char)(reason[0] - 'a' + 'A');

  char message[1024];
  int len = snprintf(message, sizeof(message), "ant:shell: %s: %s\n", subject ? subject : "process", reason);
  if (len <= 0) return;

  size_t size = (size_t)len < sizeof(message) ? (size_t)len : sizeof(message) - 1;
  (void)process_bytes_append(&run->diagnostics, message, size);
}

static ant_process_fd_ops_t *process_run_fd_list(ant_process_run_t *run, size_t list) {
  return list == 0 ? &run->plan.base : &run->plan.commands[list - 1].fds;
}

static void process_close_owned_fds(ant_process_run_t *run) {
  if (run->pipeline_fds) {
    for (size_t i = 0; i < run->pipeline_count * 2; i++) process_close_fd(&run->pipeline_fds[i]);
  }

  if (run->captures) {
    for (size_t i = 0; i < run->plan.capture_count; i++) process_close_fd(&run->captures[i].fds[1]);
  }

  for (size_t list = 0; list <= run->plan.command_count; list++) {
    ant_process_fd_ops_t *ops = process_run_fd_list(run, list);
    for (size_t i = 0; i < ops->count; i++) process_close_fd(&ops->items[i].opened);
  }
}

static void process_capture_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  (void)handle;
  size_t size = suggested > PROCESS_CHUNK_SIZE ? PROCESS_CHUNK_SIZE : suggested;
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
  if (capture && nread > 0 && !process_bytes_append(&capture->bytes, buf->base, (size_t)nread))
    process_diagnostic(capture->run, "process capture", UV_ENOMEM);
  free(buf->base);

  if (!capture || nread >= 0 || uv_is_closing((uv_handle_t *)stream)) return;
  if (nread != UV_EOF) process_diagnostic(capture->run, "process capture", (int)nread);

  uv_read_stop(stream);
  uv_close((uv_handle_t *)stream, process_capture_closed);
}

static int process_capture_start(ant_process_run_t *run, ant_process_capture_t *capture) {
  capture->run = run;
  capture->fds[0] = capture->fds[1] = -1;
  if (!process_make_pipe(capture->fds)) return uv_translate_sys_error(errno);

  int rc = uv_pipe_init(run->js->loop, &capture->handle, 0);
  if (rc != 0) {
    process_close_fd(&capture->fds[0]);
    return rc;
  }

  capture->initialized = true;
  capture->handle.data = capture;

  rc = uv_pipe_open(&capture->handle, capture->fds[0]);
  if (rc != 0) {
    process_close_fd(&capture->fds[0]);
    uv_close((uv_handle_t *)&capture->handle, process_capture_closed);
    return rc;
  }

  capture->fds[0] = -1;
  rc = uv_read_start((uv_stream_t *)&capture->handle, process_capture_alloc, process_capture_read);
  if (rc != 0) uv_close((uv_handle_t *)&capture->handle, process_capture_closed);

  return rc;
}

static void process_feeder_closed(uv_handle_t *handle) {
  ant_process_feeder_t *feeder = handle->data;
  feeder->closed = true;
  process_run_try_finish(feeder->run);
}

static void process_feeder_finish(ant_process_feeder_t *feeder) {
  uv_close((uv_handle_t *)&feeder->handle, process_feeder_closed);
}

static void process_feeder_written(uv_write_t *request, int status) {
  ant_process_feeder_t *feeder = request->data;
  if (status < 0 || feeder->source < 0) process_feeder_finish(feeder);
  else process_feeder_pump(feeder);
}

static void process_feeder_send(ant_process_feeder_t *feeder, char *data, size_t len) {
  uv_buf_t buffer = uv_buf_init(data, (unsigned int)(len > UINT_MAX ? UINT_MAX : len));
  feeder->write.data = feeder;
  if (uv_write(&feeder->write, (uv_stream_t *)&feeder->handle, &buffer, 1, process_feeder_written) != 0)
    process_feeder_finish(feeder);
}

static void process_feeder_read(uv_fs_t *request) {
  ant_process_feeder_t *feeder = request->data;
  ssize_t result = request->result;
  uv_fs_req_cleanup(request);

  if (result <= 0) process_feeder_finish(feeder);
  else process_feeder_send(feeder, feeder->chunk, (size_t)result);
}

static void process_feeder_pump(ant_process_feeder_t *feeder) {
  if (!feeder->chunk && !(feeder->chunk = malloc(PROCESS_CHUNK_SIZE))) {
    process_feeder_finish(feeder);
    return;
  }

  uv_buf_t buffer = uv_buf_init(feeder->chunk, PROCESS_CHUNK_SIZE);
  feeder->read.data = feeder;

  int rc = uv_fs_read(feeder->run->js->loop, &feeder->read, feeder->source, &buffer, 1, -1, process_feeder_read);
  if (rc >= 0) return;

  uv_fs_req_cleanup(&feeder->read);
  process_feeder_finish(feeder);
}

static int process_feeder_open(ant_process_run_t *run, ant_process_feeder_t *feeder, int *write_fd) {
  int rc = uv_pipe_init(run->js->loop, &feeder->handle, 0);
  if (rc != 0) {
    feeder->closed = true;
    process_close_fd(write_fd);
    return rc;
  }

  feeder->handle.data = feeder;
  rc = uv_pipe_open(&feeder->handle, *write_fd);
  if (rc == 0) return 0;

  process_close_fd(write_fd);
  uv_close((uv_handle_t *)&feeder->handle, process_feeder_closed);

  return rc;
}

static int process_feeder_start(ant_process_run_t *run, ant_process_fd_op_t *op) {
  int fds[2] = {-1, -1};
  if (!process_make_pipe(fds)) return uv_translate_sys_error(errno);
  op->opened = fds[0];

  if (op->len == 0 && op->source < 0) {
    process_close_fd(&fds[1]);
    return 0;
  }

  ant_process_feeder_t *feeder = calloc(1, sizeof(*feeder));
  if (!feeder) {
    process_close_fd(&fds[1]);
    return UV_ENOMEM;
  }

  feeder->run = run;
  feeder->source = op->source;
  feeder->next = run->feeders;
  run->feeders = feeder;

  int rc = process_feeder_open(run, feeder, &fds[1]);
  if (rc != 0) return rc;

  if (op->len == 0) process_feeder_pump(feeder);
  else process_feeder_send(feeder, op->data, op->len);

  return 0;
}

static bool process_apply_open(ant_process_run_t *run, ant_process_fd_op_t *op, int table[ANT_PROCESS_FD_LIMIT]) {
  if (op->error || op->opened < 0) {
    process_diagnostic(run, op->path, op->error ? op->error : UV_EBADF);
    return false;
  }
  table[op->fd] = op->opened;
  return true;
}

static bool process_apply_dup(ant_process_run_t *run, ant_process_fd_op_t *op, int table[ANT_PROCESS_FD_LIMIT]) {
  if (table[op->source] >= 0) {
    table[op->fd] = table[op->source];
    return true;
  }

  char subject[16];
  snprintf(subject, sizeof(subject), "%d", op->source);
  process_diagnostic(run, subject, UV_EBADF);

  return false;
}

static bool process_apply_bytes(ant_process_run_t *run, ant_process_fd_op_t *op, int table[ANT_PROCESS_FD_LIMIT]) {
  if (op->opened < 0) {
    int rc = process_feeder_start(run, op);
    if (rc != 0) {
      process_diagnostic(run, "here-document", rc);
      return false;
    }
  }
  table[op->fd] = op->opened;
  return true;
}

static bool process_apply_fd(ant_process_run_t *run, ant_process_fd_op_t *op, int table[ANT_PROCESS_FD_LIMIT]) {
  switch (op->kind) {
    case ANT_PROCESS_FD_OPEN_READ:
    case ANT_PROCESS_FD_OPEN_WRITE:
    case ANT_PROCESS_FD_OPEN_APPEND:
    case ANT_PROCESS_FD_OPEN_READ_WRITE:
    case ANT_PROCESS_FD_OPEN_EXCLUSIVE: return process_apply_open(run, op, table);
    case ANT_PROCESS_FD_DUP: return process_apply_dup(run, op, table);
    case ANT_PROCESS_FD_BYTES: return process_apply_bytes(run, op, table);
    case ANT_PROCESS_FD_CLOSE: table[op->fd] = -1; return true;
    case ANT_PROCESS_FD_HOST: table[op->fd] = op->source; return true;
    case ANT_PROCESS_FD_CAPTURE: table[op->fd] = run->captures[op->source].fds[1]; return true;
    case ANT_PROCESS_FD_ADOPT: table[op->fd] = op->opened; return true;
  }
  return true;
}

static bool process_apply_fds(ant_process_run_t *run, ant_process_fd_ops_t *ops, int table[ANT_PROCESS_FD_LIMIT]) {
  for (size_t i = 0; i < ops->count; i++) if (!process_apply_fd(run, &ops->items[i], table)) return false;
  return true;
}

static void process_stage_closed(ant_process_stage_t *process) {
  ant_process_plan_stage_t *stage = process ? process->owner : NULL;
  if (stage) process_run_try_finish(stage->run);
}

static void process_stage_exit(ant_process_stage_t *process, int64_t status, int signal) {
  ant_process_plan_stage_t *stage = process ? process->owner : NULL;
  if (!stage) return;

  ant_process_run_t *run = stage->run;
  run->statuses[stage->index] = signal ? 128 + signal : (int)status;
  if (stage->index + 1 == run->plan.command_count) run->final_signal = signal;
  if (run->remaining_stages > 0) run->remaining_stages--;

  process_run_try_finish(run);
}

static ant_process_plan_command_t *process_native_command(ant_process_native_stage_t *native) {
  return &native->stage->run->plan.commands[native->stage->index];
}

static void process_native_stage_finish(ant_process_native_stage_t *native) {
  ant_process_run_t *run = native->stage->run;
  ant_process_plan_command_t *command = process_native_command(native);

  process_close_fd(&native->stdout_fd);
  process_close_fd(&native->stderr_fd);

  run->statuses[native->stage->index] = native->write_failed ? 1 : command->as.native.exit_code;
  if (run->remaining_stages > 0) run->remaining_stages--;
  if (!run->spawning) process_run_try_finish(run);
}

static size_t process_native_length(ant_process_native_stage_t *native) {
  ant_process_plan_command_t *command = process_native_command(native);
  return native->writing_stderr ? command->as.native.stderr_len : command->as.native.stdout_len;
}

static void process_native_write_done(uv_fs_t *request) {
  ant_process_native_stage_t *native = request->data;
  ssize_t written = request->result;
  uv_fs_req_cleanup(request);

  if (written > 0) native->offset += (size_t)written;
  else {
    native->write_failed = true;
    if (written < 0 && written != UV_EPIPE) process_diagnostic(native->stage->run, "write error", (int)written);
    native->offset = process_native_length(native);
  }

  process_native_stage_advance(native);
}

static bool process_native_write(ant_process_native_stage_t *native) {
  ant_process_plan_command_t *command = process_native_command(native);
  const char *data = native->writing_stderr ? command->as.native.stderr_data : command->as.native.stdout_data;
  int fd = native->writing_stderr ? native->stderr_fd : native->stdout_fd;
  size_t len = process_native_length(native);
  if (fd < 0 || native->offset >= len) return false;

  size_t remaining = len - native->offset;
  uv_buf_t buffer = uv_buf_init((char *)data + native->offset, (unsigned int)(remaining > UINT_MAX ? UINT_MAX : remaining));
  native->write_request.data = native;

  uv_loop_t *loop = native->stage->run->js->loop;
  int rc = uv_fs_write(loop, &native->write_request, fd, &buffer, 1, -1, process_native_write_done);
  if (rc >= 0) return true;

  uv_fs_req_cleanup(&native->write_request);
  native->write_failed = true;
  if (rc != UV_EPIPE) process_diagnostic(native->stage->run, "write error", rc);

  return false;
}

static void process_native_stage_advance(ant_process_native_stage_t *native) {
  if (process_native_write(native)) return;
  process_close_fd(native->writing_stderr ? &native->stderr_fd : &native->stdout_fd);

  if (native->writing_stderr) {
    process_native_stage_finish(native);
    return;
  }

  native->writing_stderr = true;
  native->offset = 0;
  process_native_stage_advance(native);
}

static int process_native_dup(ant_process_native_stage_t *native, int *target, int source, size_t len) {
  if (len == 0) return 0;
  *target = process_dup_fd(source);
  if (*target >= 0) return 0;

  native->write_failed = true;
  return source < 0 ? UV_EBADF : uv_translate_sys_error(errno);
}

static void process_native_stage_start(ant_process_run_t *run, ant_process_plan_stage_t *stage, int stdout_fd, int stderr_fd) {
  ant_process_plan_command_t *command = &run->plan.commands[stage->index];
  run->statuses[stage->index] = command->as.native.exit_code;
  if (command->as.native.stdout_len == 0 && command->as.native.stderr_len == 0) return;

  ant_process_native_stage_t *native = calloc(1, sizeof(*native));
  if (!native) {
    process_diagnostic(run, "native pipeline stage", UV_ENOMEM);
    run->statuses[stage->index] = 1;
    return;
  }

  stage->native = native;
  native->stage = stage;
  native->stdout_fd = -1;
  native->stderr_fd = -1;

  int error = process_native_dup(native, &native->stdout_fd, stdout_fd, command->as.native.stdout_len);
  int stderr_error = process_native_dup(native, &native->stderr_fd, stderr_fd, command->as.native.stderr_len);
  if (!error) error = stderr_error;
  if (error) process_diagnostic(run, "write error", error);

  run->remaining_stages++;
  process_native_stage_advance(native);
}

static void process_fail_all(ant_process_run_t *run, const char *what, int error) {
  process_diagnostic(run, what, error);
  for (size_t i = 0; i < run->plan.command_count; i++) run->statuses[i] = 1;

  process_close_owned_fds(run);
  process_plan_resolve_spawned(run->js, &run->plan, NULL, 0);
  process_run_try_finish(run);
}

static bool process_start_captures(ant_process_run_t *run) {
  size_t count = run->plan.capture_count;
  if (count == 0) return true;

  run->captures = calloc(count, sizeof(*run->captures));
  if (!run->captures) {
    process_fail_all(run, "process plan", UV_ENOMEM);
    return false;
  }

  for (size_t i = 0; i < count; i++) run->captures[i].fds[0] = run->captures[i].fds[1] = -1;
  for (size_t i = 0; i < count; i++) {
    int rc = process_capture_start(run, &run->captures[i]);
    if (rc != 0) {
      process_fail_all(run, "process capture", rc);
      return false;
    }
  }

  return true;
}

static bool process_make_pipeline(ant_process_run_t *run) {
  size_t count = run->plan.command_count;
  run->pipeline_count = count > 0 ? count - 1 : 0;
  if (run->pipeline_count == 0) return true;

  run->pipeline_fds = malloc(run->pipeline_count * 2 * sizeof(int));
  if (!run->pipeline_fds) {
    process_fail_all(run, "process plan", UV_ENOMEM);
    return false;
  }

  for (size_t i = 0; i < run->pipeline_count * 2; i++) run->pipeline_fds[i] = -1;
  for (size_t i = 0; i < run->pipeline_count; i++) {
    if (process_make_pipe(&run->pipeline_fds[i * 2])) continue;
    process_fail_all(run, "pipe", uv_translate_sys_error(errno));
    return false;
  }

  return true;
}

static void process_limits_apply(ant_process_run_t *run, ant_process_limits_t *limits) {
  limits->umask = ant_process_umask_push(run->plan.umask);
#ifndef _WIN32
  for (int i = 0; i < run->plan.limit_count; i++) {
    limits->applied[i] = false;
    struct rlimit limit;
    if (getrlimit(run->plan.limit_resources[i], &limit) != 0) continue;
    limits->saved[i] = limit;

    double value = run->plan.limit_values[i];
    rlim_t soft = value < 0 ? RLIM_INFINITY : (rlim_t)value;
    if (limit.rlim_max != RLIM_INFINITY && (soft == RLIM_INFINITY || soft > limit.rlim_max)) soft = limit.rlim_max;

    limit.rlim_cur = soft;
    limits->applied[i] = setrlimit(run->plan.limit_resources[i], &limit) == 0;
  }
#endif
}

static void process_limits_restore(ant_process_run_t *run, ant_process_limits_t *limits) {
  ant_process_umask_pop(limits->umask);
#ifndef _WIN32
  for (int i = 0; i < run->plan.limit_count; i++) {
    if (limits->applied[i]) setrlimit(run->plan.limit_resources[i], &limits->saved[i]);
  }
#endif
}

static void process_stage_table(ant_process_run_t *run, size_t index, const int base[ANT_PROCESS_FD_LIMIT], int table[ANT_PROCESS_FD_LIMIT]) {
  memcpy(table, base, sizeof(int) * ANT_PROCESS_FD_LIMIT);
  if (index > 0) table[0] = run->pipeline_fds[(index - 1) * 2];
  if (index + 1 < run->plan.command_count) table[1] = run->pipeline_fds[index * 2 + 1];
}

static void process_spawn_external(ant_process_run_t *run, ant_process_plan_stage_t *stage, const int table[ANT_PROCESS_FD_LIMIT]) {
  ant_process_plan_command_t *command = &run->plan.commands[stage->index];

  int stdio_count = 3;
  for (int fd = 3; fd < ANT_PROCESS_FD_LIMIT; fd++) if (table[fd] >= 0) stdio_count = fd + 1;

  uv_stdio_container_t stdio[ANT_PROCESS_FD_LIMIT];
  memset(stdio, 0, sizeof(stdio));
  for (int fd = 0; fd < stdio_count; fd++) {
    if (table[fd] >= 0) ant_process_stdio_inherit_fd(&stdio[fd], table[fd]);
    else ant_process_stdio_ignore(&stdio[fd]);
  }

  ant_process_spawn_spec_t spec = {
    .loop = run->js->loop,
    .file = command->as.external.file,
    .args = command->as.external.argv,
    .env = command->as.external.env,
    .cwd = run->plan.cwd,
    .stdio = stdio,
    .stdio_count = stdio_count,
  };

  int rc = ant_process_stage_spawn(&stage->process, &spec);
  if (rc < 0) {
    process_diagnostic(run, command->as.external.argv[0], rc);
    run->statuses[stage->index] = rc == UV_ENOENT ? 127 : 126;
    return;
  }

  run->remaining_stages++;
}

static void process_start_stage(ant_process_run_t *run, size_t index, const int base[ANT_PROCESS_FD_LIMIT], bool base_ok) {
  ant_process_plan_stage_t *stage = &run->stages[index];
  stage->run = run;
  stage->index = index;
  ant_process_stage_init(&stage->process, stage, process_stage_exit, process_stage_closed);

  int table[ANT_PROCESS_FD_LIMIT];
  process_stage_table(run, index, base, table);

  ant_process_plan_command_t *command = &run->plan.commands[index];
  if (!base_ok || !process_apply_fds(run, &command->fds, table)) {
    run->statuses[index] = 1;
    return;
  }

  if (command->kind == ANT_PROCESS_PLAN_COMMAND_NATIVE) process_native_stage_start(run, stage, table[1], table[2]);
  else process_spawn_external(run, stage, table);
}

static void process_report_pids(ant_process_run_t *run) {
  if (!run->plan.has_spawned) return;

  size_t count = run->plan.command_count;
  int *pids = calloc(count ? count : 1, sizeof(*pids));
  if (pids) for (size_t i = 0; i < count; i++) pids[i] = (int)ant_process_stage_pid(&run->stages[i].process);

  process_plan_resolve_spawned(run->js, &run->plan, pids, pids ? count : 0);
  free(pids);
}

static void process_spawn(ant_process_run_t *run) {
  run->spawned = true;
  if (!process_start_captures(run) || !process_make_pipeline(run)) return;

  size_t count = run->plan.command_count;
  run->stages = calloc(count, sizeof(*run->stages));
  if (!run->stages) {
    process_fail_all(run, "process plan", UV_ENOMEM);
    return;
  }

  int base[ANT_PROCESS_FD_LIMIT];
  for (int fd = 0; fd < ANT_PROCESS_FD_LIMIT; fd++) base[fd] = -1;
  bool base_ok = process_apply_fds(run, &run->plan.base, base);

  run->remaining_stages = 0;
  run->spawning = true;

  ant_process_limits_t limits;
  process_limits_apply(run, &limits);
  for (size_t i = 0; i < count; i++) process_start_stage(run, i, base, base_ok);
  process_limits_restore(run, &limits);

  process_close_owned_fds(run);
  process_report_pids(run);

  run->spawning = false;
  process_run_try_finish(run);
}

static ant_process_fd_op_t *process_current_open(ant_process_run_t *run) {
  return &process_run_fd_list(run, run->open_list)->items[run->open_index];
}

static void process_open_done(uv_fs_t *request) {
  ant_process_run_t *run = request->data;
  ant_process_fd_op_t *op = process_current_open(run);
  ssize_t result = request->result;
  uv_fs_req_cleanup(request);

  if (result < 0) op->error = (int)result;
  else op->opened = (int)result;

  run->open_index++;
  process_open_next(run);
}

static bool process_open_skipped(ant_process_fd_ops_t *ops, size_t index) {
  if (ops->items[index].kind > ANT_PROCESS_FD_OPEN_EXCLUSIVE) return true;
  for (size_t i = 0; i < index; i++) if (ops->items[i].error) return true;
  return false;
}

static bool process_open_start(ant_process_run_t *run, ant_process_fd_op_t *op) {
  int flags = ant_process_open_flags(op->kind);

  if (run->plan.umask >= 0) {
    int fd = ant_process_open_sync(op->path, flags, run->plan.umask);
    if (fd < 0) op->error = fd;
    else op->opened = fd;
    return false;
  }

  run->open_request.data = run;
  int rc = uv_fs_open(run->js->loop, &run->open_request, op->path, flags, 0666, process_open_done);
  if (rc >= 0) return true;

  uv_fs_req_cleanup(&run->open_request);
  op->error = rc;

  return false;
}

static void process_open_next(ant_process_run_t *run) {
  while (run->open_list <= run->plan.command_count) {
    ant_process_fd_ops_t *ops = process_run_fd_list(run, run->open_list);
    if (run->open_index >= ops->count) {
      run->open_list++;
      run->open_index = 0;
      continue;
    }

    if (!process_open_skipped(ops, run->open_index) && process_open_start(run, &ops->items[run->open_index])) return;
    run->open_index++;
  }

  process_spawn(run);
}

static void process_run_free(ant_process_run_t *run) {
  if (!run) return;
  process_plan_resolve_spawned(run->js, &run->plan, NULL, 0);
  process_close_owned_fds(run);

  if (run->captures) {
    for (size_t i = 0; i < run->plan.capture_count; i++) {
      process_close_fd(&run->captures[i].fds[0]);
      free(run->captures[i].bytes.data);
    }
  }
  free(run->captures);

  while (run->feeders) {
    ant_process_feeder_t *next = run->feeders->next;
    free(run->feeders->chunk);
    free(run->feeders);
    run->feeders = next;
  }

  if (run->stages) {
    for (size_t i = 0; i < run->plan.command_count; i++) free(run->stages[i].native);
  }

  free(run->pipeline_fds);
  free(run->stages);
  free(run->statuses);
  ant_process_plan_dispose(&run->plan);
  free(run->diagnostics.data);
  free(run);
}

static ant_value_t process_make_bytes(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return Ant_Error_CreateOutOfMemory(js);
  if (len) memcpy(buffer->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.buffer_proto);
}

static ant_value_t process_make_captures(ant_t *js, ant_process_run_t *run) {
  ant_value_t captures = Ant_Array_Create(js);
  if (is_err(captures)) return captures;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, captures);

  for (size_t i = 0; i < run->plan.capture_count; i++) {
    ant_process_bytes_t *bytes = &run->captures[i].bytes;
    ant_value_t value = process_make_bytes(js, bytes->data, bytes->len);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return value;
    }
    js_arr_push(js, captures, value);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return captures;
}

static ant_value_t process_make_statuses(ant_t *js, ant_process_run_t *run) {
  ant_value_t statuses = Ant_Array_Create(js);
  if (is_err(statuses)) return statuses;
  for (size_t i = 0; i < run->plan.command_count; i++) js_arr_push(js, statuses, Ant_Value_FromNumber((double)run->statuses[i]));
  return statuses;
}

static ant_value_t process_plan_make_result(ant_t *js, ant_process_run_t *run) {
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t result = Ant_Object_CreateBare(js);
  if (is_err(result)) goto failed;
  GC_ROOT_PIN(js, result);

  ant_value_t captures = process_make_captures(js, run);
  if (is_err(captures)) goto failed;
  GC_ROOT_PIN(js, captures);

  ant_value_t statuses = process_make_statuses(js, run);
  if (is_err(statuses)) goto failed;
  GC_ROOT_PIN(js, statuses);

  ant_value_t diagnostics = js_mkstr(js, run->diagnostics.data ? run->diagnostics.data : "", run->diagnostics.len);
  if (is_err(diagnostics)) goto failed;
  GC_ROOT_PIN(js, diagnostics);

  const char *signal_name = process_signal_name(run->final_signal);
  js_set(js, result, "captures", captures);
  js_set(js, result, "statuses", statuses);
  js_set(js, result, "diagnostics", diagnostics);
  js_set(js, result, "signalCode", signal_name ? js_mkstr(js, signal_name, strlen(signal_name)) : Ant_Value_Null);

  GC_ROOT_RESTORE(js, root_mark);
  return result;

failed: {
    ant_value_t error = Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Error_CreateOutOfMemory(js);
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
}

static bool process_run_busy(ant_process_run_t *run) {
  if (run->settled || !run->spawned || run->spawning || run->remaining_stages != 0) return true;

  for (size_t i = 0; run->stages && i < run->plan.command_count; i++) {
    if (run->stages[i].process.handle_open && !run->stages[i].process.closed) return true;
  }

  for (size_t i = 0; run->captures && i < run->plan.capture_count; i++) {
    if (run->captures[i].initialized && !run->captures[i].closed) return true;
  }

  for (ant_process_feeder_t *feeder = run->feeders; feeder; feeder = feeder->next) {
    if (!feeder->closed) return true;
  }

  return false;
}

static void process_run_try_finish(ant_process_run_t *run) {
  if (!run || process_run_busy(run)) return;
  run->settled = true;

  ant_t *js = run->js;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, run->promise);

  ant_value_t result = process_plan_make_result(js, run);
  if (is_err(result)) {
    ant_value_t error = js_take_thrown(js, result);
    GC_ROOT_PIN(js, error);
    js_reject_promise(js, run->promise, error);
  } else js_resolve_promise(js, run->promise, result);

  GC_ROOT_RESTORE(js, root_mark);
  gc_unroot_pending_promise(js, js_obj_ptr(run->promise));
  process_run_free(run);
}

ant_value_t ant_process_plan_submit(ant_t *js, ant_process_plan_t *plan) {
  if (!plan || plan->command_count == 0) {
    ant_value_t error = js_mkerr(js, "invalid process plan");
    if (plan) ant_process_plan_release(js, plan);
    return ant_process_plan_rejected_result(js, error);
  }

  ant_process_run_t *run = calloc(1, sizeof(*run));
  if (!run) {
    ant_value_t error = Ant_Error_CreateOutOfMemory(js);
    ant_process_plan_release(js, plan);
    return ant_process_plan_rejected_result(js, error);
  }

  run->js = js;
  run->plan = *plan;
  *plan = (ant_process_plan_t){ .umask = -1 };
  run->statuses = calloc(run->plan.command_count, sizeof(*run->statuses));
  run->promise = js_mkpromise(js);

  if (!run->statuses || is_err(run->promise)) {
    ant_value_t error = is_err(run->promise) ? run->promise : Ant_Error_CreateOutOfMemory(js);
    process_run_free(run);
    return ant_process_plan_rejected_result(js, error);
  }

  ant_value_t promise = run->promise;
  gc_root_pending_promise(js, js_obj_ptr(promise));
  process_open_next(run);

  return promise;
}
