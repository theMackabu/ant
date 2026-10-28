#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <utarray.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "ant.h"
#include "cfunc.h"
#include "list.h"
#include "ptr.h"
#include "errors.h"
#include "modules/timer.h"
#include "internal.h"
#include "utils.h"

#include "esm/loader.h"
#include "gc/modules.h"
#include "gc/roots.h"
#include "silver/call.h"

#include "process_stage.h"
#include "modules/net.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/process.h"
#include "modules/stream.h"
#include "modules/child_process.h"

typedef enum {
  CHILD_STREAM_STDIN = 0,
  CHILD_STREAM_STDOUT = 1,
  CHILD_STREAM_STDERR = 2,
} child_stream_kind_t;

typedef struct {
  child_process_t *cp;
  child_stream_kind_t kind;
} child_stream_ctx_t;

typedef struct 
  child_write_req_s child_write_req_t;

struct child_write_req_s {
  uv_write_t req;
  child_process_t *cp;
  ant_value_t callback;
  char *data;
  child_write_req_t *next;
  child_write_req_t *prev;
};

typedef enum {
  STDIO_PIPE = 0,
  STDIO_INHERIT,
  STDIO_IGNORE,
  STDIO_FD,
} stdio_mode_t;

static constexpr uint8_t CHILD_STDIO_MAX = 64;
static constexpr uint8_t SYNC_ENCODING_CAP = 16;

typedef struct {
  stdio_mode_t mode[CHILD_STDIO_MAX];
  int fd[CHILD_STDIO_MAX];
  int count;
} stdio_spec_t;

struct child_process_s {
  ant_t *js;
  ant_process_stage_t process;
  uv_pipe_t stdin_pipe;
  uv_pipe_t stdout_pipe;
  uv_pipe_t stderr_pipe;
  ant_value_t child_obj;
  ant_value_t stdin_obj;
  ant_value_t stdout_obj;
  ant_value_t stderr_obj;
  child_stream_ctx_t *stdin_ctx;
  child_stream_ctx_t *stdout_ctx;
  child_stream_ctx_t *stderr_ctx;
  ant_value_t promise;
  ant_value_t exec_callback;
  ant_value_t exec_command;
  ant_value_t exec_spawn_error;
  ant_value_t spawnargs;
  char *stdout_buf;
  size_t stdout_len;
  size_t stdout_cap;
  char *stderr_buf;
  size_t stderr_len;
  size_t stderr_cap;
  int64_t exit_code;
  int term_signal;
  bool exited;
  bool stdout_closed;
  bool stderr_closed;
  bool collect_output;
  bool stdout_read_paused;
  bool stderr_read_paused;
  bool stdin_closed;
  bool use_shell;
  bool close_emitted;
  bool keep_alive;
  int pending_closes;
  int spawn_error;
  char *spawn_file;
  child_write_req_t *pending_writes;
  char *cwd;
  stdio_mode_t stdio_modes[3];
  bool end_stdin_when_writes_finish;
  bool suppress_stdin_errors;
  child_process_t *next;
  child_process_t **pprev;
};

static ant_value_t child_end_impl(child_process_t *cp);
static void try_free_child(child_process_t *cp);

static ant_value_t child_write_impl(
  ant_t *js,
  child_process_t *cp,
  ant_value_t data_arg,
  ant_value_t callback
);

static void close_child_pipe(
  child_process_t *cp,
  child_stream_kind_t kind,
  bool stop_read
);

static constexpr uint32_t CHILD_PROCESS_NATIVE_TAG = Ant_NativeTag("CPRC");
static constexpr uint32_t CHILD_STREAM_NATIVE_TAG = Ant_NativeTag("CSTM");

static child_process_t *get_child_process(ant_value_t obj) {
  return (child_process_t *)js_get_native(obj, CHILD_PROCESS_NATIVE_TAG);
}

static child_stream_ctx_t *get_child_stream_ctx(ant_value_t obj) {
  return (child_stream_ctx_t *)js_get_native(obj, CHILD_STREAM_NATIVE_TAG);
}

static void child_stream_call_callback(
  ant_t *js,
  ant_value_t callback,
  ant_value_t *args,
  int nargs
);

static void emit_event(child_process_t *cp, const char *name, ant_value_t *args, int nargs) {
  if (vtype(cp->child_obj) != kTypeObject) return;
  eventemitter_emit_args(cp->js, cp->child_obj, name, args, nargs);
}

static ant_value_t child_stream_obj(child_process_t *cp, child_stream_kind_t kind) {
switch (kind) {
  case CHILD_STREAM_STDIN: return cp->stdin_obj;
  case CHILD_STREAM_STDOUT: return cp->stdout_obj;
  case CHILD_STREAM_STDERR: return cp->stderr_obj;
  default: return js_mkundef();
}}

static ant_value_t make_buffer_chunk(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static uv_pipe_t *child_pipe(child_process_t *cp, child_stream_kind_t kind) {
switch (kind) {
  case CHILD_STREAM_STDIN: return &cp->stdin_pipe;
  case CHILD_STREAM_STDOUT: return &cp->stdout_pipe;
  case CHILD_STREAM_STDERR: return &cp->stderr_pipe;
  default: return NULL;
}}

static bool child_stdio_is_pipe(child_process_t *cp, child_stream_kind_t kind) {
  return cp->stdio_modes[kind] == STDIO_PIPE;
}

static bool *child_closed_flag(child_process_t *cp, child_stream_kind_t kind) {
  switch (kind) {
    case CHILD_STREAM_STDIN: return &cp->stdin_closed;
    case CHILD_STREAM_STDOUT: return &cp->stdout_closed;
    case CHILD_STREAM_STDERR: return &cp->stderr_closed;
    default: return NULL;
  }
}

static void free_child_process(child_process_t *cp) {
  if (!cp) return;

  if (vtype(cp->child_obj) == kTypeObject) {
    js_set_slot(cp->child_obj, SLOT_DATA, js_mkundef());
    js_clear_native(cp->child_obj, CHILD_PROCESS_NATIVE_TAG);
  }
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    ant_value_t obj = child_stream_obj(cp, i);
    if (vtype(obj) != kTypeObject) continue;
    js_set_slot(obj, SLOT_DATA, js_mkundef());
    js_clear_native(obj, CHILD_STREAM_NATIVE_TAG);
  }
  
  if (cp->stdout_buf) free(cp->stdout_buf);
  if (cp->stderr_buf) free(cp->stderr_buf);
  if (cp->cwd) free(cp->cwd);
  if (cp->spawn_file) free(cp->spawn_file);
  if (cp->stdin_ctx) free(cp->stdin_ctx);
  if (cp->stdout_ctx) free(cp->stdout_ctx);
  if (cp->stderr_ctx) free(cp->stderr_ctx);
  
  free(cp);
}

static void try_free_child(child_process_t *cp) {
  if (!cp) return;

  if (cp->exited && cp->stdout_closed && cp->stderr_closed &&
    (!cp->process.handle_open || cp->process.closed) &&
    cp->pending_closes == 0 && !cp->pending_writes) {
    Ant_List_Remove(cp);
    free_child_process(cp);
  }
}

static ant_value_t child_signal_value(ant_t *js, int signal) {
  const char *name;
  if (signal == 0) return js_mknull();

  name = process_signal_name(signal);
  return name ? js_mkstr(js, name, strlen(name)) : js_mknull();
}

static ant_value_t child_exit_code_value(child_process_t *cp) {
  return cp->term_signal
    ? js_mknull()
    : js_mknum((double)cp->exit_code);
}

static void check_completion(child_process_t *cp) {
  if (cp->exited && cp->stdout_closed && cp->stderr_closed && !cp->close_emitted) {
    cp->close_emitted = true;
    
    ant_value_t exit_code_val = child_exit_code_value(cp);
    ant_value_t signal_val = child_signal_value(cp->js, cp->term_signal);

    js_set(cp->js, cp->child_obj, "exitCode", exit_code_val);
    js_set(cp->js, cp->child_obj, "signalCode", signal_val);
    
    ant_value_t close_args[2] = { exit_code_val, signal_val };
    emit_event(cp, "close", close_args, 2);
    
    if (vtype(cp->promise) != kTypeUndefined) {
      ant_value_t stdout_val = js_mkstr_utf8(cp->js, cp->stdout_buf ? cp->stdout_buf : "", cp->stdout_len);
      ant_value_t stderr_val = js_mkstr_utf8(cp->js, cp->stderr_buf ? cp->stderr_buf : "", cp->stderr_len);
      ant_value_t result = Ant_Object_CreateBare(cp->js);
      
      js_set(cp->js, result, "stdout", stdout_val);
      js_set(cp->js, result, "stderr", stderr_val);
      js_set(cp->js, result, "exitCode", exit_code_val);
      js_set(cp->js, result, "signalCode", signal_val);
      
      if (cp->exit_code != 0 || cp->term_signal != 0) {
        char err_msg[256];

        if (cp->term_signal != 0) {
          const char *signal_name = process_signal_name(cp->term_signal);
          snprintf(
            err_msg, sizeof(err_msg), "Command failed with signal %s",
            signal_name ? signal_name : "unknown"
          );
        } else snprintf(
          err_msg, sizeof(err_msg), "Command failed with exit code %lld",
          (long long)cp->exit_code
        );
        
        js_reject_promise(
          cp->js, cp->promise,
          js_mkstr(cp->js, err_msg, strlen(err_msg))
        );
      } else js_resolve_promise(cp->js, cp->promise, result);
    }
    
    try_free_child(cp);
  }
}

static ant_value_t child_syscall_error(ant_t *js, int uv_code, const char *syscall, const char *path) {
  const char *code = uv_err_name(uv_code);

  char message[256];
  snprintf(message, sizeof(message), "%s %s", syscall, code);

  ant_value_t error = Ant_Error_Create(js, JS_ERR_GENERIC, message);
  if (!is_object_type(error)) return error;

  js_set(js, error, "errno", js_mknum((double)uv_code));
  js_set(js, error, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, error, "syscall", js_mkstr(js, syscall, strlen(syscall)));
  if (path) js_set(js, error, "path", js_mkstr(js, path, strlen(path)));

  return error;
}

static ant_value_t child_spawnargs_value(ant_t *js, char *const *argv) {
  ant_value_t spawnargs = js_mkarr(js);
  for (int i = 0; argv[i]; i++) js_arr_push(js, spawnargs, js_mkstr(js, argv[i], strlen(argv[i])));
  return spawnargs;
}

static ant_value_t child_spawn_failure_cb(ant_params_t) {
  ant_value_t child = js_func_data(js->current_func);
  child_process_t *cp = get_child_process(child);
  if (!cp) return js_mkundef();

  const char *file = cp->spawn_file ? cp->spawn_file : "";

  char syscall[224];
  snprintf(syscall, sizeof(syscall), "spawn %s", file);
  ant_value_t error = child_syscall_error(js, cp->spawn_error, syscall, file);
  if (is_object_type(error) && is_object_type(cp->spawnargs)) {
    ant_value_t spawnargs = js_mkarr(js);
    ant_offset_t len = js_arr_len(js, cp->spawnargs);
    for (ant_offset_t i = 1; i < len; i++) js_arr_push(js, spawnargs, js_arr_get(js, cp->spawnargs, i));
    js_set(js, error, "spawnargs", spawnargs);
  }

  ant_value_t error_args[1] = { error };
  emit_event(cp, "error", error_args, 1);

  cp->exited = true;
  cp->exit_code = cp->spawn_error;
  cp->term_signal = 0;
  check_completion(cp);

  return js_mkundef();
}

static void child_schedule_spawn_failure(ant_t *js, child_process_t *cp) {
  ant_value_t callback = js_heavy_mkfun(js, child_spawn_failure_cb, cp->child_obj);
  queue_microtask(js, callback);
}

static void on_handle_close(uv_handle_t *handle) {
  child_process_t *cp = (child_process_t *)handle->data;
  if (cp) {
    cp->pending_closes--;
    try_free_child(cp);
  }
}

static void close_child_handle(child_process_t *cp, uv_handle_t *handle) {
  if (!cp || !handle || uv_is_closing(handle)) return;
  cp->pending_closes++;
  uv_close(handle, on_handle_close);
}

static void close_child_pipe(child_process_t *cp, child_stream_kind_t kind, bool stop_read) {
  bool *closed = NULL;
  uv_pipe_t *pipe = NULL;

  if (!cp || !child_stdio_is_pipe(cp, kind)) return;

  closed = child_closed_flag(cp, kind);
  pipe = child_pipe(cp, kind);
  if (!closed || !pipe || *closed || uv_is_closing((uv_handle_t *)pipe)) return;

  if (stop_read && kind != CHILD_STREAM_STDIN) {
    uv_read_stop((uv_stream_t *)pipe);
  }

  *closed = true;

  if (kind != CHILD_STREAM_STDIN) {
    ant_value_t obj = child_stream_obj(cp, kind);
    if (vtype(obj) == kTypeObject) stream_readable_push(cp->js, obj, js_mknull(), js_mkundef());
  }

  close_child_handle(cp, (uv_handle_t *)pipe);
}

