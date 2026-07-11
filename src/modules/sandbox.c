#include <compat.h> // IWYU pragma: keep

#include "modules/sandbox.h"
#include "modules/json.h"
#include "modules/timer.h"

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "object.h"
#include "ptr.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/transport.h"
#include "sandbox/vm.h"
#include "silver/engine.h"
#include "modules/symbol.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <uv.h>

#ifndef _WIN32
#include <sys/socket.h>
#endif

enum { SANDBOX_NATIVE_TAG = 0x53424f58u }; // SBOX

typedef struct sandbox_state {
  ant_sandbox_assets_t assets;
  ant_sandbox_launch_options_t launch;
  ant_sandbox_vm_session_t *session;
  
  uint32_t capabilities;
  uint16_t tty_rows;
  uint16_t tty_cols;
  
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  unsigned int cpu_time_ms;
  unsigned long long memory_size;
  ant_sandbox_vm_stats_t last_stats;
  
  bool verbose;
  bool closed;
  bool running;
  bool worker_started;
  bool run_done;
  bool close_requested;
  bool async_initialized;
  bool mutex_initialized;
  
  pthread_t worker;
  pthread_mutex_t mutex;
  uv_async_t async;
  
  ant_t *js;
  ant_value_t self;
  ant_value_t run_promise;
  ant_value_t close_promise;
  
  bool has_run_promise;
  bool has_close_promise;
  uint8_t *run_request;
  size_t run_request_len;
  int run_rc;
  
  ant_sandbox_vm_result_t run_result;
  struct sandbox_message_node *message_head;
  struct sandbox_message_node *message_tail;
  struct sandbox_message_waiter *waiter_head;
  struct sandbox_message_waiter *waiter_tail;
  struct sandbox_state *next_active;
  struct sandbox_state *prev_active;
} sandbox_state_t;

typedef struct sandbox_message_node {
  char *data;
  size_t len;
  struct sandbox_message_node *next;
} sandbox_message_node_t;

typedef struct sandbox_message_waiter {
  ant_value_t promise;
  char *type;
  bool iterator;
  struct sandbox_message_waiter *next;
} sandbox_message_waiter_t;

static sandbox_state_t *g_active_sandboxes = NULL;

static bool sandbox_parse_memory_size(const char *input, unsigned long long *out) {
  if (!input || !input[0] || !out) return false;

  char *end = NULL;
  errno = 0;
  double value = strtod(input, &end);
  if (errno != 0 || end == input || !isfinite(value) || value <= 0) return false;

  while (*end == ' ' || *end == '\t') end++;
  unsigned long long scale = 1;
  if (*end != '\0') {
    if (strcasecmp(end, "kb") == 0 || strcasecmp(end, "kib") == 0) scale = 1024ull;
    else if (strcasecmp(end, "mb") == 0 || strcasecmp(end, "mib") == 0) scale = 1024ull * 1024ull;
    else if (strcasecmp(end, "gb") == 0 || strcasecmp(end, "gib") == 0) scale = 1024ull * 1024ull * 1024ull;
    else return false;
  }

  long double bytes = (long double)value * (long double)scale;
  if (bytes < 1 || bytes > (long double)ULLONG_MAX) return false;
  *out = (unsigned long long)bytes;
  return true;
}

static ant_value_t g_sandbox_proto = 0;
static ant_value_t g_sandbox_ctor = 0;

typedef struct {
  ant_t *js;
  ant_value_t obj;
  uv_poll_t poll;
  bool poll_initialized;
  bool closing;
  unsigned char *input;
  size_t input_len;
  size_t input_cap;
} sandbox_guest_port_t;

static sandbox_guest_port_t g_guest_port = {0};

static uint32_t sandbox_load_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sandbox_guest_port_close_cb(uv_handle_t *handle) {
  sandbox_guest_port_t *port = handle ? handle->data : NULL;
  if (!port) return;
  port->poll_initialized = false;
  free(port->input);
  port->input = NULL;
  port->input_len = 0;
  port->input_cap = 0;
}

static void sandbox_guest_port_close(sandbox_guest_port_t *port) {
  if (!port || port->closing) return;
  port->closing = true;
  if (port->poll_initialized) {
    uv_poll_stop(&port->poll);
    if (!uv_is_closing((uv_handle_t *)&port->poll))
      uv_close((uv_handle_t *)&port->poll, sandbox_guest_port_close_cb);
  }
}

static void sandbox_guest_deliver_message(sandbox_guest_port_t *port, const void *payload, size_t payload_len) {
  if (!port || !port->js || !is_object_type(port->obj)) return;
  ant_t *js = port->js;
  ant_value_t encoded = js_mkstr(js, payload, payload_len);
  ant_value_t message = json_parse_value(js, encoded);
  if (is_err(message)) return;

  ant_value_t handler = js_get(js, port->obj, "onmessage");
  if (!is_callable(handler)) handler = js_get(js, port->obj, "_messageHandler");
  if (!is_callable(handler)) return;

  ant_value_t args[1] = { message };
  sv_vm_call(js->vm, js, handler, port->obj, args, 1, NULL, false);
  js_maybe_drain_microtasks_after_async_settle(js);
}

static bool sandbox_guest_append_input(sandbox_guest_port_t *port, const unsigned char *data, size_t len) {
  if (!port || !data || len == 0) return true;
  if (port->input_len > ANT_SANDBOX_FRAME_MAX_SIZE - len) return false;
  size_t needed = port->input_len + len;
  if (needed > port->input_cap) {
    size_t cap = port->input_cap ? port->input_cap * 2 : 4096;
    while (cap < needed) cap *= 2;
    unsigned char *next = realloc(port->input, cap);
    if (!next) return false;
    port->input = next;
    port->input_cap = cap;
  }
  memcpy(port->input + port->input_len, data, len);
  port->input_len += len;
  return true;
}

static bool sandbox_guest_process_input(sandbox_guest_port_t *port) {
  size_t off = 0;
  while (port->input_len - off >= ANT_SANDBOX_FRAME_HEADER_SIZE) {
    const unsigned char *frame = port->input + off;
    if (memcmp(frame, ANT_SANDBOX_FRAME_MAGIC, 4) != 0 ||
        frame[4] != ANT_SANDBOX_FRAME_VERSION) return false;
    uint32_t payload_len = sandbox_load_u32(frame + 8);
    if (payload_len > ANT_SANDBOX_FRAME_MAX_SIZE - ANT_SANDBOX_FRAME_HEADER_SIZE) return false;
    size_t frame_len = ANT_SANDBOX_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (port->input_len - off < frame_len) break;

    if (frame[5] == ANT_SANDBOX_FRAME_MESSAGE)
      sandbox_guest_deliver_message(port, frame + ANT_SANDBOX_FRAME_HEADER_SIZE, payload_len);
    else if (frame[5] == ANT_SANDBOX_FRAME_CLOSE)
      sandbox_guest_port_close(port);
    else return false;
    off += frame_len;
  }
  if (off > 0) {
    memmove(port->input, port->input + off, port->input_len - off);
    port->input_len -= off;
  }
  return true;
}

