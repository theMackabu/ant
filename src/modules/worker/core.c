#include <compat.h> // IWYU pragma: keep

#include <libgen.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "arena.h"
#include "base64.h"
#include "bootstrap.h"
#include "errors.h"
#include "internal.h"
#include "interrupt.h"
#include "list.h"
#include "ptr.h"
#include "reactor.h"
#include "runtime.h"
#include "output.h"
#include "silver/call.h"

#include "esm/commonjs.h"
#include "esm/loader.h"
#include "esm/remote.h"
#include "modules/atomics.h"
#include "modules/blob.h"
#include "modules/process.h"
#include "modules/url.h"
#include "modules/worker_threads.h"

#include "worker_internal.h"

static constexpr uint32_t WORKER_NATIVE_TAG = Ant_NativeTag("WRKR");

static _Atomic uint64_t next_thread_id = 1;
static constexpr double WORKER_DEFAULT_STACK_MB = 4.0;
static constexpr double WORKER_DEFAULT_OLD_MB = 512.0;

const char *const worker_limit_names[WORKER_LIMIT_COUNT] = {
  "maxYoungGenerationSizeMb", 
  "maxOldGenerationSizeMb", 
  "codeRangeSizeMb", 
  "stackSizeMb",
};

ant_worker_t *worker_new(ant_t *js, ant_value_t obj, const worker_ops_t *ops) {
  ant_worker_t *w = calloc(1, sizeof(*w));
  if (!w) return NULL;

  w->lock = (ant_mutex_t)ANT_MUTEX_INIT;
  atomic_init(&w->refs, 1);

  w->ops = ops;
  w->obj = obj;
  w->parent = js;
  w->bundle = js_esm_bundle(js);
  w->exit_promise = js_mkundef();
  w->stdin_obj = js_mkundef();
  w->stdout_obj = js_mkundef();
  w->stderr_obj = js_mkundef();
  w->child_stdin = js_mkundef();
  w->pipe_stdout = true;
  w->pipe_stderr = true;

  for (int i = 0; i < WORKER_LIMIT_COUNT; i++) w->limits[i] = w->requested_limits[i] = -1;
  w->requested_limits[WORKER_LIMIT_STACK] = WORKER_DEFAULT_STACK_MB;
  w->limits[WORKER_LIMIT_OLD] = WORKER_DEFAULT_OLD_MB;
  w->limits[WORKER_LIMIT_STACK] = WORKER_DEFAULT_STACK_MB;

  return w;
}

ant_worker_t *worker_data(ant_value_t obj) {
  return js_get_native(obj, WORKER_NATIVE_TAG);
}

static void worker_free_events(worker_event_t *e) {
  for (worker_event_t *next; e; e = next) {
    next = e->next;
    ant_wire_release(e->wire);
    free(e);
  }
}

void worker_release(ant_worker_t *w) {
  if (atomic_fetch_sub_explicit(&w->refs, 1, memory_order_acq_rel) != 1) return;
  worker_free_events(w->events_head);
  worker_free_events(w->stdin_head);

  if (w->child_end) ant_port_end_release(w->child_end);
  ant_wire_release(w->worker_data);
  ant_wire_release(w->environment_data);

  for (int i = 0; i < w->argc; i++) free(w->argv[i]);
  free(w->argv);
  for (char **e = w->env; e && *e; e++) free(*e);

  free(w->env);
  free(w->entry);
  free(w->name);
  free(w);
}

void worker_post_event(ant_worker_t *w, worker_event_t *e) {
  ant_mutex_lock(&w->lock);
  if (w->events_tail) w->events_tail->next = e;
  else w->events_head = e;
  w->events_tail = e;
  if (w->parent_async_open) uv_async_send(&w->parent_async);
  ant_mutex_unlock(&w->lock);
}

void worker_post(ant_worker_t *w, worker_event_kind_t kind, int code, ant_wire_t *wire) {
  worker_event_t *e = calloc(1, sizeof(*e));
  if (!e) {
    ant_wire_release(wire);
    return;
  }
  e->kind = kind;
  e->code = code;
  e->wire = wire;
  worker_post_event(w, e);
}

static void worker_child_wake(uv_async_t *handle) {
  ant_worker_t *w = handle->data;
  ant_t *js = w->child;
  if (!js) return;

  if (!worker_stop_requested(w, NULL)) {
    worker_stdio_child_wake(js, w);
    return;
  }

  js->terminating = true;
  atomic_store_explicit(&js->interrupt_pending, 1, memory_order_relaxed);
  uv_stop(js->loop);
}

