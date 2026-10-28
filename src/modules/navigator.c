#include <compat.h> // IWYU pragma: keep

#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <uthash.h>
#include <uv.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "sync.h"
#include "silver/call.h"
#include "modules/abort.h"
#include "modules/domexception.h"
#include "modules/navigator.h"
#include "modules/timer.h"
#include "modules/worker_threads.h"
#include "gc/modules.h"

typedef enum {
  LOCK_MODE_EXCLUSIVE,
  LOCK_MODE_SHARED
} lock_mode_t;

typedef enum {
  LOCK_PENDING,
  LOCK_HELD,
  LOCK_STOLEN,
  LOCK_RELEASED,
} lock_state_t;

typedef enum {
  LOCK_REGISTER_OOM,
  LOCK_REGISTER_UNAVAILABLE,
  LOCK_REGISTER_OK,
} lock_register_t;

typedef struct lock_resource lock_resource_t;

typedef struct lock_request {
  struct lock_request *link;
  struct lock_request *notify_next;
  struct lock_request *next;
  struct lock_request *prev;

  lock_resource_t *resource;
  navigator_locks_t *client;

  char *name;
  size_t name_len;
  uint64_t id;
  lock_mode_t mode;
  _Atomic lock_state_t state;
  bool notify_queued;
  bool dispatched;

  ant_value_t callback;
  ant_value_t promise;
  ant_value_t signal;
  ant_value_t abort_fn;
} lock_request_t;

struct lock_resource {
  char *name;
  size_t name_len;
  lock_request_t *held;
  lock_request_t *queue;
  UT_hash_handle hh;
};

struct navigator_locks {
  ant_mutex_t lock;
  ant_t *js;
  bool closed;
  uv_async_t async;
  lock_request_t *notify_head;
  lock_request_t *notify_tail;
  lock_request_t *requests;
  lock_request_t *requests_tail;
  uint64_t next_id;
  char client_id[48];
};

typedef struct {
  lock_mode_t mode;
  bool if_available;
  bool steal;
  ant_value_t signal;
} lock_options_t;

static ant_mutex_t lock_registry = ANT_MUTEX_INIT;
static lock_resource_t *lock_resources = NULL;

static void locks_drain(ant_t *js, navigator_locks_t *client);

static ant_value_t lock_mode_value(ant_t *js, lock_mode_t mode) {
  return mode == LOCK_MODE_EXCLUSIVE ? js_mkstr(js, "exclusive", 9) : js_mkstr(js, "shared", 6);
}

static lock_state_t lock_state(const lock_request_t *req) {
  return atomic_load_explicit(&req->state, memory_order_acquire);
}

static void lock_set_state(lock_request_t *req, lock_state_t state) {
  atomic_store_explicit(&req->state, state, memory_order_release);
}

static char *lock_copy_name(const char *name, size_t len) {
  char *copy = malloc(len + 1);
  if (!copy) return NULL;
  
  memcpy(copy, name, len);
  copy[len] = '\0';
  
  return copy;
}

static void lock_list_append(navigator_locks_t *client, lock_request_t *req) {
  req->next = NULL;
  req->prev = client->requests_tail;
  if (client->requests_tail) client->requests_tail->next = req;
  else client->requests = req;
  client->requests_tail = req;
}

static void lock_list_remove(navigator_locks_t *client, lock_request_t *req) {
  if (req->prev) req->prev->next = req->next;
  else client->requests = req->next;
  if (req->next) req->next->prev = req->prev;
  else client->requests_tail = req->prev;
  req->next = req->prev = NULL;
}

static lock_request_t *lock_find(navigator_locks_t *client, ant_value_t id) {
  if (!client || vtype(id) != kTypeNumber) return NULL;
  uint64_t want = (uint64_t)js_getnum(id);
  for (lock_request_t *req = client->requests; req; req = req->next)
    if (req->id == want) return req;
  return NULL;
}

static void locks_async_cb(uv_async_t *handle) {
  navigator_locks_t *client = handle->data;
  if (client->closed || client->js->destroying) return;
  locks_drain(client->js, client);
  process_microtasks(client->js);
}