static void on_process_exit(
  ant_process_stage_t *process, int64_t exit_status, int term_signal
) {
  child_process_t *cp = process ? process->owner : NULL;
  if (!cp) return;
  cp->exit_code = exit_status;
  cp->term_signal = term_signal;
  cp->exited = true;

  ant_value_t exit_code_val = child_exit_code_value(cp);
  ant_value_t signal_val = child_signal_value(cp->js, term_signal);
  js_set(cp->js, cp->child_obj, "exitCode", exit_code_val);
  js_set(cp->js, cp->child_obj, "signalCode", signal_val);

  ant_value_t exit_args[2] = { exit_code_val, signal_val };
  emit_event(cp, "exit", exit_args, 2);

  close_child_pipe(cp, CHILD_STREAM_STDIN, false);
  
  check_completion(cp);
}

static void on_process_close(ant_process_stage_t *process) {
  child_process_t *cp = process ? process->owner : NULL;
  if (cp) try_free_child(cp);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
#ifdef _WIN32
  buf->len = buf->base ? (ULONG)(suggested_size > (size_t)ULONG_MAX ? ULONG_MAX : suggested_size) : 0;
#else
  buf->len = buf->base ? suggested_size : 0;
#endif
}

static void on_child_read(
  uv_stream_t *stream,
  child_stream_kind_t kind,
  ssize_t nread,
  const uv_buf_t *buf
) {
  child_process_t *cp = (child_process_t *)stream->data;
  bool is_stdout = kind == CHILD_STREAM_STDOUT;
  char **acc = is_stdout ? &cp->stdout_buf : &cp->stderr_buf;
  
  size_t *acc_len = is_stdout ? &cp->stdout_len : &cp->stderr_len;
  size_t *acc_cap = is_stdout ? &cp->stdout_cap : &cp->stderr_cap;
  ant_value_t obj = child_stream_obj(cp, kind);

  if (nread > 0) {
    if (cp->collect_output && *acc_len + nread > *acc_cap) {
      size_t new_cap = *acc_cap == 0 ? 4096 : *acc_cap * 2;
      while (new_cap < *acc_len + nread) new_cap *= 2;
      char *new_buf = realloc(*acc, new_cap);
      if (new_buf) {
        *acc = new_buf;
        *acc_cap = new_cap;
      }
    }
    if (cp->collect_output && *acc && *acc_len + (size_t)nread <= *acc_cap) {
      memcpy(*acc + *acc_len, buf->base, nread);
      *acc_len += nread;
    }

    if (vtype(obj) == kTypeObject) {
      ant_value_t chunk = make_buffer_chunk(cp->js, buf->base, (size_t)nread);
      if (is_err(chunk)) {
        GC_ROOT_SAVE(root_mark, cp->js);
        chunk = js_take_thrown(cp->js, chunk);

        GC_ROOT_PIN(cp->js, chunk);
        eventemitter_emit_args(cp->js, obj, "error", &chunk, 1);

        GC_ROOT_RESTORE(cp->js, root_mark);
        close_child_pipe(cp, kind, true);
        check_completion(cp);

        if (buf->base) free(buf->base);
        return;
      }

      ant_value_t accepted = stream_readable_push(cp->js, obj, chunk, js_mkundef());

      if (!js_truthy(cp->js, accepted)) {
        uv_read_stop(stream);
        if (is_stdout) cp->stdout_read_paused = true;
        else cp->stderr_read_paused = true;
      }
    }
  }

  if (buf->base) free(buf->base);

  if (nread < 0) {
    if (nread != UV_EOF) {
      ant_value_t err_args[1] = { js_mkstr(cp->js, uv_strerror((int)nread), (int)strlen(uv_strerror((int)nread))) };
      if (is_stdout) emit_event(cp, "error", err_args, 1);
      if (vtype(obj) == kTypeObject) eventemitter_emit_args(cp->js, obj, "error", err_args, 1);
    }

    close_child_pipe(cp, kind, true);
    check_completion(cp);
  }
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  on_child_read(stream, CHILD_STREAM_STDOUT, nread, buf);
}

static void on_stderr_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  on_child_read(stream, CHILD_STREAM_STDERR, nread, buf);
}

static ant_value_t child_kill(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  
  child_process_t *cp = get_child_process(this_obj);
  if (!cp) return js_false;
  if (cp->exited) return js_false;
  
  int sig = SIGTERM;
  if (nargs > 0) {
    if (vtype(args[0]) == kTypeNumber) {
      sig = (int)js_getnum(args[0]);
    } else if (vtype(args[0]) == kTypeString) {
      size_t sig_len;
      char *sig_str = js_getstr(js, args[0], &sig_len);
      if (sig_len == 7 && strncmp(sig_str, "SIGTERM", 7) == 0) sig = SIGTERM;
      else if (sig_len == 7 && strncmp(sig_str, "SIGKILL", 7) == 0) sig = SIGKILL;
      else if (sig_len == 6 && strncmp(sig_str, "SIGINT", 6) == 0) sig = SIGINT;
      else if (sig_len == 6 && strncmp(sig_str, "SIGHUP", 6) == 0) sig = SIGHUP;
      else if (sig_len == 7 && strncmp(sig_str, "SIGQUIT", 7) == 0) sig = SIGQUIT;
    }
  }
  
  int result = ant_process_stage_kill(&cp->process, sig);
  if (result == 0) js_set(js, this_obj, "killed", js_true);
  return js_bool(result == 0);
}

static ant_value_t child_dispose(ant_params_t) {
  if (js_truthy(js, js_get(js, js_getthis(js), "killed"))) return js_mkundef();
  child_kill(js, NULL, 0, js_mkundef());
  return js_mkundef();
}

static void child_write_link(child_process_t *cp, child_write_req_t *write) {
  write->prev = NULL;
  write->next = cp->pending_writes;
  if (cp->pending_writes) cp->pending_writes->prev = write;
  cp->pending_writes = write;
}

static void child_write_unlink(child_write_req_t *write) {
  child_process_t *cp = write->cp;

  if (write->prev) write->prev->next = write->next;
  else if (cp && cp->pending_writes == write) cp->pending_writes = write->next;
  if (write->next) write->next->prev = write->prev;

  write->next = NULL;
  write->prev = NULL;
}

static void on_child_write_done(uv_write_t *req, int status) {
  child_write_req_t *write = (child_write_req_t *)req->data;
  child_process_t *cp = NULL;
  ant_value_t callback_args[1];

  if (!write) return;
  cp = write->cp;

  if (is_callable(write->callback)) {
    if (status < 0) {
      callback_args[0] = Ant_Error_Create(
        write->cp->js, JS_ERR_TYPE, uv_strerror(status)
      );
      child_stream_call_callback(
        write->cp->js, write->callback, callback_args, 1
      );
    } else child_stream_call_callback(
      write->cp->js, write->callback, NULL, 0
    );
  } else if (status < 0 && write->cp && !write->cp->suppress_stdin_errors &&
             vtype(write->cp->stdin_obj) == kTypeObject) {
    callback_args[0] = Ant_Error_Create(
      write->cp->js, JS_ERR_TYPE, uv_strerror(status)
    );
    eventemitter_emit_args(
      write->cp->js, write->cp->stdin_obj,
      "error", callback_args, 1
    );
  }

  child_write_unlink(write);
  free(write->data);
  free(write);
  if (cp && !cp->pending_writes && cp->end_stdin_when_writes_finish) {
    cp->end_stdin_when_writes_finish = false;
    (void)child_end_impl(cp);
  }
  try_free_child(cp);
}

static ant_value_t child_write_impl(
  ant_t *js,
  child_process_t *cp,
  ant_value_t data_arg,
  ant_value_t callback
) {
  if (cp->stdin_closed) return js_false;
  
  const char *data = NULL;
  size_t data_len = 0;
  
  if (vtype(data_arg) == kTypeString) {
    data = js_getstr(js, data_arg, &data_len);
    if (!data) return js_mkerr(js, "Data must be a string or Buffer");
  } else {
    TypedArrayData *ta_data = buffer_get_typedarray_data(data_arg);
    uint8_t *bytes;
    if (!Ant_Buffer_GetViewBytes(ta_data, &bytes, &data_len)) {
      return js_mkerr(js, "Data must be a string or Buffer");
    }
    data = (const char *)bytes;
  }

  child_write_req_t *write = calloc(1, sizeof(*write));
  char *buf_data = data_len ? malloc(data_len) : NULL;
  uv_buf_t buf;
  int result = 0;

  if (!write || (data_len && !buf_data)) {
    free(write); free(buf_data);
    return Ant_Error_CreateOutOfMemory(js);
  }

  if (data_len) memcpy(buf_data, data, data_len);
  write->cp = cp;
  write->callback = callback;
  write->data = buf_data;
  write->req.data = write;

  buf = uv_buf_init(buf_data, (unsigned int)data_len);
  result = uv_write(
    &write->req,
    (uv_stream_t *)&cp->stdin_pipe,
    &buf, 1,
    on_child_write_done
  );

  if (result < 0) {
    free(buf_data);
    free(write);
    return js_mkerr(js, "%s", uv_strerror(result));
  }

  child_write_link(cp, write);
  return js_true;
}

static ant_value_t child_end_impl(child_process_t *cp) {
  close_child_pipe(cp, CHILD_STREAM_STDIN, false);
  return js_mkundef();
}

static ant_value_t child_ref(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  child_process_t *cp = get_child_process(this_obj);
  
  if (!cp) return this_obj;
  cp->keep_alive = true;
  
  ant_process_stage_ref(&cp->process);
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    uv_handle_t *h = (uv_handle_t *)child_pipe(cp, i);
    if (child_stdio_is_pipe(cp, i) && !uv_is_closing(h)) uv_ref(h);
  }
  
  return this_obj;
}

static ant_value_t child_unref(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  child_process_t *cp = get_child_process(this_obj);
  
  if (!cp) return this_obj;
  cp->keep_alive = false;
  
  ant_process_stage_unref(&cp->process);
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    uv_handle_t *h = (uv_handle_t *)child_pipe(cp, i);
    if (child_stdio_is_pipe(cp, i) && !uv_is_closing(h)) uv_unref(h);
  }
  
  return this_obj;
}

static ant_value_t child_process_ctor(ant_params_t) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_object_type(js->builtins.child_process_proto)) js_set_proto_init(obj, js->builtins.child_process_proto);

  js_set(js, obj, "pid", js_mkundef());
  js_set(js, obj, "exitCode", js_mknull());
  js_set(js, obj, "signalCode", js_mknull());
  js_set(js, obj, "killed", js_false);
  js_set(js, obj, "connected", js_false);
  js_set(js, obj, "stdin", js_mknull());
  js_set(js, obj, "stdout", js_mknull());
  js_set(js, obj, "stderr", js_mknull());

  return obj;
}

static void child_process_init_constructor(ant_t *js) {
  if (js->builtins.child_process_ctor && js->builtins.child_process_proto) return;
  ant_value_t ee_proto = eventemitter_prototype(js);

  js->builtins.child_process_proto = Ant_Object_CreateBare(js);
  if (is_object_type(ee_proto)) js_set_proto_init(js->builtins.child_process_proto, ee_proto);
  
  js_set(js, js->builtins.child_process_proto, "kill", Ant_Function_CreateWithArity(child_kill, 1));
  js_set(js, js->builtins.child_process_proto, "ref", Ant_Function_Create(child_ref));
  js_set(js, js->builtins.child_process_proto, "unref", Ant_Function_Create(child_unref));
  js_set_sym(js, js->builtins.child_process_proto, js->sym.dispose_sym, Ant_Function_Create(child_dispose));

  js->builtins.child_process_ctor = Ant_Function_CreateConstructor(js, child_process_ctor, js->builtins.child_process_proto, "ChildProcess", 0);
}

static uv_handle_t *child_stream_handle(child_process_t *cp, child_stream_kind_t kind) {
  if (!cp) return NULL;
  switch (kind) {
    case CHILD_STREAM_STDIN: return (uv_handle_t *)&cp->stdin_pipe;
    case CHILD_STREAM_STDOUT: return (uv_handle_t *)&cp->stdout_pipe;
    case CHILD_STREAM_STDERR: return (uv_handle_t *)&cp->stderr_pipe;
    default: return NULL;
  }
}

static ant_value_t child_stream_ref(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);
  if (!ctx || !ctx->cp) return this_obj;

  uv_handle_t *h = child_stream_handle(ctx->cp, ctx->kind);
  if (h && !uv_is_closing(h)) uv_ref(h);
  return this_obj;
}