void worker_request_stop(ant_worker_t *w, int code) {
  ant_mutex_lock(&w->lock);
  if (!w->terminate_requested) {
    w->terminate_requested = true;
    w->requested_exit_code = code;
  }
  if (w->child) {
    atomic_store_explicit(&w->child->interrupt_pending, 1, memory_order_relaxed);
    atomics_wake_isolate(w->child);
    if (w->child_async_open) uv_async_send(&w->child_async);
  }
  ant_mutex_unlock(&w->lock);
}

bool worker_stop_requested(ant_worker_t *w, int *code) {
  ant_mutex_lock(&w->lock);
  bool requested = w->terminate_requested;
  if (requested && code) *code = w->requested_exit_code;
  ant_mutex_unlock(&w->lock);
  return requested;
}

void worker_set_ref(ant_worker_t *w, bool refed) {
  if (!w->parent_async_open) return;
  w->refed = refed;
  if (refed) uv_ref((uv_handle_t *)&w->parent_async);
  else uv_unref((uv_handle_t *)&w->parent_async);
}

void worker_flush_online(ant_t *js, ant_worker_t *w) {
  for (;;) {
    ant_mutex_lock(&w->lock);
    worker_event_t *e = w->events_head;
    bool online = e && e->kind == WORKER_EVENT_ONLINE;
    if (online) {
      w->events_head = e->next;
      if (!w->events_head) w->events_tail = NULL;
    }
    ant_mutex_unlock(&w->lock);
    if (!online) return;
    free(e);
    w->ops->online(js, w);
  }
}

static ant_wire_t *worker_serialize_error(ant_t *js, ant_value_t error) {
  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize_error(js, error, &wire);
  if (!is_err(r)) return wire;

  js_take_thrown(js, r);
  ant_value_t text = js_tostring_val(js, error);
  if (is_err(text)) {
    js_take_thrown(js, text);
    text = js_mkstr(js, "Uncaught exception in worker", 28);
  }

  r = ant_wire_serialize(js, text, js_mkundef(), js_mkundef(), &wire);
  if (is_err(r)) {
    js_take_thrown(js, r);
    return NULL;
  }

  return wire;
}

bool ant_worker_report_uncaught(ant_t *js, ant_value_t reason) {
  ant_worker_t *w = js->worker;
  if (!w) return false;
  if (worker_stop_requested(w, NULL)) return true;

  bool oom = is_err(js->exception_oom) && reason == Ant_Exception_Value(js, js->exception_oom);
  ant_wire_t *wire = NULL;
  if (!oom) {
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, reason);
    wire = worker_serialize_error(js, reason);
    GC_ROOT_RESTORE(js, mark);
  }

  worker_post(w, WORKER_EVENT_ERROR, oom ? WORKER_ERROR_OOM : WORKER_ERROR_THROWN, wire);
  worker_request_stop(w, 1);
  js->terminating = true;
  return true;
}

ant_value_t ant_worker_exit(ant_t *js, int code) {
  ant_worker_t *w = js->worker;
  if (w) worker_request_stop(w, code);
  return js_interrupt_raise(js);
}

bool ant_worker_is_worker(ant_t *js) {
  return js->worker != NULL;
}

uint64_t ant_worker_thread_id(ant_t *js) {
  return js->worker ? js->worker->thread_id : 0;
}

ant_value_t worker_parent_port(ant_t *js) {
  ant_worker_t *w = js->worker;
  if (is_object_type(js->mutable_roots.worker_parent_port)) return js->mutable_roots.worker_parent_port;
  if (!w || !w->child_end) return js_mknull();

  ant_port_end_t *end = w->child_end;
  w->child_end = NULL;

  ant_value_t port = ant_message_port_adopt(js, end);
  if (is_err(port)) return js_take_thrown(js, port);
  js->mutable_roots.worker_parent_port = port;

  return port;
}

