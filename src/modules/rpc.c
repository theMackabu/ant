#include <compat.h> // IWYU pragma: keep

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "ptr.h"
#include "silver/engine.h"

#include "gc/modules.h"
#include "gc/roots.h"
#include "modules/assert.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/rpc.h"
#include "modules/symbol.h"

#include "wirecall/client.h"
#include "wirecall/protocol.h"
#include "wirecall/server.h"

enum {
  RPC_SERVER_NATIVE_TAG = 0x52504353u, // RPCS
  RPC_CLIENT_NATIVE_TAG = 0x52504343u, // RPCC
  RPC_TASK_NATIVE_TAG   = 0x52504354u  // RPCT
};

typedef struct rpc_server rpc_server_t;
typedef struct rpc_route rpc_route_t;
typedef struct rpc_deferred_task rpc_deferred_task_t;
typedef struct rpc_client rpc_client_t;
typedef struct rpc_client_op rpc_client_op_t;

typedef enum {
  RPC_CLIENT_OP_CONNECT,
  RPC_CLIENT_OP_CALL,
  RPC_CLIENT_OP_PING,
  RPC_CLIENT_OP_CLOSE
} rpc_client_op_type_t;

struct rpc_route {
  rpc_server_t *owner;
  ant_value_t handler;
  char *name;
  atomic_uint refs;
  atomic_bool removed;
  rpc_route_t *next;
  rpc_route_t *prev;
};

struct rpc_deferred_task {
  rpc_server_t *server;
  rpc_route_t *route;
  wirecall_deferred *call;
  const wirecall_value *args;
  size_t argc;
  atomic_uint refs;
  bool pending_promise;
  bool completed;
  rpc_deferred_task_t *next;
  rpc_deferred_task_t *prev;
};

struct rpc_server {
  ant_t *js;
  ant_value_t obj;
  ant_value_t listen_promise;
  wirecall_server *server;
  uv_async_t async;
  pthread_t thread;
  bool async_initialized;
  bool thread_started;
  bool listening;
  bool closing;
  bool closed;
  uint16_t port;
  rpc_route_t *routes;
  rpc_deferred_task_t *tasks_head;
  rpc_deferred_task_t *tasks_tail;
  pthread_mutex_t task_mutex;
  rpc_deferred_task_t *pending_head;
  rpc_deferred_task_t *pending_tail;
  rpc_server_t *next_active;
  rpc_server_t *prev_active;
};

struct rpc_client_op {
  rpc_client_op_type_t type;
  ant_value_t promise;
  char *name;
  wirecall_writer writer;
  bool writer_initialized;
  wirecall_value *values;
  size_t value_count;
  int status;
  char error[192];
  rpc_client_op_t *next;
  rpc_client_op_t *done_next;
};

struct rpc_client {
  ant_t *js;
  ant_value_t obj;
  wirecall_client *client;
  char *host;
  char *port;
  uint32_t integrity;
  uint8_t mac_key[16];
  bool has_mac_key;
  bool worker_started;
  bool closing;
  bool closed;
  bool connected;
  uv_async_t async;
  bool async_initialized;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  rpc_client_op_t *head;
  rpc_client_op_t *tail;
  rpc_client_op_t *done_head;
  rpc_client_op_t *done_tail;
  rpc_client_t *next_active;
  rpc_client_t *prev_active;
};

static ant_value_t g_rpc_server_proto = 0;
static ant_value_t g_rpc_server_ctor = 0;
static ant_value_t g_rpc_client_proto = 0;
static ant_value_t g_rpc_client_ctor = 0;
static rpc_server_t *g_active_servers = NULL;
static rpc_client_t *g_active_clients = NULL;

static void rpc_server_close_impl(rpc_server_t *server);
static void rpc_client_close_impl(rpc_client_t *client);

static void rpc_add_active_server(rpc_server_t *server) {
  server->next_active = g_active_servers;
  server->prev_active = NULL;
  if (g_active_servers) g_active_servers->prev_active = server;
  g_active_servers = server;
}

static void rpc_remove_active_server(rpc_server_t *server) {
  if (server->prev_active) server->prev_active->next_active = server->next_active;
  else if (g_active_servers == server) g_active_servers = server->next_active;
  if (server->next_active) server->next_active->prev_active = server->prev_active;
  server->next_active = NULL;
  server->prev_active = NULL;
}

static void rpc_add_active_client(rpc_client_t *client) {
  client->next_active = g_active_clients;
  client->prev_active = NULL;
  if (g_active_clients) g_active_clients->prev_active = client;
  g_active_clients = client;
}

static void rpc_remove_active_client(rpc_client_t *client) {
  if (client->prev_active) client->prev_active->next_active = client->next_active;
  else if (g_active_clients == client) g_active_clients = client->next_active;
  if (client->next_active) client->next_active->prev_active = client->prev_active;
  client->next_active = NULL;
  client->prev_active = NULL;
}

static rpc_server_t *rpc_server_data(ant_value_t value) {
  return (rpc_server_t *)js_get_native(value, RPC_SERVER_NATIVE_TAG);
}

static rpc_client_t *rpc_client_data(ant_value_t value) {
  return (rpc_client_t *)js_get_native(value, RPC_CLIENT_NATIVE_TAG);
}

static char *rpc_strdup_len(const char *str, size_t len) {
  char *out = malloc(len + 1);
  if (!out) return NULL;
  if (len) memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

static char *rpc_value_to_cstring(ant_t *js, ant_value_t value, const char *what) {
  if (vtype(value) != T_STR) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "%s must be a string", what);
    (void)err;
    return NULL;
  }
  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  return rpc_strdup_len(str ? str : "", len);
}

static bool rpc_parse_integrity_name(const char *name, uint32_t *out) {
  if (!name || !out) return false;
  if (strcmp(name, "none") == 0) {
    *out = WIRECALL_INTEGRITY_NONE;
    return true;
  }
  if (strcmp(name, "checksum") == 0) {
    *out = WIRECALL_INTEGRITY_CHECKSUM;
    return true;
  }
  if (strcmp(name, "mac") == 0) {
    *out = WIRECALL_INTEGRITY_MAC;
    return true;
  }
  if (strcmp(name, "checksum+mac") == 0 || strcmp(name, "mac+checksum") == 0) {
    *out = WIRECALL_INTEGRITY_CHECKSUM | WIRECALL_INTEGRITY_MAC;
    return true;
  }
  return false;
}

