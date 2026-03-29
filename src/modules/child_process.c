#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <uthash.h>
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
#include "errors.h"
#include "internal.h"
#include "silver/engine.h"

#include "gc/modules.h"
#include "modules/child_process.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

#define MAX_CHILD_LISTENERS 16
#define PIPE_READ_BUF_SIZE 65536

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

typedef struct {
  ant_value_t callback;
  bool once;
} child_listener_t;

typedef struct {
  char *event_name;
  child_listener_t listeners[MAX_CHILD_LISTENERS];
  int count;
  UT_hash_handle hh;
} child_event_t;

typedef enum {
  STDIO_PIPE = 0,
  STDIO_INHERIT,
  STDIO_IGNORE,
} stdio_mode_t;

struct child_process_s {
  ant_t *js;
  uv_process_t process;
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
  child_event_t *events;
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
  bool stdin_closed;
  bool use_shell;
  bool detached;
  bool close_emitted;
  bool keep_alive;
  int pending_closes;
  char *cwd;
  stdio_mode_t stdio_modes[3];
  struct child_process_s *next;
  struct child_process_s *prev;
};

static child_process_t *pending_children_head = NULL;
static child_process_t *pending_children_tail = NULL;

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
  child_event_t *evt = NULL;
  HASH_FIND_STR(cp->events, name, evt);
  if (!evt || evt->count == 0) return;
  
  int i = 0;
  while (i < evt->count) {
    child_listener_t *l = &evt->listeners[i];
    ant_value_t result = sv_vm_call(cp->js->vm, cp->js, l->callback, js_mkundef(), args, nargs, NULL, false);
    if (vtype(result) == T_ERR) log_listener_error(cp->js, name, result);
    if (l->once) {
      for (int j = i; j < evt->count - 1; j++) 
        evt->listeners[j] = evt->listeners[j + 1];
      evt->count--;
    } else i++;
  }
}

static const char *stream_kind_name(child_stream_kind_t kind) {
switch (kind) {
  case CHILD_STREAM_STDIN: return "stdin";
  case CHILD_STREAM_STDOUT: return "stdout";
  case CHILD_STREAM_STDERR: return "stderr";
  default: return "unknown";
}}

static void emit_stream_event(
  child_process_t *cp,
  child_stream_kind_t kind,
  const char *event,
  ant_value_t *args,
  int nargs
) {
  char full_name[64];
  snprintf(full_name, sizeof(full_name), "%s:%s", stream_kind_name(kind), event);
  emit_event(cp, full_name, args, nargs);
}

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
  if (cp->prev) cp->prev->next = cp->next;
  else pending_children_head = cp->next;
  if (cp->next) cp->next->prev = cp->prev;
  else pending_children_tail = cp->prev;
  cp->next = NULL;
  cp->prev = NULL;
}

static void free_child_process(child_process_t *cp) {
  if (!cp) return;

  if (vtype(cp->child_obj) == T_OBJ) js_set_slot(cp->child_obj, SLOT_DATA, js_mkundef());
  if (vtype(cp->stdin_obj) == T_OBJ) js_set_slot(cp->stdin_obj, SLOT_DATA, js_mkundef());
  if (vtype(cp->stdout_obj) == T_OBJ) js_set_slot(cp->stdout_obj, SLOT_DATA, js_mkundef());
  if (vtype(cp->stderr_obj) == T_OBJ) js_set_slot(cp->stderr_obj, SLOT_DATA, js_mkundef());
  
  if (cp->stdout_buf) free(cp->stdout_buf);
  if (cp->stderr_buf) free(cp->stderr_buf);
  if (cp->cwd) free(cp->cwd);
  if (cp->stdin_ctx) free(cp->stdin_ctx);
  if (cp->stdout_ctx) free(cp->stdout_ctx);
  if (cp->stderr_ctx) free(cp->stderr_ctx);
  
  child_event_t *evt, *tmp;
  HASH_ITER(hh, cp->events, evt, tmp) {
    HASH_DEL(cp->events, evt);
    free(evt->event_name);
    free(evt);
  }
  
  free(cp);
}

static child_event_t *find_or_create_event(child_process_t *cp, const char *name) {
  child_event_t *evt = NULL;
  HASH_FIND_STR(cp->events, name, evt);
  if (!evt) {
    evt = calloc(1, sizeof(child_event_t));
    evt->event_name = strdup(name);
    evt->count = 0;
    HASH_ADD_KEYPTR(hh, cp->events, evt->event_name, strlen(evt->event_name), evt);
  }
  return evt;
}

