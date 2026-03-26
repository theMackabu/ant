// stub: minimal node:worker_threads implementation
// just enough for rolldown to run transforms

#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <io.h>
#define WT_WRITE _write
#define WT_STDOUT_FD 1
extern char **_environ;
#define environ _environ
#else
#include <unistd.h>
#define WT_WRITE write
#define WT_STDOUT_FD STDOUT_FILENO
extern char **environ;
#endif

#include "ant.h"
#include "internal.h"
#include "runtime.h"
#include "descriptors.h"
#include "silver/engine.h"
#include "modules/json.h"
#include "modules/symbol.h"
#include "modules/worker_threads.h"
#include "gc/modules.h"

#define WT_ENV_MODE "ANT_WORKER_THREADS_MODE"
#define WT_ENV_DATA_JSON "ANT_WORKER_DATA_JSON"
#define WT_ENV_STORE_JSON "ANT_WORKER_ENV_DATA_JSON"
#define WT_MSG_PREFIX "ANT_WT_MSG:"

typedef struct ant_worker_thread {
  ant_t *js;
  uv_process_t process;
  uv_pipe_t stdout_pipe;
  bool spawned;
  bool exited;
  bool closing;
  bool closed;
  bool refed;
  int close_pending;
  int64_t exit_status;
  int term_signal;
  char *line_buf;
  size_t line_len;
  size_t line_cap;
  ant_value_t self_val;
  ant_value_t terminate_val;
  bool has_terminate_val;
  struct ant_worker_thread *next;
  struct ant_worker_thread *prev;
} ant_worker_thread_t;

static ant_worker_thread_t *active_workers_head = NULL;

static bool wt_is_worker_mode(void) {
  const char *mode = getenv(WT_ENV_MODE);
  return mode && strcmp(mode, "1") == 0;
}

static ant_value_t wt_get_or_create_env_store(ant_t *js) {
  ant_value_t store = js_get_slot(js->global, SLOT_WT_ENV_STORE);
  if (!is_object_type(store)) {
    store = js_mkobj(js);
    js_set_slot(js->global, SLOT_WT_ENV_STORE, store);
  }
  return store;
}

static void wt_init_env_store(ant_t *js, bool is_worker) {
  ant_value_t store = js_mkobj(js);
  if (is_worker) {
    const char *raw = getenv(WT_ENV_STORE_JSON);
    if (raw && raw[0]) {
      ant_value_t input = js_mkstr(js, raw, strlen(raw));
      ant_value_t parsed = json_parse_value(js, input);
      if (is_object_type(parsed)) store = parsed;
    }
  }
  js_set_slot(js->global, SLOT_WT_ENV_STORE, store);
}

static ant_worker_thread_t *wt_get_worker(ant_t *js, ant_value_t this_obj) {
  if (!is_object_type(this_obj)) return NULL;
  ant_value_t data = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(data) != T_NUM) return NULL;
  return (ant_worker_thread_t *)(uintptr_t)js_getnum(data);
}

static bool wt_is_message_port(ant_t *js, ant_value_t obj) {
  if (!is_object_type(obj)) return false;
  ant_value_t tag = js_get_slot(obj, SLOT_WT_PORT_TAG);
  return js_truthy(js, tag);
}

static ant_value_t wt_get_message_port_proto(ant_t *js) {
  ant_value_t proto = js_get_slot(js->global, SLOT_WT_PORT_PROTO);
  if (!is_object_type(proto)) proto = js_mkobj(js);
  return proto;
}

static ant_value_t wt_make_message_port(ant_t *js) {
  ant_value_t port = js_mkobj(js);
  js_set_proto_init(port, wt_get_message_port_proto(js));
  js_set_slot(port, SLOT_WT_PORT_TAG, js_true);
  js_set_slot(port, SLOT_WT_PORT_QUEUE, js_mkarr(js));
  js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum(0));
  js_set_slot(port, SLOT_WT_PORT_PEER, js_mknull());
  js_set_slot(port, SLOT_WT_PORT_CLOSED, js_false);
  js_set_slot(port, SLOT_WT_PORT_STARTED, js_false);
  js_set_slot(port, SLOT_WT_PORT_ON_MESSAGE, js_mkundef());
  js_set_slot(port, SLOT_WT_PORT_ONCE_MESSAGE, js_mkundef());
  js_set(js, port, "onmessage", js_mkundef());
  return port;
}

static bool wt_port_is_closed(ant_t *js, ant_value_t port) {
  return js_truthy(js, js_get_slot(port, SLOT_WT_PORT_CLOSED));
}

static void wt_port_set_closed(ant_t *js, ant_value_t port, bool closed) {
  js_set_slot(port, SLOT_WT_PORT_CLOSED, js_bool(closed));
}