static ant_value_t rpc_parse_integrity_options(
  ant_t *js,
  ant_value_t options,
  uint32_t *integrity,
  uint8_t mac_key[16],
  bool *has_mac_key
) {
  if (integrity) *integrity = WIRECALL_INTEGRITY_DEFAULT;
  if (has_mac_key) *has_mac_key = false;
  if (!is_object_type(options)) return js_mkundef();

  ant_value_t iv = js_get(js, options, "integrity");
  if (vtype(iv) == T_NUM) {
    uint32_t raw = (uint32_t)js_getnum(iv);
    if (raw & ~(WIRECALL_INTEGRITY_CHECKSUM | WIRECALL_INTEGRITY_MAC))
      return js_mkerr_typed(js, JS_ERR_RANGE, "invalid rpc integrity");
    *integrity = raw;
  } else if (vtype(iv) == T_STR) {
    const char *s = js_getstr(js, iv, NULL);
    if (!rpc_parse_integrity_name(s, integrity))
      return js_mkerr_typed(js, JS_ERR_RANGE, "invalid rpc integrity '%s'", s ? s : "");
  } else if (vtype(iv) != T_UNDEF && vtype(iv) != T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "rpc integrity must be a string or number");
  }

  ant_value_t key = js_get(js, options, "macKey");
  if (vtype(key) == T_STR) {
    size_t len = 0;
    const char *s = js_getstr(js, key, &len);
    if (len != 16) return js_mkerr_typed(js, JS_ERR_TYPE, "rpc macKey must be exactly 16 bytes");
    memcpy(mac_key, s, 16);
    *has_mac_key = true;
  } else if (buffer_is_binary_source(key)) {
    const uint8_t *bytes = NULL;
    size_t len = 0;
    if (!buffer_source_get_bytes(js, key, &bytes, &len) || len != 16)
      return js_mkerr_typed(js, JS_ERR_TYPE, "rpc macKey must be exactly 16 bytes");
    memcpy(mac_key, bytes, 16);
    *has_mac_key = true;
  } else if (vtype(key) != T_UNDEF && vtype(key) != T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "rpc macKey must be a string or bytes");
  }

  if ((*integrity & WIRECALL_INTEGRITY_MAC) && !*has_mac_key)
    return js_mkerr_typed(js, JS_ERR_TYPE, "rpc integrity 'mac' requires macKey");

  return js_mkundef();
}

static ant_value_t rpc_get_error_value(ant_t *js, const char *prefix, const char *name, ant_value_t reason) {
  const char *msg = NULL;
  if (vtype(reason) == T_ERR && js->thrown_exists) reason = js->thrown_value;
  if (is_object_type(reason)) {
    ant_value_t message = js_get(js, reason, "message");
    if (vtype(message) == T_STR) msg = js_getstr(js, message, NULL);
  }
  if (!msg) msg = js_str(js, reason);
  return js_mkerr(js, "%s '%s' failed: %s", prefix, name ? name : "<unknown>", msg ? msg : "error");
}

static int rpc_write_js_value(ant_t *js, wirecall_writer *writer, ant_value_t value, const char **error) {
  uint8_t type = vtype(value);
  if (type == T_NULL) return wirecall_writer_null(writer);
  if (type == T_BOOL) return wirecall_writer_bool(writer, value == js_true || vdata(value) != 0);
  if (type == T_NUM) return wirecall_writer_f64(writer, js_getnum(value));
  if (type == T_BIGINT) {
    if (bigint_is_negative(js, value)) {
      ant_value_t min = bigint_from_int64(js, INT64_MIN);
      ant_value_t max = bigint_from_int64(js, INT64_MAX);
      if (bigint_compare(js, value, min) < 0 || bigint_compare(js, value, max) > 0) {
        if (error) *error = "BigInt is not representable as i64";
        return -1;
      }
      int64_t out = 0;
      if (!bigint_to_int64_wrapping(js, value, &out)) {
        if (error) *error = "BigInt is not representable as i64";
        return -1;
      }
      return wirecall_writer_i64(writer, out);
    }
    ant_value_t max = bigint_from_uint64(js, UINT64_MAX);
    if (bigint_compare(js, value, max) > 0) {
      if (error) *error = "BigInt is not representable as u64";
      return -1;
    }
    uint64_t out = 0;
    if (!bigint_to_uint64_wrapping(js, value, &out)) {
      if (error) *error = "BigInt is not representable as u64";
      return -1;
    }
    return wirecall_writer_u64(writer, out);
  }
  if (type == T_STR) {
    size_t len = 0;
    const char *str = js_getstr(js, value, &len);
    if (len > UINT32_MAX) {
      if (error) *error = "string payload is too large";
      return -1;
    }
    return wirecall_writer_string(writer, str ? str : "", (uint32_t)len);
  }
  if (buffer_is_binary_source(value)) {
    const uint8_t *bytes = NULL;
    size_t len = 0;
    if (!buffer_source_get_bytes(js, value, &bytes, &len) || len > UINT32_MAX) {
      if (error) *error = "bytes payload is invalid or too large";
      return -1;
    }
    return wirecall_writer_bytes(writer, bytes, (uint32_t)len);
  }
  if (error) *error = "unsupported rpc value";
  return -1;
}