static ant_value_t worker_run_entry(ant_t *js, ant_worker_t *w) {
  if (w->is_eval) {
    const char *tag = "[worker eval]";
    js_set_filename(js, tag);
    js_setup_import_meta(js, tag);
    ant_value_t ns = Ant_Object_CreateBare(js);
    if (is_err(ns)) return ns;
    return esm_load_commonjs_module(js, tag, w->entry, strlen(w->entry), ns, js_mkundef());
  }

  ant_value_t specifier;
  if (strncmp(w->entry, "data:", 5) == 0) {
    js_set_global_builtin(js, "__dirname", js_mkundef());
    specifier = js_mkstr(js, w->entry, strlen(w->entry));
    js_set_filename(js, w->entry);
    js_setup_import_meta(js, w->entry);
  } else {
    char *copy = strdup(w->entry);
    if (copy) {
      char *dir = dirname(copy);
      js_set_global_builtin(js, "__dirname", js_mkstr(js, dir, strlen(dir)));
      free(copy);
    }

    const char *interned = intern_string(js, w->entry, strlen(w->entry));
    js_set_filename(js, interned ? interned : w->entry);
    js_setup_import_meta(js, interned ? interned : w->entry);
    specifier = js_esm_make_file_url(js, w->entry);
  }

  js_set_global_builtin(js, "__filename", js_mkstr(js, w->entry, strlen(w->entry)));
  return js_esm_import_sync(js, specifier);
}

static void worker_report_pending(ant_t *js) {
  if (!Ant_Exception_Pending(js)) return;
  ant_value_t thrown = js_take_thrown(js, js_mkundef());
  if (js_is_termination(thrown)) return;
  ant_worker_report_uncaught(js, thrown);
}

static void worker_child_attach(ant_t *js, ant_worker_t *w) {
  if (uv_async_init(js->loop, &w->child_async, worker_child_wake) != 0) return;
  w->child_async.data = w;
  uv_unref((uv_handle_t *)&w->child_async);

  ant_mutex_lock(&w->lock);
  w->child = js;
  w->child_async_open = true;
  if (w->terminate_requested) atomic_store_explicit(&js->interrupt_pending, 1, memory_order_relaxed);
  ant_mutex_unlock(&w->lock);
}

static void worker_child_detach(ant_worker_t *w) {
  ant_mutex_lock(&w->lock);
  w->child = NULL;
  bool close_async = w->child_async_open;
  w->child_async_open = false;
  ant_mutex_unlock(&w->lock);
  if (close_async) uv_close((uv_handle_t *)&w->child_async, NULL);
}

static void worker_child_boot(ant_t *js, ant_worker_t *w) {
  ant_output_set_thread_writer(worker_stdio_writer, w);
  ant_runtime_init(js, w->argc, w->argv, NULL);

  if (!w->share_env) process_set_private_env(js, w->env);
  ant_bootstrap_modules(js);
  worker_stdio_child_init(js, w);

  if (w->bundle && !js_esm_bundle_activate(js, w->bundle)) {
    worker_post(w, WORKER_EVENT_ERROR, WORKER_ERROR_INIT, NULL);
    worker_request_stop(w, 1);
  }

  if (w->ops->child_init) w->ops->child_init(js, w);
}

static int worker_child_run(ant_t *js, ant_worker_t *w) {
  int code = 0;
  if (worker_stop_requested(w, &code)) return 0;

  worker_post(w, WORKER_EVENT_ONLINE, 0, NULL);
  ant_value_t result = worker_run_entry(js, w);
  if (is_err(result) && !Ant_Exception_Pending(js)) ant_worker_report_uncaught(js, Ant_Exception_Value(js, result));
  worker_report_pending(js);

  if (!worker_stop_requested(w, NULL)) {
    atomic_store_explicit(&w->loop_start_ns, uv_hrtime(), memory_order_release);
    js_run_event_loop(js);
    worker_report_pending(js);
  }

  if (worker_stop_requested(w, &code)) return code;
  code = process_run_exit_handlers(js, process_exit_code(js));
  worker_stop_requested(w, &code);

  return code;
}