static bool wt_port_queue_dequeue(ant_t *js, ant_value_t port, ant_value_t *out) {
  ant_value_t queue = js_get_slot(port, SLOT_WT_PORT_QUEUE);
  if (vtype(queue) != T_ARR) return false;

  ant_value_t head_val = js_get_slot(port, SLOT_WT_PORT_HEAD);
  ant_offset_t head = (vtype(head_val) == T_NUM) ? (ant_offset_t)js_getnum(head_val) : 0;
  ant_offset_t len = js_arr_len(js, queue);
  if (len <= 0 || head >= len) {
    js_set_slot(port, SLOT_WT_PORT_QUEUE, js_mkarr(js));
    js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum(0));
    return false;
  }

  if (out) *out = js_arr_get(js, queue, head);
  ant_offset_t next_head = head + 1;

  if (next_head >= len) {
    js_set_slot(port, SLOT_WT_PORT_QUEUE, js_mkarr(js));
    js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum(0));
    return true;
  }

  if (next_head > 32 && next_head * 2 >= len) {
    ant_value_t compact = js_mkarr(js);
    for (ant_offset_t i = next_head; i < len; i++) js_arr_push(js, compact, js_arr_get(js, queue, i));
    js_set_slot(port, SLOT_WT_PORT_QUEUE, compact);
    js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum(0));
    return true;
  }

  js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum((double)next_head));
  return true;
}

static void wt_port_queue_push(ant_t *js, ant_value_t port, ant_value_t value) {
  ant_value_t queue = js_get_slot(port, SLOT_WT_PORT_QUEUE);
  if (vtype(queue) != T_ARR) {
    queue = js_mkarr(js);
    js_set_slot(port, SLOT_WT_PORT_QUEUE, queue);
    js_set_slot(port, SLOT_WT_PORT_HEAD, js_mknum(0));
  }
  js_arr_push(js, queue, value);
}

static void wt_port_call_listener(ant_t *js, ant_value_t this_obj, ant_value_t fn, ant_value_t arg) {
  if (!is_callable(fn)) return;
  ant_value_t argv[1] = {arg};
  
  sv_vm_call(js->vm, js, fn, this_obj, argv, 1, NULL, false);
}

static bool wt_port_should_deliver(ant_t *js, ant_value_t port) {
  if (wt_port_is_closed(js, port)) return false;
  bool started = js_truthy(js, js_get_slot(port, SLOT_WT_PORT_STARTED));
  ant_value_t on_fn = js_get_slot(port, SLOT_WT_PORT_ON_MESSAGE);
  ant_value_t once_fn = js_get_slot(port, SLOT_WT_PORT_ONCE_MESSAGE);
  ant_value_t onmessage = js_get(js, port, "onmessage");

  bool has_event_listener = is_callable(on_fn) || is_callable(once_fn);
  if (is_callable(onmessage)) return true;
  return started && has_event_listener;
}

static void wt_port_drain(ant_t *js, ant_value_t port) {
  if (!wt_is_message_port(js, port)) return;

  while (wt_port_should_deliver(js, port)) {
    ant_value_t msg = js_mkundef();
    if (!wt_port_queue_dequeue(js, port, &msg)) break;

    ant_value_t on_fn = js_get_slot(port, SLOT_WT_PORT_ON_MESSAGE);
    wt_port_call_listener(js, port, on_fn, msg);

    ant_value_t once_fn = js_get_slot(port, SLOT_WT_PORT_ONCE_MESSAGE);
    if (is_callable(once_fn)) {
      wt_port_call_listener(js, port, once_fn, msg);
      js_set_slot(port, SLOT_WT_PORT_ONCE_MESSAGE, js_mkundef());
    }

    ant_value_t onmessage = js_get(js, port, "onmessage");
    if (is_callable(onmessage)) {
      ant_value_t event_obj = js_mkobj(js);
      js_set(js, event_obj, "data", msg);
      wt_port_call_listener(js, port, onmessage, event_obj);
    }

    if (wt_port_is_closed(js, port)) break;
  }
}

static ant_value_t wt_make_resolved_promise(ant_t *js, ant_value_t value) {
  ant_value_t p = js_mkpromise(js);
  js_resolve_promise(js, p, value);
  return p;
}

static void wt_call_listener(ant_t *js, ant_value_t this_obj, ant_value_t fn, ant_value_t arg) {
  if (!is_callable(fn)) return;
  ant_value_t argv[1] = {arg};

  sv_vm_call(js->vm, js, fn, this_obj, argv, 1, NULL, false);
}

static void wt_emit(ant_worker_thread_t *wt, const char *event, ant_value_t arg) {
  if (!wt || !wt->js) return;
  ant_t *js = wt->js;
  ant_value_t this_obj = wt->self_val;
  if (!is_object_type(this_obj)) return;

  internal_slot_t on_slot, once_slot;
  if (strcmp(event, "message") == 0) {
    on_slot = SLOT_WT_ON_MESSAGE;
    once_slot = SLOT_WT_ONCE_MESSAGE;
  } else if (strcmp(event, "exit") == 0) {
    on_slot = SLOT_WT_ON_EXIT;
    once_slot = SLOT_WT_ONCE_EXIT;
  } else return;

  ant_value_t on_fn = js_get_slot(this_obj, on_slot);
  wt_call_listener(js, this_obj, on_fn, arg);

  ant_value_t once_fn = js_get_slot(this_obj, once_slot);
  if (is_callable(once_fn)) {
    wt_call_listener(js, this_obj, once_fn, arg);
    js_set_slot(this_obj, once_slot, js_mkundef());
  }
}

