#include "napi_internal.h"

static ant_napi_env_t *g_napi_env = NULL;
static int64_t g_napi_external_memory = 0;

static const napi_node_version g_napi_node_version = {
  .major = 25,
  .minor = 9,
  .patch = 0,
  .release = "ant",
};

static const char *napi_status_text(napi_status status) {
  switch (status) {
    case napi_ok: return "ok";
    case napi_invalid_arg: return "invalid argument";
    case napi_object_expected: return "object expected";
    case napi_string_expected: return "string expected";
    case napi_name_expected: return "name expected";
    case napi_function_expected: return "function expected";
    case napi_number_expected: return "number expected";
    case napi_boolean_expected: return "boolean expected";
    case napi_array_expected: return "array expected";
    case napi_generic_failure: return "generic failure";
    case napi_pending_exception: return "pending exception";
    case napi_cancelled: return "cancelled";
    case napi_escape_called_twice: return "escape called twice";
    case napi_handle_scope_mismatch: return "handle scope mismatch";
    case napi_callback_scope_mismatch: return "callback scope mismatch";
    case napi_queue_full: return "queue full";
    case napi_closing: return "closing";
    case napi_bigint_expected: return "bigint expected";
    case napi_date_expected: return "date expected";
    case napi_arraybuffer_expected: return "arraybuffer expected";
    case napi_detachable_arraybuffer_expected: return "detachable arraybuffer expected";
    case napi_would_deadlock: return "would deadlock";
    case napi_no_external_buffers_allowed: return "no external buffers allowed";
    case napi_cannot_run_js: return "cannot run js";
    default: return "unknown";
  }
}

static napi_status napi_set_last_raw(napi_env env, napi_status status, const char *message) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv) return status;

  const char *msg = message ? message : napi_status_text(status);
  snprintf(nenv->last_error_msg, sizeof(nenv->last_error_msg), "%s", msg);

  nenv->last_error.error_message = nenv->last_error_msg;
  nenv->last_error.engine_reserved = NULL;
  nenv->last_error.engine_error_code = 0;
  nenv->last_error.error_code = status;

  if (status != napi_ok) {
  }

  return status;
}

napi_status ant_napi_set_last(napi_env env, napi_status status, const char *message) {
  return napi_set_last_raw(env, status, message);
}

ant_napi_env_t *ant_napi_get_or_create_env(ant_t *js) {
  if (!g_napi_env) {
    g_napi_env = (ant_napi_env_t *)calloc(1, sizeof(*g_napi_env));
    if (!g_napi_env) return NULL;
    g_napi_env->version = 8;
    napi_set_last((napi_env)g_napi_env, napi_ok, NULL);
  }
  g_napi_env->js = js;
  return g_napi_env;
}

napi_env ant_napi_get_env(ant_t *js) {
  return (napi_env)ant_napi_get_or_create_env(js);
}

void gc_mark_napi(ant_t *js, gc_mark_fn mark) {
  if (!g_napi_env || g_napi_env->js != js) return;
  ant_napi_env_t *nenv = g_napi_env;

  if (nenv->has_pending_exception)
    mark(js, (ant_value_t)nenv->pending_exception);

  for (struct napi_ref__ *r = nenv->refs; r; r = r->next)
    if (r->refcount > 0) mark(js, r->ref_val);

  for (struct napi_deferred__ *d = nenv->deferreds; d; d = d->next)
    if (!d->settled) mark(js, d->promise_val);

  for (struct napi_threadsafe_function__ *t = nenv->tsfns; t; t = t->next)
    mark(js, t->func_val);
  for (size_t i = 0; i < nenv->handle_slots_len; i++)
    mark(js, nenv->handle_slots[i]);
}

void gc_clear_napi_weak_refs(ant_t *js, bool minor) {
  if (!g_napi_env || g_napi_env->js != js) return;
  ant_napi_env_t *nenv = g_napi_env;

  for (struct napi_ref__ *r = nenv->refs; r; r = r->next) {
    if (r->refcount > 0 || !r->value) continue;
    ant_value_t value = (ant_value_t)r->value;
    ant_object_t *obj = is_object_type(value) ? js_obj_ptr(value) : NULL;
    if (obj && (!minor || obj->flags.generation == 0) && !gc_obj_is_marked(obj))
      r->value = 0;
  }
}

napi_value ant_napi_scope_pin(ant_napi_env_t *nenv, napi_value val) {
  ant_value_t v = (ant_value_t)val;
  if (!nenv || !is_object_type(v)) return val;

  if (nenv->handle_slots_len >= nenv->handle_slots_cap) {
    size_t new_cap = nenv->handle_slots_cap ? nenv->handle_slots_cap * 2 : 256;
    ant_value_t *new_slots = realloc(nenv->handle_slots, new_cap * sizeof(ant_value_t));
    if (!new_slots) return val;
    nenv->handle_slots = new_slots;
    nenv->handle_slots_cap = new_cap;
  }
  nenv->handle_slots[nenv->handle_slots_len++] = v;
  return val;
}

static void napi_mark_pending_exception(napi_env env, napi_value exception) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv) return;
  nenv->has_pending_exception = true;
  nenv->pending_exception = exception;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (nenv->has_pending_exception || nenv->js->thrown_exists)
    return napi_set_last(env, napi_pending_exception, "pending exception");
  js_throw(nenv->js, (ant_value_t)error);
  napi_mark_pending_exception(env, error);
  return napi_set_last_raw(env, napi_ok, NULL);
}

