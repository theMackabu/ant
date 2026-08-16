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
#include "ptr.h"
#include "errors.h"
#include "internal.h"

#include "gc/modules.h"
#include "silver/engine.h"

#include "process_plan.h"
#include "process_stage.h"

#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/process.h"
#include "modules/stream.h"
#include "modules/symbol.h"
#include "modules/child_process.h"

typedef struct 
  child_process_s child_process_t;

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
} stdio_mode_t;

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
  size_t stdout_seen;
  size_t stderr_seen;
  bool collect_output;
  bool stdout_read_paused;
  bool stderr_read_paused;
  bool stdin_closed;
  bool use_shell;
  bool detached;
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
  struct child_process_s *next;
  struct child_process_s *prev;
};

static child_process_t *pending_children_head = NULL;
static child_process_t *pending_children_tail = NULL;

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

enum {
  CHILD_PROCESS_NATIVE_TAG = 0x43505243u, // CPRC
  CHILD_STREAM_NATIVE_TAG = 0x4353544du   // CSTM
};

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

static void fprint_js_str_raw(FILE *out, ant_t *js, ant_value_t s) {
  if (vtype(s) != T_STR) {
    fprintf(out, "%s\n", js_str(js, s));
    return;
  }

  ant_offset_t len = 0;
  ant_offset_t off = vstr(js, s, &len);
  const char *ptr = (const char *)(uintptr_t)off;
  if (ptr && len > 0) fwrite(ptr, 1, (size_t)len, out);
  if (len == 0 || ptr[len - 1] != '\n') fputc('\n', out);
}

static void log_listener_error(ant_t *js, const char *event_name, ant_value_t err) {
  ant_value_t thrown_stack = js->thrown_stack;
  if (vtype(thrown_stack) == T_STR) {
    fprintf(stderr, "Error in child_process '%s' listener:\n", event_name);
    fprint_js_str_raw(stderr, js, thrown_stack);
    return;
  }

  ant_value_t thrown_value = js->thrown_value;
  ant_value_t src = (vtype(thrown_value) != T_UNDEF) ? thrown_value : err;
  
  ant_value_t stack = js_get(js, src, "stack");
  if (vtype(stack) == T_STR) {
    fprintf(stderr, "Error in child_process '%s' listener:\n", event_name);
    fprint_js_str_raw(stderr, js, stack);
    return;
  }

  ant_value_t name = js_get(js, src, "name");
  ant_value_t message = js_get(js, src, "message");

  const char *detail = NULL;
  if (vtype(name) == T_STR && vtype(message) == T_STR) {
    const char *name_s = js_str(js, name);
    const char *msg_s = js_str(js, message);
    if (msg_s && msg_s[0]) fprintf(stderr, "Error in child_process '%s' listener: %s: %s\n", event_name, name_s, msg_s);
    else detail = name_s;
  } 
  else if (vtype(message) == T_STR) detail = js_str(js, message);
  else detail = js_str(js, src);
  
  if (detail) fprintf(stderr, "Error in child_process '%s' listener: %s\n", event_name, detail);
  js_print_stack_trace_vm(js, stderr);
}

static void emit_event(child_process_t *cp, const char *name, ant_value_t *args, int nargs) {
  if (vtype(cp->child_obj) != T_OBJ) return;
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
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "Out of memory");
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
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

static void add_pending_child(child_process_t *cp) {
  cp->next = NULL;
  cp->prev = pending_children_tail;
  if (pending_children_tail) {
    pending_children_tail->next = cp;
  } else pending_children_head = cp;
  pending_children_tail = cp;
}

static void remove_pending_child(child_process_t *cp) {
  if (!cp->prev && !cp->next && pending_children_head != cp) return;
  if (cp->prev) cp->prev->next = cp->next;
  else pending_children_head = cp->next;
  if (cp->next) cp->next->prev = cp->prev;
  else pending_children_tail = cp->prev;
  cp->next = NULL;
  cp->prev = NULL;
}

