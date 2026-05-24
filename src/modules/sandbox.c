#include <compat.h> // IWYU pragma: keep

#include "modules/sandbox.h"

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "object.h"
#include "ptr.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "silver/engine.h"
#include "modules/symbol.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  SANDBOX_NATIVE_TAG = 0x53424f58u, // SBOX
};

typedef struct {
  ant_sandbox_assets_t assets;
  ant_sandbox_launch_options_t launch;
  uint32_t capabilities;
  uint16_t tty_rows;
  uint16_t tty_cols;
  ant_sandbox_vm_session_t *session;
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  bool verbose;
  bool closed;
} sandbox_state_t;

static ant_value_t g_sandbox_proto = 0;
static ant_value_t g_sandbox_ctor = 0;

static sandbox_state_t *sandbox_get_state(ant_value_t value) {
  return (sandbox_state_t *)js_get_native(value, SANDBOX_NATIVE_TAG);
}

static void sandbox_state_destroy(sandbox_state_t *state) {
  if (!state) return;
  ant_sandbox_vm_session_destroy(state->session);
  state->session = NULL;
  ant_sandbox_launch_options_cleanup(&state->launch);
  free(state);
}

static void sandbox_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sandbox_state_t *state = sandbox_get_state(value);
  if (!state) return;
  js_clear_native(value, SANDBOX_NATIVE_TAG);
  sandbox_state_destroy(state);
}

typedef int (*sandbox_string_cb_t)(ant_t *js, const char *value, void *udata);

static int sandbox_for_each_string(
  ant_t *js,
  ant_value_t value,
  const char *name,
  sandbox_string_cb_t cb,
  void *udata,
  ant_value_t *error_out
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

static int sandbox_add_mount_option(ant_t *js, const char *value, void *udata) {
  (void)js;
  sandbox_mount_parse_t *ctx = udata;
  return ant_sandbox_launch_add_mount(&ctx->state->launch,
                                      value,
                                      ctx->readonly,
                                      ctx->error,
                                      sizeof(ctx->error));
}

typedef struct {
  sandbox_state_t *state;
  char error[512];
} sandbox_forward_parse_t;

static int sandbox_add_forward_option(ant_t *js, const char *value, void *udata) {
  (void)js;
  sandbox_forward_parse_t *ctx = udata;
  return ant_sandbox_launch_add_forward(&ctx->state->launch,
                                        value,
                                        ctx->error,
                                        sizeof(ctx->error));
}

static ant_value_t sandbox_apply_string_list_option(
  ant_t *js,
  ant_value_t opts,
  const char *key,
  sandbox_string_cb_t cb,
  void *udata,
  const char *type_name
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

  ant_value_t tty = js_get(js, opts, "tty");
  if (vtype(tty) != T_UNDEF && vtype(tty) != T_NULL) {
    if (js_truthy(js, tty)) {
      state->capabilities |= ANT_SANDBOX_CAP_STDOUT_TTY | ANT_SANDBOX_CAP_STDERR_TTY;
    } else {
      state->capabilities &= ~(ANT_SANDBOX_CAP_STDOUT_TTY | ANT_SANDBOX_CAP_STDERR_TTY);
    }
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
      /* Keep the host-derived capability bits. */
    } else if (strcmp(policy, "force") == 0) {
      state->capabilities &= ~ANT_SANDBOX_CAP_COLOR_STRIP;
      state->capabilities |= ANT_SANDBOX_CAP_COLOR_FORCE;
    } else if (strcmp(policy, "strip") == 0) {
      state->capabilities &= ~ANT_SANDBOX_CAP_COLOR_FORCE;
      state->capabilities |= ANT_SANDBOX_CAP_COLOR_STRIP;
    } else if (strcmp(policy, "preserve") == 0) {
      state->capabilities &= ~(ANT_SANDBOX_CAP_COLOR_FORCE | ANT_SANDBOX_CAP_COLOR_STRIP);
    } else {
      return js_mkerr_typed(js, JS_ERR_TYPE, "color must be 'auto', 'force', 'strip', or 'preserve'");
    }
  }

  return js_mkundef();
}

static ant_value_t sandbox_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox constructor requires 'new'");

  sandbox_state_t *state = calloc(1, sizeof(*state));
  if (!state) return js_mkerr(js, "out of memory");

  ant_sandbox_launch_options_init(&state->launch);
  state->boot_timeout_ms = ANT_SANDBOX_DEFAULT_BOOT_TIMEOUT_MS;
  state->capabilities = ant_sandbox_terminal_capabilities(&state->tty_rows, &state->tty_cols);

  char err[512] = { 0 };
  int rc = ant_sandbox_assets_resolve(&state->assets, err, sizeof(err));
  if (rc != 0) {
    sandbox_state_destroy(state);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", err[0] ? err : "failed to resolve sandbox assets");
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
      snprintf(message,
               sizeof(message),
               "native Windows sandbox backend is not implemented; use WSL for sandbox support");
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
    .memory_size = 1024ull * 1024ull * 1024ull,
    .timeout_ms = state->timeout_ms,
    .boot_timeout_ms = state->boot_timeout_ms,
    .verbose = state->verbose,
    .result = result,
  };

  (void)js;
  return ant_sandbox_vm_session_create(&config, &state->session);
}