static void try_free_child(child_process_t *cp) {
  if (!cp) return;
  if (cp->exited && cp->stdout_closed && cp->stderr_closed && cp->pending_closes == 0) {
    remove_pending_child(cp);
    free_child_process(cp);
  }
}

static void check_completion(child_process_t *cp) {
  if (cp->exited && cp->stdout_closed && cp->stderr_closed && !cp->close_emitted) {
    cp->close_emitted = true;
    
    ant_value_t stdout_val = js_mkstr(cp->js, cp->stdout_buf ? cp->stdout_buf : "", cp->stdout_len);
    ant_value_t stderr_val = js_mkstr(cp->js, cp->stderr_buf ? cp->stderr_buf : "", cp->stderr_len);

    if (vtype(cp->stdout_obj) == T_OBJ) {
      js_set(cp->js, cp->stdout_obj, "text", stdout_val);
      js_set(cp->js, cp->stdout_obj, "length", js_mknum((double)cp->stdout_len));
    }
    
    if (vtype(cp->stderr_obj) == T_OBJ) {
      js_set(cp->js, cp->stderr_obj, "text", stderr_val);
      js_set(cp->js, cp->stderr_obj, "length", js_mknum((double)cp->stderr_len));
    }

    js_set(cp->js, cp->child_obj, "stdoutText", stdout_val);
    js_set(cp->js, cp->child_obj, "stderrText", stderr_val);
    js_set(cp->js, cp->child_obj, "exitCode", js_mknum((double)cp->exit_code));
    js_set(cp->js, cp->child_obj, "signalCode", cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull());
    
    ant_value_t close_args[2] = { js_mknum((double)cp->exit_code), cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull() };
    emit_event(cp, "close", close_args, 2);
    
    if (vtype(cp->promise) != T_UNDEF) {
      ant_value_t result = js_mkobj(cp->js);
      js_set(cp->js, result, "stdout", stdout_val);
      js_set(cp->js, result, "stderr", stderr_val);
      js_set(cp->js, result, "exitCode", js_mknum((double)cp->exit_code));
      js_set(cp->js, result, "signalCode", cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull());
      
      if (cp->exit_code != 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command failed with exit code %lld", (long long)cp->exit_code);
        ant_value_t err = js_mkstr(cp->js, err_msg, strlen(err_msg));
        js_reject_promise(cp->js, cp->promise, err);
      } else js_resolve_promise(cp->js, cp->promise, result);
    }
    
    try_free_child(cp);
  }
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
  close_child_handle(cp, (uv_handle_t *)pipe);
}