static void worker_thread_main(void *arg) {
  ant_worker_t *w = arg;
  volatile char stack_base;

  ant_t *js = ant_create(&(ant_isolate_options_t){
    .object_arena_bytes = (size_t)(w->limits[WORKER_LIMIT_OLD] * 1024 * 1024),
  });

  if (!js) {
    worker_post(w, WORKER_EVENT_ERROR, WORKER_ERROR_INIT, NULL);
    worker_post(w, WORKER_EVENT_EXIT, 1, NULL);
    worker_release(w);
    return;
  }

  js->worker = w;
  js->interrupts_armed = true;
  uv_loop_configure(js->loop, UV_METRICS_IDLE_TIME);
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, (size_t)(w->limits[WORKER_LIMIT_STACK] * 1024 * 1024) * 3 / 4);

  worker_child_attach(js, w);
  worker_child_boot(js, w);
  int exit_code = worker_child_run(js, w);
  worker_child_detach(w);

  js->terminating = false;
  atomic_store_explicit(&js->interrupt_pending, 0, memory_order_relaxed);
  js_destroy(js);
  ant_output_set_thread_writer(NULL, NULL);
  ant_thread_cleanup();

  worker_post(w, WORKER_EVENT_EXIT, exit_code, NULL);
  worker_release(w);
}

static void worker_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_worker_t *w = worker_data(value);
  if (!w) return;
  js_clear_native(value, WORKER_NATIVE_TAG);
  w->obj = js_mkundef();
  if (w->binding) {
    ant_port_binding_free(w->binding);
    w->binding = NULL;
  }
  worker_release(w);
}

static void worker_parent_async_closed(uv_handle_t *handle) {
  worker_release(handle->data);
}

static bool worker_close_parent_async(ant_worker_t *w) {
  ant_mutex_lock(&w->lock);
  bool close_async = w->parent_async_open;
  w->parent_async_open = false;
  ant_mutex_unlock(&w->lock);
  if (close_async) uv_close((uv_handle_t *)&w->parent_async, worker_parent_async_closed);
  return close_async;
}

static void worker_join(ant_worker_t *w) {
  if (!w->thread_started) return;
  uv_thread_join(&w->thread);
  w->thread_started = false;
}

static void worker_finish(ant_t *js, ant_worker_t *w, int code) {
  if (w->exited) return;
  w->exited = true;
  w->exit_code = code;

  if (w->binding) {
    ant_port_binding_drain(w->binding);
    ant_port_binding_free(w->binding);
    w->binding = NULL;
  }

  worker_join(w);
  worker_close_parent_async(w);

  GC_ROOT_SAVE(mark, js);
  ant_value_t obj = w->obj;
  ant_value_t promise = w->exit_promise;
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, promise);
  Ant_List_Remove(w);

  worker_stdio_end(js, w);
  w->ops->exit(js, w, code);

  if (vtype(promise) == kTypePromise) js_resolve_promise(js, promise, js_mknum(code));
  w->exit_promise = js_mkundef();
  GC_ROOT_RESTORE(js, mark);
}

ant_value_t worker_error_value(ant_t *js, worker_event_t *e) {
  if (e->wire) return ant_wire_deserialize(js, e->wire, NULL);
  if (e->code == WORKER_ERROR_OOM) return js_mkerr_code(
    js, JS_ERR_GENERIC, "ERR_WORKER_OUT_OF_MEMORY",
    "Worker terminated due to reaching memory limit: JS heap out of memory"
  );
  return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_WORKER_INIT_FAILED", "Worker initialization failure: out of memory");
}

static void worker_dispatch_error(ant_t *js, ant_worker_t *w, worker_event_t *e) {
  ant_value_t error = worker_error_value(js, e);
  if (is_err(error)) error = js_take_thrown(js, error);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, error);
  w->ops->error(js, w, error);
  GC_ROOT_RESTORE(js, mark);
}

static void worker_parent_wake(uv_async_t *handle) {
  ant_worker_t *w = handle->data;
  ant_t *js = w->parent;

  for (;;) {
    ant_mutex_lock(&w->lock);
    worker_event_t *e = w->events_head;
    if (e) {
      w->events_head = e->next;
      if (!w->events_head) w->events_tail = NULL;
      e->next = NULL;
    }
    ant_mutex_unlock(&w->lock);
    if (!e) break;

    if (e->kind == WORKER_EVENT_EXIT) {
      int code = e->code;
      worker_free_events(e);
      worker_finish(js, w, code);
      return;
    }

    switch (e->kind) {
      case WORKER_EVENT_ONLINE: w->ops->online(js, w); break;
      case WORKER_EVENT_STDOUT:
      case WORKER_EVENT_STDERR: worker_stdio_forward(js, w, e); break;
      case WORKER_EVENT_ERROR: worker_dispatch_error(js, w, e); break;
      default: break;
    }

    worker_free_events(e);
  }
}