napi_status ant_napi_check_pending_from_result(napi_env env, ant_value_t result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  if (is_err(result) || nenv->js->thrown_exists) {
    napi_mark_pending_exception(
      env,
      nenv->js->thrown_exists ? nenv->js->thrown_value : result
    );
    napi_set_last_raw(env, napi_pending_exception, "pending exception");
    return napi_pending_exception;
  }
  return napi_set_last(env, napi_ok, NULL);
}

napi_status ant_napi_return_pending_if_any(napi_env env) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  if (nenv->has_pending_exception) {
    napi_set_last_raw(env, napi_pending_exception, "pending exception");
    return napi_pending_exception;
  }

  if (nenv->js->thrown_exists) {
    napi_mark_pending_exception(env, (napi_value)nenv->js->thrown_value);
    napi_set_last_raw(env, napi_pending_exception, "pending exception");
    return napi_pending_exception;
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_last_error_info(
  node_api_basic_env env,
  const napi_extended_error_info **result
) {
  if (!env || !result) return napi_invalid_arg;
  *result = &((ant_napi_env_t *)env)->last_error;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_version(
  node_api_basic_env env,
  uint32_t *result
) {
  if (!env || !result) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  *result = 8;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_node_version(
  node_api_basic_env env,
  const napi_node_version **version
) {
  if (!env || !version) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  *version = &g_napi_node_version;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

static napi_status napi_throw_with_message(
  napi_env env,
  js_err_type_t err_type,
  const char *msg
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  ant_value_t error = js_mkerr_typed(nenv->js, err_type, "%s", msg ? msg : "");
  if (is_err(error) && nenv->js->thrown_exists) {
    napi_mark_pending_exception(env, (napi_value)nenv->js->thrown_value);
    return napi_pending_exception;
  }
  if (is_err(error)) return napi_set_last(env, napi_generic_failure, "failed to create error");
  return napi_throw(env, (napi_value)error);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_error(
  napi_env env,
  const char *code,
  const char *msg
) {
  (void)code;
  return napi_throw_with_message(env, JS_ERR_GENERIC, msg ? msg : "");
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_type_error(
  napi_env env,
  const char *code,
  const char *msg
) {
  (void)code;
  return napi_throw_with_message(env, JS_ERR_TYPE, msg ? msg : "");
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_range_error(
  napi_env env,
  const char *code,
  const char *msg
) {
  (void)code;
  return napi_throw_with_message(env, JS_ERR_RANGE, msg ? msg : "");
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_exception_pending(
  napi_env env,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = nenv->has_pending_exception || nenv->js->thrown_exists;
  return napi_set_last_raw(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_and_clear_last_exception(
  napi_env env,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (nenv->has_pending_exception) {
    *result = nenv->pending_exception;
    nenv->has_pending_exception = false;
    nenv->pending_exception = (napi_value)js_mkundef();
  } else if (nenv->js->thrown_exists) {
    *result = NAPI_RETURN(nenv, nenv->js->thrown_value);
    nenv->js->thrown_exists = false;
    nenv->js->thrown_value = js_mkundef();
    nenv->js->thrown_stack = js_mkundef();
  } else {
    *result = NAPI_RETURN(nenv, js_mkundef());
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN void NAPI_CDECL napi_fatal_error(
  const char *location,
  size_t location_len,
  const char *message,
  size_t message_len
) {
  fprintf(
    stderr,
    "N-API fatal error at %.*s: %.*s\n",
    (int)location_len,
    location ? location : "",
    (int)message_len,
    message ? message : ""
  );
  abort();
}

NAPI_EXTERN napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  return napi_throw(env, err);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_run_script(
  napi_env env,
  napi_value script,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)script) != kTypeString) return napi_set_last(env, napi_string_expected, "script must be string");

  size_t len = 0;
  const char *src = js_getstr(nenv->js, (ant_value_t)script, &len);
  if (!src) return napi_set_last(env, napi_string_expected, "script must be string");

  ant_value_t out = js_eval_bytecode_eval(nenv->js, src, len);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_adjust_external_memory(
  node_api_basic_env env,
  int64_t change_in_bytes,
  int64_t *adjusted_value
) {
  if (!env) return napi_invalid_arg;
  g_napi_external_memory += change_in_bytes;
  if (adjusted_value) *adjusted_value = g_napi_external_memory;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_add_env_cleanup_hook(
  node_api_basic_env env,
  napi_cleanup_hook fun,
  void *arg
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !fun) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");

  napi_cleanup_hook_entry_t *entry = (napi_cleanup_hook_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return napi_set_last((napi_env)env, napi_generic_failure, "out of memory");
  entry->hook = fun;
  entry->arg = arg;
  entry->next = nenv->cleanup_hooks;
  nenv->cleanup_hooks = entry;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_env_cleanup_hook(
  node_api_basic_env env,
  napi_cleanup_hook fun,
  void *arg
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !fun) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");

  napi_cleanup_hook_entry_t **pp = &nenv->cleanup_hooks;
  while (*pp) {
    if ((*pp)->hook == fun && (*pp)->arg == arg) {
      napi_cleanup_hook_entry_t *victim = *pp;
      *pp = victim->next;
      free(victim);
      return napi_set_last((napi_env)env, napi_ok, NULL);
    }
    pp = &(*pp)->next;
  }
  return napi_set_last((napi_env)env, napi_invalid_arg, "cleanup hook not found");
}