static void on_process_exit(uv_process_t *proc, int64_t exit_status, int term_signal) {
  child_process_t *cp = (child_process_t *)proc->data;
  cp->exit_code = exit_status;
  cp->term_signal = term_signal;
  cp->exited = true;
  
  js_set(cp->js, cp->child_obj, "exitCode", js_mknum((double)exit_status));
  if (term_signal) {
    js_set(cp->js, cp->child_obj, "signalCode", js_mknum((double)term_signal));
    js_set(cp->js, cp->child_obj, "killed", js_true);
  }
  
  ant_value_t exit_args[2] = { js_mknum((double)exit_status), term_signal ? js_mknum((double)term_signal) : js_mknull() };
  emit_event(cp, "exit", exit_args, 2);
  
  close_child_handle(cp, (uv_handle_t *)proc);
  close_child_pipe(cp, CHILD_STREAM_STDIN, false);
  close_child_pipe(cp, CHILD_STREAM_STDOUT, true);
  close_child_pipe(cp, CHILD_STREAM_STDERR, true);
  
  check_completion(cp);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = buf->base ? suggested_size : 0;
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  child_process_t *cp = (child_process_t *)stream->data;
  
  if (nread > 0) {
    if (cp->stdout_len + nread > cp->stdout_cap) {
      size_t new_cap = cp->stdout_cap == 0 ? 4096 : cp->stdout_cap * 2;
      while (new_cap < cp->stdout_len + nread) new_cap *= 2;
      char *new_buf = realloc(cp->stdout_buf, new_cap);
      if (new_buf) {
        cp->stdout_buf = new_buf;
        cp->stdout_cap = new_cap;
      }
    }
    if (cp->stdout_buf) {
      memcpy(cp->stdout_buf + cp->stdout_len, buf->base, nread);
      cp->stdout_len += nread;
    }
    
    ant_value_t text_data = js_mkstr(cp->js, buf->base, nread);
    ant_value_t text_args[1] = { text_data };
    emit_event(cp, "stdout", text_args, 1);

    ant_value_t byte_data = make_buffer_chunk(cp->js, buf->base, (size_t)nread);
    ant_value_t byte_args[1] = { byte_data };
    emit_stream_event(cp, CHILD_STREAM_STDOUT, "data", byte_args, 1);
    
    if (vtype(cp->stdout_obj) == T_OBJ) js_set(
      cp->js, cp->stdout_obj, "length", 
      js_mknum((double)cp->stdout_len)
    );
  }
  
  if (buf->base) free(buf->base);
  
  if (nread < 0) {
    if (nread != UV_EOF) {
      ant_value_t err_args[1] = { js_mkstr(cp->js, uv_strerror((int)nread), (int)strlen(uv_strerror((int)nread))) };
      emit_event(cp, "error", err_args, 1);
      emit_stream_event(cp, CHILD_STREAM_STDOUT, "error", err_args, 1);
    } else emit_stream_event(cp, CHILD_STREAM_STDOUT, "end", NULL, 0);
    
    if (nread != UV_EOF) 
      emit_stream_event(cp, CHILD_STREAM_STDOUT, "end", NULL, 0);
    
    close_child_pipe(cp, CHILD_STREAM_STDOUT, true);
    check_completion(cp);
  }
}

static void on_stderr_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  child_process_t *cp = (child_process_t *)stream->data;
  
  if (nread > 0) {
    if (cp->stderr_len + nread > cp->stderr_cap) {
      size_t new_cap = cp->stderr_cap == 0 ? 4096 : cp->stderr_cap * 2;
      while (new_cap < cp->stderr_len + nread) new_cap *= 2;
      char *new_buf = realloc(cp->stderr_buf, new_cap);
      if (new_buf) {
        cp->stderr_buf = new_buf;
        cp->stderr_cap = new_cap;
      }
    }
    if (cp->stderr_buf) {
      memcpy(cp->stderr_buf + cp->stderr_len, buf->base, nread);
      cp->stderr_len += nread;
    }
    
    ant_value_t text_data = js_mkstr(cp->js, buf->base, nread);
    ant_value_t text_args[1] = { text_data };
    emit_event(cp, "stderr", text_args, 1);

    ant_value_t byte_data = make_buffer_chunk(cp->js, buf->base, (size_t)nread);
    ant_value_t byte_args[1] = { byte_data };
    emit_stream_event(cp, CHILD_STREAM_STDERR, "data", byte_args, 1);
    
    if (vtype(cp->stderr_obj) == T_OBJ) js_set(
      cp->js, cp->stderr_obj, "length",
      js_mknum((double)cp->stderr_len)
    );
  }
  
  if (buf->base) free(buf->base);
  
  if (nread < 0) {
    if (nread != UV_EOF) {
      ant_value_t err_args[1] = { 
        js_mkstr(cp->js, uv_strerror((int)nread),
        (int)strlen(uv_strerror((int)nread)))
      };
      emit_stream_event(cp, CHILD_STREAM_STDERR, "error", err_args, 1);
    }
    
    emit_stream_event(cp, CHILD_STREAM_STDERR, "end", NULL, 0);
    close_child_pipe(cp, CHILD_STREAM_STDERR, true);
    check_completion(cp);
  }
}

static ant_value_t child_on(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "on() requires event name and callback");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Event name must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "Callback must be a function");
  
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(cp_ptr) == T_UNDEF) return js_mkerr(js, "Invalid child process object");
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  
  size_t name_len;
  char *name = js_getstr(js, args[0], &name_len);
  char *name_cstr = strndup(name, name_len);
  
  child_event_t *evt = find_or_create_event(cp, name_cstr);
  free(name_cstr);
  
  if (evt->count >= MAX_CHILD_LISTENERS) {
    return js_mkerr(js, "Maximum listeners reached for event");
  }
  
  evt->listeners[evt->count].callback = args[1];
  evt->listeners[evt->count].once = false;
  evt->count++;
  
  return this_obj;
}