static void sandbox_guest_poll_cb(uv_poll_t *handle, int status, int events) {
#ifdef _WIN32
  (void)handle; (void)status; (void)events;
#else
  sandbox_guest_port_t *port = handle ? handle->data : NULL;
  if (!port || status < 0 || !(events & UV_READABLE)) {
    sandbox_guest_port_close(port);
    return;
  }
  unsigned char buf[8192];
  for (;;) {
    ssize_t n = recv(ant_sandbox_transport_fd(), buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      if (!sandbox_guest_append_input(port, buf, (size_t)n) || !sandbox_guest_process_input(port)) {
        sandbox_guest_port_close(port);
        return;
      }
      continue;
    }
    if (n == 0) sandbox_guest_port_close(port);
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      sandbox_guest_port_close(port);
    break;
  }
#endif
}

static ant_value_t sandbox_guest_send(ant_t *js, ant_value_t *args, int nargs) {
  if (!ant_sandbox_is_guest_process()) return js_mkerr_typed(js, JS_ERR_TYPE, "parentPort is only available inside a sandbox");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "parentPort.send(value) requires a value");
  ant_value_t encoded = json_stringify_value(js, args[0]);
  if (is_err(encoded)) return encoded;
  size_t len = 0;
  const char *payload = js_getstr(js, encoded, &len);
  if (!payload || !ant_sandbox_transport_send_frame(ANT_SANDBOX_FRAME_MESSAGE, payload, len))
    return js_mkerr(js, "failed to send sandbox message");
  return js_mkundef();
}

static ant_value_t sandbox_guest_on(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "parentPort.on('message', handler) requires a handler");
  const char *event = js_getstr(js, args[0], NULL);
  if (!event || strcmp(event, "message") != 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "parentPort only supports the 'message' event");
  js_set(js, js->this_val, "_messageHandler", args[1]);
  return js->this_val;
}

static ant_value_t sandbox_guest_close(ant_t *js, ant_value_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  sandbox_guest_port_close(&g_guest_port);
  return js_mkundef();
}

static ant_value_t sandbox_guest_parent_port(ant_t *js) {
  if (!ant_sandbox_is_guest_process()) return js_mkundef();
  if (g_guest_port.js == js && is_object_type(g_guest_port.obj)) return g_guest_port.obj;

  memset(&g_guest_port, 0, sizeof(g_guest_port));
  g_guest_port.js = js;
  g_guest_port.obj = js_mkobj(js);
  js_set(js, g_guest_port.obj, "send", js_mkfun_arity(sandbox_guest_send, 1));
  js_set(js, g_guest_port.obj, "on", js_mkfun_arity(sandbox_guest_on, 2));
  js_set(js, g_guest_port.obj, "close", js_mkfun(sandbox_guest_close));
  js_set(js, g_guest_port.obj, "onmessage", js_mkundef());

#ifndef _WIN32
  int fd = ant_sandbox_transport_fd();
  if (fd >= 0 && uv_poll_init(uv_default_loop(), &g_guest_port.poll, fd) == 0) {
    g_guest_port.poll.data = &g_guest_port;
    g_guest_port.poll_initialized = true;
    if (uv_poll_start(&g_guest_port.poll, UV_READABLE, sandbox_guest_poll_cb) != 0)
      sandbox_guest_port_close(&g_guest_port);
  }
#endif
  return g_guest_port.obj;
}

static inline sandbox_state_t *sandbox_get_state(ant_value_t value) {
  return (sandbox_state_t *)js_get_native(value, SANDBOX_NATIVE_TAG);
}

static void sandbox_add_active(sandbox_state_t *state) {
  if (!state || state->next_active || state->prev_active || g_active_sandboxes == state) return;
  state->next_active = g_active_sandboxes;
  if (g_active_sandboxes) g_active_sandboxes->prev_active = state;
  g_active_sandboxes = state;
}

static void sandbox_remove_active(sandbox_state_t *state) {
  if (!state) return;
  if (state->prev_active) state->prev_active->next_active = state->next_active;
  else if (g_active_sandboxes == state) g_active_sandboxes = state->next_active;
  if (state->next_active) state->next_active->prev_active = state->prev_active;
  state->next_active = NULL;
  state->prev_active = NULL;
}

static void sandbox_free_messages(sandbox_state_t *state) {
  sandbox_message_node_t *node = state ? state->message_head : NULL;
  while (node) {
    sandbox_message_node_t *next = node->next;
    free(node->data);
    free(node);
    node = next;
  }
  if (state) state->message_head = state->message_tail = NULL;
}

static void sandbox_free_waiters(sandbox_state_t *state) {
  sandbox_message_waiter_t *waiter = state ? state->waiter_head : NULL;
  while (waiter) {
    sandbox_message_waiter_t *next = waiter->next;
    free(waiter->type);
    free(waiter);
    waiter = next;
  }
  if (state) state->waiter_head = state->waiter_tail = NULL;
}

static void sandbox_update_stats(sandbox_state_t *state) {
  if (!state || !state->session) return;
  ant_sandbox_vm_stats_t current;
  if (ant_sandbox_vm_session_stats(state->session, &current) == 0)
    state->last_stats = current;
}

static void sandbox_state_destroy(sandbox_state_t *state) {
  if (!state) return;
  sandbox_remove_active(state);
  sandbox_update_stats(state);
  if (state->worker_started) {
    ant_sandbox_vm_session_cancel(state->session);
    pthread_join(state->worker, NULL);
  }
  if (state->async_initialized && !uv_is_closing((uv_handle_t *)&state->async))
    uv_close((uv_handle_t *)&state->async, NULL);
  ant_sandbox_vm_session_destroy(state->session);
  state->session = NULL;
  ant_sandbox_launch_options_cleanup(&state->launch);
  sandbox_free_messages(state);
  sandbox_free_waiters(state);
  free(state->run_request);
  if (state->mutex_initialized) pthread_mutex_destroy(&state->mutex);
  free(state);
}

static void sandbox_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sandbox_state_t *state = sandbox_get_state(value);
  if (!state) return;
  
  js_clear_native(value, SANDBOX_NATIVE_TAG);
  sandbox_state_destroy(state);
}

typedef int (*sandbox_string_cb_t)(
  ant_t *js,
  const char *value,
  void *udata
);