static ant_value_t child_stream_unref(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);
  if (!ctx || !ctx->cp) return this_obj;

  uv_handle_t *h = child_stream_handle(ctx->cp, ctx->kind);
  if (h && !uv_is_closing(h)) uv_unref(h);
  return this_obj;
}

static void child_stream_call_callback(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs) {
  if (!is_callable(callback)) return;
  sv_vm_call(js->vm, js, callback, js_mkundef(), args, nargs, NULL, js_mkundef());
}

static void child_stream_close(child_stream_ctx_t *ctx) {
  child_process_t *cp = ctx->cp;
  uv_handle_t *h = NULL;

  if (ctx->kind == CHILD_STREAM_STDIN) {
    (void)child_end_impl(cp);
    return;
  }

  h = child_stream_handle(cp, ctx->kind);
  if (h && !uv_is_closing(h)) {
    close_child_pipe(cp, ctx->kind, true);
    check_completion(cp);
  }
}

static ant_value_t child_stream__destroy(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);

  if (ctx && ctx->cp) child_stream_close(ctx);
  child_stream_call_callback(js, callback, &error, 1);
  return js_mkundef();
}

static ant_value_t child_stream__read(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);
  child_process_t *cp = NULL;
  bool *paused = NULL;
  uv_read_cb cb = NULL;

  if (!ctx || !ctx->cp) return js_mkundef();
  cp = ctx->cp;

  if (ctx->kind == CHILD_STREAM_STDOUT) { paused = &cp->stdout_read_paused; cb = on_stdout_read; }
  else if (ctx->kind == CHILD_STREAM_STDERR) { paused = &cp->stderr_read_paused; cb = on_stderr_read; }
  else return js_mkundef();

  if (*paused && !*child_closed_flag(cp, ctx->kind)) {
    *paused = false;
    uv_read_start((uv_stream_t *)child_pipe(cp, ctx->kind), alloc_buffer, cb);
  }

  return js_mkundef();
}

static ant_value_t child_stream__write(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);
  ant_value_t result = 0;

  if (!ctx || !ctx->cp) return js_mkerr(js, "Invalid stream context");
  result = child_write_impl(
    js, ctx->cp,
    nargs > 0 ? args[0] : js_mkundef(),
    callback
  );
  if (is_err(result)) return result;
  if (result == js_false) return js_mkerr(js, "write after end");

  return js_mkundef();
}

static ant_value_t child_stream__final(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);

  if (ctx && ctx->cp) (void)child_end_impl(ctx->cp);
  child_stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static void child_stream_init_protos(ant_t *js) {
  if (js->builtins.child_readable_proto && js->builtins.child_writable_proto) return;

  js->builtins.child_readable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.child_readable_proto, stream_readable_prototype(js));
  js_set(js, js->builtins.child_readable_proto, "ref", Ant_Function_Create(child_stream_ref));
  js_set(js, js->builtins.child_readable_proto, "unref", Ant_Function_Create(child_stream_unref));
  js_set(js, js->builtins.child_readable_proto, "_destroy", Ant_Function_Create(child_stream__destroy));
  js_set(js, js->builtins.child_readable_proto, "_read", Ant_Function_Create(child_stream__read));

  js->builtins.child_writable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.child_writable_proto, stream_writable_prototype(js));
  js_set(js, js->builtins.child_writable_proto, "ref", Ant_Function_Create(child_stream_ref));
  js_set(js, js->builtins.child_writable_proto, "unref", Ant_Function_Create(child_stream_unref));
  js_set(js, js->builtins.child_writable_proto, "_destroy", Ant_Function_Create(child_stream__destroy));
  js_set(js, js->builtins.child_writable_proto, "_write", Ant_Function_Create(child_stream__write));
  js_set(js, js->builtins.child_writable_proto, "_final", Ant_Function_Create(child_stream__final));
}

static ant_value_t create_child_stream_object(ant_t *js, child_process_t *cp, child_stream_kind_t kind) {
  bool writable = kind == CHILD_STREAM_STDIN;
  ant_value_t obj = Ant_Object_CreateBare(js);
  child_stream_ctx_t *ctx = calloc(1, sizeof(child_stream_ctx_t));
  if (!ctx) return Ant_Error_CreateOutOfMemory(js);
  
  ctx->cp = cp;
  ctx->kind = kind;
  
  if (kind == CHILD_STREAM_STDIN) cp->stdin_ctx = ctx;
  else if (kind == CHILD_STREAM_STDOUT) cp->stdout_ctx = ctx;
  else cp->stderr_ctx = ctx;

  child_stream_init_protos(js);
  js_set_proto_init(obj, writable ? js->builtins.child_writable_proto : js->builtins.child_readable_proto);
  js_set_native(obj, ctx, CHILD_STREAM_NATIVE_TAG);

  if (writable) stream_init_writable_object(js, obj, js_mkundef());
  else stream_init_readable_object(js, obj, js_mkundef());

  return obj;
}

static void abort_child_object_creation(child_process_t *cp) {
  cp->close_emitted = true;
  cp->promise = js_mkundef();

  if (cp->process.started) ant_process_stage_kill(&cp->process, SIGKILL);
  else cp->exited = true;

  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    *child_closed_flag(cp, i) = true;
    if (!child_stdio_is_pipe(cp, i)) continue;
    uv_pipe_t *pipe = child_pipe(cp, i);
    if (i != CHILD_STREAM_STDIN) uv_read_stop((uv_stream_t *)pipe);
    close_child_handle(cp, (uv_handle_t *)pipe);
  }

  try_free_child(cp);
}

static void child_set_stdio_array(ant_t *js, child_process_t *cp, int count) {
  ant_value_t stdio = js_mkarr(js);
  js_arr_push(js, stdio, cp->stdin_obj);
  js_arr_push(js, stdio, cp->stdout_obj);
  js_arr_push(js, stdio, cp->stderr_obj);
  for (int i = 3; i < count; i++) js_arr_push(js, stdio, js_mknull());
  js_set(js, cp->child_obj, "stdio", stdio);
}

static ant_value_t create_child_object(ant_t *js, child_process_t *cp) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  cp->child_obj = obj;

  if (is_object_type(js->builtins.child_process_proto))
    js_set_proto_init(obj, js->builtins.child_process_proto);
  
  js_set_native(obj, cp, CHILD_PROCESS_NATIVE_TAG);
  js_set(js, obj, "pid", js_mknum((double)ant_process_stage_pid(&cp->process)));
  js_set(js, obj, "exitCode", js_mknull());
  js_set(js, obj, "signalCode", js_mknull());
  js_set(js, obj, "killed", js_false);
  js_set(js, obj, "connected", js_true);

  static const struct { child_stream_kind_t kind; const char *name; } streams[] = {
    { CHILD_STREAM_STDIN, "stdin" },
    { CHILD_STREAM_STDOUT, "stdout" },
    { CHILD_STREAM_STDERR, "stderr" },
  };
  
  ant_value_t *stream_objs[] = { &cp->stdin_obj, &cp->stdout_obj, &cp->stderr_obj };
  for (int i = 0; i < 3; i++) {
    if (child_stdio_is_pipe(cp, streams[i].kind)) {
      ant_value_t stream_obj = create_child_stream_object(js, cp, streams[i].kind);
      if (is_err(stream_obj)) {
        abort_child_object_creation(cp);
        return stream_obj;
      }
      *stream_objs[i] = stream_obj;
    } else *stream_objs[i] = js_mknull();
    js_set(js, obj, streams[i].name, *stream_objs[i]);
  }
  
  child_set_stdio_array(js, cp, 3);
  if (is_object_type(cp->spawnargs)) {
    js_set(js, obj, "spawnfile", js_arr_get(js, cp->spawnargs, 0));
    js_set(js, obj, "spawnargs", cp->spawnargs);
  }
  js_set_tostringtag(js, obj, js_mkstr(js, "ChildProcess", 12));
  
  return obj;
}

static char **parse_args_array(ant_t *js, ant_value_t arr, int *count) {
  ant_value_t len_val = js_get(js, arr, "length");
  int len = (int)js_getnum(len_val);
  
  char **args = calloc(len + 1, sizeof(char *));
  if (!args) {
    *count = 0;
    return NULL;
  }
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    ant_value_t val = js_get(js, arr, idx);
    if (vtype(val) == kTypeString) {
      size_t arg_len;
      char *arg = js_getstr(js, val, &arg_len);
      if (arg && memchr(arg, '\0', arg_len)) {
        for (int j = 0; j < i; j++) free(args[j]);
        free(args);
        *count = -1;
        js_mkerr_typed(js, JS_ERR_TYPE, "Child process arguments cannot contain NUL bytes");
        return NULL;
      }
      args[i] = strndup(arg, arg_len);
    } else args[i] = strdup("");
  }
  
  args[len] = NULL;
  *count = len;
  return args;
}

static bool child_process_command_append(char **buf, size_t *len, size_t *cap, const char *text, size_t text_len) {
  if (text_len > SIZE_MAX - *len - 1) return false;
  size_t need = *len + text_len + 1;
  
  if (need > *cap) {
    size_t next = *cap > SIZE_MAX / 2 ? need : *cap * 2;
    if (next < need) next = need;
    
    char *grown = realloc(*buf, next);
    if (!grown) return false;
    
    *buf = grown;
    *cap = next;
  }
  
  if (text_len) memcpy(*buf + *len, text, text_len);
  *len += text_len;
  
  return true;
}

static ant_value_t child_process_command_value(ant_t *js, ant_value_t file, ant_value_t argv) {
  if (vtype(file) != kTypeString) return js_mkundef();

  size_t file_len = 0;
  char *file_text = js_getstr(js, file, &file_len);
  size_t len = 0, cap = file_len + 64;
  
  char *command = malloc(cap);
  bool ok = command && child_process_command_append(&command, &len, &cap, file_text, file_len);

  ant_offset_t argc = js_is_array(argv) ? js_arr_len(js, argv) : 0;
  for (ant_offset_t i = 0; ok && i < argc; i++) {
    ant_value_t arg = js_arr_get(js, argv, i);
    size_t arg_len = 0;
    
    char *arg_text = vtype(arg) == kTypeString ? js_getstr(js, arg, &arg_len) : NULL;
    ok = 
      child_process_command_append(&command, &len, &cap, " ", 1) && 
      child_process_command_append(&command, &len, &cap, arg_text, arg_len);
  }

  ant_value_t result = ok ? js_mkstr(js, command, len) : js_mkundef();
  free(command);
  
  return result;
}

static void free_args_array(char **args, int count) {
  if (!args) return;
  for (int i = 0; i < count; i++)
    if (args[i]) free(args[i]);
  free(args);
}

static ant_value_t child_process_options_arg(ant_value_t *args, int nargs) {
  if (nargs >= 3 && is_special_object(args[2])) return args[2];
  if (nargs >= 2 && !js_is_array(args[1]) && is_special_object(args[1])) return args[1];
  return js_mkundef();
}

static char **parse_env_object(ant_t *js, ant_value_t env_obj) {
  if (!is_special_object(env_obj)) return process_private_env_snapshot(js);

  ant_value_t keys = js_own_property_keys(js, env_obj, false, true);
  if (is_err(keys) || vtype(keys) != kTypeArray) return NULL;

  ant_offset_t len = js_arr_len(js, keys);
  char **env = calloc((size_t)len + 1, sizeof(char *));
  if (!env) return NULL;

  size_t out = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    if (vtype(key_val) != kTypeString) continue;

    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    if (!key || key_len == 0 || memchr(key, '=', key_len)) continue;

    char *key_cstr = strndup(key, key_len);
    if (!key_cstr) continue;

    ant_value_t value = js_get(js, env_obj, key_cstr);
    if (is_undefined(value)) {
      free(key_cstr);
      continue;
    }

    ant_value_t value_str = vtype(value) == kTypeString ? value : js_tostring_val(js, value);
    if (is_err(value_str) || vtype(value_str) != kTypeString) {
      free(key_cstr);
      continue;
    }

    size_t value_len = 0;
    char *value_ptr = js_getstr(js, value_str, &value_len);
    if (!value_ptr) value_ptr = "";

    char *entry = malloc(key_len + 1 + value_len + 1);
    if (!entry) {
      free(key_cstr);
      continue;
    }

    memcpy(entry, key_cstr, key_len);
    entry[key_len] = '=';
    memcpy(entry + key_len + 1, value_ptr, value_len);
    entry[key_len + 1 + value_len] = '\0';
    env[out++] = entry;

    free(key_cstr);
  }

  env[out] = NULL;
  return env;
}

static void free_env_array(char **env) {
  if (!env) return;
  for (size_t i = 0; env[i]; i++) free(env[i]);
  free(env);
}

static void stdio_spec_init(stdio_spec_t *spec) {
  spec->count = 3;
  for (int i = 0; i < CHILD_STDIO_MAX; i++) {
    spec->mode[i] = i < 3 ? STDIO_PIPE : STDIO_IGNORE;
    spec->fd[i] = -1;
  }
}