static navigator_locks_t *locks_client(ant_t *js) {
  if (js->navigator_locks) return js->navigator_locks;
  navigator_locks_t *client = calloc(1, sizeof(*client));

  if (!client) return NULL;
  if (uv_async_init(js->loop, &client->async, locks_async_cb) != 0) {
    free(client);
    return NULL;
  }

  client->async.data = client;
  uv_unref((uv_handle_t *)&client->async);

  client->lock = (ant_mutex_t)ANT_MUTEX_INIT;
  client->js = js;

  snprintf(
    client->client_id, sizeof(client->client_id),
    "ant-%d-%llu", (int)getpid(), (unsigned long long)ant_worker_thread_id(js)
  );

  js->navigator_locks = client;
  return client;
}

static lock_resource_t *lock_resource_get(const char *name, size_t len) {
  lock_resource_t *r = NULL;
  HASH_FIND(hh, lock_resources, name, len, r);
  if (r) return r;

  r = calloc(1, sizeof(*r));
  if (!r) return NULL;

  r->name = lock_copy_name(name, len);
  if (!r->name) {
    free(r);
    return NULL;
  }

  r->name_len = len;
  HASH_ADD_KEYPTR(hh, lock_resources, r->name, r->name_len, r);

  return r;
}

static void lock_resource_maybe_free(lock_resource_t *r) {
  if (r->held || r->queue) return;
  HASH_DELETE(hh, lock_resources, r);
  free(r->name);
  free(r);
}

static void lock_unlink(lock_request_t **list, lock_request_t *req) {
  for (lock_request_t **p = list; *p; p = &(*p)->link) if (*p == req) {
    *p = req->link;
    req->link = NULL;
    return;
  }
}

static bool lock_grantable(lock_resource_t *r, lock_request_t *req) {
  if (r->queue && r->queue != req) return false;
  for (lock_request_t *h = r->held; h; h = h->link)
    if (req->mode == LOCK_MODE_EXCLUSIVE || h->mode == LOCK_MODE_EXCLUSIVE) return false;
  return true;
}

static void lock_notify(lock_request_t *req) {
  navigator_locks_t *client = req->client;
  ant_mutex_lock(&client->lock);

  if (!client->closed && !req->notify_queued) {
    req->notify_queued = true;
    req->notify_next = NULL;
    if (client->notify_tail) client->notify_tail->notify_next = req;
    else client->notify_head = req;
    client->notify_tail = req;
    uv_async_send(&client->async);
  }

  ant_mutex_unlock(&client->lock);
}

static void lock_grant(lock_resource_t *r, lock_request_t *req) {
  req->link = r->held;
  r->held = req;
  lock_set_state(req, LOCK_HELD);
  lock_notify(req);
}

static void lock_resource_process(lock_resource_t *r) {
  while (r->queue && lock_grantable(r, r->queue)) {
    lock_request_t *req = r->queue;
    r->queue = req->link;
    lock_grant(r, req);
  }
}

static void lock_steal_held(lock_resource_t *r) {
  while (r->held) {
    lock_request_t *h = r->held;
    r->held = h->link;
    h->link = NULL;
    h->resource = NULL;
    lock_set_state(h, LOCK_STOLEN);
    lock_notify(h);
  }
}

static void lock_detach(lock_request_t *req, lock_state_t state) {
  lock_resource_t *r = req->resource;
  if (!r) return;

  lock_unlink(lock_state(req) == LOCK_HELD ? &r->held : &r->queue, req);
  req->resource = NULL;
  lock_set_state(req, state);

  lock_resource_process(r);
  lock_resource_maybe_free(r);
}