static ant_value_t rpc_values_to_js_array(ant_t *js, const wirecall_value *values, size_t count) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t arr = js_mkarr(js);
  GC_ROOT_PIN(js, arr);

  for (size_t i = 0; i < count; i++) {
    ant_value_t value = js_mkundef();
    switch (values[i].type) {
      case WIRECALL_TYPE_NULL:
        value = js_mknull();
        break;
      case WIRECALL_TYPE_BOOL:
        value = js_bool(values[i].as.boolean);
        break;
      case WIRECALL_TYPE_I64:
        value = bigint_from_int64(js, values[i].as.i64);
        break;
      case WIRECALL_TYPE_U64:
        value = bigint_from_uint64(js, values[i].as.u64);
        break;
      case WIRECALL_TYPE_F64:
        value = js_mknum(values[i].as.f64);
        break;
      case WIRECALL_TYPE_STRING:
        value = js_mkstr(js, values[i].as.string.data, values[i].as.string.len);
        break;
      case WIRECALL_TYPE_BYTES: {
        uint32_t len = values[i].as.bytes.len;
        ArrayBufferData *ab = create_array_buffer_data(len);
        if (!ab) {
          GC_ROOT_RESTORE(js, root_mark);
          return js_mkerr(js, "out of memory");
        }
        if (len) memcpy(ab->data, values[i].as.bytes.data, len);
        value = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
        break;
      }
      default:
        GC_ROOT_RESTORE(js, root_mark);
        return js_mkerr_typed(js, JS_ERR_TYPE, "unsupported rpc response value");
    }
    GC_ROOT_PIN(js, value);
    js_arr_push(js, arr, value);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

static int rpc_write_js_array(ant_t *js, wirecall_writer *writer, ant_value_t arr, const char **error) {
  if (vtype(arr) != T_ARR) {
    if (error) *error = "rpc handler must return an array";
    return -1;
  }
  ant_offset_t len = js_arr_len(js, arr);
  for (ant_offset_t i = 0; i < len; i++) {
    if (rpc_write_js_value(js, writer, js_arr_get(js, arr, i), error) != 0) return -1;
  }
  return 0;
}

static rpc_route_t *rpc_route_new(rpc_server_t *server, const char *name, ant_value_t handler) {
  rpc_route_t *route = calloc(1, sizeof(*route));
  if (!route) return NULL;
  route->name = strdup(name);
  if (!route->name) {
    free(route);
    return NULL;
  }
  route->owner = server;
  route->handler = handler;
  atomic_init(&route->refs, 1);
  atomic_init(&route->removed, false);
  route->next = server->routes;
  if (server->routes) server->routes->prev = route;
  server->routes = route;
  return route;
}

static void rpc_route_unlink(rpc_route_t *route) {
  if (!route || !route->owner) return;
  rpc_server_t *server = route->owner;
  if (route->prev) route->prev->next = route->next;
  else if (server->routes == route) server->routes = route->next;
  if (route->next) route->next->prev = route->prev;
  route->next = NULL;
  route->prev = NULL;
}

static void rpc_route_free(rpc_route_t *route) {
  if (!route) return;
  route->handler = js_mkundef();
  free(route->name);
  free(route);
}

static void rpc_route_release(rpc_route_t *route) {
  if (!route) return;
  if (atomic_fetch_sub_explicit(&route->refs, 1, memory_order_acq_rel) == 1)
    rpc_route_free(route);
}

static void rpc_route_retain(rpc_route_t *route) {
  if (route) atomic_fetch_add_explicit(&route->refs, 1, memory_order_relaxed);
}

static void rpc_route_finalizer(void *user_data) {
  rpc_route_t *route = (rpc_route_t *)user_data;
  if (!route) return;
  atomic_store_explicit(&route->removed, true, memory_order_release);
  route->handler = js_mkundef();
  rpc_route_release(route);
}

static rpc_route_t *rpc_route_find(rpc_server_t *server, const char *name) {
  for (rpc_route_t *r = server ? server->routes : NULL; r; r = r->next)
    if (r->name && strcmp(r->name, name) == 0) return r;
  return NULL;
}

static void rpc_task_link(rpc_server_t *server, rpc_deferred_task_t *task) {
  task->next = NULL;
  task->prev = server->tasks_tail;
  if (server->tasks_tail) server->tasks_tail->next = task;
  else server->tasks_head = task;
  server->tasks_tail = task;
}

static void rpc_task_unlink(rpc_deferred_task_t *task) {
  rpc_server_t *server = task ? task->server : NULL;
  if (!server) return;
  if (task->prev) task->prev->next = task->next;
  else if (server->tasks_head == task) server->tasks_head = task->next;
  if (task->next) task->next->prev = task->prev;
  else if (server->tasks_tail == task) server->tasks_tail = task->prev;
  task->next = NULL;
  task->prev = NULL;
}

static void rpc_task_destroy(rpc_deferred_task_t *task) {
  if (!task) return;
  rpc_route_release(task->route);
  free(task);
}

static void rpc_task_retain(rpc_deferred_task_t *task) {
  if (task) atomic_fetch_add_explicit(&task->refs, 1, memory_order_relaxed);
}

static void rpc_task_release(rpc_deferred_task_t *task) {
  if (!task) return;
  if (atomic_fetch_sub_explicit(&task->refs, 1, memory_order_acq_rel) == 1)
    rpc_task_destroy(task);
}

static void rpc_server_queue_task(rpc_server_t *server, rpc_deferred_task_t *task) {
  if (!server || server->closing || server->closed) {
    if (task) {
      (void)wirecall_deferred_fail(task->call, "RpcServer closed");
      rpc_task_release(task);
    }
    return;
  }

  pthread_mutex_lock(&server->task_mutex);
  task->next = NULL;
  if (server->pending_tail) server->pending_tail->next = task;
  else server->pending_head = task;
  server->pending_tail = task;
  pthread_mutex_unlock(&server->task_mutex);
  if (server->async_initialized) uv_async_send(&server->async);
}

static void rpc_complete_task(rpc_deferred_task_t *task, ant_value_t result, bool rejected) {
  if (!task || task->completed) return;
  task->completed = true;

  if (rejected || is_err(result)) {
    ant_t *js = task->server->js;
    ant_value_t reason = js->thrown_exists ? js->thrown_value : result;
    ant_value_t err = rpc_get_error_value(js, "rpc procedure", task->route ? task->route->name : NULL, reason);
    const char *msg = js_str(js, err);
    (void)wirecall_deferred_fail(task->call, msg ? msg : "rpc procedure failed");
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
  } else {
    const char *error = NULL;
    wirecall_writer *writer = wirecall_deferred_response(task->call);
    if (rpc_write_js_array(task->server->js, writer, result, &error) != 0) {
      char msg[192];
      snprintf(msg, sizeof(msg), "rpc procedure '%s' failed: %s", task->route ? task->route->name : "<unknown>",
               error ? error : "invalid response");
      (void)wirecall_deferred_fail(task->call, msg);
    } else {
      (void)wirecall_deferred_complete(task->call);
    }
  }

  rpc_task_unlink(task);
  rpc_task_release(task);
}

static ant_value_t rpc_task_promise_resolve(ant_params_t) {
  rpc_deferred_task_t *task = (rpc_deferred_task_t *)js_get_native(js_getcurrentfunc(js), RPC_TASK_NATIVE_TAG);
  rpc_complete_task(task, nargs > 0 ? args[0] : js_mkundef(), false);
  rpc_task_release(task);
  return js_mkundef();
}

static ant_value_t rpc_task_promise_reject(ant_params_t) {
  rpc_deferred_task_t *task = (rpc_deferred_task_t *)js_get_native(js_getcurrentfunc(js), RPC_TASK_NATIVE_TAG);
  rpc_complete_task(task, nargs > 0 ? args[0] : js_mkundef(), true);
  rpc_task_release(task);
  return js_mkundef();
}

static void rpc_run_task(ant_t *js, rpc_deferred_task_t *task) {
  if (!task || !task->route || atomic_load_explicit(&task->route->removed, memory_order_acquire)) {
    if (task) rpc_complete_task(task, js_mkerr(js, "rpc procedure was removed"), true);
    return;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t args_arr = rpc_values_to_js_array(js, task->args, task->argc);
  GC_ROOT_PIN(js, args_arr);
  if (is_err(args_arr)) {
    rpc_complete_task(task, args_arr, true);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t call_args[] = { args_arr };
  ant_value_t result = sv_vm_call(js->vm, js, task->route->handler, js_mkundef(), call_args, 1, NULL, false);
  GC_ROOT_PIN(js, result);

  bool result_is_awaitable = vtype(result) == T_PROMISE || (is_object_type(result) && is_callable(js_get(js, result, "then")));
  if (is_err(result) || (js->thrown_exists && !result_is_awaitable)) {
    rpc_complete_task(task, result, true);
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  if (result_is_awaitable) {
    task->pending_promise = true;
    ant_value_t awaited = js_promise_assimilate_awaitable(js, result);
    GC_ROOT_PIN(js, awaited);
    if (vtype(result) == T_PROMISE) promise_mark_handled(result);
    if (vtype(awaited) == T_PROMISE) promise_mark_handled(awaited);
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    ant_value_t on_resolve = js_heavy_mkfun_native(js, rpc_task_promise_resolve, task, RPC_TASK_NATIVE_TAG);
    GC_ROOT_PIN(js, on_resolve);
    ant_value_t on_reject = js_heavy_mkfun_native(js, rpc_task_promise_reject, task, RPC_TASK_NATIVE_TAG);
    GC_ROOT_PIN(js, on_reject);
    ant_value_t then_result = js_promise_then(js, awaited, on_resolve, on_reject);
    if (is_err(then_result)) {
      rpc_complete_task(task, then_result, true);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }
    rpc_task_retain(task);
    promise_mark_handled(then_result);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  rpc_complete_task(task, result, false);
  GC_ROOT_RESTORE(js, root_mark);
}

static void rpc_server_async_cb(uv_async_t *handle) {
  rpc_server_t *server = (rpc_server_t *)handle->data;
  if (!server || !server->js) return;

  for (;;) {
    pthread_mutex_lock(&server->task_mutex);
    rpc_deferred_task_t *task = server->pending_head;
    if (task) {
      server->pending_head = task->next;
      if (!server->pending_head) server->pending_tail = NULL;
    }
    pthread_mutex_unlock(&server->task_mutex);
    if (!task) break;
    task->next = NULL;
    rpc_task_link(server, task);
    rpc_run_task(server->js, task);
  }

  js_maybe_drain_microtasks_after_async_settle(server->js);
}

static int rpc_deferred_handler(wirecall_deferred *call, const wirecall_value *args, size_t argc, void *user_data) {
  rpc_route_t *route = (rpc_route_t *)user_data;
  if (!route || !route->owner || route->owner->closing || route->owner->closed) {
    (void)wirecall_deferred_fail(call, "RpcServer closed");
    return 0;
  }
  rpc_route_retain(route);

  rpc_deferred_task_t *task = calloc(1, sizeof(*task));
  if (!task) {
    rpc_route_release(route);
    return -1;
  }
  task->server = route->owner;
  task->route = route;
  task->call = call;
  task->args = args;
  task->argc = argc;
  atomic_init(&task->refs, 1);
  rpc_server_queue_task(route->owner, task);
  return 0;
}

static void rpc_server_fail_deferred_tasks(rpc_server_t *server, const char *message) {
  if (!server || !server->js) return;

  while (server->tasks_head) {
    rpc_deferred_task_t *task = server->tasks_head;
    rpc_complete_task(task, js_mkerr(server->js, "%s", message ? message : "RpcServer closed"), true);
  }

  pthread_mutex_lock(&server->task_mutex);
  rpc_deferred_task_t *pending = server->pending_head;
  server->pending_head = NULL;
  server->pending_tail = NULL;
  pthread_mutex_unlock(&server->task_mutex);

  while (pending) {
    rpc_deferred_task_t *next = pending->next;
    pending->next = NULL;
    pending->prev = NULL;
    rpc_complete_task(pending, js_mkerr(server->js, "%s", message ? message : "RpcServer closed"), true);
    pending = next;
  }
}

static void *rpc_server_thread_main(void *arg) {
  rpc_server_t *server = (rpc_server_t *)arg;
  (void)wirecall_server_run(server->server);
  return NULL;
}

static void rpc_server_async_close_cb(uv_handle_t *handle) {
  rpc_server_t *server = handle ? (rpc_server_t *)handle->data : NULL;
  if (server) server->async_initialized = false;
}

static void rpc_server_finalizer(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  rpc_server_t *server = rpc_server_data(value);
  if (!server) return;
  js_clear_native(value, RPC_SERVER_NATIVE_TAG);
  rpc_server_close_impl(server);
  pthread_mutex_destroy(&server->task_mutex);
  free(server);
}

static rpc_server_t *rpc_require_server(ant_t *js, ant_value_t this_val) {
  rpc_server_t *server = rpc_server_data(this_val);
  if (!server) {
    js->thrown_exists = true;
    js->thrown_value = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid RpcServer");
    return NULL;
  }
  return server;
}

static ant_value_t rpc_server_register_impl(ant_t *js, rpc_server_t *server, ant_value_t name_val, ant_value_t handler) {
  char *name = rpc_value_to_cstring(js, name_val, "route name");
  if (!name) return js->thrown_value;
  if (!is_callable(handler)) {
    free(name);
    return js_mkerr_typed(js, JS_ERR_TYPE, "rpc route handler must be a function");
  }

  rpc_route_t *old = rpc_route_find(server, name);
  if (old) {
    rpc_route_unlink(old);
    if (server->server) (void)wirecall_server_remove_route_name(server->server, name);
  }

  rpc_route_t *route = rpc_route_new(server, name, handler);
  if (!route) {
    free(name);
    return js_mkerr(js, "out of memory");
  }
  if (wirecall_server_add_deferred_route_name_ex(server->server, name, rpc_deferred_handler, route, rpc_route_finalizer) != 0) {
    rpc_route_unlink(route);
    rpc_route_release(route);
    free(name);
    return js_mkerr(js, "failed to register rpc route '%s'", name);
  }

  free(name);
  return js_getthis(js);
}

static ant_value_t rpc_server_register(ant_t *js, ant_value_t *args, int nargs) {
  rpc_server_t *server = rpc_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer.register requires name and handler");
  return rpc_server_register_impl(js, server, args[0], args[1]);
}

static ant_value_t rpc_server_unregister(ant_t *js, ant_value_t *args, int nargs) {
  rpc_server_t *server = rpc_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer.unregister requires a name");

  char *name = rpc_value_to_cstring(js, args[0], "route name");
  if (!name) return js->thrown_value;
  rpc_route_t *route = rpc_route_find(server, name);
  if (route) {
    rpc_route_unlink(route);
    if (server->server) (void)wirecall_server_remove_route_name(server->server, name);
  }
  free(name);
  return js_getthis(js);
}

static ant_value_t rpc_server_port_getter(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  rpc_server_t *server = rpc_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  if (server->server) server->port = wirecall_server_port(server->server);
  return js_mknum((double)server->port);
}

static ant_value_t rpc_server_listen(ant_t *js, ant_value_t *args, int nargs) {
  rpc_server_t *server = rpc_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;

  ant_value_t promise = js_mkpromise(js);
  if (server->listening) {
    js_resolve_promise(js, promise, js_getthis(js));
    return promise;
  }
  if (nargs < 1 || !is_object_type(args[0])) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer.listen requires options"));
    return promise;
  }

  ant_value_t opts = args[0];
  char port_buf[32];
  ant_value_t port_val = js_get(js, opts, "port");
  if (vtype(port_val) != T_NUM) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer.listen options.port must be a number"));
    return promise;
  }
  snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)(uint16_t)js_getnum(port_val));

  char *host = NULL;
  ant_value_t host_val = js_get(js, opts, "host");
  if (vtype(host_val) == T_STR) host = rpc_value_to_cstring(js, host_val, "host");
  else host = strdup("127.0.0.1");
  if (!host) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }

  uint32_t integrity = WIRECALL_INTEGRITY_DEFAULT;
  uint8_t mac_key[16] = {0};
  bool has_mac_key = false;
  ant_value_t perr = rpc_parse_integrity_options(js, opts, &integrity, mac_key, &has_mac_key);
  if (is_err(perr)) {
    free(host);
    js_reject_promise(js, promise, perr);
    return promise;
  }

  ant_value_t workers_val = js_get(js, opts, "workers");
  if (vtype(workers_val) == T_NUM) {
    uint32_t workers = (uint32_t)js_getnum(workers_val);
    if (workers > 0 && wirecall_server_set_workers(server->server, workers) != 0) {
      free(host);
      js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_RANGE, "invalid rpc worker count"));
      return promise;
    }
  }

  if (wirecall_server_set_integrity(server->server, integrity, has_mac_key ? mac_key : NULL) != 0 ||
      wirecall_server_bind(server->server, host, port_buf) != 0 ||
      wirecall_server_listen(server->server) != 0) {
    free(host);
    js_reject_promise(js, promise, js_mkerr(js, "RpcServer.listen failed"));
    return promise;
  }
  free(host);

  if (!server->async_initialized) {
    if (uv_async_init(uv_default_loop(), &server->async, rpc_server_async_cb) != 0) {
      js_reject_promise(js, promise, js_mkerr(js, "failed to initialize rpc async handle"));
      return promise;
    }
    server->async.data = server;
    server->async_initialized = true;
  }

  if (pthread_create(&server->thread, NULL, rpc_server_thread_main, server) != 0) {
    wirecall_server_stop(server->server);
    js_reject_promise(js, promise, js_mkerr(js, "failed to start rpc server thread"));
    return promise;
  }

  server->thread_started = true;
  server->listening = true;
  server->port = wirecall_server_port(server->server);
  server->listen_promise = promise;
  js_resolve_promise(js, promise, js_getthis(js));
  return promise;
}