static int sandbox_for_each_string(
  ant_t *js, ant_value_t value,
  const char *name, sandbox_string_cb_t cb,
  void *udata, ant_value_t *error_out
) {
  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return 0;

  if (vtype(value) == T_STR) {
    const char *str = js_getstr(js, value, NULL);
    if (!str) {
      *error_out = js_mkerr(js, "oom");
      return -ENOMEM;
    }
    return cb(js, str, udata);
  }

  if (vtype(value) == T_ARR) {
    ant_offset_t len = js_arr_len(js, value);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t item = js_arr_get(js, value, i);
      if (vtype(item) != T_STR) {
        *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "%s entries must be strings", name);
        return -EINVAL;
      }
      
      const char *str = js_getstr(js, item, NULL);
      if (!str) {
        *error_out = js_mkerr(js, "oom");
        return -ENOMEM;
      }
      
      int rc = cb(js, str, udata);
      if (rc != 0) return rc;
    }
    
    return 0;
  }

  *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "%s must be a string or array of strings", name);
  return -EINVAL;
}

typedef struct {
  sandbox_state_t *state;
  bool readonly;
  char error[512];
} sandbox_mount_parse_t;

typedef struct {
  sandbox_state_t *state;
  char error[512];
} sandbox_forward_parse_t;

static int sandbox_add_mount_option(ant_t *js, const char *value, void *udata) {
  sandbox_mount_parse_t *ctx = udata;
  return ant_sandbox_launch_add_mount(&ctx->state->launch, value, ctx->readonly, ctx->error, sizeof(ctx->error));
}

static int sandbox_add_forward_option(ant_t *js, const char *value, void *udata) {
  sandbox_forward_parse_t *ctx = udata;
  return ant_sandbox_launch_add_forward(&ctx->state->launch, value, ctx->error, sizeof(ctx->error));
}

static ant_value_t sandbox_apply_string_list_option(
  ant_t *js, ant_value_t opts,
  const char *key, sandbox_string_cb_t cb,
  void *udata, const char *type_name
) {
  ant_value_t value = js_get(js, opts, key);
  ant_value_t error = js_mkundef();
  
  int rc = sandbox_for_each_string(js, value, type_name, cb, udata, &error);
  if (rc == 0) return js_mkundef();
  if (vtype(error) != T_UNDEF) return error;
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "invalid %s option", key);
}

static ant_value_t sandbox_apply_options(ant_t *js, sandbox_state_t *state, ant_value_t opts) {
  if (vtype(opts) == T_UNDEF || vtype(opts) == T_NULL) return js_mkundef();

  if (vtype(opts) == T_STR) {
    sandbox_mount_parse_t mount_ctx = { .state = state, .readonly = true };
    const char *opts_str = js_getstr(js, opts, NULL);
    
    if (!opts_str) return js_mkerr(js, "out of memory");
    int rc = sandbox_add_mount_option(js, opts_str, &mount_ctx);
    if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", mount_ctx.error[0] ? mount_ctx.error : "invalid mount option");
    
    return js_mkundef();
  }

  if (!is_object_type(opts)) return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox options must be an object");

  sandbox_mount_parse_t mount_ctx = { .state = state, .readonly = true };
  ant_value_t result = sandbox_apply_string_list_option(js, opts, "mount", sandbox_add_mount_option, &mount_ctx, "mount");
  if (is_err(result)) return mount_ctx.error[0]
    ? js_mkerr_typed(js, JS_ERR_TYPE, "%s", mount_ctx.error)
    : result;

  sandbox_mount_parse_t write_ctx = { .state = state, .readonly = false };
  result = sandbox_apply_string_list_option(js, opts, "write", sandbox_add_mount_option, &write_ctx, "write");
  if (is_err(result)) return write_ctx.error[0]
    ? js_mkerr_typed(js, JS_ERR_TYPE, "%s", write_ctx.error)
    : result;

  sandbox_forward_parse_t forward_ctx = { .state = state };
  result = sandbox_apply_string_list_option(js, opts, "forward", sandbox_add_forward_option, &forward_ctx, "forward");
  if (is_err(result)) return forward_ctx.error[0]
    ? js_mkerr_typed(js, JS_ERR_TYPE, "%s", forward_ctx.error)
    : result;

  ant_value_t cwd = js_get(js, opts, "cwd");
  if (vtype(cwd) != T_UNDEF && vtype(cwd) != T_NULL) {
    if (vtype(cwd) != T_STR) return js_mkerr_typed(js, JS_ERR_TYPE, "cwd must be a string");
    const char *cwd_str = js_getstr(js, cwd, NULL);
    
    if (!cwd_str || cwd_str[0] != '/') return js_mkerr_typed(js, JS_ERR_TYPE, "cwd must be an absolute guest path");
    int written = snprintf(state->launch.guest_cwd, sizeof(state->launch.guest_cwd), "%s", cwd_str);
    
    if (written < 0 || (size_t)written >= sizeof(state->launch.guest_cwd))
      return js_mkerr_typed(js, JS_ERR_RANGE, "cwd is too long");
  }

  ant_value_t verbose = js_get(js, opts, "verbose");
  if (vtype(verbose) != T_UNDEF && vtype(verbose) != T_NULL) state->verbose = js_truthy(js, verbose);

  ant_value_t timeout = js_get(js, opts, "timeoutMs");
  if (vtype(timeout) == T_UNDEF) timeout = js_get(js, opts, "timeout");
  
  if (vtype(timeout) != T_UNDEF && vtype(timeout) != T_NULL) {
    if (vtype(timeout) != T_NUM) return js_mkerr_typed(js, JS_ERR_TYPE, "timeoutMs must be a number");
    double n = js_getnum(timeout);
    if (n < 0 || n > UINT_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "timeoutMs is out of range");
    state->timeout_ms = (unsigned int)n;
  }

  ant_value_t boot_timeout = js_get(js, opts, "bootTimeoutMs");
  if (vtype(boot_timeout) == T_UNDEF) boot_timeout = js_get(js, opts, "bootTimeout");
  if (vtype(boot_timeout) == T_UNDEF) boot_timeout = js_get(js, opts, "requestTimeoutMs");
  if (vtype(boot_timeout) == T_UNDEF) boot_timeout = js_get(js, opts, "requestTimeout");
  
  if (vtype(boot_timeout) != T_UNDEF && vtype(boot_timeout) != T_NULL) {
    if (vtype(boot_timeout) != T_NUM) return js_mkerr_typed(js, JS_ERR_TYPE, "bootTimeoutMs must be a number");
    double n = js_getnum(boot_timeout);
    
    if (n < 0 || n > UINT_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "bootTimeoutMs is out of range");
    state->boot_timeout_ms = (unsigned int)n;
  }

  ant_value_t cpu_time = js_get(js, opts, "cpuTimeMs");
  if (vtype(cpu_time) != T_UNDEF && vtype(cpu_time) != T_NULL) {
    if (vtype(cpu_time) != T_NUM)
      return js_mkerr_typed(js, JS_ERR_TYPE, "cpuTimeMs must be a number");
    double n = js_getnum(cpu_time);
    if (!isfinite(n) || n < 0 || n > UINT_MAX)
      return js_mkerr_typed(js, JS_ERR_RANGE, "cpuTimeMs is out of range");
    state->cpu_time_ms = (unsigned int)n;
  }

  ant_value_t memory_mb = js_get(js, opts, "memoryMb");
  ant_value_t memory = js_get(js, opts, "memory");
  if (vtype(memory_mb) != T_UNDEF && vtype(memory_mb) != T_NULL) {
    if (vtype(memory_mb) != T_NUM) return js_mkerr_typed(js, JS_ERR_TYPE, "memoryMb must be a number");
    double n = js_getnum(memory_mb);
    long double bytes = (long double)n * 1024.0L * 1024.0L;
    if (!isfinite(n) || n <= 0 || bytes > (long double)ULLONG_MAX)
      return js_mkerr_typed(js, JS_ERR_RANGE, "memoryMb is out of range");
    state->memory_size = (unsigned long long)bytes;
  } else if (vtype(memory) != T_UNDEF && vtype(memory) != T_NULL) {
    if (vtype(memory) == T_NUM) {
      double n = js_getnum(memory);
      if (!isfinite(n) || n <= 0 || (long double)n > (long double)ULLONG_MAX)
        return js_mkerr_typed(js, JS_ERR_RANGE, "memory is out of range");
      state->memory_size = (unsigned long long)n;
    } else if (vtype(memory) == T_STR) {
      const char *value = js_getstr(js, memory, NULL);
      if (!sandbox_parse_memory_size(value, &state->memory_size))
        return js_mkerr_typed(js, JS_ERR_TYPE, "memory must be bytes or a size such as '256mb'");
    } else return js_mkerr_typed(js, JS_ERR_TYPE, "memory must be a number or size string");
  }
  if (state->memory_size < ANT_SANDBOX_MIN_MEMORY_SIZE)
    return js_mkerr_typed(js, JS_ERR_RANGE, "memory must be at least 64mb");

  ant_value_t tty = js_get(js, opts, "tty");
  if (vtype(tty) != T_UNDEF && vtype(tty) != T_NULL) {
    if (js_truthy(js, tty)) state->capabilities |= ANT_SANDBOX_CAP_STDOUT_TTY | ANT_SANDBOX_CAP_STDERR_TTY;
    else state->capabilities &= ~(ANT_SANDBOX_CAP_STDOUT_TTY | ANT_SANDBOX_CAP_STDERR_TTY);
  }

  ant_value_t rows = js_get(js, opts, "ttyRows");
  if (vtype(rows) != T_UNDEF && vtype(rows) != T_NULL) {
    if (vtype(rows) != T_NUM) return js_mkerr_typed(js, JS_ERR_TYPE, "ttyRows must be a number");
    double n = js_getnum(rows);
    
    if (n < 1 || n > UINT16_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "ttyRows is out of range");
    state->tty_rows = (uint16_t)n;
  }

  ant_value_t cols = js_get(js, opts, "ttyCols");
  if (vtype(cols) != T_UNDEF && vtype(cols) != T_NULL) {
    if (vtype(cols) != T_NUM) return js_mkerr_typed(js, JS_ERR_TYPE, "ttyCols must be a number");
    double n = js_getnum(cols);
    
    if (n < 1 || n > UINT16_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "ttyCols is out of range");
    state->tty_cols = (uint16_t)n;
  }

  ant_value_t color = js_get(js, opts, "color");
  if (vtype(color) != T_UNDEF && vtype(color) != T_NULL) {
    if (vtype(color) != T_STR) return js_mkerr_typed(js, JS_ERR_TYPE, "color must be 'auto', 'force', 'strip', or 'preserve'");
    const char *policy = js_getstr(js, color, NULL);
    
    if (strcmp(policy, "auto") == 0) {
      // keep the host-derived capability bits
    } else if (strcmp(policy, "force") == 0) {
      state->capabilities &= ~ANT_SANDBOX_CAP_COLOR_STRIP;
      state->capabilities |= ANT_SANDBOX_CAP_COLOR_FORCE;
    } else if (strcmp(policy, "strip") == 0) {
      state->capabilities &= ~ANT_SANDBOX_CAP_COLOR_FORCE;
      state->capabilities |= ANT_SANDBOX_CAP_COLOR_STRIP;
    } else if (strcmp(policy, "preserve") == 0) {
      state->capabilities &= ~(ANT_SANDBOX_CAP_COLOR_FORCE | ANT_SANDBOX_CAP_COLOR_STRIP);
    } else return js_mkerr_typed(js, JS_ERR_TYPE, "color must be 'auto', 'force', 'strip', or 'preserve'");
  }

  return js_mkundef();
}