static lock_register_t lock_register(lock_request_t *req, const lock_options_t *opts) {
  ant_mutex_lock(&lock_registry);
  lock_resource_t *r = lock_resource_get(req->name, req->name_len);
  lock_register_t result = LOCK_REGISTER_OK;

  if (!r) result = LOCK_REGISTER_OOM;
  else if (opts->if_available && !lock_grantable(r, req)) {
    result = LOCK_REGISTER_UNAVAILABLE;
    lock_resource_maybe_free(r);
  } else {
    req->resource = r;
    
    if (opts->steal) lock_steal_held(r);
    if (opts->steal || opts->if_available) lock_grant(r, req);
    
    else {
      lock_request_t **tail = &r->queue;
      while (*tail) tail = &(*tail)->link;
      *tail = req;
      lock_resource_process(r);
    }
  }

  ant_mutex_unlock(&lock_registry);
  return result;
}

static void lock_request_free(ant_t *js, lock_request_t *req) {
  navigator_locks_t *client = req->client;
  ant_mutex_lock(&client->lock);

  if (req->notify_queued) {
    lock_request_t *prev = NULL;
    for (lock_request_t *n = client->notify_head; n; prev = n, n = n->notify_next) if (n == req) {
      if (prev) prev->notify_next = n->notify_next;
      else client->notify_head = n->notify_next;
      if (client->notify_tail == n) client->notify_tail = prev;
      break;
    }
  }

  ant_mutex_unlock(&client->lock);
  if (is_object_type(req->signal) && is_callable(req->abort_fn))
    abort_signal_remove_listener(js, req->signal, req->abort_fn);

  lock_list_remove(client, req);
  free(req->name);
  free(req);
}

static void lock_finish(ant_t *js, lock_request_t *req) {
  ant_mutex_lock(&lock_registry);
  lock_detach(req, LOCK_RELEASED);
  ant_mutex_unlock(&lock_registry);
  lock_request_free(js, req);
}

static void lock_settle_handler(ant_t *js, ant_value_t value, bool fulfilled) {
  ant_value_t data = js_func_data(js->current_func);
  ant_value_t outer_promise = js_get(js, data, "outerPromise");
  navigator_locks_t *client = js->navigator_locks;

  lock_request_t *req = lock_find(client, js_get(js, data, "lockId"));
  if (req) lock_finish(js, req);
  if (fulfilled) js_resolve_promise(js, outer_promise, value);
  else js_reject_promise(js, outer_promise, value);

  if (client) locks_drain(js, client);
}

static ant_value_t lock_then_handler(ant_params_t) {
  lock_settle_handler(js, nargs > 0 ? args[0] : js_mkundef(), true);
  return js_mkundef();
}

static ant_value_t lock_catch_handler(ant_params_t) {
  lock_settle_handler(js, nargs > 0 ? args[0] : js_mkundef(), false);
  return js_mkundef();
}

static void lock_reject_callback(ant_t *js, lock_request_t *req, ant_value_t promise, ant_value_t result) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t reason = js_take_thrown(js, result);

  GC_ROOT_PIN(js, reason);
  GC_ROOT_PIN(js, promise);
  if (req) lock_finish(js, req);

  js_reject_promise(js, promise, reason);
  GC_ROOT_RESTORE(js, root_mark);
}

static void lock_await_callback(ant_t *js, lock_request_t *req, ant_value_t result, ant_value_t outer_promise) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  GC_ROOT_PIN(js, outer_promise);

  ant_value_t data = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, data);
  js_set(js, data, "lockId", req ? js_mknum((double)req->id) : js_mkundef());
  js_set(js, data, "outerPromise", outer_promise);

  ant_value_t on_resolve = js_heavy_mkfun(js, lock_then_handler, data);
  GC_ROOT_PIN(js, on_resolve);
  ant_value_t on_reject = js_heavy_mkfun(js, lock_catch_handler, data);
  GC_ROOT_PIN(js, on_reject);

  ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
  if (is_err(then_result)) lock_reject_callback(js, req, outer_promise, then_result);
  GC_ROOT_RESTORE(js, root_mark);
}