static ant_value_t child_once(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "once() requires event name and callback");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Event name must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "Callback must be a function");
  
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(cp_ptr) == T_UNDEF) return js_mkerr(js, "Invalid child process object");
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  
  size_t name_len;
  char *name = js_getstr(js, args[0], &name_len);
  char *name_cstr = strndup(name, name_len);
  
  child_event_t *evt = find_or_create_event(cp, name_cstr);
  free(name_cstr);
  
  if (evt->count >= MAX_CHILD_LISTENERS) {
    return js_mkerr(js, "Maximum listeners reached for event");
  }
  
  evt->listeners[evt->count].callback = args[1];
  evt->listeners[evt->count].once = true;
  evt->count++;
  
  return this_obj;
}

static ant_value_t child_kill(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(cp_ptr) == T_UNDEF) return js_false;
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
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
  
  int result = uv_process_kill(&cp->process, sig);
  return js_bool(result == 0);
}

static ant_value_t child_write_impl(ant_t *js, child_process_t *cp, ant_value_t data_arg) {
  if (cp->stdin_closed) return js_false;
  
  const char *data = NULL;
  size_t data_len = 0;
  
  if (vtype(data_arg) == T_STR) {
    data = js_getstr(js, data_arg, &data_len);
    if (!data) return js_mkerr(js, "Data must be a string or Buffer");
  } else {
    ant_value_t ta_data_val = js_get_slot(data_arg, SLOT_BUFFER);
    TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
    if (!ta_data || !ta_data->buffer || !ta_data->buffer->data) {
      return js_mkerr(js, "Data must be a string or Buffer");
    }
    data = (const char *)(ta_data->buffer->data + ta_data->byte_offset);
    data_len = ta_data->byte_length;
  }

  uv_write_t *write_req = malloc(sizeof(uv_write_t));
  char *buf_data = malloc(data_len);
  memcpy(buf_data, data, data_len);
  
  uv_buf_t buf = uv_buf_init(buf_data, (unsigned int)data_len);
  write_req->data = buf_data;
  
  int result = uv_write(write_req, (uv_stream_t *)&cp->stdin_pipe, &buf, 1, NULL);
  if (result < 0) {
    ant_value_t err_args[1] = { js_mkstr(js, uv_strerror(result), strlen(uv_strerror(result))) };
    emit_stream_event(cp, CHILD_STREAM_STDIN, "error", err_args, 1);
    free(buf_data); free(write_req);
    return js_false;
  }
  
  return js_true;
}

static ant_value_t child_end_impl(child_process_t *cp) {
  close_child_pipe(cp, CHILD_STREAM_STDIN, false);
  return js_mkundef();
}

static ant_value_t child_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 1) return js_mkerr(js, "write() requires data argument");
  
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(cp_ptr) == T_UNDEF) return js_mkerr(js, "Invalid child process object");
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  return child_write_impl(js, cp, args[0]);
}

static ant_value_t child_ref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  
  if (vtype(cp_ptr) == T_UNDEF) return this_obj;
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  
  if (!cp) return this_obj;
  cp->keep_alive = true;
  
  if (!uv_is_closing((uv_handle_t *)&cp->process)) uv_ref((uv_handle_t *)&cp->process);
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    uv_handle_t *h = (uv_handle_t *)child_pipe(cp, i);
    if (child_stdio_is_pipe(cp, i) && !uv_is_closing(h)) uv_ref(h);
  }
  
  return this_obj;
}

static ant_value_t child_unref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  
  if (vtype(cp_ptr) == T_UNDEF) return this_obj;
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  
  if (!cp) return this_obj;
  cp->keep_alive = false;
  
  if (!uv_is_closing((uv_handle_t *)&cp->process)) uv_unref((uv_handle_t *)&cp->process);
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
    uv_handle_t *h = (uv_handle_t *)child_pipe(cp, i);
    if (child_stdio_is_pipe(cp, i) && !uv_is_closing(h)) uv_unref(h);
  }
  
  return this_obj;
}

static ant_value_t child_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  ant_value_t cp_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(cp_ptr) == T_UNDEF) return js_mkundef();
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  return child_end_impl(cp);
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

static ant_value_t child_stream_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 1) return js_mkerr(js, "write() requires data argument");
  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return js_mkerr(js, "Invalid stream object");
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return js_mkerr(js, "Invalid stream context");
  return child_write_impl(js, ctx->cp, args[0]);
}