static int stdio_spec_source_fd(const stdio_spec_t *spec, int index) {
  if (spec->mode[index] == STDIO_FD) return spec->fd[index];
  if (spec->mode[index] == STDIO_INHERIT) return index;
  return -1;
}

static ant_value_t stdio_invalid_value(ant_t *js, ant_value_t value) {
  ant_value_t shown = js_mkundef();
  ant_value_t util = js_esm_import_sync_cstr(js, "util", 4);
  ant_value_t inspect = is_err(util) ? js_mkundef() : js_get(js, util, "inspect");

  if (is_callable(inspect)) shown = sv_vm_call(js->vm, js, inspect, util, &value, 1, NULL, js_mkundef());
  if (vtype(shown) != kTypeString) shown = js_mkstr(js, "<value>", 7);

  size_t len = 0;
  const char *text = js_getstr(js, shown, &len);
  
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_ARG_VALUE", 21));
  
  return js_mkerr_props(
    js, JS_ERR_TYPE, props, "The argument 'stdio' is invalid. Received %.*s",
    (int)(len < 200 ? len : 200), text ? text : ""
  );
}

static int stdio_object_fd(ant_t *js, ant_value_t value) {
  ant_value_t fd_val = js_get(js, value, "fd");
  if (vtype(fd_val) == kTypeNumber && js_getnum(fd_val) >= 0) return (int)js_getnum(fd_val);

  #ifndef _WIN32
  child_stream_ctx_t *ctx = get_child_stream_ctx(value);
  if (ctx && ctx->cp && child_stdio_is_pipe(ctx->cp, ctx->kind)) {
    uv_pipe_t *pipe = child_pipe(ctx->cp, ctx->kind);
    uv_os_fd_t fd;
    if (pipe && !uv_is_closing((uv_handle_t *)pipe) && uv_fileno((uv_handle_t *)pipe, &fd) == 0) return (int)fd;
  }
  #endif

  return net_socket_fileno(value);
}

static ant_value_t parse_stdio_entry(ant_t *js, ant_value_t value, int index, stdio_spec_t *spec) {
  if (is_undefined(value) || is_null(value)) {
    spec->mode[index] = index < 3 ? STDIO_PIPE : STDIO_IGNORE;
    return js_mkundef();
  }

  if (vtype(value) == kTypeString) {
    const char *str = js_getstr(js, value, NULL);
    if (str && strcmp(str, "ignore") == 0) spec->mode[index] = STDIO_IGNORE;
    else if (str && strcmp(str, "inherit") == 0) spec->mode[index] = STDIO_INHERIT;
    else if (str && strcmp(str, "ipc") == 0) spec->mode[index] = STDIO_IGNORE;
    else spec->mode[index] = STDIO_PIPE;
    return js_mkundef();
  }

  if (vtype(value) == kTypeNumber) {
    double fd = js_getnum(value);
    if (fd < 0 || fd > INT_MAX || fd != (double)(int)fd) return stdio_invalid_value(js, value);
    spec->mode[index] = STDIO_FD;
    spec->fd[index] = (int)fd;
    return js_mkundef();
  }

  if (is_object_type(value)) {
    int fd = stdio_object_fd(js, value);
    if (fd >= 0) {
      spec->mode[index] = STDIO_FD;
      spec->fd[index] = fd;
      return js_mkundef();
    }
  }

  return stdio_invalid_value(js, value);
}

static ant_value_t parse_stdio_option(ant_t *js, ant_value_t stdio_val, stdio_spec_t *spec) {
  if (is_undefined(stdio_val) || is_null(stdio_val)) return js_mkundef();

  if (vtype(stdio_val) == kTypeString) {
    const char *str = js_getstr(js, stdio_val, NULL);
    stdio_mode_t mode;
    if (str && (strcmp(str, "pipe") == 0 || strcmp(str, "overlapped") == 0)) mode = STDIO_PIPE;
    else if (str && strcmp(str, "ignore") == 0) mode = STDIO_IGNORE;
    else if (str && strcmp(str, "inherit") == 0) mode = STDIO_INHERIT;
    else return stdio_invalid_value(js, stdio_val);
    for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) spec->mode[i] = mode;
    return js_mkundef();
  }

  if (vtype(stdio_val) != kTypeArray) return stdio_invalid_value(js, stdio_val);

  ant_offset_t len = js_arr_len(js, stdio_val);
  if (len > CHILD_STDIO_MAX) len = CHILD_STDIO_MAX;
  spec->count = len > 3 ? (int)len : 3;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t result = parse_stdio_entry(js, js_arr_get(js, stdio_val, i), (int)i, spec);
    if (is_err(result)) return result;
  }

  return js_mkundef();
}

static ant_value_t builtin_spawn(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "spawn() requires a command");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  if (cmd && memchr(cmd, '\0', cmd_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Child process command cannot contain NUL bytes");
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *cwd = NULL;
  char **env = NULL;
  
  bool use_shell = false;
  bool detached = false;
  ant_value_t options_arg = child_process_options_arg(args, nargs);
  
  if (nargs >= 2 && js_is_array(args[1])) spawn_args = parse_args_array(js, args[1], &spawn_argc);
  if (spawn_argc < 0) {
    free(cmd_str);
    return Ant_Exception_Current(js);
  }
  
  stdio_spec_t stdio_spec;
  stdio_spec_init(&stdio_spec);
  
  if (is_special_object(options_arg)) {
    ant_value_t cwd_val = js_get(js, options_arg, "cwd");
    if (vtype(cwd_val) == kTypeString) {
      size_t cwd_len;
      char *cwd_str = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_str, cwd_len);
    }
    
    ant_value_t shell_val = js_get(js, options_arg, "shell");
    use_shell = js_truthy(js, shell_val);
    
    ant_value_t detached_val = js_get(js, options_arg, "detached");
    detached = js_truthy(js, detached_val);
    
    ant_value_t stdio_val = js_get(js, options_arg, "stdio");
    ant_value_t stdio_result = parse_stdio_option(js, stdio_val, &stdio_spec);
    if (is_err(stdio_result)) {
      free(cmd_str);
      free_args_array(spawn_args, spawn_argc);
      if (cwd) free(cwd);
      return stdio_result;
    }

    ant_value_t env_val = js_get(js, options_arg, "env");
    env = parse_env_object(js, env_val);
  }
  
  child_process_t *cp = calloc(1, sizeof(child_process_t));
  if (!cp) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    free_env_array(env);
    if (cwd) free(cwd);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  cp->js = js;
  cp->use_shell = use_shell;
  cp->cwd = cwd;
  cp->promise = js_mkundef();
  cp->exec_callback = js_mkundef();
  cp->exec_command = js_mkundef();
  cp->exec_spawn_error = js_mkundef();
  cp->spawnargs = js_mkundef();
  cp->keep_alive = true;
  
  memcpy(cp->stdio_modes, stdio_spec.mode, sizeof(cp->stdio_modes));
  ant_process_stage_init(&cp->process, cp, on_process_exit, on_process_close);
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) if (stdio_spec.mode[i] == STDIO_PIPE) {
    uv_pipe_t *p = child_pipe(cp, i);
    uv_pipe_init(js->loop, p, 0);
    p->data = cp;
  }
  
  uv_stdio_container_t stdio[CHILD_STDIO_MAX];
  for (int i = 0; i < stdio_spec.count; i++) {
    int source_fd = stdio_spec_source_fd(&stdio_spec, i);
    if (source_fd >= 0) ant_process_stdio_inherit_fd(&stdio[i], source_fd);
    else if (stdio_spec.mode[i] == STDIO_PIPE && i <= CHILD_STREAM_STDERR)
    ant_process_stdio_create_pipe(&stdio[i], (uv_stream_t *)child_pipe(cp, i), i == CHILD_STREAM_STDIN);
    else ant_process_stdio_ignore(&stdio[i]);
  }
  
  char **final_args;
  int final_argc;
  char *shell_cmd = NULL;
  
  if (use_shell) {
    final_args = calloc(4, sizeof(char *));
    final_args[0] = strdup("/bin/sh");
    final_args[1] = strdup("-c");
    
    size_t total_len = cmd_len + 1;
    for (int i = 0; i < spawn_argc; i++) {
      total_len += strlen(spawn_args[i]) + 3;
    }
    shell_cmd = malloc(total_len);
    strcpy(shell_cmd, cmd_str);
    for (int i = 0; i < spawn_argc; i++) {
      strcat(shell_cmd, " ");
      strcat(shell_cmd, spawn_args[i]);
    }
    final_args[2] = shell_cmd;
    final_args[3] = NULL;
    final_argc = 3;
    
    free(cmd_str);
    cmd_str = strdup("/bin/sh");
  } else {
    final_argc = spawn_argc + 1;
    final_args = calloc(final_argc + 1, sizeof(char *));
    final_args[0] = cmd_str;
    for (int i = 0; i < spawn_argc; i++) {
      final_args[i + 1] = spawn_args ? spawn_args[i] : NULL;
    }
    final_args[final_argc] = NULL;
    if (spawn_args) free(spawn_args);
    spawn_args = NULL;
  }
  
  ant_process_spawn_spec_t spec = {
    .loop = js->loop,
    .file = final_args[0],
    .args = final_args,
    .env = env,
    .cwd = cwd,
    .flags = detached ? UV_PROCESS_DETACHED : 0,
    .stdio = stdio,
    .stdio_count = stdio_spec.count,
  };
  
  cp->spawnargs = child_spawnargs_value(js, final_args);
  int r = ant_process_stage_spawn(&cp->process, &spec);
  
  char spawn_file[192];
  snprintf(spawn_file, sizeof(spawn_file), "%s", spec.file ? spec.file : "");

  if (use_shell) {
    free(final_args[0]);
    free(final_args[1]);
    free(shell_cmd);
    free(final_args);
  } else {
    for (int i = 0; i < final_argc; i++) {
      if (final_args[i]) free(final_args[i]);
    } free(final_args);
  }
  
  free_args_array(spawn_args, spawn_argc);
  free_env_array(env);
  
  if (r < 0) {
    cp->spawn_error = r;
    cp->spawn_file = strdup(spawn_file);

    for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
      uv_pipe_t *pipe = child_pipe(cp, i);
      bool *closed = child_closed_flag(cp, i);

      if (closed) *closed = true;
      if (child_stdio_is_pipe(cp, i) && pipe) close_child_handle(cp, (uv_handle_t *)pipe);
    }

    Ant_List_Insert(&js->active_children, cp);
    ant_value_t child_obj = create_child_object(js, cp);
    if (is_err(child_obj)) return child_obj;
    if (stdio_spec.count > 3) child_set_stdio_array(js, cp, stdio_spec.count);
    js_set(js, cp->child_obj, "pid", js_mkundef());
    child_schedule_spawn_failure(js, cp);

    return cp->child_obj;
  }
  
  static const uv_read_cb read_cbs[] = { NULL, on_stdout_read, on_stderr_read };
  bool *closed[] = { &cp->stdin_closed, &cp->stdout_closed, &cp->stderr_closed };
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    if (stdio_spec.mode[i] == STDIO_PIPE && read_cbs[i]) {
      uv_read_start((uv_stream_t *)child_pipe(cp, i), alloc_buffer, read_cbs[i]);
    } else if (stdio_spec.mode[i] != STDIO_PIPE) *closed[i] = true;
  }
  
  Ant_List_Insert(&js->active_children, cp);
  ant_value_t child_obj = create_child_object(js, cp);
  
  if (is_err(child_obj)) return child_obj;
  if (stdio_spec.count > 3) child_set_stdio_array(js, cp, stdio_spec.count);
  
  return cp->child_obj;
}

static ant_value_t exec_file_close_callback(ant_params_t);
static ant_value_t exec_spawn_error_capture(ant_params_t);

static void exec_attach_callback(
  ant_t *js, child_process_t *cp, ant_value_t callback, ant_value_t command, bool capture_spawn_error
) {
  cp->exec_callback = callback;
  cp->exec_command = command;
  
  if (capture_spawn_error) eventemitter_add_listener(
    js, cp->child_obj, "error",
    js_heavy_mkfun(js, exec_spawn_error_capture, cp->child_obj), true
  );
  
  eventemitter_add_listener(
    js, cp->child_obj, "close",
    js_heavy_mkfun(js, exec_file_close_callback, cp->child_obj), true
  );
}