static void lock_run_callback(ant_t *js, lock_request_t *req, ant_value_t callback, ant_value_t arg, ant_value_t outer_promise) {
  ant_value_t result = sv_vm_call(js->vm, js, callback, js_mkundef(), &arg, 1, NULL, js_mkundef());

  if (is_err(result)) lock_reject_callback(js, req, outer_promise, result);
  else if (vtype(result) == kTypePromise) lock_await_callback(js, req, result, outer_promise);
  else {
    if (req) lock_finish(js, req);
    js_resolve_promise(js, outer_promise, result);
  }
}

static void lock_dispatch(ant_t *js, lock_request_t *req) {
  req->dispatched = true;

  if (is_object_type(req->signal) && is_callable(req->abort_fn))
    abort_signal_remove_listener(js, req->signal, req->abort_fn);

  req->signal = js_mkundef();
  req->abort_fn = js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t callback = req->callback;
  ant_value_t promise = req->promise;
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, promise);

  ant_value_t lock_obj = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, lock_obj);
  js_set(js, lock_obj, "name", js_mkstr(js, req->name, req->name_len));
  js_set(js, lock_obj, "mode", lock_mode_value(js, req->mode));
  js_set_tostringtag(js, lock_obj, js_mkstr(js, "Lock", 4));

  lock_run_callback(js, req, callback, lock_obj, promise);
  GC_ROOT_RESTORE(js, root_mark);
}

static void lock_stolen(ant_t *js, lock_request_t *req) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t promise = req->promise;
  GC_ROOT_PIN(js, promise);

  if (!req->dispatched) lock_request_free(js, req);
  ant_value_t err = make_dom_exception(js, "Lock broken by another request with the 'steal' option.", "AbortError");

  GC_ROOT_PIN(js, err);
  js_reject_promise(js, promise, err);
  GC_ROOT_RESTORE(js, root_mark);
}

static void locks_drain(ant_t *js, navigator_locks_t *client) {
  for (;;) {
    ant_mutex_lock(&client->lock);
    lock_request_t *req = client->notify_head;

    if (req) {
      client->notify_head = req->notify_next;
      if (!client->notify_head) client->notify_tail = NULL;
      req->notify_queued = false;
    }

    ant_mutex_unlock(&client->lock);
    if (!req) return;

    lock_state_t state = lock_state(req);
    if (state == LOCK_HELD && !req->dispatched) lock_dispatch(js, req);
    else if (state == LOCK_STOLEN) lock_stolen(js, req);
  }
}

static ant_value_t lock_abort_handler(ant_params_t) {
  navigator_locks_t *client = js->navigator_locks;
  lock_request_t *req = lock_find(client, js_func_data(js->current_func));
  if (!req) return js_mkundef();

  ant_mutex_lock(&lock_registry);
  bool pending = lock_state(req) == LOCK_PENDING;
  if (pending) lock_detach(req, LOCK_RELEASED);
  ant_mutex_unlock(&lock_registry);

  if (!pending) return js_mkundef();
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t promise = req->promise;
  ant_value_t reason = abort_signal_get_reason(req->signal);

  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, reason);

  lock_request_free(js, req);
  js_reject_promise(js, promise, reason);

  GC_ROOT_RESTORE(js, root_mark);
  locks_drain(js, client);

  return js_mkundef();
}

static ant_value_t locks_rejected(ant_t *js, ant_value_t reason) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, reason);
  return promise;
}

static ant_value_t locks_not_supported(ant_t *js, const char *message) {
  return locks_rejected(js, make_dom_exception(js, message, "NotSupportedError"));
}

static ant_value_t lock_read_options(ant_t *js, ant_value_t options, lock_options_t *out) {
  *out = (lock_options_t){ .mode = LOCK_MODE_EXCLUSIVE, .signal = js_mkundef() };
  if (!is_special_object(options)) return js_mkundef();

  ant_value_t mode = js_get(js, options, "mode");
  if (is_err(mode)) return mode;

  if (vtype(mode) == kTypeString) {
    size_t len;
    const char *str = js_getstr(js, mode, &len);
    if (str && len == 6 && memcmp(str, "shared", 6) == 0) out->mode = LOCK_MODE_SHARED;
  }

  ant_value_t if_available = js_get(js, options, "ifAvailable");
  if (is_err(if_available)) return if_available;
  out->if_available = if_available == js_true;

  ant_value_t steal = js_get(js, options, "steal");
  if (is_err(steal)) return steal;
  out->steal = js_truthy(js, steal);

  out->signal = js_get(js, options, "signal");
  if (is_err(out->signal)) return out->signal;
  if (vtype(out->signal) != kTypeUndefined && !abort_signal_is_signal(out->signal))
    return js_mkerr_typed(js, JS_ERR_TYPE, "signal must be an AbortSignal");

  return js_mkundef();
}