static ant_value_t child_stream_end(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return js_mkundef();
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return js_mkundef();
  return child_end_impl(ctx->cp);
}

static ant_value_t child_stream_ref(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return this_obj;
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return this_obj;

  uv_handle_t *h = child_stream_handle(ctx->cp, ctx->kind);
  if (h && !uv_is_closing(h)) uv_ref(h);
  return this_obj;
}

static ant_value_t child_stream_unref(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return this_obj;
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return this_obj;

  uv_handle_t *h = child_stream_handle(ctx->cp, ctx->kind);
  if (h && !uv_is_closing(h)) uv_unref(h);
  return this_obj;
}

static ant_value_t child_stream_destroy(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return this_obj;
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return this_obj;

  child_process_t *cp = ctx->cp;
  if (ctx->kind == CHILD_STREAM_STDIN) {
    (void)child_end_impl(cp);
    return this_obj;
  }

  uv_handle_t *h = child_stream_handle(cp, ctx->kind);
  if (h && !uv_is_closing(h)) {
    if (ctx->kind == CHILD_STREAM_STDOUT) close_child_pipe(cp, CHILD_STREAM_STDOUT, true);
    else if (ctx->kind == CHILD_STREAM_STDERR) close_child_pipe(cp, CHILD_STREAM_STDERR, true);
    check_completion(cp);
  }

  return this_obj;
}

static ant_value_t child_stream_on(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "on() requires event name and callback");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Event name must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "Callback must be a function");

  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return js_mkerr(js, "Invalid stream object");
  
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return js_mkerr(js, "Invalid stream context");
  
  child_process_t *cp = ctx->cp;
  child_stream_kind_t kind = ctx->kind;
  
  size_t name_len = 0;
  char *name = js_getstr(js, args[0], &name_len);
  if (!name) return js_mkerr(js, "Event name must be a string");

  char full_name[64];
  snprintf(
    full_name, sizeof(full_name),
    "%s:%.*s", stream_kind_name(kind),
    (int)name_len, name
  );

  child_event_t *evt = find_or_create_event(cp, full_name);
  if (evt->count >= MAX_CHILD_LISTENERS) {
    return js_mkerr(js, "Maximum listeners reached for event");
  }

  evt->listeners[evt->count].callback = args[1];
  evt->listeners[evt->count].once = false;
  evt->count++;
  return this_obj;
}

static ant_value_t child_stream_once(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "once() requires event name and callback");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Event name must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "Callback must be a function");

  ant_value_t ctx_ptr = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(ctx_ptr) == T_UNDEF) return js_mkerr(js, "Invalid stream object");
  
  child_stream_ctx_t *ctx = (child_stream_ctx_t *)(uintptr_t)js_getnum(ctx_ptr);
  if (!ctx || !ctx->cp) return js_mkerr(js, "Invalid stream context");
  
  child_process_t *cp = ctx->cp;
  child_stream_kind_t kind = ctx->kind;
  
  size_t name_len = 0;
  char *name = js_getstr(js, args[0], &name_len);
  if (!name) return js_mkerr(js, "Event name must be a string");

  char full_name[64];
  snprintf(
    full_name, sizeof(full_name),
    "%s:%.*s", stream_kind_name(kind),
    (int)name_len, name
  );

  child_event_t *evt = find_or_create_event(cp, full_name);
  if (evt->count >= MAX_CHILD_LISTENERS) {
    return js_mkerr(js, "Maximum listeners reached for event");
  }

  evt->listeners[evt->count].callback = args[1];
  evt->listeners[evt->count].once = true;
  evt->count++;
  return this_obj;
}