static void free_child_process(child_process_t *cp) {
  if (!cp) return;

  if (vtype(cp->child_obj) == T_OBJ) {
    js_set_slot(cp->child_obj, SLOT_DATA, js_mkundef());
    js_clear_native(cp->child_obj, CHILD_PROCESS_NATIVE_TAG);
  }
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    ant_value_t obj = child_stream_obj(cp, i);
    if (vtype(obj) != T_OBJ) continue;
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
    remove_pending_child(cp);
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
    
    ant_value_t stdout_val = js_mkstr(cp->js, cp->stdout_buf ? cp->stdout_buf : "", cp->stdout_len);
    ant_value_t stderr_val = js_mkstr(cp->js, cp->stderr_buf ? cp->stderr_buf : "", cp->stderr_len);
    ant_value_t exit_code_val = child_exit_code_value(cp);
    ant_value_t signal_val = child_signal_value(cp->js, cp->term_signal);

    if (vtype(cp->stdout_obj) == T_OBJ) {
      js_set(cp->js, cp->stdout_obj, "text", stdout_val);
      js_set(cp->js, cp->stdout_obj, "length", js_mknum((double)cp->stdout_seen));
    }
    
    if (vtype(cp->stderr_obj) == T_OBJ) {
      js_set(cp->js, cp->stderr_obj, "text", stderr_val);
      js_set(cp->js, cp->stderr_obj, "length", js_mknum((double)cp->stderr_seen));
    }

    js_set(cp->js, cp->child_obj, "stdoutText", stdout_val);
    js_set(cp->js, cp->child_obj, "stderrText", stderr_val);
    js_set(cp->js, cp->child_obj, "exitCode", exit_code_val);
    js_set(cp->js, cp->child_obj, "signalCode", signal_val);
    
    ant_value_t close_args[2] = { exit_code_val, signal_val };
    emit_event(cp, "close", close_args, 2);
    
    if (vtype(cp->promise) != T_UNDEF) {
      ant_value_t result = js_mkobj(cp->js);
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

static ant_value_t child_spawn_failure_cb(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t child = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  child_process_t *cp = get_child_process(child);
  if (!cp) return js_mkundef();

  const char *file = cp->spawn_file ? cp->spawn_file : "";
  const char *name = uv_err_name(cp->spawn_error);

  char syscall[224];
  char message[256];
  snprintf(syscall, sizeof(syscall), "spawn %s", file);
  snprintf(message, sizeof(message), "%s %s", syscall, name);

  ant_value_t error = js_make_error_silent(js, JS_ERR_GENERIC, message);

  if (is_object_type(error)) {
    js_set(js, error, "code", js_mkstr(js, name, strlen(name)));
    js_set(js, error, "errno", js_mknum((double)cp->spawn_error));
    js_set(js, error, "syscall", js_mkstr(js, syscall, strlen(syscall)));
    js_set(js, error, "path", js_mkstr(js, file, strlen(file)));
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
  ant_value_t promise = js_mkpromise(js);
  ant_value_t callback = js_heavy_mkfun(js, child_spawn_failure_cb, cp->child_obj);

  js_resolve_promise(js, promise, js_mkundef());
  promise_mark_handled(js_promise_then(js, promise, callback, js_mkundef()));
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
    if (vtype(obj) == T_OBJ) stream_readable_push(cp->js, obj, js_mknull(), js_mkundef());
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
  size_t *seen = is_stdout ? &cp->stdout_seen : &cp->stderr_seen;
  
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

    if (vtype(obj) == T_OBJ) {
      ant_value_t accepted = stream_readable_push(
        cp->js, obj,
        make_buffer_chunk(cp->js, buf->base, (size_t)nread),
        js_mkundef()
      );
      *seen += (size_t)nread;
      js_set(cp->js, obj, "length", js_mknum((double)*seen));

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
      if (vtype(obj) == T_OBJ) eventemitter_emit_args(cp->js, obj, "error", err_args, 1);
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

static ant_value_t child_kill(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  child_process_t *cp = get_child_process(this_obj);
  if (!cp) return js_false;
  if (cp->exited) return js_false;
  
  int sig = SIGTERM;
  if (nargs > 0) {
    if (vtype(args[0]) == T_NUM) {
      sig = (int)js_getnum(args[0]);
    } else if (vtype(args[0]) == T_STR) {
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
      callback_args[0] = js_mkerr(
        write->cp->js, "%s", uv_strerror(status)
      );
      child_stream_call_callback(
        write->cp->js, write->callback, callback_args, 1
      );
    } else child_stream_call_callback(
      write->cp->js, write->callback, NULL, 0
    );
  } else if (status < 0 && write->cp && !write->cp->suppress_stdin_errors &&
             vtype(write->cp->stdin_obj) == T_OBJ) {
    callback_args[0] = js_mkerr(
      write->cp->js, "%s", uv_strerror(status)
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
  
  if (vtype(data_arg) == T_STR) {
    data = js_getstr(js, data_arg, &data_len);
    if (!data) return js_mkerr(js, "Data must be a string or Buffer");
  } else {
    TypedArrayData *ta_data = buffer_get_typedarray_data(data_arg);
    if (!ta_data || !ta_data->buffer || !ta_data->buffer->data) {
      return js_mkerr(js, "Data must be a string or Buffer");
    }
    data = (const char *)(ta_data->buffer->data + ta_data->byte_offset);
    data_len = ta_data->byte_length;
  }

  child_write_req_t *write = calloc(1, sizeof(*write));
  char *buf_data = data_len ? malloc(data_len) : NULL;
  uv_buf_t buf;
  int result = 0;

  if (!write || (data_len && !buf_data)) {
    free(write); free(buf_data);
    return js_mkerr(js, "Out of memory");
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

static ant_value_t child_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 1) return js_mkerr(js, "write() requires data argument");
  
  child_process_t *cp = get_child_process(this_obj);
  if (!cp) return js_mkerr(js, "Invalid child process object");
  return child_write_impl(js, cp, args[0], js_mkundef());
}

static ant_value_t child_ref(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t child_unref(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t child_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  child_process_t *cp = get_child_process(this_obj);
  if (!cp) return js_mkundef();
  return child_end_impl(cp);
}

static ant_value_t child_process_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
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

  js->builtins.child_process_proto = js_mkobj(js);
  if (is_object_type(ee_proto)) js_set_proto_init(js->builtins.child_process_proto, ee_proto);
  js_set(js, js->builtins.child_process_proto, "kill", js_mkfun(child_kill));
  js_set(js, js->builtins.child_process_proto, "ref", js_mkfun(child_ref));
  js_set(js, js->builtins.child_process_proto, "unref", js_mkfun(child_unref));

  js->builtins.child_process_ctor = js_make_ctor(
    js, child_process_ctor,
    js->builtins.child_process_proto, "ChildProcess", 12
  );
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

static ant_value_t child_stream_ref(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);
  if (!ctx || !ctx->cp) return this_obj;

  uv_handle_t *h = child_stream_handle(ctx->cp, ctx->kind);
  if (h && !uv_is_closing(h)) uv_ref(h);
  return this_obj;
}

static ant_value_t child_stream_unref(ant_t *js, ant_value_t *args, int nargs) {
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
  sv_vm_call(js->vm, js, callback, js_mkundef(), args, nargs, NULL, false);
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

static ant_value_t child_stream__destroy(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);

  if (ctx && ctx->cp) child_stream_close(ctx);
  child_stream_call_callback(js, callback, &error, 1);
  return js_mkundef();
}

static ant_value_t child_stream__read(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t child_stream__write(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t child_stream__final(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  child_stream_ctx_t *ctx = get_child_stream_ctx(this_obj);

  if (ctx && ctx->cp) (void)child_end_impl(ctx->cp);
  child_stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static void child_stream_init_protos(ant_t *js) {
  if (js->builtins.child_readable_proto && js->builtins.child_writable_proto) return;

  js->builtins.child_readable_proto = js_mkobj(js);
  js_set_proto_init(js->builtins.child_readable_proto, stream_readable_prototype(js));
  js_set(js, js->builtins.child_readable_proto, "ref", js_mkfun(child_stream_ref));
  js_set(js, js->builtins.child_readable_proto, "unref", js_mkfun(child_stream_unref));
  js_set(js, js->builtins.child_readable_proto, "_destroy", js_mkfun(child_stream__destroy));
  js_set(js, js->builtins.child_readable_proto, "_read", js_mkfun(child_stream__read));

  js->builtins.child_writable_proto = js_mkobj(js);
  js_set_proto_init(js->builtins.child_writable_proto, stream_writable_prototype(js));
  js_set(js, js->builtins.child_writable_proto, "ref", js_mkfun(child_stream_ref));
  js_set(js, js->builtins.child_writable_proto, "unref", js_mkfun(child_stream_unref));
  js_set(js, js->builtins.child_writable_proto, "_destroy", js_mkfun(child_stream__destroy));
  js_set(js, js->builtins.child_writable_proto, "_write", js_mkfun(child_stream__write));
  js_set(js, js->builtins.child_writable_proto, "_final", js_mkfun(child_stream__final));
}

static ant_value_t create_child_stream_object(ant_t *js, child_process_t *cp, child_stream_kind_t kind) {
  bool writable = kind == CHILD_STREAM_STDIN;
  ant_value_t obj = js_mkobj(js);
  child_stream_ctx_t *ctx = calloc(1, sizeof(child_stream_ctx_t));
  if (!ctx) return js_mkerr(js, "Out of memory");
  
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

  if (!writable) {
    js_set(js, obj, "encoding", js_mknull());
    js_set(js, obj, "readableEncoding", js_mknull());
  }

  return obj;
}

static ant_value_t create_child_object(ant_t *js, child_process_t *cp) {
  ant_value_t obj = js_mkobj(js);
  if (is_object_type(js->builtins.child_process_proto)) js_set_proto_init(obj, js->builtins.child_process_proto);
  
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
      *stream_objs[i] = create_child_stream_object(js, cp, streams[i].kind);
    } else *stream_objs[i] = js_mknull();
    js_set(js, obj, streams[i].name, *stream_objs[i]);
  }
  
  js_set(js, obj, "ref", js_mkfun(child_ref));
  js_set(js, obj, "unref", js_mkfun(child_unref));
  js_set(js, obj, "kill", js_mkfun(child_kill));
  js_set(js, obj, "write", js_mkfun(child_write));
  js_set(js, obj, "end", js_mkfun(child_end));
  
  js_set_sym(js, obj, get_toStringTag_sym(), js_mkstr(js, "ChildProcess", 12));
  
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
    if (vtype(val) == T_STR) {
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

static ant_value_t child_process_command_value(
  ant_t *js,
  ant_value_t file,
  ant_value_t argv
) {
  size_t file_len = 0;
  char *file_text;
  ant_offset_t argc;
  size_t total;
  char *command;
  size_t at;

  if (vtype(file) != T_STR) return js_mkundef();
  file_text = js_getstr(js, file, &file_len);
  argc = vtype(argv) == T_ARR ? js_arr_len(js, argv) : 0;
  total = file_len;

  for (ant_offset_t i = 0; i < argc; i++) {
    ant_value_t arg = js_arr_get(js, argv, i);
    size_t arg_len = 0;
    if (vtype(arg) == T_STR) js_getstr(js, arg, &arg_len);
    if (arg_len > SIZE_MAX - 2 || total > SIZE_MAX - arg_len - 2)
      return js_mkundef();
    total += arg_len + 1;
  }

  command = malloc(total + 1);
  if (!command) return js_mkundef();

  memcpy(command, file_text, file_len);
  at = file_len;
  for (ant_offset_t i = 0; i < argc; i++) {
    ant_value_t arg = js_arr_get(js, argv, i);
    size_t arg_len = 0;
    char *arg_text = vtype(arg) == T_STR
      ? js_getstr(js, arg, &arg_len)
      : NULL;
    command[at++] = ' ';
    if (arg_len > 0) {
      memcpy(command + at, arg_text, arg_len);
      at += arg_len;
    }
  }
  command[at] = '\0';

  ant_value_t result = js_mkstr(js, command, at);
  free(command);
  return result;
}

static void free_args_array(char **args, int count) {
  if (!args) return;
  for (int i = 0; i < count; i++) {
    if (args[i]) free(args[i]);
  }
  free(args);
}

static ant_value_t child_process_options_arg(ant_value_t *args, int nargs) {
  if (nargs >= 3 && is_special_object(args[2])) return args[2];
  if (nargs >= 2 && vtype(args[1]) != T_ARR && is_special_object(args[1])) return args[1];
  return js_mkundef();
}

static char **parse_env_object(ant_t *js, ant_value_t env_obj) {
  if (!is_special_object(env_obj)) return NULL;

  ant_value_t keys = js_own_property_keys(js, env_obj, false, true);
  if (is_err(keys) || vtype(keys) != T_ARR) return NULL;

  ant_offset_t len = js_arr_len(js, keys);
  char **env = calloc((size_t)len + 1, sizeof(char *));
  if (!env) return NULL;

  size_t out = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    if (vtype(key_val) != T_STR) continue;

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

    ant_value_t value_str = vtype(value) == T_STR ? value : js_tostring_val(js, value);
    if (is_err(value_str) || vtype(value_str) != T_STR) {
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

static stdio_mode_t parse_stdio_mode(ant_t *js, ant_value_t val) {
  if (vtype(val) != T_STR) return STDIO_PIPE;
  char *s = js_getstr(js, val, NULL);
  if (strcmp(s, "inherit") == 0) return STDIO_INHERIT;
  if (strcmp(s, "ignore") == 0) return STDIO_IGNORE;
  return STDIO_PIPE;
}

static void parse_stdio_option(ant_t *js, ant_value_t stdio_val, stdio_mode_t *modes) {
if (vtype(stdio_val) == T_STR) {
  stdio_mode_t mode = parse_stdio_mode(js, stdio_val);
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) modes[i] = mode;
} else if (is_special_object(stdio_val)) {
  ant_offset_t len = js_arr_len(js, stdio_val);
  if (len > 3) len = 3;
  for (ant_offset_t i = 0; i < len; i++) 
    modes[i] = parse_stdio_mode(js, js_arr_get(js, stdio_val, i));
}}

static ant_value_t builtin_spawn(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "spawn() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  
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
  
  if (nargs >= 2 && vtype(args[1]) == T_ARR) spawn_args = parse_args_array(js, args[1], &spawn_argc);
  if (spawn_argc < 0) {
    free(cmd_str);
    return mkval(T_ERR, 0);
  }
  
  stdio_mode_t stdio_modes[3] = { 
    STDIO_PIPE, STDIO_PIPE, STDIO_PIPE 
  };
  
  if (is_special_object(options_arg)) {
    ant_value_t cwd_val = js_get(js, options_arg, "cwd");
    if (vtype(cwd_val) == T_STR) {
      size_t cwd_len;
      char *cwd_str = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_str, cwd_len);
    }
    
    ant_value_t shell_val = js_get(js, options_arg, "shell");
    use_shell = js_truthy(js, shell_val);
    
    ant_value_t detached_val = js_get(js, options_arg, "detached");
    detached = js_truthy(js, detached_val);
    
    ant_value_t stdio_val = js_get(js, options_arg, "stdio");
    parse_stdio_option(js, stdio_val, stdio_modes);

    ant_value_t env_val = js_get(js, options_arg, "env");
    env = parse_env_object(js, env_val);
  }
  
  child_process_t *cp = calloc(1, sizeof(child_process_t));
  if (!cp) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    free_env_array(env);
    if (cwd) free(cwd);
    return js_mkerr(js, "Out of memory");
  }
  
  cp->js = js;
  cp->use_shell = use_shell;
  cp->detached = detached;
  cp->cwd = cwd;
  cp->promise = js_mkundef();
  cp->keep_alive = true;
  memcpy(cp->stdio_modes, stdio_modes, sizeof(stdio_modes));
  ant_process_stage_init(
    &cp->process, cp, on_process_exit, on_process_close
  );
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
  if (stdio_modes[i] == STDIO_PIPE) {
    uv_pipe_t *p = child_pipe(cp, i);
    uv_pipe_init(uv_default_loop(), p, 0);
    p->data = cp;
  }}
  
  uv_stdio_container_t stdio[3];
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
  if (stdio_modes[i] == STDIO_INHERIT) {
    ant_process_stdio_inherit_fd(&stdio[i], i);
  } else if (stdio_modes[i] == STDIO_IGNORE) {
    ant_process_stdio_ignore(&stdio[i]);
  } else {
    ant_process_stdio_create_pipe(
      &stdio[i], (uv_stream_t *)child_pipe(cp, i),
      i == CHILD_STREAM_STDIN
    );
  }}
  
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
    .file = final_args[0],
    .args = final_args,
    .env = env,
    .cwd = cwd,
    .flags = detached ? UV_PROCESS_DETACHED : 0,
    .stdio = stdio,
    .stdio_count = 3,
  };
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

    add_pending_child(cp);
    cp->child_obj = create_child_object(js, cp);
    js_set(js, cp->child_obj, "pid", js_mkundef());
    child_schedule_spawn_failure(js, cp);

    return cp->child_obj;
  }
  
  static const uv_read_cb read_cbs[] = { NULL, on_stdout_read, on_stderr_read };
  bool *closed[] = { &cp->stdin_closed, &cp->stdout_closed, &cp->stderr_closed };
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    if (stdio_modes[i] == STDIO_PIPE && read_cbs[i]) {
      uv_read_start((uv_stream_t *)child_pipe(cp, i), alloc_buffer, read_cbs[i]);
    } else if (stdio_modes[i] != STDIO_PIPE) *closed[i] = true;
  }
  
  add_pending_child(cp);
  cp->child_obj = create_child_object(js, cp);
  
  return cp->child_obj;
}

static ant_value_t exec_file_close_callback(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t exec_spawn_error_capture(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t builtin_exec(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = js_mkundef();

  if (nargs < 1) return js_mkerr(js, "exec() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  if (nargs >= 2 && is_callable(args[nargs - 1])) callback = args[nargs - 1];
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  if (cmd && memchr(cmd, '\0', cmd_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Child process command cannot contain NUL bytes");
  char *cmd_str = strndup(cmd, cmd_len);
  
  char *cwd = NULL;
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t cwd_val = js_get(js, args[1], "cwd");
    if (vtype(cwd_val) == T_STR) {
      size_t cwd_len;
      char *cwd_s = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_s, cwd_len);
    }
  }
  
  child_process_t *cp = calloc(1, sizeof(child_process_t));
  if (!cp) {
    free(cmd_str);
    if (cwd) free(cwd);
    return js_mkerr(js, "Out of memory");
  }
  
  cp->js = js;
  cp->use_shell = true;
  cp->cwd = cwd;
  cp->collect_output = true;
  cp->promise = is_callable(callback) ? js_mkundef() : js_mkpromise(js);
  cp->keep_alive = true;
  
  cp->stdio_modes[CHILD_STREAM_STDIN] = STDIO_IGNORE;
  cp->stdio_modes[CHILD_STREAM_STDOUT] = STDIO_PIPE;
  cp->stdio_modes[CHILD_STREAM_STDERR] = STDIO_PIPE;
  
  uv_pipe_init(uv_default_loop(), &cp->stdout_pipe, 0);
  uv_pipe_init(uv_default_loop(), &cp->stderr_pipe, 0);
  
  cp->stdout_pipe.data = cp;
  cp->stderr_pipe.data = cp;
  ant_process_stage_init(
    &cp->process, cp, on_process_exit, on_process_close
  );
  cp->stdin_closed = true;
  
  uv_stdio_container_t stdio[3];
  ant_process_stdio_ignore(&stdio[0]);
  ant_process_stdio_create_pipe(
    &stdio[1], (uv_stream_t *)&cp->stdout_pipe, false
  );
  ant_process_stdio_create_pipe(
    &stdio[2], (uv_stream_t *)&cp->stderr_pipe, false
  );
  
  char *shell_args[4];
  shell_args[0] = "/bin/sh";
  shell_args[1] = "-c";
  shell_args[2] = cmd_str;
  shell_args[3] = NULL;
  
  ant_process_spawn_spec_t spec = {
    .file = "/bin/sh",
    .args = shell_args,
    .cwd = cwd,
    .stdio = stdio,
    .stdio_count = 3,
  };
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

    add_pending_child(cp);
    cp->child_obj = create_child_object(js, cp);
    js_set(js, cp->child_obj, "pid", js_mkundef());

    if (is_callable(callback)) {
      ant_value_t ctx = js_mkobj(js);
      js_set(js, ctx, "callback", callback);
      js_set(js, ctx, "child", cp->child_obj);
      js_set(js, ctx, "command", args[0]);
      eventemitter_add_listener(
        js, cp->child_obj, "error",
        js_heavy_mkfun(js, exec_spawn_error_capture, ctx), true
      );
      eventemitter_add_listener(
        js, cp->child_obj, "close",
        js_heavy_mkfun(js, exec_file_close_callback, ctx), true
      );
      child_schedule_spawn_failure(js, cp);
      return cp->child_obj;
    }

    child_schedule_spawn_failure(js, cp);
    return cp->promise;
  }
  
  uv_read_start((uv_stream_t *)&cp->stdout_pipe, alloc_buffer, on_stdout_read);
  uv_read_start((uv_stream_t *)&cp->stderr_pipe, alloc_buffer, on_stderr_read);
  
  add_pending_child(cp);
  
  cp->child_obj = create_child_object(js, cp);

  if (is_callable(callback)) {
    ant_value_t ctx = js_mkobj(js);
    js_set(js, ctx, "callback", callback);
    js_set(js, ctx, "child", cp->child_obj);
    js_set(js, ctx, "command", args[0]);
    eventemitter_add_listener(
      js, cp->child_obj, "close",
      js_heavy_mkfun(js, exec_file_close_callback, ctx), true
    );
    return cp->child_obj;
  }

  return cp->promise;
}

static ant_value_t exec_spawn_error_capture(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctx = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0) js_set(js, ctx, "spawnError", args[0]);
  return js_mkundef();
}

static ant_value_t exec_file_close_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t ctx = js_get_slot(fn, SLOT_DATA);
  
  ant_value_t callback = js_get(js, ctx, "callback");
  ant_value_t child = js_get(js, ctx, "child");
  
  ant_value_t stdout_val = js_get(js, child, "stdoutText");
  ant_value_t stderr_val = js_get(js, child, "stderrText");
  ant_value_t exit_code_val = js_get(js, child, "exitCode");
  ant_value_t signal_code_val = js_get(js, child, "signalCode");
  ant_value_t command_val = js_get(js, ctx, "command");
  ant_value_t cb_args[3];
  bool exited_nonzero =
    vtype(exit_code_val) == T_NUM && (int)js_getnum(exit_code_val) != 0;
  bool was_signaled = vtype(signal_code_val) == T_STR;

  if (!is_callable(callback)) return js_mkundef();

  ant_value_t spawn_error = js_get(js, ctx, "spawnError");
  if (is_object_type(spawn_error)) {
    if (vtype(command_val) == T_STR) js_set(js, spawn_error, "cmd", command_val);
    js_set(js, spawn_error, "killed", js_false);
    cb_args[0] = spawn_error;
    cb_args[1] = js_mkstr(js, "", 0);
    cb_args[2] = js_mkstr(js, "", 0);
    sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 3, NULL, false);
    return js_mkundef();
  }

  if (exited_nonzero || was_signaled) {
    size_t command_len = 0;
    size_t stderr_len = 0;
    char *command = vtype(command_val) == T_STR
      ? js_getstr(js, command_val, &command_len)
      : NULL;
    char *stderr_text = vtype(stderr_val) == T_STR
      ? js_getstr(js, stderr_val, &stderr_len)
      : NULL;
    char fallback_message[256];

    if (command) {
      static const char prefix[] = "Command failed: ";
      size_t message_len = sizeof(prefix) - 1 + command_len + 1 + stderr_len;
      char *message = malloc(message_len + 1);

      if (message) {
        size_t at = 0;
        memcpy(message + at, prefix, sizeof(prefix) - 1);
        at += sizeof(prefix) - 1;
        if (command_len > 0) {
          memcpy(message + at, command, command_len);
          at += command_len;
        }
        message[at++] = '\n';
        if (stderr_len > 0) {
          memcpy(message + at, stderr_text, stderr_len);
          at += stderr_len;
        }
        message[at] = '\0';
        cb_args[0] = js_make_error_silent(js, JS_ERR_GENERIC, message);
        free(message);
      } else {
        cb_args[0] = js_make_error_silent(js, JS_ERR_GENERIC, "Command failed");
      }
    } else {
      if (was_signaled) {
        size_t signal_len = 0;
        char *signal_name = js_getstr(js, signal_code_val, &signal_len);
        snprintf(
          fallback_message, sizeof(fallback_message),
          "Command failed with signal %.*s",
          (int)signal_len, signal_name
        );
      } else {
        snprintf(
          fallback_message, sizeof(fallback_message),
          "Command failed with exit code %d",
          (int)js_getnum(exit_code_val)
        );
      }
      cb_args[0] =
        js_make_error_silent(js, JS_ERR_GENERIC, fallback_message);
    }

    if (is_object_type(cb_args[0])) {
      ant_value_t signal_val = js_mknull();
      if (was_signaled) signal_val = signal_code_val;

      js_set(js, cb_args[0], "code", was_signaled ? js_mknull() : exit_code_val);
      js_set(js, cb_args[0], "signal", signal_val);
      js_set(js, cb_args[0], "killed", js_get(js, child, "killed"));
      if (vtype(command_val) == T_STR) js_set(js, cb_args[0], "cmd", command_val);
    }
  } else cb_args[0] = js_mknull();

  cb_args[1] = stdout_val;
  cb_args[2] = stderr_val;

  ant_value_t result = sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 3, NULL, false);
  if (vtype(result) == T_ERR) log_listener_error(js, "execFile", result);
  return js_mkundef();
}

static ant_value_t exec_file_promisify_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  if (vtype(settled) == T_BOOL && settled == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  if (vtype(promise) != T_PROMISE) return js_mkundef();

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

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", stdout_val);
  js_set(js, result, "stderr", stderr_val);
  js_resolve_promise(js, promise, result);
  
  return js_mkundef();
}

static ant_value_t exec_callback_promisified_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t original = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_callable(original)) return js_mkerr(js, "exec promisify target is not callable");

  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = js_mkobj(js);
  
  js_set_slot(state, SLOT_DATA, promise);
  js_set_slot(state, SLOT_SETTLED, js_false);
  
  ant_value_t callback = js_heavy_mkfun(js, exec_file_promisify_callback, state);
  ant_value_t *call_args = malloc((size_t)(nargs + 1) * sizeof(ant_value_t));
  
  if (!call_args) {
    js_reject_promise(js, promise, js_mkerr(js, "Out of memory"));
    return promise;
  }

  for (int i = 0; i < nargs; i++) call_args[i] = args[i];
  call_args[nargs] = callback;

  ant_value_t call_result = sv_vm_call(
    js->vm, js, original, js_getthis(js), 
    call_args, nargs + 1, NULL, false
  ); free(call_args);

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  bool is_settled = (vtype(settled) == T_BOOL && settled == js_true);
  
  if (!is_settled && (is_err(call_result) || js->thrown_exists)) {
    ant_value_t ex = js->thrown_exists ? js->thrown_value : call_result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    js_set_slot(state, SLOT_SETTLED, js_true);
    js_reject_promise(js, promise, ex);
  }

  return promise;
}