static ant_value_t builtin_exec(ant_params_t) {
  ant_value_t callback = js_mkundef();

  if (nargs < 1) return js_mkerr(js, "exec() requires a command");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Command must be a string");
  if (nargs >= 2 && is_callable(args[nargs - 1])) callback = args[nargs - 1];
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  if (cmd && memchr(cmd, '\0', cmd_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Child process command cannot contain NUL bytes");
  char *cmd_str = strndup(cmd, cmd_len);
  
  char *cwd = NULL;
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t cwd_val = js_get(js, args[1], "cwd");
    if (vtype(cwd_val) == kTypeString) {
      size_t cwd_len;
      char *cwd_s = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_s, cwd_len);
    }
  }
  
  child_process_t *cp = calloc(1, sizeof(child_process_t));
  if (!cp) {
    free(cmd_str);
    if (cwd) free(cwd);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  cp->js = js;
  cp->use_shell = true;
  cp->cwd = cwd;
  cp->collect_output = true;
  cp->promise = is_callable(callback) ? js_mkundef() : js_mkpromise(js);
  cp->exec_callback = js_mkundef();
  cp->exec_command = js_mkundef();
  cp->exec_spawn_error = js_mkundef();
  cp->spawnargs = js_mkundef();
  cp->keep_alive = true;
  
  cp->stdio_modes[CHILD_STREAM_STDIN] = STDIO_IGNORE;
  cp->stdio_modes[CHILD_STREAM_STDOUT] = STDIO_PIPE;
  cp->stdio_modes[CHILD_STREAM_STDERR] = STDIO_PIPE;
  
  uv_pipe_init(js->loop, &cp->stdout_pipe, 0);
  uv_pipe_init(js->loop, &cp->stderr_pipe, 0);
  
  cp->stdout_pipe.data = cp;
  cp->stderr_pipe.data = cp;
  
  ant_process_stage_init(
    &cp->process, cp, 
    on_process_exit, on_process_close
  );
  
  cp->stdin_closed = true;
  uv_stdio_container_t stdio[3];
  ant_process_stdio_ignore(&stdio[0]);
  
  ant_process_stdio_create_pipe(&stdio[1], (uv_stream_t *)&cp->stdout_pipe, false);
  ant_process_stdio_create_pipe(&stdio[2], (uv_stream_t *)&cp->stderr_pipe, false);
  
  char *shell_args[4];
  shell_args[0] = "/bin/sh";
  shell_args[1] = "-c";
  shell_args[2] = cmd_str;
  shell_args[3] = NULL;
  
  ant_process_spawn_spec_t spec = {
    .loop = js->loop,
    .file = "/bin/sh",
    .args = shell_args,
    .cwd = cwd,
    .stdio = stdio,
    .stdio_count = 3,
  };
  cp->spawnargs = child_spawnargs_value(js, shell_args);
  int r = ant_process_stage_spawn(&cp->process, &spec);
  free(cmd_str);
  
  if (r < 0) {
    cp->spawn_error = r;
    cp->spawn_file = strdup("/bin/sh");

    for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
      uv_pipe_t *pipe = child_pipe(cp, i);
      bool *closed = child_closed_flag(cp, i);

      if (closed) *closed = true;
      if (child_stdio_is_pipe(cp, i) && pipe) close_child_handle(cp, (uv_handle_t *)pipe);
    }

    Ant_List_Insert(&js->active_children, cp);
    ant_value_t child_obj = create_child_object(js, cp);
    if (is_err(child_obj)) return child_obj;
    js_set(js, cp->child_obj, "pid", js_mkundef());

    if (is_callable(callback)) {
      exec_attach_callback(js, cp, callback, args[0], true);
      child_schedule_spawn_failure(js, cp);
      return cp->child_obj;
    }

    child_schedule_spawn_failure(js, cp);
    return cp->promise;
  }
  
  uv_read_start((uv_stream_t *)&cp->stdout_pipe, alloc_buffer, on_stdout_read);
  uv_read_start((uv_stream_t *)&cp->stderr_pipe, alloc_buffer, on_stderr_read);
  Ant_List_Insert(&js->active_children, cp);
  
  ant_value_t child_obj = create_child_object(js, cp);
  if (is_err(child_obj)) return child_obj;

  if (is_callable(callback)) {
    exec_attach_callback(js, cp, callback, args[0], false);
    return cp->child_obj;
  }

  return cp->promise;
}

static ant_value_t exec_spawn_error_capture(ant_params_t) {
  child_process_t *cp = get_child_process(js_func_data(js->current_func));
  if (cp && nargs > 0) cp->exec_spawn_error = args[0];
  return js_mkundef();
}

static ant_value_t exec_command_error(
  ant_t *js, child_process_t *cp, ant_value_t stderr_val, ant_value_t exit_code_val, ant_value_t signal_val
) {
  size_t command_len = 0;
  size_t stderr_len = 0;
  const char *command = js_getstr(js, cp->exec_command, &command_len);
  const char *stderr_text = js_getstr(js, stderr_val, &stderr_len);

  ant_value_t error = Ant_Error_CreateFormatted(
    js, JS_ERR_GENERIC, "Command failed: %.*s\n%.*s",
    (int)command_len, command ? command : "", (int)stderr_len, stderr_text ? stderr_text : ""
  );
  
  if (!is_object_type(error)) return error;

  js_set(js, error, "code", exit_code_val);
  js_set(js, error, "killed", js_get(js, cp->child_obj, "killed"));
  js_set(js, error, "signal", signal_val);
  js_set(js, error, "cmd", cp->exec_command);

  return error;
}

static ant_value_t exec_file_close_callback(ant_params_t) {
  child_process_t *cp = get_child_process(js_func_data(js->current_func));
  if (!cp || !is_callable(cp->exec_callback)) return js_mkundef();

  ant_value_t exit_code_val = nargs > 0 ? args[0] : js_mknull();
  ant_value_t signal_val = nargs > 1 ? args[1] : js_mknull();
  
  ant_value_t cb_args[3] = {
    cp->exec_spawn_error,
    js_mkstr_utf8(js, cp->stdout_buf ? cp->stdout_buf : "", cp->stdout_len),
    js_mkstr_utf8(js, cp->stderr_buf ? cp->stderr_buf : "", cp->stderr_len),
  };

  bool failed =
    (vtype(exit_code_val) == kTypeNumber && js_getnum(exit_code_val) != 0) ||
    vtype(signal_val) == kTypeString;

  if (is_object_type(cb_args[0])) js_set(js, cb_args[0], "cmd", cp->exec_command);
  else cb_args[0] = failed ? exec_command_error(js, cp, cb_args[2], exit_code_val, signal_val) : js_mknull();

  ant_value_t result = sv_vm_call(js->vm, js, cp->exec_callback, js_mkundef(), cb_args, 3, NULL, js_mkundef());
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t exec_file_promisify_callback(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  if (vtype(settled) == kTypeBool && settled == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  if (vtype(promise) != kTypePromise) return js_mkundef();

  ant_value_t stdout_val = nargs > 1 ? args[1] : js_mkstr(js, "", 0);
  ant_value_t stderr_val = nargs > 2 ? args[2] : js_mkstr(js, "", 0);

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_object_type(args[0])) {
      js_set(js, args[0], "stdout", stdout_val);
      js_set(js, args[0], "stderr", stderr_val);
    }
    js_reject_promise(js, promise, args[0]);
    return js_mkundef();
  }

  ant_value_t result = Ant_Object_Create(js);
  js_set(js, result, "stdout", stdout_val);
  js_set(js, result, "stderr", stderr_val);
  js_resolve_promise(js, promise, result);
  
  return js_mkundef();
}

static ant_value_t exec_callback_promisified_call(ant_params_t) {
  ant_value_t original = js_func_data(js->current_func);
  if (!is_callable(original)) return js_mkerr(js, "exec promisify target is not callable");

  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = Ant_Object_CreateBare(js);
  
  js_set_slot(state, SLOT_DATA, promise);
  js_set_slot(state, SLOT_SETTLED, js_false);
  
  ant_value_t callback = js_heavy_mkfun(js, exec_file_promisify_callback, state);
  ant_value_t *call_args = malloc((size_t)(nargs + 1) * sizeof(ant_value_t));
  
  if (!call_args) {
    js_reject_promise(js, promise, Ant_Error_CreateOutOfMemory(js));
    return promise;
  }

  for (int i = 0; i < nargs; i++) call_args[i] = args[i];
  call_args[nargs] = callback;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, state);
  
  ant_value_t call_result = sv_vm_call(
    js->vm, js, original, js_getthis(js), 
    call_args, nargs + 1, NULL, js_mkundef()
  );
  
  free(call_args);
  GC_ROOT_RESTORE(js, root_mark);

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  bool is_settled = (vtype(settled) == kTypeBool && settled == js_true);
  
  if (!is_settled && (is_err(call_result) || Ant_Exception_Pending(js))) {
    ant_value_t ex = js_take_thrown(js, call_result);
    js_set_slot(state, SLOT_SETTLED, js_true);
    js_reject_promise(js, promise, ex);
  }

  return promise;
}

static ant_value_t builtin_execFile(ant_params_t) {
  ant_value_t argv = js_mkundef();
  ant_value_t options = js_mkundef();
  ant_value_t callback = js_mkundef();
  
  ant_value_t spawn_args[3];
  ant_value_t child;

  if (nargs < 1) return js_mkerr(js, "execFile() requires a file");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "File must be a string");

  if (nargs >= 2 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    nargs--;
  }

  if (nargs >= 2) {
    if (js_is_array(args[1])) {
      argv = args[1];
      if (nargs >= 3 && is_special_object(args[2])) options = args[2];
    } else if (is_special_object(args[1])) options = args[1];
  }

  spawn_args[0] = args[0];
  spawn_args[1] = argv;
  spawn_args[2] = options;

  child = builtin_spawn(js, spawn_args, 3, js_mkundef());
  if (vtype(child) != kTypeObject || !is_callable(callback)) return child;

  child_process_t *spawned = get_child_process(child);
  if (!spawned) return child;

  ant_value_t command = child_process_command_value(js, args[0], argv);
  if (vtype(command) != kTypeString) command = args[0];

  spawned->collect_output = true;
  exec_attach_callback(js, spawned, callback, command, true);

  return child;
}

static void sync_output_encoding(ant_t *js, ant_value_t options_arg, char out[SYNC_ENCODING_CAP]) {
  out[0] = '\0';
  if (!is_special_object(options_arg)) return;

  ant_value_t encoding_val = js_get(js, options_arg, "encoding");
  if (vtype(encoding_val) != kTypeString) return;

  size_t encoding_len = 0;
  char *encoding = js_getstr(js, encoding_val, &encoding_len);
  if (!encoding) return;

  if (encoding_len == 6 && strncmp(encoding, "buffer", 6) == 0) return;
  if (encoding_len >= SYNC_ENCODING_CAP) encoding_len = 0;
  
  memcpy(out, "utf8", 5);
  if (encoding_len > 0) {
    memcpy(out, encoding, encoding_len);
    out[encoding_len] = '\0';
  }
}

static bool sync_value_bytes(ant_t *js, ant_value_t value, const char **out, size_t *len) {
  *out = NULL;
  *len = 0;

  if (vtype(value) == kTypeString) {
    *out = js_getstr(js, value, len);
    return *out != NULL;
  }

  const uint8_t *bytes = NULL;
  if (buffer_source_get_bytes(js, value, &bytes, len)) {
    *out = (const char *)bytes;
    return true;
  }

  return false;
}

static ant_value_t sync_make_output(ant_t *js, const char *bytes, size_t len, const char *encoding) {
  if (encoding[0]) {
    ant_value_t str = js_mkundef();
    if (buffer_bytes_to_string(js, (const uint8_t *)(bytes ? bytes : ""), len, encoding, strlen(encoding), &str))
      return str;
    return js_mkstr_utf8(js, bytes ? bytes : "", len);
  }

  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return Ant_Error_CreateOutOfMemory(js);
  if (bytes && len > 0) memcpy(buffer->data, bytes, len);

  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.buffer_proto);
}

typedef struct {
  ant_value_t error;
  ant_value_t status;
  ant_value_t signal;
  ant_value_t output;
  ant_value_t pid;
  ant_value_t stdout_val;
  ant_value_t stderr_val;
  bool stdio_specified;
} sync_outcome_t;

static void sync_outcome_init(sync_outcome_t *o) {
  *o = (sync_outcome_t){
    .error = js_mkundef(),
    .status = js_mknull(),
    .signal = js_mknull(),
    .output = js_mknull(),
    .pid = js_mknum(0),
    .stdout_val = js_mkundef(),
    .stderr_val = js_mkundef(),
  };
}

static ant_value_t sync_outcome_object(ant_t *js, const sync_outcome_t *o) {
  ant_value_t result = Ant_Object_Create(js);
  if (!is_undefined(o->error)) js_set(js, result, "error", o->error);
  js_set(js, result, "status", o->status);
  js_set(js, result, "signal", o->signal);
  js_set(js, result, "output", o->output);
  js_set(js, result, "pid", o->pid);
  js_set(js, result, "stdout", o->stdout_val);
  js_set(js, result, "stderr", o->stderr_val);
  return result;
}