static ant_value_t sandbox_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox constructor requires 'new'");

  if (ant_sandbox_is_guest_process())
    return js_mkerr_typed(js, JS_ERR_TYPE, "Nested Sandbox creation is not supported inside ant sandbox");

  sandbox_state_t *state = calloc(1, sizeof(*state));
  if (!state) return js_mkerr(js, "out of memory");
  state->js = js;
  state->self = js_mkundef();
  state->run_promise = js_mkundef();
  state->close_promise = js_mkundef();
  if (pthread_mutex_init(&state->mutex, NULL) != 0) {
    free(state);
    return js_mkerr(js, "failed to initialize Sandbox");
  }
  state->mutex_initialized = true;

  ant_sandbox_launch_options_init(&state->launch);
  state->boot_timeout_ms = ANT_SANDBOX_DEFAULT_BOOT_TIMEOUT_MS;
  state->memory_size = ANT_SANDBOX_DEFAULT_MEMORY_SIZE;
  state->capabilities = ant_sandbox_terminal_capabilities(&state->tty_rows, &state->tty_cols);

  char err[512] = { 0 };
  int rc = ant_sandbox_assets_resolve(&state->assets, err, sizeof(err));
  
  if (rc != 0) {
    sandbox_state_destroy(state);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", err[0] ? err : "Failed to resolve sandbox assets");
  }

  ant_value_t result = sandbox_apply_options(js, state, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(result)) {
    sandbox_state_destroy(state);
    return result;
  }

  if (!state->launch.explicit_mounts) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
      sandbox_state_destroy(state);
      return js_mkerr_typed(js, JS_ERR_TYPE, "failed to read current directory");
    }
    
    rc = ant_sandbox_launch_add_default_mount(&state->launch, cwd, err, sizeof(err));
    if (rc != 0) {
      sandbox_state_destroy(state);
      return js_mkerr_typed(js, JS_ERR_TYPE, "%s", err[0] ? err : "failed to add default mount");
    }
  }

  ant_value_t obj = js_mkobj(js);
  state->self = obj;
  ant_value_t proto = js_instance_proto_from_new_target(js, g_sandbox_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, state, SANDBOX_NATIVE_TAG);
  js_set_finalizer(obj, sandbox_finalize);
  
  return obj;
}

static ant_value_t sandbox_rejected(ant_t *js, ant_value_t error) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, error);
  return promise;
}