static void wt_free_env(char **env) {
  if (!env) return;
  for (char **p = env; *p; p++) free(*p);
  free(env);
}

static char **wt_build_worker_env(const char *worker_data_json, const char *env_store_json) {
  size_t count = 0;
  if (environ) {
    while (environ[count]) count++;
  }

  size_t extra = 2;
  if (worker_data_json) extra++;
  if (env_store_json) extra++;
  char **env = (char **)calloc(count + extra, sizeof(char *));
  if (!env) return NULL;

  size_t out = 0;
  for (size_t i = 0; i < count; i++) {
    env[out] = strdup(environ[i]);
    if (!env[out]) {
      wt_free_env(env);
      return NULL;
    }
    out++;
  }

  env[out++] = strdup(WT_ENV_MODE "=1");
  if (!env[out - 1]) {
    wt_free_env(env);
    return NULL;
  }

  if (worker_data_json) {
    size_t key_len = strlen(WT_ENV_DATA_JSON);
    size_t val_len = strlen(worker_data_json);
    char *entry = (char *)malloc(key_len + 1 + val_len + 1);
    if (!entry) {
      wt_free_env(env);
      return NULL;
    }
    memcpy(entry, WT_ENV_DATA_JSON, key_len);
    entry[key_len] = '=';
    memcpy(entry + key_len + 1, worker_data_json, val_len);
    entry[key_len + 1 + val_len] = '\0';
    env[out++] = entry;
  }

  if (env_store_json) {
    size_t key_len = strlen(WT_ENV_STORE_JSON);
    size_t val_len = strlen(env_store_json);
    char *entry = (char *)malloc(key_len + 1 + val_len + 1);
    if (!entry) {
      wt_free_env(env);
      return NULL;
    }
    memcpy(entry, WT_ENV_STORE_JSON, key_len);
    entry[key_len] = '=';
    memcpy(entry + key_len + 1, env_store_json, val_len);
    entry[key_len + 1 + val_len] = '\0';
    env[out++] = entry;
  }

  env[out] = NULL;
  return env;
}

static void wt_cleanup(ant_worker_thread_t *wt) {
  if (!wt) return;
  free(wt->line_buf);
  free(wt);
}

static void wt_detach(ant_worker_thread_t *wt) {
  if (!wt) return;

  if (wt->prev) wt->prev->next = wt->next;
  else active_workers_head = wt->next;
  if (wt->next) wt->next->prev = wt->prev;
  wt->prev = NULL;
  wt->next = NULL;

  wt->self_val = js_mkundef();
  wt->terminate_val = js_mkundef();
  wt->has_terminate_val = false;
  free(wt->line_buf);
  wt->line_buf = NULL;
  wt->line_len = 0;
  wt->line_cap = 0;
  wt->spawned = false;
  wt->refed = false;
  wt->closed = true;
  wt->js = NULL;
}

static void wt_on_handle_closed(uv_handle_t *h) {
  ant_worker_thread_t *wt = (ant_worker_thread_t *)h->data;
  if (!wt) return;
  if (wt->close_pending > 0) wt->close_pending--;
  if (wt->close_pending == 0) wt_detach(wt);
}

static void wt_finish_exit(ant_worker_thread_t *wt) {
  if (!wt) return;
  if (wt->closed || wt->closing) return;
  wt->closing = true;
  wt->spawned = false;
  wt->refed = false;
  wt->close_pending = 0;

  if (!uv_is_closing((uv_handle_t *)&wt->stdout_pipe)) {
    wt->close_pending++;
    uv_close((uv_handle_t *)&wt->stdout_pipe, wt_on_handle_closed);
  }
  if (!uv_is_closing((uv_handle_t *)&wt->process)) {
    wt->close_pending++;
    uv_close((uv_handle_t *)&wt->process, wt_on_handle_closed);
  }

  if (wt->close_pending == 0) wt_detach(wt);
}

static void wt_on_process_exit(uv_process_t *proc, int64_t exit_status, int term_signal) {
  ant_worker_thread_t *wt = (ant_worker_thread_t *)proc->data;
  if (!wt || !wt->js) return;

  wt->exited = true;
  wt->exit_status = exit_status;
  wt->term_signal = term_signal;

  if (wt->has_terminate_val) {
    ant_value_t p = wt->terminate_val;
    js_resolve_promise(wt->js, p, js_mknum((double)exit_status));
    wt->terminate_val = js_mkundef();
    wt->has_terminate_val = false;
  }

  wt_emit(wt, "exit", js_mknum((double)exit_status));
  wt_finish_exit(wt);
}

static void wt_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = (char *)malloc(suggested_size);
  buf->len = buf->base ? suggested_size : 0;
}

static void wt_emit_message_from_json(ant_worker_thread_t *wt, const char *json, size_t len) {
  if (!wt || !wt->js) return;
  ant_t *js = wt->js;

  ant_value_t s = js_mkstr(js, json, len);
  ant_value_t msg = json_parse_value(js, s);
  if (is_err(msg)) msg = s;

  wt_emit(wt, "message", msg);
}