static ant_value_t sync_error_result(ant_t *js, ant_value_t error, sync_outcome_t *o) {
  o->error = error;
  return sync_outcome_object(js, o);
}

static ant_value_t sync_finish_outputs(
  ant_t *js, sync_outcome_t *o, ant_value_t stdout_val, ant_value_t stderr_val, int stdio_count
) {
  o->stdout_val = stdout_val;
  o->stderr_val = stderr_val;
  o->output = js_mkarr(js);
  js_arr_push(js, o->output, js_mknull());
  js_arr_push(js, o->output, stdout_val);
  js_arr_push(js, o->output, stderr_val);
  for (int i = 3; i < stdio_count; i++) js_arr_push(js, o->output, js_mknull());
  return sync_outcome_object(js, o);
}

#ifdef _WIN32
static void win_close_handle(HANDLE *h) {
  if (*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
  *h = NULL;
}

static void win_close_handles(HANDLE *h) {
  for (int i = 0; i < 3; i++) win_close_handle(&h[i]);
}

static HANDLE win_inheritable_handle(HANDLE h) {
  HANDLE dup = NULL;
  if (!h || h == INVALID_HANDLE_VALUE) return NULL;
  if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
  return dup;
}

static int win_sync_stdio_open(const stdio_spec_t *spec, SECURITY_ATTRIBUTES *sa, HANDLE *child_end, HANDLE *parent_end) {
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++)
    if (spec->mode[i] == STDIO_PIPE) {
      HANDLE read_end = NULL, write_end = NULL;
      if (!CreatePipe(&read_end, &write_end, sa, 0)) return uv_translate_sys_error(GetLastError());
      child_end[i] = i == CHILD_STREAM_STDIN ? read_end : write_end;
      parent_end[i] = i == CHILD_STREAM_STDIN ? write_end : read_end;
      SetHandleInformation(parent_end[i], HANDLE_FLAG_INHERIT, 0);
    } else if (spec->mode[i] == STDIO_IGNORE) {
      DWORD access = i == CHILD_STREAM_STDIN ? GENERIC_READ : GENERIC_WRITE;
      HANDLE h = CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, sa, OPEN_EXISTING, 0, NULL);
      if (h == INVALID_HANDLE_VALUE) return uv_translate_sys_error(GetLastError());
      child_end[i] = h;
    } else {
      child_end[i] = win_inheritable_handle(uv_get_osfhandle(stdio_spec_source_fd(spec, i)));
      if (!child_end[i] && spec->mode[i] == STDIO_FD) return UV_EBADF;
    }
  return 0;
}

typedef struct {
  HANDLE pipe;
  HANDLE thread;
  char *buf;
  size_t len;
  bool ok;
} win_sync_reader_t;

static DWORD WINAPI win_sync_reader_main(LPVOID arg) {
  win_sync_reader_t *r = arg;
  size_t cap = 4096;
  char chunk[4096];
  DWORD n;

  r->buf = malloc(cap);
  r->len = 0;
  r->ok = r->buf != NULL;

  while (r->ok && ReadFile(r->pipe, chunk, sizeof(chunk), &n, NULL) && n > 0) {
    if (r->len + n > cap) {
      while (r->len + n > cap) cap *= 2;
      char *grown = realloc(r->buf, cap);
      if (!grown) {
        r->ok = false;
        break;
      }
      r->buf = grown;
    }
    memcpy(r->buf + r->len, chunk, n);
    r->len += n;
  }
  return 0;
}

static bool win_sync_reader_start(win_sync_reader_t *r, HANDLE pipe) {
  *r = (win_sync_reader_t){ .pipe = pipe, .ok = true };
  if (!pipe) return true;
  r->thread = CreateThread(NULL, 0, win_sync_reader_main, r, 0, NULL);
  return r->thread != NULL;
}

static void win_sync_reader_join(win_sync_reader_t *r) {
  if (!r->thread) return;
  WaitForSingleObject(r->thread, INFINITE);
  CloseHandle(r->thread);
  r->thread = NULL;
}

static ant_value_t spawn_sync_impl(ant_native_params_t, bool force_shell, sync_outcome_t *outcome) {
  sync_outcome_init(outcome);
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Command must be a string");

  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);

  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *input = NULL;
  size_t input_len = 0;
  bool use_shell = force_shell;
  
  ant_value_t options_arg = child_process_options_arg(args, nargs);
  stdio_spec_t stdio_spec;
  stdio_spec_init(&stdio_spec);

  if (nargs >= 2 && js_is_array(args[1])) spawn_args = parse_args_array(js, args[1], &spawn_argc);
  if (spawn_argc < 0) {
    free(cmd_str);
    return Ant_Exception_Current(js);
  }

  if (is_special_object(options_arg)) {
    ant_value_t input_val = js_get(js, options_arg, "input");
    
    if (vtype(input_val) == kTypeString) input = js_getstr(js, input_val, &input_len);
    if (!use_shell) use_shell = js_truthy(js, js_get(js, options_arg, "shell"));
    
    ant_value_t stdio_val = js_get(js, options_arg, "stdio");
    outcome->stdio_specified = !is_undefined(stdio_val);
    
    ant_value_t stdio_result = parse_stdio_option(js, stdio_val, &stdio_spec);
    if (is_err(stdio_result)) {
      free(cmd_str);
      free_args_array(spawn_args, spawn_argc);
      return stdio_result;
    }
  }

  size_t cmdline_len = cmd_len + 3;
  for (int i = 0; i < spawn_argc; i++) {
    cmdline_len += strlen(spawn_args[i]) + 3;
  }
  
  char *raw_cmdline = malloc(cmdline_len);
  if (!raw_cmdline) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  char *p = raw_cmdline;
  p += sprintf(p, "%s", cmd_str);
  for (int i = 0; i < spawn_argc; i++) {
    p += sprintf(p, " \"%s\"", spawn_args[i]);
  }

  char *cmdline = raw_cmdline;
  if (use_shell) {
    const char *shell = getenv("COMSPEC");
    if (!shell || !shell[0]) shell = "cmd.exe";
    size_t shell_len = strlen(shell) + strlen(raw_cmdline) + 16;
    cmdline = malloc(shell_len);
    if (!cmdline) {
      free(raw_cmdline);
      free(cmd_str);
      free_args_array(spawn_args, spawn_argc);
      return Ant_Error_CreateOutOfMemory(js);
    }
    snprintf(cmdline, shell_len, "%s /d /s /c \"%s\"", shell, raw_cmdline);
  }
  
  SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  HANDLE child_end[3] = { NULL, NULL, NULL };
  HANDLE parent_end[3] = { NULL, NULL, NULL };

  int stdio_rc = win_sync_stdio_open(&stdio_spec, &sa, child_end, parent_end);
  if (stdio_rc < 0) {
    win_close_handles(child_end);
    win_close_handles(parent_end);
    if (cmdline != raw_cmdline) free(raw_cmdline);
    free(cmdline);
    free_args_array(spawn_args, spawn_argc);

    ant_value_t result;
    if (stdio_rc == UV_EBADF) {
      char syscall[224];
      snprintf(syscall, sizeof(syscall), "spawnSync %.*s", (int)(cmd_len < 120 ? cmd_len : 120), cmd_str);
      result = sync_error_result(js, child_syscall_error(js, stdio_rc, syscall, cmd_str), outcome);
    } else result = js_mkerr(js, "Failed to create pipes");
    
    free(cmd_str);
    return result;
  }
  
  STARTUPINFOA si = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_end[CHILD_STREAM_STDIN];
  si.hStdOutput = child_end[CHILD_STREAM_STDOUT];
  si.hStdError = child_end[CHILD_STREAM_STDERR];
  
  PROCESS_INFORMATION pi = {0};
  BOOL success = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  
  free(cmdline);
  if (cmdline != raw_cmdline) free(raw_cmdline);
  
  free(cmd_str);
  free_args_array(spawn_args, spawn_argc);
  win_close_handles(child_end);
  
  if (!success) {
    win_close_handles(parent_end);
    return js_mkerr(js, "Failed to create process");
  }
  
  win_sync_reader_t out, err;
  bool started = win_sync_reader_start(&out, parent_end[CHILD_STREAM_STDOUT]);
  started = win_sync_reader_start(&err, parent_end[CHILD_STREAM_STDERR]) && started;
  if (!started) TerminateProcess(pi.hProcess, 1);

  if (started && parent_end[CHILD_STREAM_STDIN] && input && input_len > 0) {
    DWORD written;
    WriteFile(parent_end[CHILD_STREAM_STDIN], input, (DWORD)input_len, &written, NULL);
  }
  
  win_close_handle(&parent_end[CHILD_STREAM_STDIN]);
  win_sync_reader_join(&out);
  win_sync_reader_join(&err);
  win_close_handles(parent_end);
  
  char *stdout_buf = out.buf, *stderr_buf = err.buf;
  size_t stdout_len = out.len, stderr_len = err.len;
  bool read_ok = started && out.ok && err.ok;
  
  WaitForSingleObject(pi.hProcess, INFINITE);
  
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  DWORD pid = pi.dwProcessId;
  
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  
  if (!read_ok) {
    free(stdout_buf);
    free(stderr_buf);
    return js_mkerr(js, "Failed to read process output");
  }
  
  char encoding[SYNC_ENCODING_CAP];
  sync_output_encoding(js, options_arg, encoding);

  ant_value_t stdout_val = sync_make_output(js, stdout_buf, stdout_len, encoding);
  if (is_err(stdout_val)) {
    free(stdout_buf);
    free(stderr_buf);
    return stdout_val;
  }

  ant_value_t stderr_val = sync_make_output(js, stderr_buf, stderr_len, encoding);
  if (is_err(stderr_val)) {
    free(stdout_buf);
    free(stderr_buf);
    return stderr_val;
  }

  outcome->status = js_mknum((double)exit_code);
  outcome->pid = js_mknum((double)pid);
  ant_value_t result = sync_finish_outputs(js, outcome, stdout_val, stderr_val, 3);

  if (stdout_buf) free(stdout_buf);
  if (stderr_buf) free(stderr_buf);

  return result;
}

#else
static void close_if_valid(int fd) {
  if (fd >= 0) close(fd);
}

static void child_redirect_stdio_to_devnull(int fd, int flags) {
  int null_fd = open("/dev/null", flags);
  if (null_fd < 0) _exit(127);
  if (null_fd == fd) return;
  dup2(null_fd, fd);
  close(null_fd);
}

static bool append_sync_output(char **buf, size_t *len, size_t *cap, const char *data, size_t data_len) {
  if (data_len == 0) return true;

  if (*cap == 0) {
    *cap = 4096;
    *buf = malloc(*cap);
    if (!*buf) return false;
  }

  if (*len + data_len > *cap) {
    size_t new_cap = *cap;
    while (new_cap < *len + data_len) new_cap *= 2;
    char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return false;
    *buf = new_buf;
    *cap = new_cap;
  }

  memcpy(*buf + *len, data, data_len);
  *len += data_len;
  return true;
}

static uint64_t sync_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

typedef struct {
  int fd;
  char *buf;
  size_t len;
  size_t cap;
  bool open;
} sync_reader_t;

typedef struct {
  pid_t pid;
  int timeout_ms;
  int kill_signal;
  size_t max_buffer;
  bool timed_out;
  bool over_buffer;
} sync_read_ctl_t;

static void sync_reader_init(sync_reader_t *reader, int fd) {
  *reader = (sync_reader_t){ .fd = fd, .open = fd >= 0 };
}

static bool sync_reader_pump(sync_reader_t *reader, sync_read_ctl_t *ctl) {
  char chunk[4096];
  
  ssize_t n = read(reader->fd, chunk, sizeof(chunk));
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;

  if (n <= 0) {
    close_if_valid(reader->fd);
    reader->open = false;
    return true;
  }

  size_t room = reader->len < ctl->max_buffer ? ctl->max_buffer - reader->len : 0;
  size_t take = (size_t)n < room ? (size_t)n : room;
  if (take < (size_t)n) ctl->over_buffer = true;

  return append_sync_output(&reader->buf, &reader->len, &reader->cap, chunk, take);
}

static void sync_reader_shutdown(sync_reader_t *reader) {
  if (!reader->open) return;
  close_if_valid(reader->fd);
  reader->open = false;
}

typedef struct {
  int fd;
  const char *data;
  size_t len;
  size_t written;
  bool open;
} sync_writer_t;