static sandbox_state_t *sandbox_require_open_state(ant_t *js, ant_value_t *error_out) {
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  if (!state) {
    *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Sandbox");
    return NULL;
  }
  if (state->closed) {
    *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox is closed");
    return NULL;
  }
  return state;
}

typedef struct {
  ant_t *js;
  bool has_result;
  bool has_error;
  ant_value_t result;
  ant_value_t error;
} sandbox_frame_capture_t;

static bool sandbox_capture_frame(uint8_t type, const void *payload, size_t payload_len, void *user) {
  sandbox_frame_capture_t *capture = user;
  if (!capture || !capture->js) return false;

  if (type == ANT_SANDBOX_FRAME_RESULT) {
    ant_value_t result = js_mkundef();
    if (ant_sandbox_decode_result_value(capture->js, payload, payload_len, &result)) {
      capture->result = result;
      capture->has_result = true;
      return true;
    }
    return false;
  }

  if (type == ANT_SANDBOX_FRAME_ERROR) {
    capture->error = ant_sandbox_decode_error_value(capture->js, payload, payload_len);
    capture->has_error = true;
    return true;
  }

  return false;
}

static void sandbox_fill_result_from_rc(ant_sandbox_vm_result_t *result, int rc) {
  if (!result || result->kind != ANT_SANDBOX_VM_RESULT_NONE) return;
  if (rc == 0) return;
  if (rc == -ENOSYS) result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
  else if (rc == -EINVAL) result->kind = ANT_SANDBOX_VM_RESULT_CONFIG_ERROR;
  else if (rc == -ETIMEDOUT) result->kind = ANT_SANDBOX_VM_RESULT_TIMEOUT;
  else if (rc == ANT_SANDBOX_CPU_TIME_LIMIT_CODE) result->kind = ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT;
  else result->kind = ANT_SANDBOX_VM_RESULT_VM_ERROR;
  result->code = rc;
}