static void wt_process_lines(ant_worker_thread_t *wt) {
  if (!wt || !wt->line_buf) return;

  for (;;) {
    char *nl = memchr(wt->line_buf, '\n', wt->line_len);
    if (!nl) break;

    size_t line_len = (size_t)(nl - wt->line_buf);
    if (line_len > 0 && wt->line_buf[line_len - 1] == '\r') line_len--;

    size_t prefix_len = strlen(WT_MSG_PREFIX);
    if (line_len >= prefix_len && memcmp(wt->line_buf, WT_MSG_PREFIX, prefix_len) == 0) {
      const char *payload = wt->line_buf + prefix_len;
      size_t payload_len = line_len - prefix_len;
      wt_emit_message_from_json(wt, payload, payload_len);
    }

    size_t consumed = (size_t)(nl - wt->line_buf) + 1;
    size_t remain = wt->line_len - consumed;
    memmove(wt->line_buf, wt->line_buf + consumed, remain);
    wt->line_len = remain;
  }
}

static bool wt_append_stdout(ant_worker_thread_t *wt, const char *data, size_t len) {
  if (!wt || !data || len == 0) return true;

  size_t needed = wt->line_len + len;
  if (needed + 1 > wt->line_cap) {
    size_t cap = wt->line_cap ? wt->line_cap : 1024;
    while (cap < needed + 1) cap *= 2;
    char *next = (char *)realloc(wt->line_buf, cap);
    if (!next) return false;
    wt->line_buf = next;
    wt->line_cap = cap;
  }

  memcpy(wt->line_buf + wt->line_len, data, len);
  wt->line_len += len;
  wt->line_buf[wt->line_len] = '\0';
  return true;
}

static void wt_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  ant_worker_thread_t *wt = (ant_worker_thread_t *)stream->data;
  if (!wt) {
    free(buf->base);
    return;
  }

  if (nread > 0) {
    if (wt_append_stdout(wt, buf->base, (size_t)nread)) wt_process_lines(wt);
  } else if (nread < 0) {
    uv_read_stop(stream);
  }

  free(buf->base);
}

static char *wt_path_from_specifier(ant_t *js, ant_value_t spec) {
  const char *raw = NULL;
  size_t len = 0;

  if (vtype(spec) == T_STR) {
    raw = js_getstr(js, spec, &len);
  } else if (is_object_type(spec)) {
    ant_value_t pathname = js_get(js, spec, "pathname");
    if (vtype(pathname) == T_STR) raw = js_getstr(js, pathname, &len);
    if (!raw) {
      ant_value_t href = js_get(js, spec, "href");
      if (vtype(href) == T_STR) raw = js_getstr(js, href, &len);
    }
  }

  if (!raw || len == 0) return NULL;

  if (len >= 7 && strncmp(raw, "file://", 7) == 0) {
    const char *p = raw + 7;
    if (strncmp(p, "localhost/", 10) == 0) p += 9;
    if (*p == '\0') return NULL;
    return strndup(p, len - (size_t)(p - raw));
  }

  return strndup(raw, len);
}

static int wt_spawn_worker(
  ant_worker_thread_t *wt,
  const char *script_path,
  const char *worker_data_json,
  const char *env_store_json
) {
  if (!wt || !wt->js || !script_path || !rt || !rt->argv || rt->argc <= 0) return UV_EINVAL;

  uv_loop_t *loop = uv_default_loop();
  uv_pipe_init(loop, &wt->stdout_pipe, 0);

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[1].data.stream = (uv_stream_t *)&wt->stdout_pipe;
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = 2;

  char *argv0 = strdup(rt->argv[0]);
  char *argv1 = strdup(script_path);
  if (!argv0 || !argv1) {
    free(argv0);
    free(argv1);
    uv_close((uv_handle_t *)&wt->stdout_pipe, NULL);
    return UV_ENOMEM;
  }

  char *args[3] = {argv0, argv1, NULL};
  char **env = wt_build_worker_env(worker_data_json, env_store_json);
  if (!env) {
    free(argv0);
    free(argv1);
    uv_close((uv_handle_t *)&wt->stdout_pipe, NULL);
    return UV_ENOMEM;
  }

  uv_process_options_t options;
  memset(&options, 0, sizeof(options));
  options.file = argv0;
  options.args = args;
  options.env = env;
  options.stdio_count = 3;
  options.stdio = stdio;
  options.exit_cb = wt_on_process_exit;

  wt->process.data = wt;
  wt->stdout_pipe.data = wt;

  int rc = uv_spawn(loop, &wt->process, &options);

  wt_free_env(env);
  free(argv0);
  free(argv1);

  if (rc != 0) {
    uv_close((uv_handle_t *)&wt->stdout_pipe, NULL);
    return rc;
  }

  wt->spawned = true;
  wt->refed = true;

  rc = uv_read_start((uv_stream_t *)&wt->stdout_pipe, wt_alloc_cb, wt_read_cb);
  if (rc != 0) {
    uv_process_kill(&wt->process, SIGTERM);
  }
  return rc;
}

