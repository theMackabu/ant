#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <uthash.h>
#include <utarray.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "ant.h"
#include "modules/child_process.h"
#include "modules/symbol.h"

#define MAX_CHILD_LISTENERS 16
#define PIPE_READ_BUF_SIZE 65536

typedef struct {
  jsval_t callback;
  bool once;
} child_listener_t;

typedef struct {
  char *event_name;
  child_listener_t listeners[MAX_CHILD_LISTENERS];
  int count;
  UT_hash_handle hh;
} child_event_t;

typedef struct child_process_s {
  struct js *js;
  uv_process_t process;
  uv_pipe_t stdin_pipe;
  uv_pipe_t stdout_pipe;
  uv_pipe_t stderr_pipe;
  jsval_t child_obj;
  jsval_t promise;
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
  int pending_closes;
  char *cwd;
  struct child_process_s *next;
  struct child_process_s *prev;
} child_process_t;

static child_process_t *pending_children_head = NULL;
static child_process_t *pending_children_tail = NULL;
static uv_loop_t *cp_loop = NULL;

static void ensure_cp_loop(void) {
  if (cp_loop == NULL) cp_loop = uv_default_loop();
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
  if (cp->stdout_buf) free(cp->stdout_buf);
  if (cp->stderr_buf) free(cp->stderr_buf);
  if (cp->cwd) free(cp->cwd);
  
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

static void emit_event(child_process_t *cp, const char *name, jsval_t *args, int nargs) {
  child_event_t *evt = NULL;
  HASH_FIND_STR(cp->events, name, evt);
  if (!evt || evt->count == 0) return;
  
  int i = 0;
  while (i < evt->count) {
    child_listener_t *l = &evt->listeners[i];
    jsval_t result = js_call(cp->js, l->callback, args, nargs);
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Error in child_process '%s' listener: %s\n", name, js_str(cp->js, result));
    }
    if (l->once) {
      for (int j = i; j < evt->count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->count--;
    } else i++;
  }
}

static void try_free_child(child_process_t *cp) {
  if (cp->exited && cp->stdout_closed && cp->stderr_closed && cp->pending_closes == 0) {
    remove_pending_child(cp);
    free_child_process(cp);
  }
}

static void check_completion(child_process_t *cp) {
  if (cp->exited && cp->stdout_closed && cp->stderr_closed && !cp->close_emitted) {
    cp->close_emitted = true;
    
    jsval_t stdout_val = js_mkstr(cp->js, cp->stdout_buf ? cp->stdout_buf : "", cp->stdout_len);
    jsval_t stderr_val = js_mkstr(cp->js, cp->stderr_buf ? cp->stderr_buf : "", cp->stderr_len);
    
    js_set(cp->js, cp->child_obj, "stdout", stdout_val);
    js_set(cp->js, cp->child_obj, "stderr", stderr_val);
    js_set(cp->js, cp->child_obj, "exitCode", js_mknum((double)cp->exit_code));
    js_set(cp->js, cp->child_obj, "signalCode", cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull());
    
    jsval_t close_args[2] = { js_mknum((double)cp->exit_code), cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull() };
    emit_event(cp, "close", close_args, 2);
    
    if (js_type(cp->promise) != JS_UNDEF) {
      jsval_t result = js_mkobj(cp->js);
      js_set(cp->js, result, "stdout", stdout_val);
      js_set(cp->js, result, "stderr", stderr_val);
      js_set(cp->js, result, "exitCode", js_mknum((double)cp->exit_code));
      js_set(cp->js, result, "signalCode", cp->term_signal ? js_mknum((double)cp->term_signal) : js_mknull());
      
      if (cp->exit_code != 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command failed with exit code %lld", (long long)cp->exit_code);
        jsval_t err = js_mkstr(cp->js, err_msg, strlen(err_msg));
        js_reject_promise(cp->js, cp->promise, err);
      } else {
        js_resolve_promise(cp->js, cp->promise, result);
      }
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

static void on_exit(uv_process_t *proc, int64_t exit_status, int term_signal) {
  child_process_t *cp = (child_process_t *)proc->data;
  cp->exit_code = exit_status;
  cp->term_signal = term_signal;
  cp->exited = true;
  
  js_set(cp->js, cp->child_obj, "exitCode", js_mknum((double)exit_status));
  if (term_signal) {
    js_set(cp->js, cp->child_obj, "signalCode", js_mknum((double)term_signal));
    js_set(cp->js, cp->child_obj, "killed", js_mktrue());
  }
  
  jsval_t exit_args[2] = { js_mknum((double)exit_status), term_signal ? js_mknum((double)term_signal) : js_mknull() };
  emit_event(cp, "exit", exit_args, 2);
  
  cp->pending_closes++;
  uv_close((uv_handle_t *)proc, on_handle_close);
  
  if (!cp->stdin_closed && !uv_is_closing((uv_handle_t *)&cp->stdin_pipe)) {
    cp->pending_closes++;
    uv_close((uv_handle_t *)&cp->stdin_pipe, on_handle_close);
    cp->stdin_closed = true;
  }
  if (!cp->stdout_closed && !uv_is_closing((uv_handle_t *)&cp->stdout_pipe)) {
    uv_read_stop((uv_stream_t *)&cp->stdout_pipe);
    cp->pending_closes++;
    uv_close((uv_handle_t *)&cp->stdout_pipe, on_handle_close);
    cp->stdout_closed = true;
  }
  if (!cp->stderr_closed && !uv_is_closing((uv_handle_t *)&cp->stderr_pipe)) {
    uv_read_stop((uv_stream_t *)&cp->stderr_pipe);
    cp->pending_closes++;
    uv_close((uv_handle_t *)&cp->stderr_pipe, on_handle_close);
    cp->stderr_closed = true;
  }
  
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
    
    jsval_t data = js_mkstr(cp->js, buf->base, nread);
    jsval_t data_args[1] = { data };
    emit_event(cp, "stdout", data_args, 1);
  }
  
  if (buf->base) free(buf->base);
  
  if (nread < 0) {
    if (nread != UV_EOF) {
      jsval_t err_args[1] = { js_mkstr(cp->js, uv_strerror(nread), strlen(uv_strerror(nread))) };
      emit_event(cp, "error", err_args, 1);
    }
    cp->pending_closes++;
    uv_close((uv_handle_t *)stream, on_handle_close);
    cp->stdout_closed = true;
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
    
    jsval_t data = js_mkstr(cp->js, buf->base, nread);
    jsval_t data_args[1] = { data };
    emit_event(cp, "stderr", data_args, 1);
  }
  
  if (buf->base) free(buf->base);
  
  if (nread < 0) {
    cp->pending_closes++;
    uv_close((uv_handle_t *)stream, on_handle_close);
    cp->stderr_closed = true;
    check_completion(cp);
  }
}

static jsval_t child_on(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "on() requires event name and callback");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Event name must be a string");
  if (js_type(args[1]) != JS_FUNC) return js_mkerr(js, "Callback must be a function");
  
  jsval_t cp_ptr = js_get(js, this_obj, "_cp_ptr");
  if (js_type(cp_ptr) == JS_UNDEF) return js_mkerr(js, "Invalid child process object");
  
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

static jsval_t child_once(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return js_mkerr(js, "once() requires event name and callback");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Event name must be a string");
  if (js_type(args[1]) != JS_FUNC) return js_mkerr(js, "Callback must be a function");
  
  jsval_t cp_ptr = js_get(js, this_obj, "_cp_ptr");
  if (js_type(cp_ptr) == JS_UNDEF) return js_mkerr(js, "Invalid child process object");
  
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

static jsval_t child_kill(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  jsval_t cp_ptr = js_get(js, this_obj, "_cp_ptr");
  if (js_type(cp_ptr) == JS_UNDEF) return js_mkfalse();
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  if (cp->exited) return js_mkfalse();
  
  int sig = SIGTERM;
  if (nargs > 0) {
    if (js_type(args[0]) == JS_NUM) {
      sig = (int)js_getnum(args[0]);
    } else if (js_type(args[0]) == JS_STR) {
      size_t sig_len;
      char *sig_str = js_getstr(js, args[0], &sig_len);
      if (strncmp(sig_str, "SIGTERM", sig_len) == 0) sig = SIGTERM;
      else if (strncmp(sig_str, "SIGKILL", sig_len) == 0) sig = SIGKILL;
      else if (strncmp(sig_str, "SIGINT", sig_len) == 0) sig = SIGINT;
      else if (strncmp(sig_str, "SIGHUP", sig_len) == 0) sig = SIGHUP;
      else if (strncmp(sig_str, "SIGQUIT", sig_len) == 0) sig = SIGQUIT;
    }
  }
  
  int result = uv_process_kill(&cp->process, sig);
  return result == 0 ? js_mktrue() : js_mkfalse();
}

static jsval_t child_write(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 1) return js_mkerr(js, "write() requires data argument");
  
  jsval_t cp_ptr = js_get(js, this_obj, "_cp_ptr");
  if (js_type(cp_ptr) == JS_UNDEF) return js_mkerr(js, "Invalid child process object");
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  if (cp->stdin_closed) return js_mkfalse();
  
  size_t data_len;
  char *data = js_getstr(js, args[0], &data_len);
  if (!data) return js_mkerr(js, "Data must be a string");
  
  uv_write_t *write_req = malloc(sizeof(uv_write_t));
  char *buf_data = malloc(data_len);
  memcpy(buf_data, data, data_len);
  
  uv_buf_t buf = uv_buf_init(buf_data, data_len);
  write_req->data = buf_data;
  
  int result = uv_write(write_req, (uv_stream_t *)&cp->stdin_pipe, &buf, 1, NULL);
  if (result < 0) {
    free(buf_data);
    free(write_req);
    return js_mkfalse();
  }
  
  return js_mktrue();
}

static jsval_t child_end(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  
  jsval_t cp_ptr = js_get(js, this_obj, "_cp_ptr");
  if (js_type(cp_ptr) == JS_UNDEF) return js_mkundef();
  
  child_process_t *cp = (child_process_t *)(uintptr_t)js_getnum(cp_ptr);
  if (!cp->stdin_closed) {
    uv_close((uv_handle_t *)&cp->stdin_pipe, NULL);
    cp->stdin_closed = true;
  }
  
  return js_mkundef();
}

static jsval_t create_child_object(struct js *js, child_process_t *cp) {
  jsval_t obj = js_mkobj(js);
  
  js_set(js, obj, "_cp_ptr", js_mknum((double)(uintptr_t)cp));
  js_set(js, obj, "pid", js_mknum((double)cp->process.pid));
  js_set(js, obj, "exitCode", js_mknull());
  js_set(js, obj, "signalCode", js_mknull());
  js_set(js, obj, "killed", js_mkfalse());
  js_set(js, obj, "connected", js_mktrue());
  
  js_set(js, obj, "on", js_mkfun(child_on));
  js_set(js, obj, "once", js_mkfun(child_once));
  js_set(js, obj, "kill", js_mkfun(child_kill));
  js_set(js, obj, "write", js_mkfun(child_write));
  js_set(js, obj, "end", js_mkfun(child_end));
  
  js_set(js, obj, get_toStringTag_sym_key(), js_mkstr(js, "ChildProcess", 12));
  
  return obj;
}

static char **parse_args_array(struct js *js, jsval_t arr, int *count) {
  jsval_t len_val = js_get(js, arr, "length");
  int len = (int)js_getnum(len_val);
  
  char **args = calloc(len + 1, sizeof(char *));
  if (!args) {
    *count = 0;
    return NULL;
  }
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t val = js_get(js, arr, idx);
    if (js_type(val) == JS_STR) {
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

static jsval_t builtin_spawn(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "spawn() requires a command");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Command must be a string");
  
  ensure_cp_loop();
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *cwd = NULL;
  bool use_shell = false;
  bool detached = false;
  
  if (nargs >= 2 && js_type(args[1]) == JS_OBJ) {
    jsval_t len_val = js_get(js, args[1], "length");
    if (js_type(len_val) == JS_NUM) {
      spawn_args = parse_args_array(js, args[1], &spawn_argc);
    }
  }
  
  if (nargs >= 3 && js_type(args[2]) == JS_OBJ) {
    jsval_t cwd_val = js_get(js, args[2], "cwd");
    if (js_type(cwd_val) == JS_STR) {
      size_t cwd_len;
      char *cwd_str = js_getstr(js, cwd_val, &cwd_len);
      cwd = strndup(cwd_str, cwd_len);
    }
    
    jsval_t shell_val = js_get(js, args[2], "shell");
    use_shell = js_truthy(js, shell_val);
    
    jsval_t detached_val = js_get(js, args[2], "detached");
    detached = js_truthy(js, detached_val);
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
  
  uv_pipe_init(cp_loop, &cp->stdin_pipe, 0);
  uv_pipe_init(cp_loop, &cp->stdout_pipe, 0);
  uv_pipe_init(cp_loop, &cp->stderr_pipe, 0);
  
  cp->stdin_pipe.data = cp;
  cp->stdout_pipe.data = cp;
  cp->stderr_pipe.data = cp;
  cp->process.data = cp;
  
  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
  stdio[0].data.stream = (uv_stream_t *)&cp->stdin_pipe;
  stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[1].data.stream = (uv_stream_t *)&cp->stdout_pipe;
  stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[2].data.stream = (uv_stream_t *)&cp->stderr_pipe;
  
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
  options.exit_cb = on_exit;
  options.file = final_args[0];
  options.args = final_args;
  options.stdio_count = 3;
  options.stdio = stdio;
  if (cwd) options.cwd = cwd;
  if (detached) options.flags = UV_PROCESS_DETACHED;
  
  int r = uv_spawn(cp_loop, &cp->process, &options);
  
  if (use_shell) {
    free(final_args[0]);
    free(final_args[1]);
    free(shell_cmd);
    free(final_args);
  } else {
    for (int i = 0; i < final_argc; i++) {
      if (final_args[i]) free(final_args[i]);
    }
    free(final_args);
  }
  
  free_args_array(spawn_args, spawn_argc);
  
  if (r < 0) {
    free_child_process(cp);
    return js_mkerr(js, "Failed to spawn process: %s", uv_strerror(r));
  }
  
  uv_read_start((uv_stream_t *)&cp->stdout_pipe, alloc_buffer, on_stdout_read);
  uv_read_start((uv_stream_t *)&cp->stderr_pipe, alloc_buffer, on_stderr_read);
  
  add_pending_child(cp);
  
  cp->child_obj = create_child_object(js, cp);
  return cp->child_obj;
}

static jsval_t builtin_exec(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "exec() requires a command");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Command must be a string");
  
  ensure_cp_loop();
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  char *cwd = NULL;
  if (nargs >= 2 && js_type(args[1]) == JS_OBJ) {
    jsval_t cwd_val = js_get(js, args[1], "cwd");
    if (js_type(cwd_val) == JS_STR) {
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
  
  uv_pipe_init(cp_loop, &cp->stdin_pipe, 0);
  uv_pipe_init(cp_loop, &cp->stdout_pipe, 0);
  uv_pipe_init(cp_loop, &cp->stderr_pipe, 0);
  
  cp->stdin_pipe.data = cp;
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
  options.exit_cb = on_exit;
  options.file = "/bin/sh";
  options.args = shell_args;
  options.stdio_count = 3;
  options.stdio = stdio;
  if (cwd) options.cwd = cwd;
  
  int r = uv_spawn(cp_loop, &cp->process, &options);
  free(cmd_str);
  
  if (r < 0) {
    jsval_t promise = cp->promise;
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

static jsval_t builtin_execSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "execSync() requires a command");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Command must be a string");
  
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
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  
  if (output_len > 0 && output[output_len - 1] == '\n') {
    output_len--;
  }
  
  if (exit_code != 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Command failed with exit code %d", exit_code);
    free(output);
    return js_mkerr(js, err_msg);
  }
  
  jsval_t result = js_mkstr(js, output, output_len);
  free(output);
  return result;
}

static jsval_t builtin_spawnSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "spawnSync() requires a command");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Command must be a string");
  
  size_t cmd_len;
  char *cmd = js_getstr(js, args[0], &cmd_len);
  char *cmd_str = strndup(cmd, cmd_len);
  
  char **spawn_args = NULL;
  int spawn_argc = 0;
  char *input = NULL;
  size_t input_len = 0;
  
  if (nargs >= 2 && js_type(args[1]) == JS_OBJ) {
    jsval_t len_val = js_get(js, args[1], "length");
    if (js_type(len_val) == JS_NUM) {
      spawn_args = parse_args_array(js, args[1], &spawn_argc);
    }
  }
  
  if (nargs >= 3 && js_type(args[2]) == JS_OBJ) {
    jsval_t input_val = js_get(js, args[2], "input");
    if (js_type(input_val) == JS_STR) {
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
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "stdout", js_mkstr(js, stdout_buf ? stdout_buf : "", stdout_len));
  js_set(js, result, "stderr", js_mkstr(js, stderr_buf ? stderr_buf : "", stderr_len));
  js_set(js, result, "status", js_mknum((double)exit_code));
  js_set(js, result, "signal", signal_code ? js_mknum((double)signal_code) : js_mknull());
  js_set(js, result, "pid", js_mknum((double)pid));
  
  if (stdout_buf) free(stdout_buf);
  if (stderr_buf) free(stderr_buf);
  
  return result;
}

static jsval_t builtin_fork(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fork() requires a module path");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "Module path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *path_str = strndup(path, path_len);
  
  char exe_path[1024];
  uint32_t size = sizeof(exe_path);
  
#if defined(__APPLE__)
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
  
  jsval_t spawn_args[3];
  spawn_args[0] = js_mkstr(js, exe_path, strlen(exe_path));
  
  jsval_t args_arr = js_mkarr(js);
  js_arr_push(js, args_arr, js_mkstr(js, path_str, path_len));
  
  if (nargs >= 2 && js_type(args[1]) == JS_OBJ) {
    jsval_t exec_args = js_get(js, args[1], "execArgv");
    if (js_type(exec_args) == JS_OBJ) {
      jsval_t len_val = js_get(js, exec_args, "length");
      int len = (int)js_getnum(len_val);
      for (int i = 0; i < len; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        jsval_t arg = js_get(js, exec_args, idx);
        js_arr_push(js, args_arr, arg);
      }
    }
  }
  
  spawn_args[1] = args_arr;
  spawn_args[2] = js_mkobj(js);
  
  free(path_str);
  
  return builtin_spawn(js, spawn_args, 3);
}

jsval_t child_process_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  js_set(js, lib, "spawn", js_mkfun(builtin_spawn));
  js_set(js, lib, "exec", js_mkfun(builtin_exec));
  js_set(js, lib, "execSync", js_mkfun(builtin_execSync));
  js_set(js, lib, "spawnSync", js_mkfun(builtin_spawnSync));
  js_set(js, lib, "fork", js_mkfun(builtin_fork));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "child_process", 13));
  
  return lib;
}

int has_pending_child_processes(void) {
  return pending_children_head != NULL;
}

void child_process_poll_events(void) {
  if (cp_loop && uv_loop_alive(cp_loop)) {
    uv_run(cp_loop, UV_RUN_NOWAIT);
  }
}

void child_process_gc_update_roots(GC_FWD_ARGS) {
  for (child_process_t *cp = pending_children_head; cp; cp = cp->next) {
    cp->child_obj = fwd_val(ctx, cp->child_obj);
    cp->promise = fwd_val(ctx, cp->promise);
    
    child_event_t *evt, *tmp;
    HASH_ITER(hh, cp->events, evt, tmp) {
      for (int i = 0; i < evt->count; i++) evt->listeners[i].callback = fwd_val(ctx, evt->listeners[i].callback);
    }
  }
}