static ant_value_t sandbox_vm_result_error(ant_t *js, const ant_sandbox_vm_result_t *result, int rc) {
  ant_sandbox_vm_result_t fallback = {0};
  if (!result || result->kind == ANT_SANDBOX_VM_RESULT_NONE) {
    fallback.code = rc;
    sandbox_fill_result_from_rc(&fallback, rc);
    result = &fallback;
  }

  const char *name = ant_sandbox_vm_result_name(result->kind);
  char message[256];
  
  switch (result->kind) {
    case ANT_SANDBOX_VM_RESULT_GUEST_EXIT:
      snprintf(message, sizeof(message), "sandbox script exited with code %d", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE:
    #if defined(__APPLE__) && !defined(__aarch64__)
      snprintf(message, sizeof(message), "sandbox VM backend requires Apple Silicon");
    #elif defined(_WIN32)
      snprintf(
        message, sizeof(message),
        "native Windows sandbox backend is not implemented; use WSL for sandbox support"
      );
    #else
      snprintf(message, sizeof(message), "sandbox VM backend is not available");
    #endif
      break;
    case ANT_SANDBOX_VM_RESULT_CONFIG_ERROR:
      snprintf(message, sizeof(message), "sandbox VM configuration failed (%d)", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_TIMEOUT:
      snprintf(message, sizeof(message), "sandbox VM timed out");
      break;
    case ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT:
      snprintf(message, sizeof(message), "sandbox VM exceeded its CPU time budget");
      break;
    case ANT_SANDBOX_VM_RESULT_KERNEL_PANIC:
      snprintf(message, sizeof(message), "sandbox kernel panic");
      break;
    case ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR:
      snprintf(message, sizeof(message), "sandbox daemon protocol error (%d)", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR:
      snprintf(message, sizeof(message), "sandbox transport error (%d)", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_CANCELED:
      snprintf(message, sizeof(message), "sandbox VM canceled");
      break;
    case ANT_SANDBOX_VM_RESULT_VM_ERROR:
    case ANT_SANDBOX_VM_RESULT_NONE:
    default:
      snprintf(message, sizeof(message), "sandbox VM failed (%d)", result->code ? result->code : rc);
      break;
  }

  ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE | JS_ERR_NO_STACK, "%s", message);
  ant_value_t err_obj = js_as_obj(err);
  
  js_set(js, err_obj, "name", js_mkstr(js, name, strlen(name)));
  js_set(js, err_obj, "code", js_mknum((double)result->code));
  
  return err;
}

static int sandbox_ensure_session(ant_t *js, sandbox_state_t *state, ant_sandbox_vm_result_t *result) {
  if (state->session) return 0;

  ant_sandbox_vm_config_t config = {
    .image_path = state->assets.image,
    .kernel_path = state->assets.kernel,
    .capabilities = state->capabilities,
    .mounts = state->launch.mounts,
    .mount_count = state->launch.mount_count,
    .network_enabled = true,
    .forwards = state->launch.forwards,
    .forward_count = state->launch.forward_count,
    .cpu_count = 1,
    .memory_size = state->memory_size,
    .timeout_ms = state->timeout_ms,
    .boot_timeout_ms = state->boot_timeout_ms,
    .cpu_time_ms = state->cpu_time_ms,
    .verbose = state->verbose,
    .result = result,
  };

  return ant_sandbox_vm_session_create(&config, &state->session);
}

static ant_value_t sandbox_execute_request(
  ant_t *js, sandbox_state_t *state,
  uint8_t *request, size_t request_len, bool expect_result
) {
  if (!request) 
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "failed to build sandbox request"));

  ant_sandbox_vm_result_t vm_result = {0};
  int rc = sandbox_ensure_session(js, state, &vm_result);
  
  if (rc != 0) {
    free(request);
    sandbox_fill_result_from_rc(&vm_result, rc);
    return sandbox_rejected(js, sandbox_vm_result_error(js, &vm_result, rc));
  }

  sandbox_frame_capture_t capture = {
    .js = js,
    .result = js_mkundef(),
    .error = js_mkundef(),
  };

  ant_sandbox_vm_request_t vm_request = {
    .request_data = request,
    .request_len = request_len,
    .frame_handler = sandbox_capture_frame,
    .frame_handler_user = &capture,
    .result = &vm_result,
  };

  rc = ant_sandbox_vm_session_execute(state->session, &vm_request);
  sandbox_fill_result_from_rc(&vm_result, rc);
  free(request);

  GC_ROOT_SAVE(root_mark, js);
  if (capture.has_result) GC_ROOT_PIN(js, capture.result);
  if (capture.has_error) GC_ROOT_PIN(js, capture.error);

  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_PIN(js, promise);
  if (capture.has_error) js_reject_promise(js, promise, capture.error);
  else if (ant_sandbox_vm_result_is_infrastructure_failure(&vm_result))
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  else if (vm_result.kind == ANT_SANDBOX_VM_RESULT_GUEST_EXIT && vm_result.code != 0)
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  else if (rc != 0)
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  else if (expect_result)
    js_resolve_promise(js, promise, capture.has_result ? capture.result : js_mkundef());
  else js_resolve_promise(js, promise, js_mknum(0));
    
  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

static void sandbox_host_deliver_message(sandbox_state_t *state, const char *payload, size_t payload_len) {
  if (!state || !state->js || !is_object_type(state->self)) return;
  ant_t *js = state->js;
  ant_value_t encoded = js_mkstr(js, payload, payload_len);
  ant_value_t message = json_parse_value(js, encoded);
  if (is_err(message)) return;

  sandbox_message_waiter_t **link = &state->waiter_head;
  while (*link) {
    sandbox_message_waiter_t *waiter = *link;
    bool matches = waiter->type == NULL;
    if (waiter->type) {
      ant_value_t type = is_object_type(message) ? js_get(js, message, "type") : js_mkundef();
      if (vtype(type) == T_STR) {
        const char *value = js_getstr(js, type, NULL);
        matches = value && strcmp(value, waiter->type) == 0;
      }
    }
    if (!matches) {
      link = &waiter->next;
      continue;
    }

    *link = waiter->next;
    ant_value_t value = waiter->iterator ? js_iter_result(js, true, message) : message;
    js_resolve_promise(js, waiter->promise, value);
    free(waiter->type);
    free(waiter);
  }
  state->waiter_tail = NULL;
  for (sandbox_message_waiter_t *waiter = state->waiter_head; waiter; waiter = waiter->next)
    state->waiter_tail = waiter;

  ant_value_t handler = js_get(js, state->self, "onmessage");
  if (!is_callable(handler)) handler = js_get(js, state->self, "_messageHandler");
  if (!is_callable(handler)) return;
  ant_value_t args[1] = { message };
  sv_vm_call(js->vm, js, handler, state->self, args, 1, NULL, false);
}

static void sandbox_finish_waiters(sandbox_state_t *state) {
  if (!state || !state->js) return;
  ant_t *js = state->js;
  sandbox_message_waiter_t *waiter = state->waiter_head;
  state->waiter_head = state->waiter_tail = NULL;
  while (waiter) {
    sandbox_message_waiter_t *next = waiter->next;
    if (waiter->iterator)
      js_resolve_promise(js, waiter->promise, js_iter_result(js, false, js_mkundef()));
    else
      js_reject_promise(js, waiter->promise,
                        js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox message channel closed"));
    free(waiter->type);
    free(waiter);
    waiter = next;
  }
}

static bool sandbox_async_capture_frame(uint8_t type, const void *payload, size_t payload_len, void *user) {
  sandbox_state_t *state = user;
  if (!state || type != ANT_SANDBOX_FRAME_MESSAGE) return false;
  sandbox_message_node_t *node = calloc(1, sizeof(*node));
  if (!node) return true;
  node->data = malloc(payload_len + 1);
  if (!node->data) { free(node); return true; }
  if (payload_len > 0) memcpy(node->data, payload, payload_len);
  node->data[payload_len] = '\0';
  node->len = payload_len;

  pthread_mutex_lock(&state->mutex);
  if (state->message_tail) state->message_tail->next = node;
  else state->message_head = node;
  state->message_tail = node;
  pthread_mutex_unlock(&state->mutex);
  if (state->async_initialized) uv_async_send(&state->async);
  return true;
}

static void *sandbox_run_worker(void *user) {
  sandbox_state_t *state = user;
  ant_sandbox_vm_request_t request = {
    .request_data = state->run_request,
    .request_len = state->run_request_len,
    .frame_handler = sandbox_async_capture_frame,
    .frame_handler_user = state,
    .result = &state->run_result,
  };
  int rc = ant_sandbox_vm_session_execute(state->session, &request);
  sandbox_fill_result_from_rc(&state->run_result, rc);

  pthread_mutex_lock(&state->mutex);
  state->run_rc = rc;
  state->run_done = true;
  pthread_mutex_unlock(&state->mutex);
  if (state->async_initialized) uv_async_send(&state->async);
  return NULL;
}

static void sandbox_async_closed(uv_handle_t *handle) {
  sandbox_state_t *state = handle ? handle->data : NULL;
  if (!state) return;
  state->async_initialized = false;
  sandbox_remove_active(state);
}

static void sandbox_async_cb(uv_async_t *handle) {
  sandbox_state_t *state = handle ? handle->data : NULL;
  if (!state || !state->js) return;
  ant_t *js = state->js;

  pthread_mutex_lock(&state->mutex);
  sandbox_message_node_t *messages = state->message_head;
  state->message_head = state->message_tail = NULL;
  bool done = state->run_done;
  pthread_mutex_unlock(&state->mutex);

  while (messages) {
    sandbox_message_node_t *next = messages->next;
    sandbox_host_deliver_message(state, messages->data, messages->len);
    free(messages->data);
    free(messages);
    messages = next;
  }

  if (!done) {
    js_maybe_drain_microtasks_after_async_settle(js);
    return;
  }

  if (state->worker_started) {
    pthread_join(state->worker, NULL);
    state->worker_started = false;
  }
  free(state->run_request);
  state->run_request = NULL;
  state->run_request_len = 0;
  state->running = false;
  sandbox_finish_waiters(state);

  if (state->has_run_promise) {
    if (ant_sandbox_vm_result_is_infrastructure_failure(&state->run_result) || state->run_rc != 0)
      js_reject_promise(js, state->run_promise, sandbox_vm_result_error(js, &state->run_result, state->run_rc));
    else js_resolve_promise(js, state->run_promise, js_mknum(0));
    state->has_run_promise = false;
    state->run_promise = js_mkundef();
  }

  if (state->close_requested) {
    size_t close_len = 0;
    uint8_t *close_request = ant_sandbox_build_close_request_frame(&close_len);
    ant_sandbox_vm_result_t close_result = {0};
    int close_rc = -ENOMEM;
    if (close_request) {
      ant_sandbox_vm_request_t request = {
        .request_data = close_request,
        .request_len = close_len,
        .result = &close_result,
      };
      close_rc = ant_sandbox_vm_session_execute(state->session, &request);
      free(close_request);
    }
    sandbox_update_stats(state);
    ant_sandbox_vm_session_destroy(state->session);
    state->session = NULL;
    ant_sandbox_launch_options_cleanup(&state->launch);
    if (state->has_close_promise) {
      if (close_rc == 0) js_resolve_promise(js, state->close_promise, js_mkundef());
      else js_reject_promise(js, state->close_promise, sandbox_vm_result_error(js, &close_result, close_rc));
      state->has_close_promise = false;
      state->close_promise = js_mkundef();
    }
  }

  js_maybe_drain_microtasks_after_async_settle(js);
  if (state->async_initialized && !uv_is_closing((uv_handle_t *)&state->async))
    uv_close((uv_handle_t *)&state->async, sandbox_async_closed);
}

static ant_value_t sandbox_run(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_error = js_mkundef();
  sandbox_state_t *state = sandbox_require_open_state(js, &state_error);
  
  if (!state) return sandbox_rejected(js, state_error);
  if (state->running)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox already has a running entry"));
  if (state->async_initialized)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox is still finishing its previous entry"));
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.run(entry, argv?) requires an entry string"));

  char *entry = js_getstr(js, args[0], NULL);
  if (!entry) return sandbox_rejected(js, js_mkerr(js, "out of memory"));
  char **argv = NULL; int argc = 0;

  // TODO: reduce nesting split into helpers
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF && vtype(args[1]) != T_NULL) {
    if (vtype(args[1]) == T_ARR) {
      ant_offset_t len = js_arr_len(js, args[1]);
      if (len > INT32_MAX) return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_RANGE, "argv is too large"));
      argv = calloc((size_t)len + 1, sizeof(*argv));
      if (!argv) return sandbox_rejected(js, js_mkerr(js, "out of memory"));
      argc = (int)len;
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t item = js_arr_get(js, args[1], i);
        if (vtype(item) != T_STR) {
          free(argv);
          return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "argv entries must be strings"));
        }
        argv[i] = js_getstr(js, item, NULL);
        if (!argv[i]) {
          free(argv);
          return sandbox_rejected(js, js_mkerr(js, "out of memory"));
        }
      }
    } else {
      argc = nargs - 1;
      argv = calloc((size_t)argc + 1, sizeof(*argv));
      if (!argv) return sandbox_rejected(js, js_mkerr(js, "out of memory"));
      for (int i = 0; i < argc; i++) {
        if (vtype(args[i + 1]) != T_STR) {
          free(argv);
          return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "argv entries must be strings"));
        }
        argv[i] = js_getstr(js, args[i + 1], NULL);
        if (!argv[i]) {
          free(argv);
          return sandbox_rejected(js, js_mkerr(js, "out of memory"));
        }
      }
    }
  }

  uint16_t forward_ports[ANT_SANDBOX_MAX_FORWARDS];
  for (size_t i = 0; i < state->launch.forward_count; i++)
    forward_ports[i] = state->launch.forwards[i].guest_port;

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_run_request_frame(
    state->launch.guest_cwd, entry, argc, argv,
    state->capabilities, state->tty_rows, state->tty_cols,
    forward_ports, (uint32_t)state->launch.forward_count, &request_len
  ); free(argv);
  
  ant_sandbox_vm_result_t create_result = {0};
  int rc = sandbox_ensure_session(js, state, &create_result);
  if (rc != 0) {
    free(request);
    sandbox_fill_result_from_rc(&create_result, rc);
    return sandbox_rejected(js, sandbox_vm_result_error(js, &create_result, rc));
  }

  state->run_request = request;
  state->run_request_len = request_len;
  state->run_done = false;
  state->run_rc = 0;
  memset(&state->run_result, 0, sizeof(state->run_result));
  state->run_promise = js_mkpromise(js);
  state->has_run_promise = true;
  state->running = true;

  if (uv_async_init(uv_default_loop(), &state->async, sandbox_async_cb) != 0) {
    state->running = false;
    state->has_run_promise = false;
    free(state->run_request);
    state->run_request = NULL;
    return sandbox_rejected(js, js_mkerr(js, "failed to initialize sandbox message channel"));
  }
  state->async.data = state;
  state->async_initialized = true;
  sandbox_add_active(state);

  int thread_rc = pthread_create(&state->worker, NULL, sandbox_run_worker, state);
  if (thread_rc != 0) {
    state->running = false;
    state->has_run_promise = false;
    free(state->run_request);
    state->run_request = NULL;
    uv_close((uv_handle_t *)&state->async, sandbox_async_closed);
    return sandbox_rejected(js, js_mkerr(js, "failed to start sandbox worker"));
  }
  state->worker_started = true;
  return state->run_promise;
}