static char *worker_blob_entry(ant_t *js, const char *url, ant_value_t *error) {
  ant_value_t blob = url_resolve_object_url(js, url);
  blob_data_t *data = blob_is_blob(js, blob) ? blob_get_data(blob) : NULL;
  if (!data) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot find blob URL \"%s\"", url);
    return NULL;
  }

  size_t encoded_len = 0;
  char *encoded = ant_base64_encode(data->data, data->size, &encoded_len);
  if (!encoded) return NULL;

  static const char prefix[] = "data:text/javascript;base64,";
  char *entry = malloc(sizeof(prefix) + encoded_len);
  if (entry) {
    memcpy(entry, prefix, sizeof(prefix) - 1);
    memcpy(entry + sizeof(prefix) - 1, encoded, encoded_len + 1);
  }

  free(encoded);
  return entry;
}

static char *worker_bundle_entry(ant_t *js, const char *path, bool relative) {
  const ant_bundle_t *vfs = js_esm_bundle(js);
  if (!vfs) return NULL;
  if (!relative) return ant_bundle_has_key(vfs, path) ? strdup(path) : NULL;

  const char *key = NULL;
  const char *parent = js_module_eval_active_filename(js);
  const ant_bundle_module_t *entry = ant_bundle_entry(vfs);

  if (parent) key = ant_bundle_resolve(vfs, parent, path, false);
  if (!key && entry) key = ant_bundle_resolve(vfs, entry->key, path, false);

  return key ? strdup(key) : NULL;
}

static char *worker_entry_text(ant_t *js, ant_value_t spec, ant_value_t *error) {
  ant_value_t href = spec;
  if (is_object_type(spec)) href = js_get(js, spec, "href");

  if (vtype(href) != kTypeString) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "The \"filename\" argument must be of type string or an instance of URL");
    return NULL;
  }

  size_t len = 0;
  const char *str = js_getstr(js, href, &len);

  return strndup(str, len);
}

char *worker_resolve_entry(ant_t *js, ant_value_t spec, worker_entry_mode_t mode, ant_value_t *error) {
  *error = js_mkundef();
  char *path = worker_entry_text(js, spec, error);
  if (!path) return NULL;
  if (strncmp(path, "data:", 5) == 0) return path;

  if (mode == WORKER_ENTRY_WEB && strncmp(path, "blob:", 5) == 0) {
    char *entry = worker_blob_entry(js, path, error);
    free(path);
    return entry;
  }

  if (strncmp(path, "file:", 5) == 0) {
    char *file = esm_file_url_to_path(js, path);
    free(path);
    if (!file) *error = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid file URL");
    return file;
  }

  if (esm_is_url(path)) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "The URL must be of scheme file or data. Received \"%s\"", path);
    free(path);
    return NULL;
  }

  bool absolute = path[0] == '/';
  bool relative = strncmp(path, "./", 2) == 0 || strncmp(path, "../", 3) == 0;
  if (!absolute && !relative && mode == WORKER_ENTRY_NODE) {
    *error = js_mkerr_typed(
      js, JS_ERR_TYPE,
      "The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Received \"%s\"", path
    );
    free(path);
    return NULL;
  }

  if (!absolute && !relative) {
    char *dotted = malloc(strlen(path) + 3);
    if (!dotted) {
      free(path);
      return NULL;
    }
    memcpy(dotted, "./", 2);
    strcpy(dotted + 2, path);
    free(path);
    path = dotted;
  }

  char *bundled = worker_bundle_entry(js, path, !absolute);
  if (bundled) {
    free(path);
    return bundled;
  }

  char *resolved = realpath(path, NULL);
  if (!resolved) return path;
  free(path);

  return resolved;
}

static char **worker_env_from_object(ant_t *js, ant_value_t env_obj) {
  ant_value_t keys = js_own_property_keys(js, env_obj, false, true);
  if (is_err(keys) || vtype(keys) != kTypeArray) return NULL;

  ant_offset_t len = js_arr_len(js, keys);
  char **env = calloc((size_t)len + 1, sizeof(char *));
  if (!env) return NULL;

  size_t out = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key = js_arr_get(js, keys, i);
    if (vtype(key) != kTypeString) continue;
    ant_value_t value = js_tostring_val(js, js_getprop_fallback(js, env_obj, js_getstr(js, key, NULL)));

    if (is_err(value)) {
      js_take_thrown(js, value);
      continue;
    }

    size_t klen = 0, vlen = 0;
    const char *k = js_getstr(js, key, &klen);
    const char *v = js_getstr(js, value, &vlen);

    char *entry = malloc(klen + vlen + 2);
    if (!entry) continue;

    memcpy(entry, k, klen);
    entry[klen] = '=';
    memcpy(entry + klen + 1, v, vlen);
    entry[klen + vlen + 1] = '\0';
    env[out++] = entry;
  }

  return env;
}