static ant_value_t builtin_execFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t argv = js_mkundef();
  ant_value_t options = js_mkundef();
  ant_value_t callback = js_mkundef();
  
  ant_value_t spawn_args[3];
  ant_value_t child;

  if (nargs < 1) return js_mkerr(js, "execFile() requires a file");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "File must be a string");

  if (nargs >= 2 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    nargs--;
  }

  if (nargs >= 2) {
    if (vtype(args[1]) == T_ARR) {
      argv = args[1];
      if (nargs >= 3 && is_special_object(args[2])) options = args[2];
    } else if (is_special_object(args[1])) options = args[1];
  }

  spawn_args[0] = args[0];
  spawn_args[1] = argv;
  spawn_args[2] = options;

  child = builtin_spawn(js, spawn_args, 3);
  if (vtype(child) != T_OBJ || !is_callable(callback)) return child;

  {
    child_process_t *spawned = get_child_process(child);
    if (spawned) spawned->collect_output = true;
  }

  ant_value_t ctx = js_mkobj(js);
  js_set(js, ctx, "callback", callback);
  js_set(js, ctx, "child", child);
  js_set(js, ctx, "command", child_process_command_value(js, args[0], argv));

  ant_value_t close_listener = js_heavy_mkfun(js, exec_file_close_callback, ctx);
  eventemitter_add_listener(
    js, child, "error",
    js_heavy_mkfun(js, exec_spawn_error_capture, ctx), true
  );
  eventemitter_add_listener(js, child, "close", close_listener, true);

  return child;
}