static ant_value_t create_child_stream_object(ant_t *js, child_process_t *cp, child_stream_kind_t kind) {
  ant_value_t obj = js_mkobj(js);
  child_stream_ctx_t *ctx = calloc(1, sizeof(child_stream_ctx_t));
  if (!ctx) return js_mkerr(js, "Out of memory");
  
  ctx->cp = cp;
  ctx->kind = kind;
  
  if (kind == CHILD_STREAM_STDIN) cp->stdin_ctx = ctx;
  else if (kind == CHILD_STREAM_STDOUT) cp->stdout_ctx = ctx;
  else cp->stderr_ctx = ctx;

  js_set_slot(obj, SLOT_DATA, ANT_PTR(ctx));
  js_set(js, obj, "on", js_mkfun(child_stream_on));
  js_set(js, obj, "once", js_mkfun(child_stream_once));
  js_set(js, obj, "ref", js_mkfun(child_stream_ref));
  js_set(js, obj, "unref", js_mkfun(child_stream_unref));
  js_set(js, obj, "destroy", js_mkfun(child_stream_destroy));
  js_set(js, obj, "length", js_mknum(0));

  if (kind == CHILD_STREAM_STDIN) {
    js_set(js, obj, "write", js_mkfun(child_stream_write));
    js_set(js, obj, "end", js_mkfun(child_stream_end));
  }

  return obj;
}

static ant_value_t create_child_object(ant_t *js, child_process_t *cp) {
  ant_value_t obj = js_mkobj(js);
  
  js_set_slot(obj, SLOT_DATA, ANT_PTR(cp));
  js_set(js, obj, "pid", js_mknum((double)cp->process.pid));
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
  
  js_set(js, obj, "on", js_mkfun(child_on));
  js_set(js, obj, "once", js_mkfun(child_once));
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
      args[i] = strndup(arg, arg_len);
    } else args[i] = strdup("");
  }
  
  args[len] = NULL;
  *count = len;
  return args;
}