static ant_value_t worker_threads_worker_on(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1])) {
    return js_mkerr(js, "Worker.on(event, listener) requires (string, function)");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkerr(js, "invalid Worker receiver");

  size_t len = 0;
  const char *event = js_getstr(js, args[0], &len);
  if (!event) return js_mkerr(js, "invalid event name");

  if (len == 7 && memcmp(event, "message", 7) == 0) {
    js_set_slot(this_obj, SLOT_WT_ON_MESSAGE, args[1]);
  } else if (len == 4 && memcmp(event, "exit", 4) == 0) {
    js_set_slot(this_obj, SLOT_WT_ON_EXIT, args[1]);
  }

  return this_obj;
}

static ant_value_t worker_threads_worker_once(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1])) {
    return js_mkerr(js, "Worker.once(event, listener) requires (string, function)");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkerr(js, "invalid Worker receiver");

  size_t len = 0;
  const char *event = js_getstr(js, args[0], &len);
  if (!event) return js_mkerr(js, "invalid event name");

  if (len == 7 && memcmp(event, "message", 7) == 0) {
    js_set_slot(this_obj, SLOT_WT_ONCE_MESSAGE, args[1]);
  } else if (len == 4 && memcmp(event, "exit", 4) == 0) {
    js_set_slot(this_obj, SLOT_WT_ONCE_EXIT, args[1]);
  }

  return this_obj;
}

static ant_value_t worker_threads_worker_unref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_worker_thread_t *wt = wt_get_worker(js, this_obj);
  if (!wt) return js_mkerr(js, "invalid Worker receiver");
  if (wt->spawned && wt->refed) {
    if (!uv_is_closing((uv_handle_t *)&wt->process)) uv_unref((uv_handle_t *)&wt->process);
    if (!uv_is_closing((uv_handle_t *)&wt->stdout_pipe)) uv_unref((uv_handle_t *)&wt->stdout_pipe);
    wt->refed = false;
  }
  return this_obj;
}

static ant_value_t worker_threads_worker_ref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_worker_thread_t *wt = wt_get_worker(js, this_obj);
  if (!wt) return js_mkerr(js, "invalid Worker receiver");
  if (wt->spawned && !wt->refed) {
    if (!uv_is_closing((uv_handle_t *)&wt->process)) uv_ref((uv_handle_t *)&wt->process);
    if (!uv_is_closing((uv_handle_t *)&wt->stdout_pipe)) uv_ref((uv_handle_t *)&wt->stdout_pipe);
    wt->refed = true;
  }
  return this_obj;
}

static ant_value_t worker_threads_worker_terminate(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_worker_thread_t *wt = wt_get_worker(js, this_obj);
  if (!wt) return js_mkerr(js, "invalid Worker receiver");

  if (wt->exited) return wt_make_resolved_promise(js, js_mknum((double)wt->exit_status));

  if (!wt->has_terminate_val) {
    ant_value_t p = js_mkpromise(js);
    wt->terminate_val = p;
    wt->has_terminate_val = true;
  }

  int rc = uv_process_kill(&wt->process, SIGTERM);
  if (rc != 0) {
    ant_value_t p = wt->terminate_val;
    js_reject_promise(js, p, js_mkerr(js, "terminate failed: %s", uv_strerror(rc)));
    wt->terminate_val = js_mkundef();
    wt->has_terminate_val = false;
    return p;
  }

  return wt->terminate_val;
}

static ant_value_t worker_threads_worker_post_message(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr(js, "Worker.postMessage is not implemented yet");
}

static ant_value_t worker_threads_message_port_post_message(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (!wt_is_message_port(js, this_obj)) return js_mkerr(js, "invalid MessagePort receiver");
  if (wt_port_is_closed(js, this_obj)) return js_mkundef();

  ant_value_t peer = js_get_slot(this_obj, SLOT_WT_PORT_PEER);
  if (!wt_is_message_port(js, peer) || wt_port_is_closed(js, peer)) return js_mkundef();

  ant_value_t value = (nargs > 0) ? args[0] : js_mkundef();
  wt_port_queue_push(js, peer, value);
  wt_port_drain(js, peer);
  return js_mkundef();
}

static ant_value_t worker_threads_message_port_on(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1])) {
    return js_mkerr(js, "MessagePort.on(event, listener) requires (string, function)");
  }
  ant_value_t this_obj = js_getthis(js);
  if (!wt_is_message_port(js, this_obj)) return js_mkerr(js, "invalid MessagePort receiver");

  size_t len = 0;
  const char *event = js_getstr(js, args[0], &len);
  if (!event) return js_mkerr(js, "invalid event name");
  if (len == 7 && memcmp(event, "message", 7) == 0) {
    js_set_slot(this_obj, SLOT_WT_PORT_ON_MESSAGE, args[1]);
    js_set_slot(this_obj, SLOT_WT_PORT_STARTED, js_true);
    wt_port_drain(js, this_obj);
  }
  return this_obj;
}