static ant_value_t sandbox_eval(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_error = js_mkundef();
  sandbox_state_t *state = sandbox_require_open_state(js, &state_error);
  
  if (!state) return sandbox_rejected(js, state_error);
  if (state->running)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.eval() is unavailable while an entry is running"));
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.eval(source) requires a source string"));

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_eval_request_frame(
    state->launch.guest_cwd, js_getstr(js, args[0], NULL),
    state->capabilities, state->tty_rows, state->tty_cols, &request_len
  );
  
  return sandbox_execute_request(js, state, request, request_len, true);
}

static ant_value_t sandbox_close(ant_t *js, ant_value_t *args, int nargs) {
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  ant_value_t promise = js_mkpromise(js);
  
  if (!state || state->closed) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  state->closed = true;
  if (state->running) {
    size_t request_len = 0;
    uint8_t *request = ant_sandbox_build_close_request_frame(&request_len);
    if (!request) {
      js_reject_promise(js, promise, js_mkerr(js, "failed to build sandbox close request"));
      return promise;
    }
    int rc = ant_sandbox_vm_session_send(state->session, request, request_len);
    free(request);
    if (rc != 0) {
      state->closed = false;
      js_reject_promise(js, promise, js_mkerr(js, "failed to close running sandbox"));
      return promise;
    }
    state->close_requested = true;
    state->close_promise = promise;
    state->has_close_promise = true;
    return promise;
  }
  if (!state->session) {
    sandbox_finish_waiters(state);
    ant_sandbox_launch_options_cleanup(&state->launch);
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_close_request_frame(&request_len);
  
  if (!request) {
    sandbox_update_stats(state);
    ant_sandbox_vm_session_destroy(state->session);
    state->session = NULL;
    sandbox_finish_waiters(state);
    ant_sandbox_launch_options_cleanup(&state->launch);
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "failed to build sandbox close request"));
    return promise;
  }

  ant_sandbox_vm_result_t vm_result = {0};
  ant_sandbox_vm_request_t vm_request = {
    .request_data = request,
    .request_len = request_len,
    .result = &vm_result,
  };
  
  int rc = ant_sandbox_vm_session_execute(state->session, &vm_request);
  sandbox_fill_result_from_rc(&vm_result, rc);
  
  free(request);
  sandbox_update_stats(state);
  ant_sandbox_vm_session_destroy(state->session);
  
  state->session = NULL;
  sandbox_finish_waiters(state);
  ant_sandbox_launch_options_cleanup(&state->launch);

  if (rc == 0) js_resolve_promise(js, promise, js_mkundef());
  else js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  
  return promise;
}

static ant_value_t sandbox_send(ant_t *js, ant_value_t *args, int nargs) {
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  if (!state || state->closed) return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox is closed");
  if (!state->running || !state->session)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.send(value) requires a running entry");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.send(value) requires a value");

  ant_value_t encoded = json_stringify_value(js, args[0]);
  if (is_err(encoded)) return encoded;
  size_t payload_len = 0;
  const char *payload = js_getstr(js, encoded, &payload_len);
  size_t frame_len = 0;
  uint8_t *frame = ant_sandbox_build_frame(ANT_SANDBOX_FRAME_MESSAGE, payload, payload_len, &frame_len);
  if (!frame) return js_mkerr(js, "failed to encode sandbox message");
  int rc = ant_sandbox_vm_session_send(state->session, frame, frame_len);
  free(frame);
  if (rc != 0) return js_mkerr(js, "failed to send sandbox message");
  return js_mkundef();
}