ant_value_t worker_environment_map(ant_t *js) {
  if (!is_object_type(js->mutable_roots.worker_environment_data)) {
    ant_value_t ctor = js_get(js, js->global, "Map");
    ant_value_t map = sv_vm_call(js->vm, js, ctor, js_mkundef(), NULL, 0, NULL, ctor);
    if (is_err(map)) return map;
    js->mutable_roots.worker_environment_data = map;
  }
  return js->mutable_roots.worker_environment_data;
}

static ant_value_t worker_read_limits(ant_t *js, ant_worker_t *w, ant_value_t limits) {
  for (int i = 0; i < WORKER_LIMIT_COUNT && is_object_type(limits); i++) {
    ant_value_t v = js_get(js, limits, worker_limit_names[i]);
    if (vtype(v) == kTypeUndefined) continue;
    if (vtype(v) != kTypeNumber) return js_mkerr_typed(js, JS_ERR_TYPE, "The \"options.resourceLimits.%s\" property must be of type number", worker_limit_names[i]);
    if (js_getnum(v) > 0) w->limits[i] = w->requested_limits[i] = js_getnum(v);
  }

  double old_min = (double)(ANT_ARENA_MIN >> 20), old_max = (double)(ANT_ARENA_MAX >> 20);
  if (w->limits[WORKER_LIMIT_OLD] < old_min) w->limits[WORKER_LIMIT_OLD] = old_min;
  if (w->limits[WORKER_LIMIT_OLD] > old_max) w->limits[WORKER_LIMIT_OLD] = old_max;

  return js_mkundef();
}

ant_value_t worker_limits_object(ant_t *js, const double limits[WORKER_LIMIT_COUNT], double unset) {
  ant_value_t obj = Ant_Object_Create(js);
  for (int i = 0; i < WORKER_LIMIT_COUNT; i++)
    js_set(js, obj, worker_limit_names[i], js_mknum(limits[i] > 0 ? limits[i] : unset));
  return obj;
}

static ant_value_t worker_build_argv(ant_t *js, ant_worker_t *w, ant_value_t argv_opt) {
  ant_offset_t extra = vtype(argv_opt) == kTypeArray ? js_arr_len(js, argv_opt) : 0;
  w->argv = calloc((size_t)extra + 3, sizeof(char *));
  if (!w->argv) return Ant_Error_CreateOutOfMemory(js);

  const char *exe = js->runtime.argc > 0 && js->runtime.argv ? js->runtime.argv[0] : "ant";
  w->argv[w->argc++] = strdup(exe);
  w->argv[w->argc++] = strdup(w->is_eval ? "[worker eval]" : w->entry);

  for (ant_offset_t i = 0; i < extra; i++) {
    ant_value_t s = js_tostring_val(js, js_arr_get(js, argv_opt, i));
    if (is_err(s)) return s;
    size_t len = 0;
    const char *str = js_getstr(js, s, &len);
    w->argv[w->argc++] = strndup(str, len);
  }

  return js_mkundef();
}

static ant_value_t worker_option(ant_t *js, ant_value_t options, const char *name) {
  return is_object_type(options) ? js_get(js, options, name) : js_mkundef();
}

ant_value_t worker_read_options(ant_t *js, ant_worker_t *w, ant_value_t options) {
  ant_value_t r = worker_read_limits(js, w, worker_option(js, options, "resourceLimits"));
  if (!is_err(r)) r = worker_build_argv(js, w, worker_option(js, options, "argv"));
  if (is_err(r)) return r;

  ant_value_t env = worker_option(js, options, "env");
  if (vtype(env) == kTypeSymbol && env == js->builtins.worker_share_env) w->share_env = true;
  else if (is_object_type(env)) w->env = worker_env_from_object(js, env);
  else w->env = process_env_snapshot(js);

  ant_value_t name = worker_option(js, options, "name");
  if (vtype(name) == kTypeString) {
    size_t len = 0;
    const char *s = js_getstr(js, name, &len);
    w->name = strndup(s, len);
  }

  ant_value_t data = worker_option(js, options, "workerData");
  ant_value_t transfer = worker_option(js, options, "transferList");
  r = ant_wire_serialize(js, data, transfer, js_mkundef(), &w->worker_data);
  if (is_err(r)) return r;

  ant_value_t map = worker_environment_map(js);
  return is_err(map) ? map : ant_wire_serialize(js, map, js_mkundef(), js_mkundef(), &w->environment_data);
}