static void free_args_array(char **args, int count) {
  if (!args) return;
  for (int i = 0; i < count; i++) {
    if (args[i]) free(args[i]);
  }
  free(args);
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
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *cwd = NULL;
  bool use_shell = false;
  bool detached = false;
  
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t len_val = js_get(js, args[1], "length");
    if (vtype(len_val) == T_NUM) {
      spawn_args = parse_args_array(js, args[1], &spawn_argc);
    }
  }
  
  stdio_mode_t stdio_modes[3] = { 
    STDIO_PIPE, STDIO_PIPE, STDIO_PIPE 
  };
  
  if (nargs >= 3 && is_special_object(args[2])) {
    ant_value_t cwd_val = js_get(js, args[2], "cwd");
    if (vtype(cwd_val) == T_STR) {
      size_t cwd_len;
      char *cwd_str = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_str, cwd_len);
    }
    
    ant_value_t shell_val = js_get(js, args[2], "shell");
    use_shell = js_truthy(js, shell_val);
    
    ant_value_t detached_val = js_get(js, args[2], "detached");
    detached = js_truthy(js, detached_val);
    
    ant_value_t stdio_val = js_get(js, args[2], "stdio");
    parse_stdio_option(js, stdio_val, stdio_modes);
  }
  
  child_process_t *cp = calloc(1, sizeof(child_process_t));
  if (!cp) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
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
  cp->process.data = cp;
  
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
  if (stdio_modes[i] == STDIO_PIPE) {
    uv_pipe_t *p = child_pipe(cp, i);
    uv_pipe_init(uv_default_loop(), p, 0);
    p->data = cp;
  }}
  
  uv_stdio_container_t stdio[3];
  for (int i = CHILD_STREAM_STDIN; i <= CHILD_STREAM_STDERR; i++) {
  if (stdio_modes[i] == STDIO_INHERIT) {
    stdio[i].flags = UV_INHERIT_FD;
    stdio[i].data.fd = i;
  } else if (stdio_modes[i] == STDIO_IGNORE) stdio[i].flags = UV_IGNORE; else {
    stdio[i].flags = UV_CREATE_PIPE | (i == CHILD_STREAM_STDIN ? UV_READABLE_PIPE : UV_WRITABLE_PIPE);
    stdio[i].data.stream = (uv_stream_t *)child_pipe(cp, i);
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
  
  uv_process_options_t options = {0};
  options.exit_cb = on_process_exit;
  options.file = final_args[0];
  options.args = final_args;
  options.stdio_count = 3;
  options.stdio = stdio;
  
  if (cwd) options.cwd = cwd;
  if (detached) options.flags = UV_PROCESS_DETACHED;
  int r = uv_spawn(uv_default_loop(), &cp->process, &options);
  
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
  
  if (r < 0) {
    free_child_process(cp);
    return js_mkerr(js, "Failed to spawn process: %s", uv_strerror(r));
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

static ant_value_t builtin_exec(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "exec() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
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
  cp->promise = js_mkpromise(js);
  cp->keep_alive = true;
  
  cp->stdio_modes[CHILD_STREAM_STDIN] = STDIO_IGNORE;
  cp->stdio_modes[CHILD_STREAM_STDOUT] = STDIO_PIPE;
  cp->stdio_modes[CHILD_STREAM_STDERR] = STDIO_PIPE;
  
  uv_pipe_init(uv_default_loop(), &cp->stdout_pipe, 0);
  uv_pipe_init(uv_default_loop(), &cp->stderr_pipe, 0);
  
  cp->stdout_pipe.data = cp;
  cp->stderr_pipe.data = cp;
  cp->process.data = cp;
  cp->stdin_closed = true;
  
  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[1].data.stream = (uv_stream_t *)&cp->stdout_pipe;
  stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[2].data.stream = (uv_stream_t *)&cp->stderr_pipe;
  
  char *shell_args[4];
  shell_args[0] = "/bin/sh";
  shell_args[1] = "-c";
  shell_args[2] = cmd_str;
  shell_args[3] = NULL;
  
  uv_process_options_t options = {0};
  options.exit_cb = on_process_exit;
  options.file = "/bin/sh";
  options.args = shell_args;
  options.stdio_count = 3;
  options.stdio = stdio;
  
  if (cwd) options.cwd = cwd;
  int r = uv_spawn(uv_default_loop(), &cp->process, &options);
  free(cmd_str);
  
  if (r < 0) {
    ant_value_t promise = cp->promise;
    free_child_process(cp);
    js_reject_promise(js, promise, js_mkstr(js, uv_strerror(r), strlen(uv_strerror(r))));
    return promise;
  }
  
  uv_read_start((uv_stream_t *)&cp->stdout_pipe, alloc_buffer, on_stdout_read);
  uv_read_start((uv_stream_t *)&cp->stderr_pipe, alloc_buffer, on_stderr_read);
  
  add_pending_child(cp);
  
  cp->child_obj = create_child_object(js, cp);
  return cp->promise;
}

static ant_value_t builtin_execSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "execSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  FILE *fp = popen(cmd_str, "r");
  free(cmd_str);
  
  if (!fp) {
    return js_mkerr(js, "Failed to execute command");
  }
  
  char *output = NULL;
  size_t output_len = 0;
  size_t output_cap = 4096;
  output = malloc(output_cap);
  
  if (!output) {
    pclose(fp);
    return js_mkerr(js, "Out of memory");
  }
  
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
    size_t len = strlen(buffer);
    if (output_len + len >= output_cap) {
      output_cap *= 2;
      char *new_output = realloc(output, output_cap);
      if (!new_output) {
        free(output);
        pclose(fp);
        return js_mkerr(js, "Out of memory");
      }
      output = new_output;
    }
    memcpy(output + output_len, buffer, len);
    output_len += len;
  }
  
  int status = pclose(fp);
#ifdef _WIN32
  int exit_code = status;
#else
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  
  if (output_len > 0 && output[output_len - 1] == '\n') {
    output_len--;
  }
  
  if (exit_code != 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Command failed with exit code %d", exit_code);
    free(output); return js_mkerr(js, "%s", err_msg);
  }
  
  ant_value_t result = js_mkstr(js, output, output_len);
  free(output);
  return result;
}

#ifdef _WIN32
static ant_value_t builtin_spawnSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *input = NULL;
  size_t input_len = 0;
  
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t len_val = js_get(js, args[1], "length");
    if (vtype(len_val) == T_NUM) {
      spawn_args = parse_args_array(js, args[1], &spawn_argc);
    }
  }
  
  if (nargs >= 3 && is_special_object(args[2])) {
    ant_value_t input_val = js_get(js, args[2], "input");
    if (vtype(input_val) == T_STR) {
      input = js_getstr(js, input_val, &input_len);
    }
  }
  
  size_t cmdline_len = cmd_len + 3;
  for (int i = 0; i < spawn_argc; i++) {
    cmdline_len += strlen(spawn_args[i]) + 3;
  }
  
  char *cmdline = malloc(cmdline_len);
  if (!cmdline) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Out of memory");
  }
  
  char *p = cmdline;
  p += sprintf(p, "%s", cmd_str);
  for (int i = 0; i < spawn_argc; i++) {
    p += sprintf(p, " \"%s\"", spawn_args[i]);
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
  
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", js_mkstr(js, stdout_buf ? stdout_buf : "", stdout_len));
  js_set(js, result, "stderr", js_mkstr(js, stderr_buf ? stderr_buf : "", stderr_len));
  js_set(js, result, "status", js_mknum((double)exit_code));
  js_set(js, result, "signal", js_mknull());
  js_set(js, result, "pid", js_mknum((double)pid));
  
  if (stdout_buf) free(stdout_buf);
  if (stderr_buf) free(stderr_buf);
  
  return result;
}
#else
static ant_value_t builtin_spawnSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *input = NULL;
  size_t input_len = 0;
  
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t len_val = js_get(js, args[1], "length");
    if (vtype(len_val) == T_NUM) {
      spawn_args = parse_args_array(js, args[1], &spawn_argc);
    }
  }
  
  if (nargs >= 3 && is_special_object(args[2])) {
    ant_value_t input_val = js_get(js, args[2], "input");
    if (vtype(input_val) == T_STR) {
      input = js_getstr(js, input_val, &input_len);
    }
  }
  
  char **exec_args = calloc(spawn_argc + 2, sizeof(char *));
  if (!exec_args) {
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Out of memory");
  }
  
  exec_args[0] = cmd_str;
  for (int i = 0; i < spawn_argc; i++) {
    exec_args[i + 1] = spawn_args[i];
  }
  exec_args[spawn_argc + 1] = NULL;
  
  int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
  if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
    free(exec_args);
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Failed to create pipes");
  }
  
  pid_t pid = fork();
  
  if (pid < 0) {
    free(exec_args);
    free(cmd_str);
    free_args_array(spawn_args, spawn_argc);
    return js_mkerr(js, "Fork failed");
  }
  
  if (pid == 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    
    execvp(exec_args[0], exec_args);
    _exit(127);
  }
  
  free(exec_args);
  free(cmd_str);
  free_args_array(spawn_args, spawn_argc);
  
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  
  if (input && input_len > 0) {
    write(stdin_pipe[1], input, input_len);
  }
  close(stdin_pipe[1]);
  
  char *stdout_buf = NULL;
  size_t stdout_len = 0;
  size_t stdout_cap = 4096;
  stdout_buf = malloc(stdout_cap);
  
  char *stderr_buf = NULL;
  size_t stderr_len = 0;
  size_t stderr_cap = 4096;
  stderr_buf = malloc(stderr_cap);
  
  char buffer[4096];
  ssize_t n;
  int status = 0;
  
  while ((n = read(stdout_pipe[0], buffer, sizeof(buffer))) > 0) {
    if (stdout_len + n >= stdout_cap) {
      stdout_cap *= 2;
      stdout_buf = realloc(stdout_buf, stdout_cap);
    }
    memcpy(stdout_buf + stdout_len, buffer, n);
    stdout_len += n;
  }
  close(stdout_pipe[0]);
  
  while ((n = read(stderr_pipe[0], buffer, sizeof(buffer))) > 0) {
    if (stderr_len + n >= stderr_cap) {
      stderr_cap *= 2;
      stderr_buf = realloc(stderr_buf, stderr_cap);
    }
    memcpy(stderr_buf + stderr_len, buffer, n);
    stderr_len += n;
  }
  close(stderr_pipe[0]);
  
  waitpid(pid, &status, 0);
  
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  int signal_code = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", js_mkstr(js, stdout_buf ? stdout_buf : "", stdout_len));
  js_set(js, result, "stderr", js_mkstr(js, stderr_buf ? stderr_buf : "", stderr_len));
  js_set(js, result, "status", js_mknum((double)exit_code));
  js_set(js, result, "signal", signal_code ? js_mknum((double)signal_code) : js_mknull());
  js_set(js, result, "pid", js_mknum((double)pid));
  
  if (stdout_buf) free(stdout_buf);
  if (stderr_buf) free(stderr_buf);
  
  return result;
}
#endif

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
  
  js_set(js, lib, "spawn", js_mkfun(builtin_spawn));
  js_set(js, lib, "exec", js_mkfun(builtin_exec));
  js_set(js, lib, "execSync", js_mkfun(builtin_execSync));
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

  child_event_t *evt, *tmp;
  HASH_ITER(hh, cp->events, evt, tmp)
    for (int i = 0; i < evt->count; i++) mark(js, evt->listeners[i].callback);
}}