static ant_value_t sandbox_wait_for_message(
  ant_t *js, sandbox_state_t *state, const char *type, bool iterator
) {
  ant_value_t promise = js_mkpromise(js);
  if (!state || state->closed) {
    if (iterator)
      js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
    else
      js_reject_promise(
        js, promise,
        js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox message channel is closed")
      );
    return promise;
  }
  if (!state->running) {
    if (iterator)
      js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
    else
      js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox message channel requires a running entry"));
    return promise;
  }

  sandbox_message_waiter_t *waiter = calloc(1, sizeof(*waiter));
  if (!waiter) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }
  if (type) {
    waiter->type = strdup(type);
    if (!waiter->type) {
      free(waiter);
      js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
      return promise;
    }
  }
  waiter->promise = promise;
  waiter->iterator = iterator;
  if (state->waiter_tail) state->waiter_tail->next = waiter;
  else state->waiter_head = waiter;
  state->waiter_tail = waiter;
  sandbox_add_active(state);
  return promise;
}

static ant_value_t sandbox_receive(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return sandbox_wait_for_message(js, sandbox_get_state(js->this_val), NULL, false);
}

static ant_value_t sandbox_once(ant_t *js, ant_value_t *args, int nargs) {
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE,
      "Sandbox.once(type) requires a message type string"));
  const char *type = js_getstr(js, args[0], NULL);
  if (!type) return sandbox_rejected(js, js_mkerr(js, "out of memory"));
  return sandbox_wait_for_message(js, state, type, false);
}

static ant_value_t sandbox_messages_next(ant_t *js, ant_value_t *args, int nargs) {
  return sandbox_wait_for_message(js, sandbox_get_state(js->this_val), NULL, true);
}

static ant_value_t sandbox_messages_getter(ant_t *js, ant_value_t *args, int nargs) {
  return js->this_val;
}

static ant_value_t sandbox_stats(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  if (!state) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Sandbox receiver");

  ant_sandbox_vm_stats_t stats = state->last_stats;
  if (state->session) {
    ant_sandbox_vm_stats_t current;
    if (ant_sandbox_vm_session_stats(state->session, &current) == 0) {
      state->last_stats = current;
      stats = current;
    }
  }

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "cpuTimeMs", js_mknum((double)stats.cpu_time_ns / 1000000.0));
  js_set(js, result, "wallTimeMs", js_mknum((double)stats.wall_time_ns / 1000000.0));
  if (stats.resident_memory_available)
    js_set(js, result, "residentMemory", js_mknum((double)stats.resident_memory_bytes));
  return result;
}

static ant_value_t sandbox_on(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR || !is_callable(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.on('message', handler) requires a handler");
  const char *event = js_getstr(js, args[0], NULL);
  if (!event || strcmp(event, "message") != 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox only supports the 'message' event");
  js_set(js, js->this_val, "_messageHandler", args[1]);
  return js->this_val;
}

static ant_value_t sandbox_terminate(ant_t *js, ant_value_t *args, int nargs) {
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  ant_value_t promise = js_mkpromise(js);
  
  if (!state || state->closed) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  state->closed = true;
  int rc = 0;
  
  if (state->session) {
    sandbox_update_stats(state);
    rc = ant_sandbox_vm_session_cancel(state->session);
    if (state->worker_started) {
      pthread_join(state->worker, NULL);
      state->worker_started = false;
      state->running = false;
    }
    ant_sandbox_vm_session_destroy(state->session);
    state->session = NULL;
  }  ant_sandbox_launch_options_cleanup(&state->launch);

  sandbox_finish_waiters(state);

  if (rc == 0) js_resolve_promise(js, promise, js_mkundef()); else {
    ant_sandbox_vm_result_t result = {
      .kind = rc == -ENOSYS ? ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE : ANT_SANDBOX_VM_RESULT_CANCELED,
      .code = rc,
    }; js_reject_promise(js, promise, sandbox_vm_result_error(js, &result, rc));
  }
  
  return promise;
}

void gc_mark_sandbox(ant_t *js, gc_mark_fn mark) {
  for (sandbox_state_t *state = g_active_sandboxes; state; state = state->next_active) {
    if (state->js != js) continue;
    mark(js, state->self);
    if (state->has_run_promise) mark(js, state->run_promise);
    if (state->has_close_promise) mark(js, state->close_promise);
    for (sandbox_message_waiter_t *waiter = state->waiter_head; waiter; waiter = waiter->next)
      mark(js, waiter->promise);
  }
  if (g_guest_port.js == js && is_object_type(g_guest_port.obj)) mark(js, g_guest_port.obj);
}

ant_value_t sandbox_library(ant_t *js) {
  if (!g_sandbox_ctor) {
    g_sandbox_proto = js_mkobj(js);
    js_set(js, g_sandbox_proto, "run", js_mkfun_arity(sandbox_run, 1));
    js_set(js, g_sandbox_proto, "eval", js_mkfun_arity(sandbox_eval, 1));
    js_set(js, g_sandbox_proto, "send", js_mkfun_arity(sandbox_send, 1));
    js_set(js, g_sandbox_proto, "receive", js_mkfun(sandbox_receive));
    js_set(js, g_sandbox_proto, "once", js_mkfun_arity(sandbox_once, 1));
    js_set(js, g_sandbox_proto, "next", js_mkfun(sandbox_messages_next));
    js_set(js, g_sandbox_proto, "stats", js_mkfun(sandbox_stats));
    js_set(js, g_sandbox_proto, "on", js_mkfun_arity(sandbox_on, 2));
    js_set(js, g_sandbox_proto, "close", js_mkfun(sandbox_close));
    js_set(js, g_sandbox_proto, "terminate", js_mkfun(sandbox_terminate));
    js_set_getter_desc(js, g_sandbox_proto, "messages", 8, js_mkfun(sandbox_messages_getter), JS_DESC_C);
    js_set_sym(js, g_sandbox_proto, get_asyncIterator_sym(), js_mkfun(sym_this_cb));
    js_set_sym(js, g_sandbox_proto, get_toStringTag_sym(), js_mkstr(js, "Sandbox", 7));
    g_sandbox_ctor = js_make_ctor(js, sandbox_ctor, g_sandbox_proto, "Sandbox", 7);
    gc_register_root(&g_sandbox_proto);
    gc_register_root(&g_sandbox_ctor);
  }

  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "Sandbox", g_sandbox_ctor);
  js_set(js, lib, "parentPort", sandbox_guest_parent_port(js));
  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "sandbox", 7));
  
  return lib;
}