static ant_value_t locks_request(ant_params_t) {
  if (nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "locks.request requires at least 2 arguments");

  size_t name_len;
  const char *name = js_getstr(js, args[0], &name_len);

  if (!name)
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument must be a string");

  lock_options_t opts;
  ant_value_t callback = nargs == 2 ? args[1] : args[2];
  ant_value_t parsed = lock_read_options(js, nargs == 2 ? js_mkundef() : args[1], &opts);

  if (is_err(parsed)) return parsed;
  if (vtype(callback) != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Callback must be a function");

  bool has_signal = vtype(opts.signal) != kTypeUndefined;
  if (name_len > 0 && name[0] == '-')
    return locks_not_supported(js, "Names starting with '-' are reserved.");

  if (opts.steal && opts.if_available)
    return locks_not_supported(js, "The 'steal' and 'ifAvailable' options cannot be used together.");

  if (opts.steal && opts.mode != LOCK_MODE_EXCLUSIVE)
    return locks_not_supported(js, "The 'steal' option may only be used with 'exclusive' locks.");

  if (has_signal && (opts.steal || opts.if_available))
    return locks_not_supported(js, "The 'signal' option may not be used with 'steal' or 'ifAvailable'.");

  if (has_signal && abort_signal_is_aborted(opts.signal))
    return locks_rejected(js, abort_signal_get_reason(opts.signal));

  navigator_locks_t *client = locks_client(js);
  lock_request_t *req = client ? calloc(1, sizeof(*req)) : NULL;
  char *name_copy = req ? lock_copy_name(name, name_len) : NULL;

  if (!name_copy) {
    free(req);
    return Ant_Error_CreateOutOfMemory(js);
  }

  ant_value_t promise = js_mkpromise(js);
  req->client = client;
  req->name = name_copy;
  req->name_len = name_len;
  req->id = ++client->next_id;
  req->mode = opts.mode;
  req->callback = callback;
  req->promise = promise;
  req->signal = js_mkundef();
  req->abort_fn = js_mkundef();
  atomic_init(&req->state, LOCK_PENDING);

  lock_register_t registered = lock_register(req, &opts);
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);

  if (registered != LOCK_REGISTER_OK) {
    free(req->name);
    free(req);
    if (registered == LOCK_REGISTER_OOM) {
      GC_ROOT_RESTORE(js, root_mark);
      return Ant_Error_CreateOutOfMemory(js);
    }
    lock_run_callback(js, NULL, callback, js_mknull(), promise);
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  lock_list_append(client, req);
  if (has_signal && lock_state(req) == LOCK_PENDING) {
    req->signal = opts.signal;
    req->abort_fn = js_heavy_mkfun(js, lock_abort_handler, js_mknum((double)req->id));
    abort_signal_add_listener(js, opts.signal, req->abort_fn);
  }

  locks_drain(js, client);
  GC_ROOT_RESTORE(js, root_mark);

  return promise;
}

static ant_value_t locks_query(ant_params_t) {
  navigator_locks_t *client = js->navigator_locks;
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t held = js_mkarr(js);
  GC_ROOT_PIN(js, held);

  ant_value_t pending = js_mkarr(js);
  GC_ROOT_PIN(js, pending);

  for (lock_request_t *req = client ? client->requests : NULL; req; req = req->next) {
    lock_state_t state = lock_state(req);
    if (state != LOCK_HELD && state != LOCK_PENDING) continue;

    ant_value_t info = Ant_Object_CreateBare(js);
    js_set(js, info, "name", js_mkstr(js, req->name, req->name_len));
    js_set(js, info, "mode", lock_mode_value(js, req->mode));
    js_set(js, info, "clientId", js_mkstr(js, client->client_id, strlen(client->client_id)));
    js_arr_push(js, state == LOCK_HELD ? held : pending, info);
  }

  ant_value_t result = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, result);
  js_set(js, result, "held", held);
  js_set(js, result, "pending", pending);

  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  GC_ROOT_RESTORE(js, root_mark);

  return promise;
}