static void sync_writer_init(sync_writer_t *writer, int fd, const char *data, size_t len) {
  *writer = (sync_writer_t){ .fd = fd, .data = data, .len = len, .open = fd >= 0 };

  if (writer->open && (!data || len == 0)) {
    close_if_valid(fd);
    writer->open = false;
    return;
  }

  if (writer->open) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void sync_writer_shutdown(sync_writer_t *writer) {
  if (!writer->open) return;
  close_if_valid(writer->fd);
  writer->open = false;
}

static void sync_writer_pump(sync_writer_t *writer) {
  size_t remaining = writer->len - writer->written;
  ssize_t n = write(writer->fd, writer->data + writer->written, remaining);

  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN) return;
    sync_writer_shutdown(writer);
    return;
  }

  writer->written += (size_t)n;
  if (writer->written >= writer->len) sync_writer_shutdown(writer);
}

static bool read_sync_outputs(sync_reader_t *out, sync_reader_t *err, sync_writer_t *in, sync_read_ctl_t *ctl) {
  uint64_t deadline = ctl->timeout_ms > 0 ? sync_now_ms() + (uint64_t)ctl->timeout_ms : 0;

  while (out->open || err->open || in->open) {
    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    int max_fd = -1;

    sync_reader_t *sides[2] = { out, err };
    for (int i = 0; i < 2; i++) {
      if (!sides[i]->open) continue;
      FD_SET(sides[i]->fd, &readfds);
      if (sides[i]->fd > max_fd) max_fd = sides[i]->fd;
    }

    if (in->open) {
      FD_SET(in->fd, &writefds);
      if (in->fd > max_fd) max_fd = in->fd;
    }

    struct timeval tv;
    struct timeval *tv_ptr = NULL;

    if (deadline) {
      uint64_t now = sync_now_ms();
      if (now >= deadline) {
        if (ctl->pid > 0) kill(ctl->pid, ctl->kill_signal);
        ctl->timed_out = true;
        break;
      } else {
        uint64_t remaining = deadline - now;
        tv.tv_sec = (time_t)(remaining / 1000);
        tv.tv_usec = (suseconds_t)((remaining % 1000) * 1000);
        tv_ptr = &tv;
      }
    }

    int ready = select(max_fd + 1, &readfds, in->open ? &writefds : NULL, NULL, tv_ptr);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    
    if (ready == 0) continue;
    if (in->open && FD_ISSET(in->fd, &writefds)) sync_writer_pump(in);

    for (int i = 0; i < 2; i++)
      if (sides[i]->open && FD_ISSET(sides[i]->fd, &readfds) && !sync_reader_pump(sides[i], ctl)) return false;

    if (ctl->over_buffer) {
      if (ctl->pid > 0) kill(ctl->pid, ctl->kill_signal);
      break;
    }
  }

  sync_writer_shutdown(in);
  sync_reader_shutdown(out);
  sync_reader_shutdown(err);

  return true;
}

static void free_sync_exec_args(char **exec_args, bool use_shell) {
  if (!exec_args) return;
  if (use_shell) {
    free(exec_args[0]);
    free(exec_args[1]);
    free(exec_args[2]);
  }
  free(exec_args);
}


typedef struct {
  char *command;
  char **args;
  int arg_count;
  char *cwd;
  char **env;
  char *shell;
  char **argv;
  bool argv_owned;
} sync_res_t;

static void sync_res_free(sync_res_t *res) {
  free_sync_exec_args(res->argv, res->argv_owned);
  free(res->command);
  free(res->cwd);
  free(res->shell);
  free_env_array(res->env);
  free_args_array(res->args, res->arg_count);
  *res = (sync_res_t){0};
}

typedef struct {
  int fds[3][2];
} sync_pipes_t;

static void sync_pipes_init(sync_pipes_t *pipes) {
  for (int i = 0; i < 3; i++) pipes->fds[i][0] = pipes->fds[i][1] = -1;
}

static void sync_pipes_close_all(sync_pipes_t *pipes) {
  for (int i = 0; i < 3; i++) {
    close_if_valid(pipes->fds[i][0]);
    close_if_valid(pipes->fds[i][1]);
    pipes->fds[i][0] = pipes->fds[i][1] = -1;
  }
}

static bool sync_pipes_open(sync_pipes_t *pipes, const stdio_spec_t *spec) {
  for (int i = 0; i < 3; i++) {
    if (spec->mode[i] != STDIO_PIPE) continue;
    if (pipe(pipes->fds[i]) < 0) return false;
    fcntl(pipes->fds[i][0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes->fds[i][1], F_SETFD, FD_CLOEXEC);
  }
  return true;
}

typedef struct {
  const char *input;
  size_t input_len;
  bool use_shell;
  char encoding[SYNC_ENCODING_CAP];
  int timeout_ms;
  int kill_signal;
  size_t max_buffer;
  stdio_spec_t stdio;
  bool stdio_specified;
  char label[128];
} sync_opts_t;

static const size_t SYNC_MAX_BUFFER_DEFAULT = 1024 * 1024;

static int sync_parse_kill_signal(ant_t *js, ant_value_t value, int fallback) {
  if (vtype(value) == kTypeNumber) {
    int resolved = (int)js_getnum(value);
    return resolved > 0 ? resolved : fallback;
  }
  if (vtype(value) != kTypeString) return fallback;

  size_t len = 0;
  char *name = js_getstr(js, value, &len);

  char buf[32];
  if (!name || len >= sizeof(buf)) return fallback;

  memcpy(buf, name, len);
  buf[len] = '\0';

  int resolved = process_signal_number(buf);
  return resolved > 0 ? resolved : fallback;
}

static ant_value_t sync_parse_options(
  ant_t *js, ant_value_t options, bool force_shell, sync_opts_t *opts, sync_res_t *res
) {
  *opts = (sync_opts_t){
    .use_shell = force_shell,
    .kill_signal = SIGTERM,
    .max_buffer = SYNC_MAX_BUFFER_DEFAULT,
  };
  
  stdio_spec_init(&opts->stdio);
  if (!is_special_object(options)) return js_mkundef();

  ant_value_t cwd_val = js_get(js, options, "cwd");
  if (vtype(cwd_val) == kTypeString) {
    size_t cwd_len = 0;
    char *cwd = js_getstr(js, cwd_val, &cwd_len);
    if (cwd) res->cwd = strndup(cwd, cwd_len);
  }

  sync_value_bytes(js, js_get(js, options, "input"), &opts->input, &opts->input_len);
  ant_value_t stdio_val = js_get(js, options, "stdio");
  opts->stdio_specified = !is_undefined(stdio_val);
  
  ant_value_t stdio_result = parse_stdio_option(js, stdio_val, &opts->stdio);
  if (is_err(stdio_result)) return stdio_result;
  res->env = parse_env_object(js, js_get(js, options, "env"));

  ant_value_t shell_val = js_get(js, options, "shell");
  if (vtype(shell_val) == kTypeString) {
    size_t shell_len = 0;
    char *shell = js_getstr(js, shell_val, &shell_len);
    if (shell && shell_len > 0) {
      res->shell = strndup(shell, shell_len);
      opts->use_shell = true;
    }
  } 
  
  else if (!opts->use_shell) opts->use_shell = js_truthy(js, shell_val);
  sync_output_encoding(js, options, opts->encoding);

  ant_value_t timeout_val = js_get(js, options, "timeout");
  if (vtype(timeout_val) == kTypeNumber) {
    double ms = js_getnum(timeout_val);
    if (ms > 0 && ms < (double)INT_MAX) opts->timeout_ms = (int)ms;
  }

  ant_value_t max_buffer_val = js_get(js, options, "maxBuffer");
  if (vtype(max_buffer_val) == kTypeNumber) {
    double bytes = js_getnum(max_buffer_val);
    if (bytes >= 0) opts->max_buffer = bytes >= (double)SIZE_MAX ? SIZE_MAX : (size_t)bytes;
  }

  opts->kill_signal = sync_parse_kill_signal(js, js_get(js, options, "killSignal"), SIGTERM);
  return js_mkundef();
}

static bool sync_build_argv(sync_res_t *res, bool use_shell, size_t command_len) {
  int count = use_shell ? 3 : res->arg_count + 1;
  res->argv = calloc((size_t)count + 1, sizeof(char *));
  if (!res->argv) return false;

  if (!use_shell) {
    res->argv[0] = res->command;
    for (int i = 0; i < res->arg_count; i++) res->argv[i + 1] = res->args[i];
    return true;
  }

  size_t len = command_len + 1;
  for (int i = 0; i < res->arg_count; i++) len += strlen(res->args[i]) + 2;

  char *line = malloc(len);
  if (!line) return false;

  res->argv_owned = true;
  strcpy(line, res->command);
  for (int i = 0; i < res->arg_count; i++) {
    strcat(line, " ");
    strcat(line, res->args[i]);
  }

  res->argv[0] = strdup(res->shell ? res->shell : "/bin/sh");
  res->argv[1] = strdup("-c");
  res->argv[2] = line;

  return res->argv[0] && res->argv[1];
}

extern char **environ;

static void sync_child_fail(int report_fd) {
  int err = errno;
  if (report_fd >= 0) while (write(report_fd, &err, sizeof(err)) < 0 && errno == EINTR) {}
  _exit(127);
}

static void sync_child_exec(
  const sync_pipes_t *pipes, const sync_opts_t *opts, const sync_res_t *res, int report_fd
) {
  signal(SIGPIPE, SIG_DFL);
  if (res->cwd && chdir(res->cwd) != 0) sync_child_fail(report_fd);
  if (res->env) environ = res->env;

  const stdio_spec_t *spec = &opts->stdio;
  int source[CHILD_STDIO_MAX];

  for (int i = 0; i < spec->count; i++) {
    if (spec->mode[i] == STDIO_PIPE && i < 3) source[i] = i == CHILD_STREAM_STDIN ? pipes->fds[i][0] : pipes->fds[i][1];
    else source[i] = stdio_spec_source_fd(spec, i);
    if (source[i] >= 0 && source[i] != i && source[i] < spec->count) {
      source[i] = fcntl(source[i], F_DUPFD_CLOEXEC, spec->count);
      if (source[i] < 0) sync_child_fail(report_fd);
    }
  }

  for (int i = 0; i < spec->count; i++) {
    if (source[i] == i) fcntl(i, F_SETFD, 0);
    else if (source[i] >= 0) {
      if (dup2(source[i], i) < 0) sync_child_fail(report_fd);
    } else if (i < 3) child_redirect_stdio_to_devnull(i, i == CHILD_STREAM_STDIN ? O_RDONLY : O_WRONLY);
  }

  execvp(res->argv[0], res->argv);
  sync_child_fail(report_fd);
}

static int sync_report_pipe_read(int fd) {
  int err = 0;
  ssize_t n;
  while ((n = read(fd, &err, sizeof(err))) < 0 && errno == EINTR) {}
  close(fd);
  return n == (ssize_t)sizeof(err) ? err : 0;
}

static ant_value_t sync_build_spawn_error(
  ant_t *js, int child_errno, const sync_opts_t *opts, const sync_res_t *res, sync_outcome_t *outcome
) {
  char syscall[224];
  snprintf(syscall, sizeof(syscall), "spawnSync %s", opts->label);

  ant_value_t error = child_syscall_error(js, uv_translate_sys_error(child_errno), syscall, res->argv[0]);
  if (is_object_type(error)) {
    ant_value_t spawnargs = js_mkarr(js);
    for (int i = 1; res->argv[i]; i++) js_arr_push(js, spawnargs, js_mkstr(js, res->argv[i], strlen(res->argv[i])));
    js_set(js, error, "spawnargs", spawnargs);
  }

  return sync_error_result(js, error, outcome);
}

static ant_value_t sync_build_result(
  ant_t *js, pid_t pid, int status, const sync_read_ctl_t *ctl, const sync_opts_t *opts,
  const sync_reader_t *out, const sync_reader_t *err, sync_outcome_t *outcome
) {
  ant_value_t stdout_val = opts->stdio.mode[CHILD_STREAM_STDOUT] == STDIO_PIPE
    ? sync_make_output(js, out->buf, out->len, opts->encoding)
    : js_mknull();
  if (is_err(stdout_val)) return stdout_val;

  ant_value_t stderr_val = opts->stdio.mode[CHILD_STREAM_STDERR] == STDIO_PIPE
    ? sync_make_output(js, err->buf, err->len, opts->encoding)
    : js_mknull();
  if (is_err(stderr_val)) return stderr_val;

  outcome->pid = js_mknum((double)pid);

  int signal_code = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  if (signal_code) {
    const char *name = process_signal_name(signal_code);
    outcome->signal = name ? js_mkstr(js, name, strlen(name)) : js_mknull();
  } else outcome->status = js_mknum((double)(WIFEXITED(status) ? WEXITSTATUS(status) : -1));

  int failure = ctl->timed_out ? UV_ETIMEDOUT : ctl->over_buffer ? UV_ENOBUFS : 0;
  if (failure) {
    char syscall[224];
    snprintf(syscall, sizeof(syscall), "spawnSync %s", opts->label);
    outcome->error = child_syscall_error(js, failure, syscall, NULL);
  }

  return sync_finish_outputs(js, outcome, stdout_val, stderr_val, opts->stdio.count);
}
static ant_value_t spawn_sync_impl(ant_native_params_t, bool force_shell, sync_outcome_t *outcome) {
  sync_outcome_init(outcome);
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Command must be a string");

  size_t command_len = 0;
  char *command = js_getstr(js, args[0], &command_len);
  if (!command) return js_mkerr(js, "Command must be a string");
  if (memchr(command, '\0', command_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Child process command cannot contain NUL bytes");

  sync_res_t res = {0};
  res.command = strndup(command, command_len);
  if (!res.command) return Ant_Error_CreateOutOfMemory(js);

  if (nargs >= 2 && js_is_array(args[1]))
    res.args = parse_args_array(js, args[1], &res.arg_count);
  if (res.arg_count < 0) {
    free(res.command);
    return Ant_Exception_Current(js);
  }

  sync_opts_t opts;
  ant_value_t parsed = sync_parse_options(js, child_process_options_arg(args, nargs), force_shell, &opts, &res);
  if (is_err(parsed)) {
    sync_res_free(&res);
    return parsed;
  }

  outcome->stdio_specified = opts.stdio_specified;
  if (opts.use_shell) snprintf(opts.label, sizeof(opts.label), "%s", res.shell ? res.shell : "/bin/sh");
  else snprintf(opts.label, sizeof(opts.label), "%.*s", (int)(command_len < 120 ? command_len : 120), command);

  if (!sync_build_argv(&res, opts.use_shell, command_len)) {
    sync_res_free(&res);
    return Ant_Error_CreateOutOfMemory(js);
  }

  sync_pipes_t pipes;
  sync_pipes_init(&pipes);

  if (!sync_pipes_open(&pipes, &opts.stdio)) {
    sync_pipes_close_all(&pipes);
    sync_res_free(&res);
    return js_mkerr(js, "Failed to create pipes");
  }

  int report[2] = { -1, -1 };
  if (uv_pipe(report, 0, 0) != 0) {
    sync_pipes_close_all(&pipes);
    sync_res_free(&res);
    return js_mkerr(js, "Failed to create pipes");
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(report[0]);
    close(report[1]);
    sync_pipes_close_all(&pipes);
    sync_res_free(&res);
    return js_mkerr(js, "Fork failed");
  }
  if (pid == 0) {
    close(report[0]);
    sync_child_exec(&pipes, &opts, &res, report[1]);
  }

  close(report[1]);
  int child_errno = sync_report_pipe_read(report[0]);

  if (child_errno) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    sync_pipes_close_all(&pipes);
    ant_value_t result = sync_build_spawn_error(js, child_errno, &opts, &res, outcome);
    sync_res_free(&res);
    return result;
  }

  bool want_stdin = opts.stdio.mode[CHILD_STREAM_STDIN] == STDIO_PIPE;

  close_if_valid(pipes.fds[CHILD_STREAM_STDIN][0]);
  close_if_valid(pipes.fds[CHILD_STREAM_STDOUT][1]);
  close_if_valid(pipes.fds[CHILD_STREAM_STDERR][1]);

  sync_writer_t in;
  sync_writer_init(&in, want_stdin ? pipes.fds[CHILD_STREAM_STDIN][1] : -1, opts.input, opts.input_len);
  if (!want_stdin) close_if_valid(pipes.fds[CHILD_STREAM_STDIN][1]);

  sync_reader_t out, err;
  sync_reader_init(&out, pipes.fds[CHILD_STREAM_STDOUT][0]);
  sync_reader_init(&err, pipes.fds[CHILD_STREAM_STDERR][0]);

  sync_read_ctl_t ctl = {
    .pid = pid,
    .timeout_ms = opts.timeout_ms,
    .kill_signal = opts.kill_signal,
    .max_buffer = opts.max_buffer,
  };

  bool read_ok = read_sync_outputs(&out, &err, &in, &ctl);
  int status = 0;

  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  sync_res_free(&res);

  ant_value_t result = read_ok
    ? sync_build_result(js, pid, status, &ctl, &opts, &out, &err, outcome)
    : js_mkerr(js, "Failed to read process output");

  free(out.buf);
  free(err.buf);

  return result;
}


#endif

static ant_value_t builtin_spawnSync(ant_params_t) {
  sync_outcome_t outcome;
  return spawn_sync_impl(js, args, nargs, false, &outcome);
}

static ant_value_t sync_result_error(
  ant_t *js, const sync_outcome_t *o, ant_value_t command_val, const char *api
) {
  ant_value_t spawn_error = o->error;
  bool failed_status = vtype(o->status) == kTypeNumber && (int)js_getnum(o->status) != 0;
  bool signalled = vtype(o->signal) == kTypeString;
  if (!is_object_type(spawn_error) && !failed_status && !signalled) return js_mkundef();

  char message[512] = "";
  if (!is_object_type(spawn_error)) {
    size_t command_len = 0;
    char *command_str = vtype(command_val) == kTypeString ? js_getstr(js, command_val, &command_len) : NULL;
    const char *stderr_str = NULL;
    size_t stderr_len = 0;
    sync_value_bytes(js, o->stderr_val, &stderr_str, &stderr_len);

    if (stderr_len > 0) snprintf(
      message, sizeof(message), "Command failed: %.*s\n%.*s",
      (int)(command_len < 200 ? command_len : 200), command_str ? command_str : api,
      (int)(stderr_len < 250 ? stderr_len : 250), stderr_str
    );
    else snprintf(
      message, sizeof(message), "Command failed: %.*s",
      (int)(command_len < 200 ? command_len : 200), command_str ? command_str : api
    );
  }

  ant_value_t error = is_object_type(spawn_error)
    ? spawn_error
    : Ant_Error_Create(js, JS_ERR_GENERIC, message);
  if (!is_object_type(error)) return error;

  js_set(js, error, "status", o->status);
  js_set(js, error, "signal", o->signal);
  js_set(js, error, "stdout", o->stdout_val);
  js_set(js, error, "stderr", o->stderr_val);
  js_set(js, error, "pid", o->pid);
  js_set(js, error, "output", o->output);

  return error;
}

static void sync_echo_stderr(ant_t *js, const sync_outcome_t *o) {
  if (o->stdio_specified) return;

  const char *stderr_str = NULL;
  size_t stderr_len = 0;
  sync_value_bytes(js, o->stderr_val, &stderr_str, &stderr_len);

  size_t written = 0;
  while (stderr_str && written < stderr_len) {
    ssize_t n = write(STDERR_FILENO, stderr_str + written, stderr_len - written);
    if (n <= 0) break;
    written += (size_t)n;
  }
}

static ant_value_t builtin_execSync(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "execSync() requires a command");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Command must be a string");

  ant_value_t spawn_args[3];
  spawn_args[0] = args[0];
  spawn_args[1] = js_mkundef();
  spawn_args[2] = nargs >= 2 ? args[1] : js_mkundef();

  sync_outcome_t outcome;
  ant_value_t result = spawn_sync_impl(js, spawn_args, 3, true, &outcome);
  if (vtype(result) != kTypeObject) return result;

  sync_echo_stderr(js, &outcome);

  ant_value_t error = sync_result_error(js, &outcome, args[0], "execSync");
  if (!is_object_type(error)) return outcome.stdout_val;

  return js_throw(js, error);
}

static ant_value_t builtin_execFileSync(ant_params_t) {
  ant_value_t argv = js_mkundef();
  ant_value_t options = js_mkundef();
  ant_value_t spawn_args[3];

  if (nargs < 1) return js_mkerr(js, "execFileSync() requires a file");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "File must be a string");

  if (nargs >= 2) {
    if (js_is_array(args[1])) {
      argv = args[1];
      if (nargs >= 3 && is_special_object(args[2])) options = args[2];
    } else if (is_special_object(args[1])) options = args[1];
  }

  spawn_args[0] = args[0];
  spawn_args[1] = argv;
  spawn_args[2] = options;

  sync_outcome_t outcome;
  ant_value_t result = spawn_sync_impl(js, spawn_args, 3, false, &outcome);
  if (vtype(result) != kTypeObject) return result;
  sync_echo_stderr(js, &outcome);

  ant_value_t command = child_process_command_value(js, args[0], argv);
  if (vtype(command) != kTypeString) command = args[0];

  ant_value_t error = sync_result_error(js, &outcome, command, "execFileSync");
  if (is_object_type(error)) return js_throw(js, error);

  return outcome.stdout_val;
}