static ant_value_t worker_threads_message_port_once(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1])) {
    return js_mkerr(js, "MessagePort.once(event, listener) requires (string, function)");
  }
  ant_value_t this_obj = js_getthis(js);
  if (!wt_is_message_port(js, this_obj)) return js_mkerr(js, "invalid MessagePort receiver");

  size_t len = 0;
  const char *event = js_getstr(js, args[0], &len);
  if (!event) return js_mkerr(js, "invalid event name");
  if (len == 7 && memcmp(event, "message", 7) == 0) {
    js_set_slot(this_obj, SLOT_WT_PORT_ONCE_MESSAGE, args[1]);
    js_set_slot(this_obj, SLOT_WT_PORT_STARTED, js_true);
    wt_port_drain(js, this_obj);
  }
  return this_obj;
}

static ant_value_t worker_threads_message_port_start(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (!wt_is_message_port(js, this_obj)) return js_mkerr(js, "invalid MessagePort receiver");
  js_set_slot(this_obj, SLOT_WT_PORT_STARTED, js_true);
  wt_port_drain(js, this_obj);
  return js_mkundef();
}

static ant_value_t worker_threads_message_port_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (!wt_is_message_port(js, this_obj)) return js_mkerr(js, "invalid MessagePort receiver");
  wt_port_set_closed(js, this_obj, true);

  ant_value_t peer = js_get_slot(this_obj, SLOT_WT_PORT_PEER);
  js_set_slot(this_obj, SLOT_WT_PORT_PEER, js_mknull());
  if (wt_is_message_port(js, peer)) js_set_slot(peer, SLOT_WT_PORT_PEER, js_mknull());
  return js_mkundef();
}

static ant_value_t worker_threads_message_port_ref(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t worker_threads_message_port_unref(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t worker_threads_message_port_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr(js, "MessagePort constructor is not public");
}

static ant_value_t worker_threads_message_channel_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr(js, "MessageChannel constructor requires 'new'");
  }

  ant_value_t this_obj = js_getthis(js);
  ant_value_t port1 = wt_make_message_port(js);
  ant_value_t port2 = wt_make_message_port(js);
  js_set_slot(port1, SLOT_WT_PORT_PEER, port2);
  js_set_slot(port2, SLOT_WT_PORT_PEER, port1);
  js_set(js, this_obj, "port1", port1);
  js_set(js, this_obj, "port2", port2);
  return this_obj;
}

static ant_value_t worker_threads_worker_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr(js, "Worker constructor requires 'new'");
  }
  if (nargs < 1) return js_mkerr(js, "Worker() requires a filename or URL");

  char *script_path = wt_path_from_specifier(js, args[0]);
  if (!script_path) return js_mkerr(js, "Invalid Worker filename/URL");

  const char *worker_data_json = NULL;
  const char *env_store_json = NULL;
  char *worker_data_heap = NULL;
  char *env_store_heap = NULL;
  if (nargs >= 2 && is_object_type(args[1])) {
    ant_value_t worker_data = js_get(js, args[1], "workerData");
    if (!is_undefined(worker_data)) {
      ant_value_t stringify_args[1] = {worker_data};
      ant_value_t json = js_json_stringify(js, stringify_args, 1);
      if (vtype(json) != T_STR) {
        free(script_path);
        return js_mkerr(js, "Worker options.workerData must be JSON-serializable");
      }
      size_t len = 0;
      const char *raw = js_getstr(js, json, &len);
      if (!raw) {
        free(script_path);
        return js_mkerr(js, "Failed to stringify workerData");
      }
      worker_data_heap = strndup(raw, len);
      if (!worker_data_heap) {
        free(script_path);
        return js_mkerr(js, "Out of memory");
      }
      worker_data_json = worker_data_heap;
    }
  }

  ant_value_t env_store = wt_get_or_create_env_store(js);
  ant_value_t env_stringify_args[1] = {env_store};
  ant_value_t env_json = js_json_stringify(js, env_stringify_args, 1);
  if (vtype(env_json) != T_STR) {
    free(script_path);
    free(worker_data_heap);
    return js_mkerr(js, "setEnvironmentData values must be JSON-serializable");
  }
  size_t env_len = 0;
  const char *env_raw = js_getstr(js, env_json, &env_len);
  if (!env_raw) {
    free(script_path);
    free(worker_data_heap);
    return js_mkerr(js, "Failed to snapshot environment data");
  }
  env_store_heap = strndup(env_raw, env_len);
  if (!env_store_heap) {
    free(script_path);
    free(worker_data_heap);
    return js_mkerr(js, "Out of memory");
  }
  env_store_json = env_store_heap;

  ant_value_t this_obj = js_getthis(js);
  ant_worker_thread_t *wt = (ant_worker_thread_t *)calloc(1, sizeof(*wt));
  if (!wt) {
    free(script_path);
    free(worker_data_heap);
    free(env_store_heap);
    return js_mkerr(js, "Out of memory");
  }

  wt->js = js;
  wt->self_val = this_obj;
  js_set_slot(this_obj, SLOT_DATA, ANT_PTR(wt));

  wt->prev = NULL;
  wt->next = active_workers_head;
  if (active_workers_head) active_workers_head->prev = wt;
  active_workers_head = wt;

  int rc = wt_spawn_worker(wt, script_path, worker_data_json, env_store_json);
  free(script_path);
  free(worker_data_heap);
  free(env_store_heap);

  if (rc != 0) {
    js_set_slot(this_obj, SLOT_DATA, js_mkundef());
    wt_cleanup(wt);
    return js_mkerr(js, "Failed to spawn Worker: %s", uv_strerror(rc));
  }

  return this_obj;
}