static ant_value_t sandbox_execute_request(
  ant_t *js,
  sandbox_state_t *state,
  uint8_t *request,
  size_t request_len,
  bool expect_result
) {
  if (!request) return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "failed to build sandbox request"));

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
  if (capture.has_error) {
    js_reject_promise(js, promise, capture.error);
  } else if (ant_sandbox_vm_result_is_infrastructure_failure(&vm_result)) {
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  } else if (vm_result.kind == ANT_SANDBOX_VM_RESULT_GUEST_EXIT && vm_result.code != 0) {
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  } else if (rc != 0) {
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  } else if (expect_result) {
    js_resolve_promise(js, promise, capture.has_result ? capture.result : js_mkundef());
  } else {
    js_resolve_promise(js, promise, js_mknum(0));
  }
  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

static ant_value_t sandbox_run(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_error = js_mkundef();
  sandbox_state_t *state = sandbox_require_open_state(js, &state_error);
  if (!state) return sandbox_rejected(js, state_error);
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.run(entry, argv?) requires an entry string"));

  char *entry = js_getstr(js, args[0], NULL);
  if (!entry) return sandbox_rejected(js, js_mkerr(js, "out of memory"));
  char **argv = NULL;
  int argc = 0;

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
  for (size_t i = 0; i < state->launch.forward_count; i++) {
    forward_ports[i] = state->launch.forwards[i].guest_port;
  }

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_run_request_frame(state->launch.guest_cwd,
                                                         entry,
                                                         argc,
                                                         argv,
                                                         state->capabilities,
                                                         state->tty_rows,
                                                         state->tty_cols,
                                                         forward_ports,
                                                         (uint32_t)state->launch.forward_count,
                                                         &request_len);
  free(argv);
  return sandbox_execute_request(js, state, request, request_len, false);
}

static ant_value_t sandbox_eval(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_error = js_mkundef();
  sandbox_state_t *state = sandbox_require_open_state(js, &state_error);
  if (!state) return sandbox_rejected(js, state_error);
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return sandbox_rejected(js, js_mkerr_typed(js, JS_ERR_TYPE, "Sandbox.eval(source) requires a source string"));

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_eval_request_frame(state->launch.guest_cwd,
                                                          js_getstr(js, args[0], NULL),
                                                          state->capabilities,
                                                          state->tty_rows,
                                                          state->tty_cols,
                                                          &request_len);
  return sandbox_execute_request(js, state, request, request_len, true);
}

static ant_value_t sandbox_close(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  ant_value_t promise = js_mkpromise(js);
  if (!state || state->closed) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  state->closed = true;
  if (!state->session) {
    ant_sandbox_launch_options_cleanup(&state->launch);
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_close_request_frame(&request_len);
  if (!request) {
    ant_sandbox_vm_session_destroy(state->session);
    state->session = NULL;
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
  ant_sandbox_vm_session_destroy(state->session);
  state->session = NULL;
  ant_sandbox_launch_options_cleanup(&state->launch);

  if (rc == 0) js_resolve_promise(js, promise, js_mkundef());
  else js_reject_promise(js, promise, sandbox_vm_result_error(js, &vm_result, rc));
  return promise;
}

static ant_value_t sandbox_terminate(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  sandbox_state_t *state = sandbox_get_state(js->this_val);
  ant_value_t promise = js_mkpromise(js);
  if (!state || state->closed) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  state->closed = true;
  int rc = 0;
  if (state->session) {
    rc = ant_sandbox_vm_session_cancel(state->session);
    ant_sandbox_vm_session_destroy(state->session);
    state->session = NULL;
  }
  ant_sandbox_launch_options_cleanup(&state->launch);

  if (rc == 0) {
    js_resolve_promise(js, promise, js_mkundef());
  } else {
    ant_sandbox_vm_result_t result = {
      .kind = rc == -ENOSYS ? ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE : ANT_SANDBOX_VM_RESULT_CANCELED,
      .code = rc,
    };
    js_reject_promise(js, promise, sandbox_vm_result_error(js, &result, rc));
  }
  return promise;
}

ant_value_t sandbox_library(ant_t *js) {
  if (!g_sandbox_ctor) {
    g_sandbox_proto = js_mkobj(js);
    js_set(js, g_sandbox_proto, "run", js_mkfun_arity(sandbox_run, 1));
    js_set(js, g_sandbox_proto, "eval", js_mkfun_arity(sandbox_eval, 1));
    js_set(js, g_sandbox_proto, "close", js_mkfun(sandbox_close));
    js_set(js, g_sandbox_proto, "terminate", js_mkfun(sandbox_terminate));
    js_set_sym(js, g_sandbox_proto, get_toStringTag_sym(), js_mkstr(js, "Sandbox", 7));
    g_sandbox_ctor = js_make_ctor(js, sandbox_ctor, g_sandbox_proto, "Sandbox", 7);
    gc_register_root(&g_sandbox_proto);
    gc_register_root(&g_sandbox_ctor);
  }

  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "Sandbox", g_sandbox_ctor);
  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "sandbox", 7));
  return lib;
}