static void rpc_server_close_impl(rpc_server_t *server) {
  if (!server || server->closed || server->closing) return;
  server->closing = true;
  rpc_server_fail_deferred_tasks(server, "RpcServer closed");
  if (server->server) {
    wirecall_server_stop(server->server);
    if (server->thread_started) {
      pthread_join(server->thread, NULL);
      server->thread_started = false;
    }
    wirecall_server_destroy(server->server);
    server->server = NULL;
  }
  server->routes = NULL;
  if (server->async_initialized && !uv_is_closing((uv_handle_t *)&server->async))
    uv_close((uv_handle_t *)&server->async, rpc_server_async_close_cb);
  rpc_remove_active_server(server);
  server->listening = false;
  server->closed = true;
  server->closing = false;
}

static ant_value_t rpc_server_close(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  rpc_server_t *server = rpc_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  ant_value_t promise = js_mkpromise(js);
  rpc_server_close_impl(server);
  js_resolve_promise(js, promise, js_mkundef());
  return promise;
}

static ant_value_t rpc_server_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer constructor requires new");

  rpc_server_t *server = calloc(1, sizeof(*server));
  if (!server) return js_mkerr(js, "out of memory");
  server->js = js;
  server->obj = js_getthis(js);
  server->listen_promise = js_mkundef();
  pthread_mutex_init(&server->task_mutex, NULL);

  if (wirecall_server_init(&server->server) != 0) {
    pthread_mutex_destroy(&server->task_mutex);
    free(server);
    return js_mkerr(js, "failed to initialize RpcServer");
  }

  js_set_native(server->obj, server, RPC_SERVER_NATIVE_TAG);
  js_set_finalizer(server->obj, rpc_server_finalizer);
  rpc_add_active_server(server);

  if (nargs > 0 && is_object_type(args[0])) {
    ant_iter_t iter = js_prop_iter_begin(js, args[0]);
    const char *key = NULL;
    size_t key_len = 0;
    ant_value_t handler = js_mkundef();
    while (js_prop_iter_next(&iter, &key, &key_len, &handler)) {
      ant_value_t name = js_mkstr(js, key, key_len);
      ant_value_t r = rpc_server_register_impl(js, server, name, handler);
      if (is_err(r)) {
        js_prop_iter_end(&iter);
        return r;
      }
    }
    js_prop_iter_end(&iter);
  } else if (nargs > 0 && vtype(args[0]) != T_UNDEF && vtype(args[0]) != T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcServer routes must be an object");
  }

  return server->obj;
}