static bool child_process_plan_copy_string(
  ant_t *js, ant_value_t value, const char *description, char **out
) {
  if (vtype(value) != T_STR) {
    js_mkerr_typed(js, JS_ERR_TYPE, "%s must be a string", description);
    return false;
  }
  size_t len = 0;
  char *text = js_getstr(js, value, &len);
  if (!text || memchr(text, '\0', len)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "%s cannot contain NUL bytes", description);
    return false;
  }
  char *copy = malloc(len + 1);
  if (!copy) {
    js_mkerr(js, "Out of memory");
    return false;
  }
  if (len) memcpy(copy, text, len);
  copy[len] = '\0';
  *out = copy;
  return true;
}

static bool child_process_plan_apply_options(
  ant_t *js, ant_process_plan_t *plan, ant_value_t options
) {
  if (!is_special_object(options)) return true;
  ant_value_t cwd = js_get(js, options, "cwd");
  if (!is_undefined(cwd) && !child_process_plan_copy_string(
    js, cwd, "Child process cwd", &plan->cwd
  )) return false;
  ant_value_t redirects = js_get(js, options, "redirections");
  if (is_undefined(redirects)) return true;
  if (vtype(redirects) != T_ARR) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Child process redirections must be an array");
    return false;
  }
  for (ant_offset_t i = 0; i < js_arr_len(js, redirects); i++) {
    ant_value_t redirect = js_arr_get(js, redirects, i);
    ant_value_t kind_value = is_special_object(redirect)
      ? js_get(js, redirect, "kind") : js_mkundef();
    if (vtype(kind_value) != T_NUM) {
      js_mkerr_typed(js, JS_ERR_TYPE, "Invalid child process redirection");
      return false;
    }
    int kind = (int)js_getnum(kind_value);
    if (kind < ANT_PROCESS_REDIRECT_STDIN ||
        kind > ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT) {
      js_mkerr_typed(js, JS_ERR_TYPE, "Invalid child process redirection kind");
      return false;
    }
    char *path = NULL;
    if (kind != ANT_PROCESS_REDIRECT_STDERR_TO_STDOUT &&
        !child_process_plan_copy_string(
          js, js_get(js, redirect, "path"),
          "Child process redirection path", &path
        )) return false;
    bool added = ant_process_plan_add_redirect(
      plan, (ant_process_redirect_kind_t)kind, path
    );
    free(path);
    if (!added) {
      js_mkerr(js, "Out of memory");
      return false;
    }
  }
  return true;
}