static ant_value_t worker_threads_parent_post_message(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t value = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t stringify_args[1] = {value};
  ant_value_t json = js_json_stringify(js, stringify_args, 1);
  if (vtype(json) != T_STR) return js_mkerr(js, "parentPort.postMessage payload must be JSON-serializable");

  size_t json_len = 0;
  const char *json_str = js_getstr(js, json, &json_len);
  if (!json_str) return js_mkerr(js, "Failed to serialize message");

  size_t prefix_len = strlen(WT_MSG_PREFIX);
  size_t line_len = prefix_len + json_len + 1;
  char *line = (char *)malloc(line_len + 1);
  if (!line) return js_mkerr(js, "Out of memory");

  memcpy(line, WT_MSG_PREFIX, prefix_len);
  memcpy(line + prefix_len, json_str, json_len);
  line[prefix_len + json_len] = '\n';
  line[prefix_len + json_len + 1] = '\0';

  ssize_t wrote = WT_WRITE(WT_STDOUT_FD, line, (unsigned int)line_len);
  free(line);
  if (wrote < 0) return js_mkerr(js, "parentPort.postMessage failed");
  return js_mkundef();
}

static ant_value_t worker_threads_parent_unref(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t worker_threads_mark_as_untransferable(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  return args[0];
}

static ant_value_t worker_threads_receive_message_on_port(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !wt_is_message_port(js, args[0])) {
    return js_mkerr(js, "receiveMessageOnPort(port) requires a MessagePort");
  }
  ant_value_t msg = js_mkundef();
  if (!wt_port_queue_dequeue(js, args[0], &msg)) return js_mkundef();

  ant_value_t out = js_mkobj(js);
  js_set(js, out, "message", msg);
  return out;
}

static ant_value_t worker_threads_set_environment_data(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setEnvironmentData(key, value) requires 2 arguments");

  ant_value_t key_stringify_args[1] = {args[0]};
  ant_value_t key_json = js_json_stringify(js, key_stringify_args, 1);
  if (vtype(key_json) != T_STR) return js_mkerr(js, "setEnvironmentData key must be JSON-serializable");

  ant_value_t value_stringify_args[1] = {args[1]};
  ant_value_t value_json = js_json_stringify(js, value_stringify_args, 1);
  if (vtype(value_json) != T_STR) return js_mkerr(js, "setEnvironmentData value must be JSON-serializable");

  ant_value_t cloned = json_parse_value(js, value_json);
  if (is_err(cloned)) return js_mkerr(js, "setEnvironmentData value must be JSON-serializable");

  size_t key_len = 0;
  const char *key_ptr = js_getstr(js, key_json, &key_len);
  if (!key_ptr) return js_mkerr(js, "Failed to serialize environment data key");

  char *key = strndup(key_ptr, key_len);
  if (!key) return js_mkerr(js, "Out of memory");
  js_set(js, wt_get_or_create_env_store(js), key, cloned);
  free(key);
  return js_mkundef();
}

static ant_value_t worker_threads_get_environment_data(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();

  ant_value_t key_stringify_args[1] = {args[0]};
  ant_value_t key_json = js_json_stringify(js, key_stringify_args, 1);
  if (vtype(key_json) != T_STR) return js_mkundef();

  size_t key_len = 0;
  const char *key_ptr = js_getstr(js, key_json, &key_len);
  if (!key_ptr) return js_mkundef();

  char *key = strndup(key_ptr, key_len);
  if (!key) return js_mkerr(js, "Out of memory");

  ant_value_t store = wt_get_or_create_env_store(js);
  ant_offset_t off = lkp(js, store, key, key_len);
  if (off == 0) {
    free(key);
    return js_mkundef();
  }

  ant_value_t value = js_get(js, store, key);
  free(key);
  return value;
}

static ant_value_t worker_threads_move_message_port_to_context(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !wt_is_message_port(js, args[0])) {
    return js_mkerr(js, "moveMessagePortToContext(port, context) requires a MessagePort");
  }
  return args[0];
}

void gc_mark_worker_threads(ant_t *js, gc_mark_fn mark) {
  for (ant_worker_thread_t *wt = active_workers_head; wt; wt = wt->next) {
    mark(js, wt->self_val);
    if (wt->has_terminate_val) mark(js, wt->terminate_val);
  }
}