static void rpc_client_op_free(rpc_client_op_t *op) {
  if (!op) return;
  free(op->name);
  if (op->writer_initialized) wirecall_writer_free(&op->writer);
  wirecall_values_free(op->values);
  free(op);
}

static void rpc_client_free_op_list(rpc_client_op_t *op, bool done_list) {
  while (op) {
    rpc_client_op_t *next = done_list ? op->done_next : op->next;
    rpc_client_op_free(op);
    op = next;
  }
}

static void rpc_client_queue_done(rpc_client_t *client, rpc_client_op_t *op) {
  pthread_mutex_lock(&client->mutex);
  op->done_next = NULL;
  if (client->done_tail) client->done_tail->done_next = op;
  else client->done_head = op;
  client->done_tail = op;
  pthread_mutex_unlock(&client->mutex);
  if (client->async_initialized) uv_async_send(&client->async);
}

static void *rpc_client_worker_main(void *arg) {
  rpc_client_t *client = (rpc_client_t *)arg;
  for (;;) {
    pthread_mutex_lock(&client->mutex);
    while (!client->head && !client->closing) pthread_cond_wait(&client->cond, &client->mutex);
    rpc_client_op_t *op = client->head;
    if (op) {
      client->head = op->next;
      if (!client->head) client->tail = NULL;
    }
    bool should_exit = client->closing && !op;
    pthread_mutex_unlock(&client->mutex);
    if (should_exit) break;
    if (!op) continue;

    op->status = -1;
    switch (op->type) {
      case RPC_CLIENT_OP_CONNECT:
        if (client->client) {
          op->status = 0;
        } else if (wirecall_client_connect(&client->client, client->host, client->port) == 0 &&
                   wirecall_client_set_integrity(client->client, client->integrity, client->has_mac_key ? client->mac_key : NULL) == 0) {
          op->status = 0;
        } else {
          snprintf(op->error, sizeof(op->error), "RpcClient.connect failed");
        }
        break;
      case RPC_CLIENT_OP_CALL:
        if (!client->client) {
          snprintf(op->error, sizeof(op->error), "RpcClient is not connected");
        } else if (wirecall_client_call_name(client->client, op->name, &op->writer, &op->values, &op->value_count) == 0) {
          op->status = 0;
        } else {
          snprintf(op->error, sizeof(op->error), "rpc call '%s' failed: %s", op->name ? op->name : "<unknown>",
                   wirecall_client_error(client->client));
        }
        break;
      case RPC_CLIENT_OP_PING:
        if (client->client && wirecall_client_ping(client->client) == 0) op->status = 0;
        else snprintf(op->error, sizeof(op->error), "RpcClient.ping failed: %s",
                      client->client ? wirecall_client_error(client->client) : "not connected");
        break;
      case RPC_CLIENT_OP_CLOSE:
        if (client->client) {
          wirecall_client_close(client->client);
          client->client = NULL;
        }
        op->status = 0;
        break;
    }
    rpc_client_queue_done(client, op);
  }
  return NULL;
}