static int get_hardware_concurrency(void) {
#ifdef __APPLE__
  int count;
  size_t size = sizeof(count);
  if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0) {
    return count;
  }
  return 1;
#elif defined(__linux__)
  int count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? count : 1;
#elif defined(_WIN32) || defined(_WIN64)
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return (int)sysinfo.dwNumberOfProcessors;
#else
  return 1;
#endif
}

static const char *get_platform_string(void) {
#if defined(__APPLE__)
  return "MacIntel";
#elif defined(__linux__)
  return "Linux x86_64";
#elif defined(_WIN32) || defined(_WIN64)
  return "Win32";
#elif defined(__FreeBSD__)
  return "FreeBSD";
#else
  return "Unknown";
#endif
}

void init_navigator_module(ant_t *js) {
  ant_value_t navigator_obj = Ant_Object_Create(js);
  
  js_set(js, navigator_obj, "hardwareConcurrency", js_mknum((double)get_hardware_concurrency()));
  js_set(js, navigator_obj, "language", js_mkstr(js, "en-US", 5));
  
  ant_value_t languages_arr = js_mkarr(js);
  js_arr_push(js, languages_arr, js_mkstr(js, "en-US", 5));
  js_set(js, navigator_obj, "languages", languages_arr);
  
  const char *platform = get_platform_string();
  js_set(js, navigator_obj, "platform", js_mkstr(js, platform, strlen(platform)));
  
  char user_agent[64];
  snprintf(user_agent, sizeof(user_agent), "Ant/%s", ANT_VERSION);
  js_set(js, navigator_obj, "userAgent", js_mkstr(js, user_agent, strlen(user_agent)));
  
  ant_value_t locks_obj = Ant_Object_Create(js);
  js_set(js, locks_obj, "request", Ant_Function_Create(locks_request));
  js_set(js, locks_obj, "query", Ant_Function_Create(locks_query));
  js_set_tostringtag(js, locks_obj, js_mkstr(js, "LockManager", 11));
  js_set(js, navigator_obj, "locks", locks_obj);
  
  js_set_tostringtag(js, navigator_obj, js_mkstr(js, "Navigator", 9));
  js_set(js, js_glob(js), "navigator", navigator_obj);
}

static void locks_async_closed(uv_handle_t *handle) {
  free(handle->data);
}

void cleanup_navigator_module(ant_t *js) {
  navigator_locks_t *client = js->navigator_locks;
  if (!client) return;
  js->navigator_locks = NULL;

  ant_mutex_lock(&lock_registry);
  ant_mutex_lock(&client->lock);
  
  client->closed = true;
  client->notify_head = client->notify_tail = NULL;
  
  ant_mutex_unlock(&client->lock);
  for (lock_request_t *req = client->requests; req; req = req->next) lock_detach(req, LOCK_RELEASED);
  ant_mutex_unlock(&lock_registry);

  while (client->requests) {
    lock_request_t *req = client->requests;
    lock_list_remove(client, req);
    free(req->name);
    free(req);
  }
  
  uv_close((uv_handle_t *)&client->async, locks_async_closed);
}

void gc_mark_navigator(ant_t *js, gc_mark_fn mark) {
  if (!js->navigator_locks) return;
  for (lock_request_t *req = js->navigator_locks->requests; req; req = req->next) {
    mark(js, req->callback);
    mark(js, req->promise);
    if (is_object_type(req->signal)) mark(js, req->signal);
    if (is_object_type(req->abort_fn)) mark(js, req->abort_fn);
  }
}