static bool child_process_plan_add_values(
  ant_t *js, ant_process_plan_t *plan, ant_value_t values
) {
  if (vtype(values) != T_ARR || js_arr_len(js, values) == 0) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Process command must be a non-empty array");
    return false;
  }
  size_t count = (size_t)js_arr_len(js, values);
  const char **argv = calloc(count, sizeof(*argv));
  if (!argv) {
    js_mkerr(js, "Out of memory");
    return false;
  }
  bool valid = true;
  for (size_t i = 0; i < count; i++) {
    ant_value_t value = js_arr_get(js, values, (ant_offset_t)i);
    size_t len = 0;
    if (vtype(value) != T_STR) {
      js_mkerr_typed(js, JS_ERR_TYPE, "Child process arguments must be strings");
      valid = false;
      break;
    }
    argv[i] = js_getstr(js, value, &len);
    if (!argv[i] || memchr(argv[i], '\0', len)) {
      js_mkerr_typed(js, JS_ERR_TYPE, "Child process arguments cannot contain NUL bytes");
      valid = false;
      break;
    }
  }
  if (valid && argv[0][0] == '\0') {
    js_mkerr_typed(js, JS_ERR_TYPE, "Process executable cannot be empty");
    valid = false;
  }
  if (valid && !ant_process_plan_add_command(plan, argv, count)) {
    js_mkerr(js, "Out of memory");
    valid = false;
  }
  free(argv);
  return valid;
}

static bool child_process_plan_add_native_result(
  ant_t *js, ant_process_plan_t *plan, ant_value_t result
) {
  ant_value_t stdout_value = js_get(js, result, "stdout");
  ant_value_t stderr_value = js_get(js, result, "stderr");
  ant_value_t exit_code_value = js_get(js, result, "exitCode");
  
  if (vtype(exit_code_value) != T_NUM) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid native pipeline stage");
    return false;
  }
  
  size_t stdout_len = 0;
  size_t stderr_len = 0;
  
  const uint8_t *stdout_data = NULL;
  const uint8_t *stderr_data = NULL;
  
  bool stdout_valid = buffer_source_get_bytes(
    js, stdout_value, 
    &stdout_data, &stdout_len
  );
  
  bool stderr_valid = buffer_source_get_bytes(
    js, stderr_value, 
    &stderr_data, &stderr_len
  );
  
  if (!stdout_valid || !stderr_valid) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid native pipeline stage payload");
    return false;
  }

  if (!ant_process_plan_add_native_stage(
    plan, (const char *)stdout_data, stdout_len,
    (const char *)stderr_data, stderr_len,
    (int)js_getnum(exit_code_value)
  )) {
    js_mkerr(js, "Out of memory");
    return false;
  }
  
  return true;
}

ant_value_t child_process_exec_file_result(
  ant_t *js,
  ant_value_t file,
  ant_value_t argv,
  ant_value_t options
) {
  ant_process_plan_t plan;
  ant_process_plan_init(&plan);
  plan.result_mode = ANT_PROCESS_RESULT_BYTES;
  
  ant_value_t values = js_mkarr(js);
  js_arr_push(js, values, file);
  
  if (vtype(argv) == T_ARR) for (ant_offset_t i = 0; i < js_arr_len(js, argv); i++)
    js_arr_push(js, values, js_arr_get(js, argv, i));
  if (!child_process_plan_apply_options(js, &plan, options) ||
      !child_process_plan_add_values(js, &plan, values)) {
    ant_process_plan_dispose(&plan);
    return ant_process_plan_rejected_result(js,
      js->thrown_exists ? js->thrown_value : js_mkerr(js, "Invalid process plan"));
  }
  
  return ant_process_plan_submit(js, &plan);
}