static ant_value_t builtin_fork(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "fork() requires a module path");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Module path must be a string");
  size_t path_len;
  
  char *path = js_getstr(js, args[0], &path_len);
  char *path_str = strndup(path, path_len);
  char exe_path[1024];

  if (ant_get_exe_path(exe_path, sizeof(exe_path), js->runtime.argc, js->runtime.argv) != 0) {
    free(path_str);
    return js_mkerr(js, "Failed to get executable path");
  }

  bool bundled = js_esm_bundle_active(js);
  ant_value_t spawn_args[3];
  spawn_args[0] = js_mkstr(js, exe_path, strlen(exe_path));

  ant_value_t args_arr = js_mkarr(js);
  if (!bundled) js_arr_push(js, args_arr, js_mkstr(js, path_str, path_len));
  
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t exec_args = js_get(js, args[1], "execArgv");
    if (is_special_object(exec_args)) {
      ant_value_t len_val = js_get(js, exec_args, "length");
      int arr_len = (int)js_getnum(len_val);
      for (int i = 0; i < arr_len; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        ant_value_t arg = js_get(js, exec_args, idx);
        js_arr_push(js, args_arr, arg);
      }
    }
  }
  
  spawn_args[1] = args_arr;
  spawn_args[2] = Ant_Object_CreateBare(js);

  if (bundled) uv_os_setenv(ANT_INTERNAL_RUN_ENV, path_str);
  ant_value_t result = builtin_spawn(js, spawn_args, 3, js_mkundef());
  if (bundled) uv_os_unsetenv(ANT_INTERNAL_RUN_ENV);

  free(path_str);
  return result;
}

ant_value_t child_process_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  
  ant_value_t exec_fn = Ant_Function_CreateWithData(js, builtin_exec, "exec", 3, js_mkundef());
  ant_value_t exec_file_fn = Ant_Function_CreateWithData(js, builtin_execFile, "execFile", 4, js_mkundef());
  
  child_process_init_constructor(js);
  
  js_set_symbol(js, exec_fn,
    "nodejs.util.promisify.custom",
    js_heavy_mkfun(js, exec_callback_promisified_call, exec_fn)
  );

  js_set_symbol(js, exec_file_fn,
    "nodejs.util.promisify.custom",
    js_heavy_mkfun(js, exec_callback_promisified_call, exec_file_fn)
  );
  
  js_set(js, lib, "ChildProcess", js->builtins.child_process_ctor);
  js_set(js, lib, "spawn", Ant_Function_CreateWithArity(builtin_spawn, 3));
  js_set(js, lib, "exec", exec_fn);
  js_set(js, lib, "execFile", exec_file_fn);
  js_set(js, lib, "execSync", Ant_Function_CreateWithArity(builtin_execSync, 2));
  js_set(js, lib, "execFileSync", Ant_Function_CreateWithArity(builtin_execFileSync, 3));
  js_set(js, lib, "spawnSync", Ant_Function_CreateWithArity(builtin_spawnSync, 3));
  js_set(js, lib, "fork", Ant_Function_CreateWithArity(builtin_fork, 1));
  js_set_tostringtag(js, lib, js_mkstr(js, "child_process", 13));
  
  return lib;
}

void cleanup_child_process_module(ant_t *js) {
  for (child_process_t *cp = js->active_children; cp; cp = cp->next) {
    cp->exited = true;
    cp->close_emitted = true;
    close_child_pipe(cp, CHILD_STREAM_STDIN, true);
    close_child_pipe(cp, CHILD_STREAM_STDOUT, true);
    close_child_pipe(cp, CHILD_STREAM_STDERR, true);
    cp->stdout_closed = cp->stderr_closed = true;
    ant_process_stage_close(&cp->process);
  }
}

int has_pending_child_processes(ant_t *js) {
  for (child_process_t *cp = js->active_children; cp; cp = cp->next) 
    if (cp->keep_alive) return 1;
  return 0;
}

void gc_mark_child_process(ant_t *js, gc_mark_fn mark) {
  for (child_process_t *cp = js->active_children; cp; cp = cp->next) {
    mark(js, cp->child_obj);
    mark(js, cp->stdin_obj);
    mark(js, cp->stdout_obj);
    mark(js, cp->stderr_obj);
    mark(js, cp->promise);
    mark(js, cp->exec_callback);
    mark(js, cp->exec_command);
    mark(js, cp->exec_spawn_error);
    mark(js, cp->spawnargs);
    for (child_write_req_t *w = cp->pending_writes; w; w = w->next) mark(js, w->callback);
  }
}