static void rpc_client_async_close_cb(uv_handle_t *handle) {
  rpc_client_t *client = handle ? (rpc_client_t *)handle->data : NULL;
  if (client) client->async_initialized = false;
}

static void rpc_client_async_cb(uv_async_t *handle) {
  rpc_client_t *client = (rpc_client_t *)handle->data;
  if (!client || !client->js) return;
  ant_t *js = client->js;
  bool close_after_drain = false;

  for (;;) {
    pthread_mutex_lock(&client->mutex);
    rpc_client_op_t *op = client->done_head;
    if (op) {
      client->done_head = op->done_next;
      if (!client->done_head) client->done_tail = NULL;
    }
    pthread_mutex_unlock(&client->mutex);
    if (!op) break;

    if (op->status == 0) {
      if (op->type == RPC_CLIENT_OP_CALL) {
        ant_value_t arr = rpc_values_to_js_array(js, op->values, op->value_count);
        if (is_err(arr)) js_reject_promise(js, op->promise, arr);
        else js_resolve_promise(js, op->promise, arr);
      } else {
        if (op->type == RPC_CLIENT_OP_CONNECT) client->connected = true;
        if (op->type == RPC_CLIENT_OP_CLOSE) {
          client->connected = false;
          close_after_drain = true;
        }
        js_resolve_promise(js, op->promise, js_mkundef());
      }
    } else {
      js_reject_promise(js, op->promise, js_mkerr(js, "%s", op->error[0] ? op->error : "RpcClient operation failed"));
    }
    rpc_client_op_free(op);
  }

  js_maybe_drain_microtasks_after_async_settle(js);
  if (close_after_drain) rpc_client_close_impl(client);
}