ant_value_t child_process_pipeline_result(
  ant_t *js,
  ant_value_t commands,
  ant_value_t options
) {
  if (vtype(commands) != T_ARR || js_arr_len(js, commands) == 0) {
    return ant_process_plan_rejected_result(js,
      js_mkerr(js, "pipeline requires at least one command"));
  }
  ant_process_plan_t plan;
  ant_process_plan_init(&plan);
  plan.result_mode = ANT_PROCESS_RESULT_BYTES;
  if (!child_process_plan_apply_options(js, &plan, options)) goto invalid;
  for (ant_offset_t i = 0; i < js_arr_len(js, commands); i++) {
    ant_value_t command = js_arr_get(js, commands, i);
    if (vtype(command) == T_ARR) {
      if (!child_process_plan_add_values(js, &plan, command)) goto invalid;
    } else if (is_special_object(command)) {
      if (!child_process_plan_add_native_result(js, &plan, command)) goto invalid;
    } else {
      js_mkerr_typed(js, JS_ERR_TYPE, "Invalid pipeline stage");
      goto invalid;
    }
  }
  return ant_process_plan_submit(js, &plan);

invalid:
  ant_process_plan_dispose(&plan);
  return ant_process_plan_rejected_result(js,
    js->thrown_exists ? js->thrown_value : js_mkerr(js, "Invalid process plan"));
}

static bool sync_encoding_wants_string(ant_t *js, ant_value_t options_arg) {
  if (!is_special_object(options_arg)) return false;

  ant_value_t encoding_val = js_get(js, options_arg, "encoding");
  if (vtype(encoding_val) != T_STR) return false;

  size_t encoding_len = 0;
  char *encoding = js_getstr(js, encoding_val, &encoding_len);
  if (!encoding) return false;

  if (encoding_len == 6 && strncmp(encoding, "buffer", 6) == 0) return false;
  return true;
}

static bool sync_value_bytes(ant_t *js, ant_value_t value, const char **out, size_t *len) {
  *out = NULL;
  *len = 0;

  if (vtype(value) == T_STR) {
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

static ant_value_t sync_make_output(ant_t *js, const char *bytes, size_t len, bool as_string) {
  if (as_string) return js_mkstr(js, bytes ? bytes : "", len);

  ArrayBufferData *buffer = create_array_buffer_data(len);
  if (!buffer) return js_mkerr(js, "Out of memory");
  if (bytes && len > 0) memcpy(buffer->data, bytes, len);

  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
}

#ifdef _WIN32
static ant_value_t spawn_sync_impl(ant_t *js, ant_value_t *args, int nargs, bool force_shell) {
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");

  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);

  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *input = NULL;
  size_t input_len = 0;
  bool use_shell = force_shell;
  ant_value_t options_arg = child_process_options_arg(args, nargs);

  if (nargs >= 2 && vtype(args[1]) == T_ARR) spawn_args = parse_args_array(js, args[1], &spawn_argc);
  if (spawn_argc < 0) {
    free(cmd_str);
    return mkval(T_ERR, 0);
  }

  if (is_special_object(options_arg)) {
    ant_value_t input_val = js_get(js, options_arg, "input");
    if (vtype(input_val) == T_STR) {
      input = js_getstr(js, input_val, &input_len);
    }
    if (!use_shell) use_shell = js_truthy(js, js_get(js, options_arg, "shell"));
  }

  size_t cmdline_len = cmd_len + 3;
  for (int i = 0; i < spawn_argc; i++) {
    cmdline_len += strlen(spawn_args[i]) + 3;
  }
  
  char *raw_cmdline = malloc(cmdline_len);
  if (!raw_cmdline) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Out of memory");
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
      return js_mkerr(js, "Out of memory");
    }
    snprintf(cmdline, shell_len, "%s /d /s /c \"%s\"", shell, raw_cmdline);
  }
  
  SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  HANDLE stdin_read = NULL, stdin_write = NULL;
  HANDLE stdout_read = NULL, stdout_write = NULL;
  HANDLE stderr_read = NULL, stderr_write = NULL;
  
  if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
      !CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
    free(cmdline);
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Failed to create pipes");
  }
  
  SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  
  STARTUPINFOA si = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_read;
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_write;
  
  PROCESS_INFORMATION pi = {0};
  
  BOOL success = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  
  free(cmdline);
  if (cmdline != raw_cmdline) free(raw_cmdline);
  free(cmd_str);
  free_args_array(spawn_args, spawn_argc);
  
  CloseHandle(stdin_read);
  CloseHandle(stdout_write);
  CloseHandle(stderr_write);
  
  if (!success) {
    CloseHandle(stdin_write);
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    return js_mkerr(js, "Failed to create process");
  }
  
  if (input && input_len > 0) {
    DWORD written;
    WriteFile(stdin_write, input, (DWORD)input_len, &written, NULL);
  }
  CloseHandle(stdin_write);
  
  char *stdout_buf = malloc(4096);
  size_t stdout_len = 0, stdout_cap = 4096;
  char *stderr_buf = malloc(4096);
  size_t stderr_len = 0, stderr_cap = 4096;
  
  char buffer[4096];
  DWORD n;
  
  while (ReadFile(stdout_read, buffer, sizeof(buffer), &n, NULL) && n > 0) {
    if (stdout_len + n >= stdout_cap) {
      stdout_cap *= 2;
      stdout_buf = realloc(stdout_buf, stdout_cap);
    }
    memcpy(stdout_buf + stdout_len, buffer, n);
    stdout_len += n;
  }
  CloseHandle(stdout_read);
  
  while (ReadFile(stderr_read, buffer, sizeof(buffer), &n, NULL) && n > 0) {
    if (stderr_len + n >= stderr_cap) {
      stderr_cap *= 2;
      stderr_buf = realloc(stderr_buf, stderr_cap);
    }
    memcpy(stderr_buf + stderr_len, buffer, n);
    stderr_len += n;
  }
  CloseHandle(stderr_read);
  
  WaitForSingleObject(pi.hProcess, INFINITE);
  
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  DWORD pid = pi.dwProcessId;
  
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  
  bool as_string = sync_encoding_wants_string(js, options_arg);

  ant_value_t stdout_val = sync_make_output(js, stdout_buf, stdout_len, as_string);
  ant_value_t stderr_val = sync_make_output(js, stderr_buf, stderr_len, as_string);

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", stdout_val);
  js_set(js, result, "stderr", stderr_val);
  js_set(js, result, "status", js_mknum((double)exit_code));
  js_set(js, result, "signal", js_mknull());
  js_set(js, result, "pid", js_mknum((double)pid));

  ant_value_t output = js_mkarr(js);
  js_arr_push(js, output, js_mknull());
  js_arr_push(js, output, stdout_val);
  js_arr_push(js, output, stderr_val);
  js_set(js, result, "output", output);

  if (stdout_buf) free(stdout_buf);
  if (stderr_buf) free(stderr_buf);

  return result;
}

static ant_value_t builtin_spawnSync(ant_t *js, ant_value_t *args, int nargs) {
  return spawn_sync_impl(js, args, nargs, false);
}
#else
static void close_if_valid(int fd) {
  if (fd >= 0) close(fd);
}