ant_value_t worker_transfer_arg(ant_native_params_t) {
  if (nargs < 2) return js_mkundef();
  ant_value_t opt = args[1];
  if (!is_object_type(opt) || vtype(opt) == kTypeArray) return opt;
  ant_value_t transfer = js_get(js, opt, "transfer");
  return vtype(transfer) == kTypeUndefined ? js_get(js, opt, "transferList") : transfer;
}

static ant_value_t worker_bind_parent(ant_t *js, ant_worker_t *w) {
  ant_port_end_t *parent_end = NULL;
  if (!ant_port_end_pair(&parent_end, &w->child_end)) return Ant_Error_CreateOutOfMemory(js);

  w->binding = ant_port_end_bind(js, parent_end, w->obj, w->ops->on_message);
  if (!w->binding) {
    ant_port_end_release(parent_end);
    return Ant_Error_CreateOutOfMemory(js);
  }

  if (uv_async_init(js->loop, &w->parent_async, worker_parent_wake) != 0) {
    ant_port_binding_free(w->binding);
    w->binding = NULL;
    return js_mkerr(js, "failed to initialize Worker");
  }

  w->parent_async.data = w;
  w->parent_async_open = true;
  w->refed = true;
  atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);

  return js_mkundef();
}

ant_value_t worker_start(ant_t *js, ant_worker_t *w) {
  ant_value_t r = worker_bind_parent(js, w);
  if (is_err(r)) return r;

  w->thread_id = atomic_fetch_add_explicit(&next_thread_id, 1, memory_order_relaxed);
  js_set_native(w->obj, w, WORKER_NATIVE_TAG);
  js_set_finalizer(w->obj, worker_finalize);
  Ant_List_Insert(&js->workers, w);

  uv_thread_options_t opts = {
    .flags = UV_THREAD_HAS_STACK_SIZE,
    .stack_size = (size_t)(w->limits[WORKER_LIMIT_STACK] * 1024 * 1024),
  };

  atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);
  if (uv_thread_create_ex(&w->thread, &opts, worker_thread_main, w) != 0) {
    atomic_fetch_sub_explicit(&w->refs, 1, memory_order_relaxed);
    worker_post(w, WORKER_EVENT_EXIT, 1, NULL);
    return js_mkundef();
  }

  w->thread_started = true;
  return js_mkundef();
}

void cleanup_worker_threads_module(ant_t *js) {
  while (js->workers) {
    ant_worker_t *w = js->workers;
    Ant_List_Remove(w);
    worker_request_stop(w, 1);

    ant_mutex_lock(&w->lock);
    bool close_async = w->parent_async_open;
    w->parent_async_open = false;
    ant_mutex_unlock(&w->lock);

    worker_join(w);
    w->exited = true;
    w->exit_promise = js_mkundef();

    if (w->binding) {
      ant_port_binding_free(w->binding);
      w->binding = NULL;
    }

    if (close_async) uv_close((uv_handle_t *)&w->parent_async, worker_parent_async_closed);
  }

  worker_messaging_close(js);
}

void gc_mark_worker_threads(ant_t *js, gc_mark_fn mark) {
  for (ant_worker_t *w = js->workers; w; w = w->next) {
    if (is_object_type(w->obj)) mark(js, w->obj);
    if (vtype(w->exit_promise) == kTypePromise) mark(js, w->exit_promise);
    if (is_object_type(w->stdout_obj)) mark(js, w->stdout_obj);
    if (is_object_type(w->stderr_obj)) mark(js, w->stderr_obj);
    if (is_object_type(w->stdin_obj)) mark(js, w->stdin_obj);
  }

  if (js->worker && is_object_type(js->worker->child_stdin)) mark(js, js->worker->child_stdin);
  worker_messaging_gc_mark(js, mark);
}