static ant_value_t rpc_client_enqueue(rpc_client_t *client, rpc_client_op_t *op) {
  if (!op) return js_mkerr(client->js, "out of memory");
  ant_value_t promise = js_mkpromise(client->js);
  op->promise = promise;

  pthread_mutex_lock(&client->mutex);
  if (client->closing || client->closed) {
    pthread_mutex_unlock(&client->mutex);
    js_reject_promise(client->js, promise, js_mkerr(client->js, "RpcClient is closed"));
    rpc_client_op_free(op);
    return promise;
  }
  if (client->tail) client->tail->next = op;
  else client->head = op;
  client->tail = op;
  pthread_cond_signal(&client->cond);
  pthread_mutex_unlock(&client->mutex);
  return promise;
}

static rpc_client_t *rpc_require_client(ant_t *js, ant_value_t this_val) {
  rpc_client_t *client = rpc_client_data(this_val);
  if (!client) {
    js->thrown_exists = true;
    js->thrown_value = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid RpcClient");
    return NULL;
  }
  return client;
}

static ant_value_t rpc_client_connect(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  rpc_client_t *client = rpc_require_client(js, js_getthis(js));
  if (!client) return js->thrown_value;
  if (client->closed || client->closing) {
    ant_value_t promise = js_mkpromise(js);
    js_reject_promise(js, promise, js_mkerr(js, "RpcClient is closed"));
    return promise;
  }
  rpc_client_op_t *op = calloc(1, sizeof(*op));
  if (!op) return js_mkerr(js, "out of memory");
  op->type = RPC_CLIENT_OP_CONNECT;
  return rpc_client_enqueue(client, op);
}

static ant_value_t rpc_client_call(ant_t *js, ant_value_t *args, int nargs) {
  rpc_client_t *client = rpc_require_client(js, js_getthis(js));
  if (!client) return js->thrown_value;
  if (nargs < 2 || vtype(args[1]) != T_ARR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcClient.call requires name and args array");

  char *name = rpc_value_to_cstring(js, args[0], "procedure name");
  if (!name) return js->thrown_value;

  rpc_client_op_t *op = calloc(1, sizeof(*op));
  if (!op) {
    free(name);
    return js_mkerr(js, "out of memory");
  }
  op->type = RPC_CLIENT_OP_CALL;
  op->name = name;
  wirecall_writer_init(&op->writer);
  op->writer_initialized = true;
  const char *error = NULL;
  if (rpc_write_js_array(js, &op->writer, args[1], &error) != 0) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "rpc call '%s' has unsupported args: %s", name, error ? error : "invalid value");
    rpc_client_op_free(op);
    return err;
  }
  return rpc_client_enqueue(client, op);
}

static ant_value_t rpc_client_ping(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  rpc_client_t *client = rpc_require_client(js, js_getthis(js));
  if (!client) return js->thrown_value;
  if (client->closed || client->closing) {
    ant_value_t promise = js_mkpromise(js);
    js_reject_promise(js, promise, js_mkerr(js, "RpcClient is closed"));
    return promise;
  }
  rpc_client_op_t *op = calloc(1, sizeof(*op));
  if (!op) return js_mkerr(js, "out of memory");
  op->type = RPC_CLIENT_OP_PING;
  return rpc_client_enqueue(client, op);
}

static ant_value_t rpc_client_close(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  rpc_client_t *client = rpc_require_client(js, js_getthis(js));
  if (!client) return js->thrown_value;
  ant_value_t promise = js_mkpromise(js);
  if (client->closed || client->closing) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }
  rpc_client_op_t *op = calloc(1, sizeof(*op));
  if (!op) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }
  op->type = RPC_CLIENT_OP_CLOSE;
  op->promise = promise;

  pthread_mutex_lock(&client->mutex);
  client->closing = true;
  if (client->tail) client->tail->next = op;
  else client->head = op;
  client->tail = op;
  pthread_cond_signal(&client->cond);
  pthread_mutex_unlock(&client->mutex);
  return promise;
}

static void rpc_client_close_impl(rpc_client_t *client) {
  if (!client || client->closed) return;
  client->closing = true;
  pthread_mutex_lock(&client->mutex);
  pthread_cond_signal(&client->cond);
  pthread_mutex_unlock(&client->mutex);
  if (client->worker_started) {
    pthread_join(client->thread, NULL);
    client->worker_started = false;
  }
  pthread_mutex_lock(&client->mutex);
  rpc_client_op_t *queued = client->head;
  rpc_client_op_t *done = client->done_head;
  client->head = NULL;
  client->tail = NULL;
  client->done_head = NULL;
  client->done_tail = NULL;
  pthread_mutex_unlock(&client->mutex);
  rpc_client_free_op_list(queued, false);
  rpc_client_free_op_list(done, true);
  if (client->client) {
    wirecall_client_close(client->client);
    client->client = NULL;
  }
  if (client->async_initialized && !uv_is_closing((uv_handle_t *)&client->async))
    uv_close((uv_handle_t *)&client->async, rpc_client_async_close_cb);
  rpc_remove_active_client(client);
  client->connected = false;
  client->closed = true;
  client->closing = false;
}

static void rpc_client_finalizer(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  rpc_client_t *client = rpc_client_data(value);
  if (!client) return;
  js_clear_native(value, RPC_CLIENT_NATIVE_TAG);
  rpc_client_close_impl(client);
  pthread_mutex_destroy(&client->mutex);
  pthread_cond_destroy(&client->cond);
  free(client->host);
  free(client->port);
  free(client);
}