static void child_redirect_stdio_to_devnull(int fd, int flags) {
  int null_fd = open("/dev/null", flags);
  if (null_fd < 0) _exit(127);
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

static bool sync_pipes_open(sync_pipes_t *pipes, const stdio_mode_t *modes) {
  for (int i = 0; i < 3; i++)
    if (modes[i] == STDIO_PIPE && pipe(pipes->fds[i]) < 0) return false;
  return true;
}

typedef struct {
  const char *input;
  size_t input_len;
  bool use_shell;
  bool encode_as_string;
  int timeout_ms;
  int kill_signal;
  size_t max_buffer;
  stdio_mode_t stdio[3];
  char label[128];
} sync_opts_t;

static const size_t SYNC_MAX_BUFFER_DEFAULT = 1024 * 1024;

static int sync_parse_kill_signal(ant_t *js, ant_value_t value, int fallback) {
  if (vtype(value) == T_NUM) {
    int resolved = (int)js_getnum(value);
    return resolved > 0 ? resolved : fallback;
  }
  if (vtype(value) != T_STR) return fallback;

  size_t len = 0;
  char *name = js_getstr(js, value, &len);

  char buf[32];
  if (!name || len >= sizeof(buf)) return fallback;

  memcpy(buf, name, len);
  buf[len] = '\0';

  int resolved = process_signal_number(buf);
  return resolved > 0 ? resolved : fallback;
}

static void sync_parse_options(
  ant_t *js, ant_value_t options, bool force_shell, sync_opts_t *opts, sync_res_t *res
) {
  *opts = (sync_opts_t){
    .use_shell = force_shell,
    .kill_signal = SIGTERM,
    .max_buffer = SYNC_MAX_BUFFER_DEFAULT,
    .stdio = { STDIO_PIPE, STDIO_PIPE, STDIO_PIPE },
  };
  if (!is_special_object(options)) return;

  ant_value_t cwd_val = js_get(js, options, "cwd");
  if (vtype(cwd_val) == T_STR) {
    size_t cwd_len = 0;
    char *cwd = js_getstr(js, cwd_val, &cwd_len);
    if (cwd) res->cwd = strndup(cwd, cwd_len);
  }

  sync_value_bytes(js, js_get(js, options, "input"), &opts->input, &opts->input_len);
  parse_stdio_option(js, js_get(js, options, "stdio"), opts->stdio);
  res->env = parse_env_object(js, js_get(js, options, "env"));

  ant_value_t shell_val = js_get(js, options, "shell");
  if (vtype(shell_val) == T_STR) {
    size_t shell_len = 0;
    char *shell = js_getstr(js, shell_val, &shell_len);
    if (shell && shell_len > 0) {
      res->shell = strndup(shell, shell_len);
      opts->use_shell = true;
    }
  } else if (!opts->use_shell) opts->use_shell = js_truthy(js, shell_val);

  opts->encode_as_string = sync_encoding_wants_string(js, options);

  ant_value_t timeout_val = js_get(js, options, "timeout");
  if (vtype(timeout_val) == T_NUM) {
    double ms = js_getnum(timeout_val);
    if (ms > 0 && ms < (double)INT_MAX) opts->timeout_ms = (int)ms;
  }

  ant_value_t max_buffer_val = js_get(js, options, "maxBuffer");
  if (vtype(max_buffer_val) == T_NUM) {
    double bytes = js_getnum(max_buffer_val);
    if (bytes >= 0) opts->max_buffer = bytes >= (double)SIZE_MAX ? SIZE_MAX : (size_t)bytes;
  }

  opts->kill_signal = sync_parse_kill_signal(js, js_get(js, options, "killSignal"), SIGTERM);
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

static void sync_child_exec(
  const sync_pipes_t *pipes, const sync_opts_t *opts, const sync_res_t *res
) {
  signal(SIGPIPE, SIG_DFL);
  if (res->cwd && chdir(res->cwd) != 0) _exit(127);
  if (res->env) environ = res->env;

  static const int target_fd[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
  static const int null_flags[3] = { O_RDONLY, O_WRONLY, O_WRONLY };

  for (int i = 0; i < 3; i++) {
    if (opts->stdio[i] == STDIO_PIPE) {
      bool reading = i == CHILD_STREAM_STDIN;
      int keep = reading ? pipes->fds[i][0] : pipes->fds[i][1];
      int drop = reading ? pipes->fds[i][1] : pipes->fds[i][0];

      close_if_valid(drop);
      dup2(keep, target_fd[i]);
      close_if_valid(keep);
    } else if (opts->stdio[i] == STDIO_IGNORE) {
      child_redirect_stdio_to_devnull(target_fd[i], null_flags[i]);
    }
  }

  execvp(res->argv[0], res->argv);
  _exit(127);
}

static ant_value_t sync_build_result(
  ant_t *js, pid_t pid, int status, const sync_read_ctl_t *ctl, const sync_opts_t *opts,
  const sync_reader_t *out, const sync_reader_t *err
) {
  ant_value_t stdout_val = sync_make_output(js, out->buf, out->len, opts->encode_as_string);
  ant_value_t stderr_val = sync_make_output(js, err->buf, err->len, opts->encode_as_string);

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", stdout_val);
  js_set(js, result, "stderr", stderr_val);
  js_set(js, result, "pid", js_mknum((double)pid));

  ant_value_t output = js_mkarr(js);
  js_arr_push(js, output, js_mknull());
  js_arr_push(js, output, stdout_val);
  js_arr_push(js, output, stderr_val);
  js_set(js, result, "output", output);

  int signal_code = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  if (signal_code) {
    const char *name = process_signal_name(signal_code);
    js_set(js, result, "status", js_mknull());
    js_set(js, result, "signal", name ? js_mkstr(js, name, strlen(name)) : js_mknull());
  } else {
    js_set(js, result, "status", js_mknum((double)(WIFEXITED(status) ? WEXITSTATUS(status) : -1)));
    js_set(js, result, "signal", js_mknull());
  }

  int failure = ctl->timed_out ? UV_ETIMEDOUT : ctl->over_buffer ? UV_ENOBUFS : 0;
  if (failure) {
    const char *code = uv_err_name(failure);

    char syscall[224];
    char message[256];
    snprintf(syscall, sizeof(syscall), "spawnSync %s", opts->label);
    snprintf(message, sizeof(message), "%s %s", syscall, code);

    ant_value_t error = js_make_error_silent(js, JS_ERR_GENERIC, message);
    if (is_object_type(error)) {
      js_set(js, error, "code", js_mkstr(js, code, strlen(code)));
      js_set(js, error, "errno", js_mknum((double)failure));
      js_set(js, error, "syscall", js_mkstr(js, syscall, strlen(syscall)));
    }
    js_set(js, result, "error", error);
  }

  return result;
}
static ant_value_t spawn_sync_impl(ant_t *js, ant_value_t *args, int nargs, bool force_shell) {
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");

  size_t command_len = 0;
  char *command = js_getstr(js, args[0], &command_len);
  if (!command) return js_mkerr(js, "Command must be a string");
  if (memchr(command, '\0', command_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Child process command cannot contain NUL bytes");

  sync_res_t res = {0};
  res.command = strndup(command, command_len);
  if (!res.command) return js_mkerr(js, "Out of memory");

  if (nargs >= 2 && vtype(args[1]) == T_ARR)
    res.args = parse_args_array(js, args[1], &res.arg_count);
  if (res.arg_count < 0) {
    free(res.command);
    return mkval(T_ERR, 0);
  }

  sync_opts_t opts;
  sync_parse_options(js, child_process_options_arg(args, nargs), force_shell, &opts, &res);

  if (opts.use_shell) snprintf(opts.label, sizeof(opts.label), "%s", res.shell ? res.shell : "/bin/sh");
  else snprintf(opts.label, sizeof(opts.label), "%.*s", (int)(command_len < 120 ? command_len : 120), command);

  if (!sync_build_argv(&res, opts.use_shell, command_len)) {
    sync_res_free(&res);
    return js_mkerr(js, "Out of memory");
  }

  sync_pipes_t pipes;
  sync_pipes_init(&pipes);

  if (!sync_pipes_open(&pipes, opts.stdio)) {
    sync_pipes_close_all(&pipes);
    sync_res_free(&res);
    return js_mkerr(js, "Failed to create pipes");
  }

  pid_t pid = fork();
  if (pid < 0) {
    sync_pipes_close_all(&pipes);
    sync_res_free(&res);
    return js_mkerr(js, "Fork failed");
  }
  if (pid == 0) sync_child_exec(&pipes, &opts, &res);

  bool want_stdin = opts.stdio[CHILD_STREAM_STDIN] == STDIO_PIPE;

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
    ? sync_build_result(js, pid, status, &ctl, &opts, &out, &err)
    : js_mkerr(js, "Failed to read process output");

  free(out.buf);
  free(err.buf);

  return result;
}


static ant_value_t builtin_spawnSync(ant_t *js, ant_value_t *args, int nargs) {
  return spawn_sync_impl(js, args, nargs, false);
}
#endif

static ant_value_t sync_result_error(
  ant_t *js, ant_value_t result, ant_value_t command_val, const char *api
) {
  ant_value_t spawn_error = js_get(js, result, "error");
  ant_value_t status = js_get(js, result, "status");
  ant_value_t signal = js_get(js, result, "signal");

  bool failed_status = vtype(status) == T_NUM && (int)js_getnum(status) != 0;
  bool signalled = vtype(signal) == T_STR;
  if (!is_object_type(spawn_error) && !failed_status && !signalled) return js_mkundef();

  ant_value_t stdout_val = js_get(js, result, "stdout");
  ant_value_t stderr_val = js_get(js, result, "stderr");

  char message[512];
  if (is_object_type(spawn_error)) {
    ant_value_t existing = js_get(js, spawn_error, "message");
    size_t existing_len = 0;
    char *existing_str = vtype(existing) == T_STR ? js_getstr(js, existing, &existing_len) : NULL;
    snprintf(
      message, sizeof(message), "%.*s",
      (int)(existing_len < sizeof(message) - 1 ? existing_len : sizeof(message) - 1),
      existing_str ? existing_str : "Command failed"
    );
  } else {
    size_t command_len = 0;
    char *command_str = vtype(command_val) == T_STR ? js_getstr(js, command_val, &command_len) : NULL;
    const char *stderr_str = NULL;
    size_t stderr_len = 0;
    sync_value_bytes(js, stderr_val, &stderr_str, &stderr_len);

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

  ant_value_t error = js_make_error_silent(js, JS_ERR_GENERIC, message);
  if (!is_object_type(error)) return error;

  js_set(js, error, "status", status);
  js_set(js, error, "signal", signal);
  js_set(js, error, "stdout", stdout_val);
  js_set(js, error, "stderr", stderr_val);
  js_set(js, error, "pid", js_get(js, result, "pid"));
  js_set(js, error, "output", js_get(js, result, "output"));

  return error;
}

static ant_value_t builtin_execSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "execSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");

  ant_value_t spawn_args[3];
  spawn_args[0] = args[0];
  spawn_args[1] = js_mkundef();
  spawn_args[2] = nargs >= 2 ? args[1] : js_mkundef();

  ant_value_t result = spawn_sync_impl(js, spawn_args, 3, true);
  if (vtype(result) != T_OBJ) return result;

  ant_value_t options = spawn_args[2];
  bool stdio_specified =
    is_special_object(options) && vtype(js_get(js, options, "stdio")) != T_UNDEF;

  if (!stdio_specified) {
    const char *stderr_str = NULL;
    size_t stderr_len = 0;
    sync_value_bytes(js, js_get(js, result, "stderr"), &stderr_str, &stderr_len);

    size_t written = 0;
    while (stderr_str && written < stderr_len) {
      ssize_t n = write(STDERR_FILENO, stderr_str + written, stderr_len - written);
      if (n <= 0) break;
      written += (size_t)n;
    }
  }

  ant_value_t error = sync_result_error(js, result, args[0], "execSync");
  if (!is_object_type(error)) return js_get(js, result, "stdout");

  return js_throw(js, error);
}

static ant_value_t builtin_execFileSync(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t argv = js_mkundef();
  ant_value_t options = js_mkundef();
  ant_value_t spawn_args[3];

  if (nargs < 1) return js_mkerr(js, "execFileSync() requires a file");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "File must be a string");

  if (nargs >= 2) {
    if (vtype(args[1]) == T_ARR) {
      argv = args[1];
      if (nargs >= 3 && is_special_object(args[2])) options = args[2];
    } else if (is_special_object(args[1])) options = args[1];
  }

  spawn_args[0] = args[0];
  spawn_args[1] = argv;
  spawn_args[2] = options;

  ant_value_t result = builtin_spawnSync(js, spawn_args, 3);
  if (vtype(result) != T_OBJ) return result;

  ant_value_t command = child_process_command_value(js, args[0], argv);
  if (vtype(command) != T_STR) command = args[0];

  ant_value_t error = sync_result_error(js, result, command, "execFileSync");
  if (is_object_type(error)) return js_throw(js, error);

  return js_get(js, result, "stdout");
}

static ant_value_t builtin_fork(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fork() requires a module path");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Module path must be a string");
  size_t path_len;
  
  char *path = js_getstr(js, args[0], &path_len);
  char *path_str = strndup(path, path_len);
  char exe_path[1024];
  
#if defined(__APPLE__)
  uint32_t size = sizeof(exe_path);
  if (_NSGetExecutablePath(exe_path, &size) != 0) {
    free(path_str);
    return js_mkerr(js, "Failed to get executable path");
  }
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len == -1) {
    free(path_str);
    return js_mkerr(js, "Failed to get executable path");
  }
  exe_path[len] = '\0';
#else
  strncpy(exe_path, "ant", sizeof(exe_path));
#endif
  
  ant_value_t spawn_args[3];
  spawn_args[0] = js_mkstr(js, exe_path, strlen(exe_path));
  
  ant_value_t args_arr = js_mkarr(js);
  js_arr_push(js, args_arr, js_mkstr(js, path_str, path_len));
  
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
  spawn_args[2] = js_mkobj(js);
  
  free(path_str);
  
  return builtin_spawn(js, spawn_args, 3);
}

ant_value_t child_process_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  
  ant_value_t exec_fn = js_heavy_mkfun(js, builtin_exec, js_mkundef());
  ant_value_t exec_file_fn = js_heavy_mkfun(js, builtin_execFile, js_mkundef());
  
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
  js_set(js, lib, "spawn", js_mkfun(builtin_spawn));
  js_set(js, lib, "exec", exec_fn);
  js_set(js, lib, "execFile", exec_file_fn);
  js_set(js, lib, "execSync", js_mkfun(builtin_execSync));
  js_set(js, lib, "execFileSync", js_mkfun(builtin_execFileSync));
  js_set(js, lib, "spawnSync", js_mkfun(builtin_spawnSync));
  js_set(js, lib, "fork", js_mkfun(builtin_fork));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "child_process", 13));
  
  return lib;
}

int has_pending_child_processes(void) {
  for (child_process_t *cp = pending_children_head; cp; cp = cp->next) 
    if (cp->keep_alive) return 1;
  return 0;
}

void gc_mark_child_process(ant_t *js, gc_mark_fn mark) {
  for (child_process_t *cp = pending_children_head; cp; cp = cp->next) {
    mark(js, cp->child_obj);
    mark(js, cp->stdin_obj);
    mark(js, cp->stdout_obj);
    mark(js, cp->stderr_obj);
    mark(js, cp->promise);
    for (child_write_req_t *w = cp->pending_writes; w; w = w->next) mark(js, w->callback);
  }
}