ant_value_t worker_threads_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  bool is_worker = wt_is_worker_mode();

  wt_init_env_store(js, is_worker);

  js_set(js, lib, "isMainThread", js_bool(!is_worker));
  js_set(js, lib, "threadId", js_mknum((double)(is_worker ? rt->pid : 0)));
  js_set(js, lib, "SHARE_ENV", js_mksym(js, "SHARE_ENV"));

  ant_value_t message_port_ctor_obj = js_mkobj(js);
  ant_value_t message_port_proto = js_mkobj(js);
  js_set(js, message_port_proto, "postMessage", js_mkfun(worker_threads_message_port_post_message));
  js_set(js, message_port_proto, "on", js_mkfun(worker_threads_message_port_on));
  js_set(js, message_port_proto, "once", js_mkfun(worker_threads_message_port_once));
  js_set(js, message_port_proto, "start", js_mkfun(worker_threads_message_port_start));
  js_set(js, message_port_proto, "close", js_mkfun(worker_threads_message_port_close));
  js_set(js, message_port_proto, "ref", js_mkfun(worker_threads_message_port_ref));
  js_set(js, message_port_proto, "unref", js_mkfun(worker_threads_message_port_unref));
  js_set_sym(js, message_port_proto, get_toStringTag_sym(), js_mkstr(js, "MessagePort", 11));

  js_set_slot(message_port_ctor_obj, SLOT_CFUNC, js_mkfun(worker_threads_message_port_ctor));
  js_mkprop_fast(js, message_port_ctor_obj, "prototype", 9, message_port_proto);
  js_mkprop_fast(js, message_port_ctor_obj, "name", 4, js_mkstr(js, "MessagePort", 11));
  js_set_descriptor(js, message_port_ctor_obj, "name", 4, 0);
  js_set(js, lib, "MessagePort", js_obj_to_func(message_port_ctor_obj));
  js_set_slot(js->global, SLOT_WT_PORT_PROTO, message_port_proto);

  ant_value_t message_channel_ctor_obj = js_mkobj(js);
  ant_value_t message_channel_proto = js_mkobj(js);
  js_set_sym(js, message_channel_proto, get_toStringTag_sym(), js_mkstr(js, "MessageChannel", 14));
  js_set_slot(message_channel_ctor_obj, SLOT_CFUNC, js_mkfun(worker_threads_message_channel_ctor));
  js_mkprop_fast(js, message_channel_ctor_obj, "prototype", 9, message_channel_proto);
  js_mkprop_fast(js, message_channel_ctor_obj, "name", 4, js_mkstr(js, "MessageChannel", 14));
  js_set_descriptor(js, message_channel_ctor_obj, "name", 4, 0);
  js_set(js, lib, "MessageChannel", js_obj_to_func(message_channel_ctor_obj));

  if (is_worker) {
    ant_value_t parent_port = js_mkobj(js);
    js_set(js, parent_port, "postMessage", js_mkfun(worker_threads_parent_post_message));
    js_set(js, parent_port, "unref", js_mkfun(worker_threads_parent_unref));
    js_set(js, parent_port, "ref", js_mkfun(worker_threads_parent_unref));
    js_set(js, lib, "parentPort", parent_port);

    const char *worker_data_json = getenv(WT_ENV_DATA_JSON);
    if (worker_data_json && worker_data_json[0]) {
      ant_value_t raw = js_mkstr(js, worker_data_json, strlen(worker_data_json));
      ant_value_t parsed = json_parse_value(js, raw);
      js_set(js, lib, "workerData", is_err(parsed) ? js_mkundef() : parsed);
    } else js_set(js, lib, "workerData", js_mkundef());
  } else {
    js_set(js, lib, "parentPort", js_mknull());
    js_set(js, lib, "workerData", js_mkundef());
  }

  ant_value_t worker_ctor_obj = js_mkobj(js);
  ant_value_t worker_proto = js_mkobj(js);

  js_set(js, worker_proto, "on", js_mkfun(worker_threads_worker_on));
  js_set(js, worker_proto, "once", js_mkfun(worker_threads_worker_once));
  js_set(js, worker_proto, "terminate", js_mkfun(worker_threads_worker_terminate));
  js_set(js, worker_proto, "unref", js_mkfun(worker_threads_worker_unref));
  js_set(js, worker_proto, "ref", js_mkfun(worker_threads_worker_ref));
  js_set(js, worker_proto, "postMessage", js_mkfun(worker_threads_worker_post_message));
  js_set_sym(js, worker_proto, get_toStringTag_sym(), js_mkstr(js, "Worker", 6));

  js_set_slot(worker_ctor_obj, SLOT_CFUNC, js_mkfun(worker_threads_worker_ctor));
  js_mkprop_fast(js, worker_ctor_obj, "prototype", 9, worker_proto);
  js_mkprop_fast(js, worker_ctor_obj, "name", 4, js_mkstr(js, "Worker", 6));
  js_set_descriptor(js, worker_ctor_obj, "name", 4, 0);
  js_set(js, lib, "Worker", js_obj_to_func(worker_ctor_obj));

  js_set(js, lib, "markAsUntransferable", js_mkfun(worker_threads_mark_as_untransferable));
  js_set(js, lib, "receiveMessageOnPort", js_mkfun(worker_threads_receive_message_on_port));
  js_set(js, lib, "setEnvironmentData", js_mkfun(worker_threads_set_environment_data));
  js_set(js, lib, "getEnvironmentData", js_mkfun(worker_threads_get_environment_data));
  js_set(js, lib, "moveMessagePortToContext", js_mkfun(worker_threads_move_message_port_to_context));

  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "worker_threads", 14));
  return lib;
}