static ant_value_t rpc_client_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcClient constructor requires new");
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcClient requires options");

  ant_value_t opts = args[0];
  ant_value_t host_val = js_get(js, opts, "host");
  ant_value_t port_val = js_get(js, opts, "port");
  char *host = vtype(host_val) == T_STR ? rpc_value_to_cstring(js, host_val, "host") : strdup("127.0.0.1");
  if (!host) return js->thrown_exists ? js->thrown_value : js_mkerr(js, "out of memory");
  char port_buf[32];
  if (vtype(port_val) != T_NUM) {
    free(host);
    return js_mkerr_typed(js, JS_ERR_TYPE, "RpcClient options.port must be a number");
  }
  snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)(uint16_t)js_getnum(port_val));

  rpc_client_t *client = calloc(1, sizeof(*client));
  if (!client) {
    free(host);
    return js_mkerr(js, "out of memory");
  }
  client->js = js;
  client->obj = js_getthis(js);
  client->host = host;
  client->port = strdup(port_buf);
  client->integrity = WIRECALL_INTEGRITY_DEFAULT;
  if (!client->port) {
    free(client->host);
    free(client);
    return js_mkerr(js, "out of memory");
  }

  ant_value_t perr = rpc_parse_integrity_options(js, opts, &client->integrity, client->mac_key, &client->has_mac_key);
  if (is_err(perr)) {
    free(client->host);
    free(client->port);
    free(client);
    return perr;
  }

  pthread_mutex_init(&client->mutex, NULL);
  pthread_cond_init(&client->cond, NULL);
  if (uv_async_init(uv_default_loop(), &client->async, rpc_client_async_cb) != 0) {
    pthread_mutex_destroy(&client->mutex);
    pthread_cond_destroy(&client->cond);
    free(client->host);
    free(client->port);
    free(client);
    return js_mkerr(js, "failed to initialize rpc client async handle");
  }
  client->async.data = client;
  client->async_initialized = true;

  if (pthread_create(&client->thread, NULL, rpc_client_worker_main, client) != 0) {
    uv_close((uv_handle_t *)&client->async, rpc_client_async_close_cb);
    pthread_mutex_destroy(&client->mutex);
    pthread_cond_destroy(&client->cond);
    free(client->host);
    free(client->port);
    free(client);
    return js_mkerr(js, "failed to start rpc client worker");
  }
  client->worker_started = true;

  js_set_native(client->obj, client, RPC_CLIENT_NATIVE_TAG);
  js_set_finalizer(client->obj, rpc_client_finalizer);
  rpc_add_active_client(client);
  return client->obj;
}

static void rpc_init_constructors(ant_t *js) {
  if (g_rpc_server_ctor && g_rpc_client_ctor) return;

  ant_value_t object_proto = js_get_ctor_proto(js, "Object", 6);

  g_rpc_server_proto = js_mkobj(js);
  if (is_object_type(object_proto)) js_set_proto_init(g_rpc_server_proto, object_proto);
  js_set(js, g_rpc_server_proto, "register", js_mkfun(rpc_server_register));
  js_set(js, g_rpc_server_proto, "unregister", js_mkfun(rpc_server_unregister));
  js_set(js, g_rpc_server_proto, "listen", js_mkfun(rpc_server_listen));
  js_set(js, g_rpc_server_proto, "close", js_mkfun(rpc_server_close));
  js_set_getter_desc(js, g_rpc_server_proto, "port", 4, js_mkfun(rpc_server_port_getter), JS_DESC_C);
  js_set_sym(js, g_rpc_server_proto, get_toStringTag_sym(), js_mkstr(js, "RpcServer", 9));
  g_rpc_server_ctor = js_make_ctor(js, rpc_server_ctor, g_rpc_server_proto, "RpcServer", 9);

  g_rpc_client_proto = js_mkobj(js);
  if (is_object_type(object_proto)) js_set_proto_init(g_rpc_client_proto, object_proto);
  js_set(js, g_rpc_client_proto, "connect", js_mkfun(rpc_client_connect));
  js_set(js, g_rpc_client_proto, "call", js_mkfun(rpc_client_call));
  js_set(js, g_rpc_client_proto, "ping", js_mkfun(rpc_client_ping));
  js_set(js, g_rpc_client_proto, "close", js_mkfun(rpc_client_close));
  js_set_sym(js, g_rpc_client_proto, get_toStringTag_sym(), js_mkstr(js, "RpcClient", 9));
  g_rpc_client_ctor = js_make_ctor(js, rpc_client_ctor, g_rpc_client_proto, "RpcClient", 9);
}

ant_value_t rpc_library(ant_t *js) {
  rpc_init_constructors(js);
  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "RpcServer", g_rpc_server_ctor);
  js_set(js, lib, "RpcClient", g_rpc_client_ctor);
  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "rpc", 3));
  return lib;
}

void gc_mark_rpc(ant_t *js, gc_mark_fn mark) {
  if (g_rpc_server_proto) mark(js, g_rpc_server_proto);
  if (g_rpc_server_ctor) mark(js, g_rpc_server_ctor);
  if (g_rpc_client_proto) mark(js, g_rpc_client_proto);
  if (g_rpc_client_ctor) mark(js, g_rpc_client_ctor);

  for (rpc_server_t *server = g_active_servers; server; server = server->next_active) {
    mark(js, server->obj);
    if (vtype(server->listen_promise) == T_PROMISE) mark(js, server->listen_promise);
    for (rpc_route_t *route = server->routes; route; route = route->next)
      if (is_callable(route->handler)) mark(js, route->handler);
    for (rpc_deferred_task_t *task = server->tasks_head; task; task = task->next)
      if (task->route && is_callable(task->route->handler)) mark(js, task->route->handler);
  }

  for (rpc_client_t *client = g_active_clients; client; client = client->next_active) {
    mark(js, client->obj);
    pthread_mutex_lock(&client->mutex);
    for (rpc_client_op_t *op = client->head; op; op = op->next)
      if (vtype(op->promise) == T_PROMISE) mark(js, op->promise);
    for (rpc_client_op_t *op = client->done_head; op; op = op->done_next)
      if (vtype(op->promise) == T_PROMISE) mark(js, op->promise);
    pthread_mutex_unlock(&client->mutex);
  }
}

void cleanup_rpc_module(void) {
  while (g_active_servers) rpc_server_close_impl(g_active_servers);
  while (g_active_clients) rpc_client_close_impl(g_active_clients);
  
  g_rpc_server_proto = 0;
  g_rpc_server_ctor = 0;
  g_rpc_client_proto = 0;
  g_rpc_client_ctor = 0;
}
