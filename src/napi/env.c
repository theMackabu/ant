#include "gc.h"
#include "internal.h"
#include "gc/modules.h"
#include "silver/vm.h"
#include "napi_internal.h"

static constexpr uint32_t NAPI_VERSION = 8;

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

napi_status Ant_Napi_SetLastError(napi_env env, napi_status status, const char *message) {
  if (!env) return status;

  const char *text = message ? message : napi_status_text(status);
  snprintf(env->last_error_msg, sizeof(env->last_error_msg), "%s", text);

  env->last_error = (napi_extended_error_info){
    .error_message = env->last_error_msg,
    .error_code = status,
  };

  return status;
}

napi_env Ant_Napi_GetEnv(ant_t *js) {
  if (js->napi_env) return js->napi_env;

  napi_env env = calloc(1, sizeof(*env));
  if (!env) return NULL;

  env->js = js;
  env->version = NAPI_VERSION;
  env->external_next_id = 1;
  env->wrap_next_id = 1;
  js->napi_env = env;
  Napi_Ok(env);

  return env;
}

void cleanup_napi_module(ant_t *js) {
  napi_env env = js->napi_env;
  if (!env) return;

  while (env->cleanup_hooks) {
    napi_cleanup_hook_entry_t *entry = env->cleanup_hooks;
    env->cleanup_hooks = entry->next;
    entry->hook(entry->arg);
    free(entry);
  }

  while (env->async_cleanup_hooks) {
    struct napi_async_cleanup_hook_handle__ *handle = env->async_cleanup_hooks;
    env->async_cleanup_hooks = handle->next;
    if (handle->next) handle->next->prev = NULL;
    handle->env = NULL;
    handle->hook(handle, handle->arg);
  }

  Ant_Napi_CloseThreadsafeFunctions(env);
}

void free_napi_module(ant_t *js) {
  napi_env env = js->napi_env;
  if (!env) return;

  if (env->instance_data_finalize_cb)
    env->instance_data_finalize_cb(env, env->instance_data, env->instance_data_finalize_hint);

  for (struct napi_ref__ *r = env->refs, *next; r; r = next) {
    next = r->next;
    free(r);
  }
  for (struct napi_deferred__ *d = env->deferreds, *next; d; d = next) {
    next = d->next;
    free(d);
  }

  napi_external_entry_t *ext, *ext_tmp;
  HASH_ITER(hh, env->externals, ext, ext_tmp) {
    HASH_DEL(env->externals, ext);
    free(ext);
  }
  napi_wrap_entry_t *wrap, *wrap_tmp;
  HASH_ITER(hh, env->wraps, wrap, wrap_tmp) {
    HASH_DEL(env->wraps, wrap);
    free(wrap);
  }

  Ant_Napi_SweepExternalBuffers(env, true);
  for (struct napi_async_cleanup_hook_handle__ *h = env->async_cleanup_hooks; h; h = h->next) h->env = NULL;

  free(env->handle_slots);
  free(env);
  js->napi_env = NULL;
}

static bool napi_ref_weakly_held(ant_value_t value) {
  uint8_t type = vtype(value);
  return type == kTypeFunction || type == kTypeSymbol || ((1u << type) & GC_OBJ_TYPE_MASK) != 0;
}

void gc_mark_napi(ant_t *js, gc_mark_fn mark) {
  napi_env env = js->napi_env;
  if (!env) return;

  if (is_err(env->exception))
    mark(js, env->exception);

  for (struct napi_ref__ *r = env->refs; r; r = r->next) {
    ant_value_t weak = r->value;
    if (r->refcount > 0) mark(js, r->ref_val);
    else if (r->live && !napi_ref_weakly_held(weak) && gc_value_is_heap_ref(weak)) mark(js, weak);
  }

  for (struct napi_deferred__ *d = env->deferreds; d; d = d->next)
    mark(js, d->promise_val);

  for (struct napi_threadsafe_function__ *t = env->tsfns; t; t = t->next)
    mark(js, t->func_val);
  for (size_t i = 0; i < env->handle_slots_len; i++)
    mark(js, env->handle_slots[i]);
}

void gc_clear_napi_weak_refs(ant_t *js, gc_weak_key_alive_fn alive) {
  napi_env env = js->napi_env;
  if (!env) return;

  Ant_Napi_SweepExternalBuffers(env, false);

  for (struct napi_ref__ *r = env->refs; r; r = r->next) {
    if (r->refcount > 0 || !r->live) continue;
    if (napi_ref_weakly_held(r->value) && !alive(js, r->value)) r->live = false;
  }
}

napi_value Ant_Napi_Pin(napi_env env, ant_value_t v) {
  napi_value val = Napi_Encode(v);
  if (!env || !is_object_type(v)) return val;

  if (env->handle_slots_len >= env->handle_slots_cap) {
    size_t new_cap = env->handle_slots_cap ? env->handle_slots_cap * 2 : 256;
    ant_value_t *new_slots = realloc(env->handle_slots, new_cap * sizeof(ant_value_t));
    if (!new_slots) return val;
    env->handle_slots = new_slots;
    env->handle_slots_cap = new_cap;
  }
  env->handle_slots[env->handle_slots_len++] = v;
  return val;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_last_error_info(node_api_basic_env env, const napi_extended_error_info **result) {
  if (!env || !result) return napi_invalid_arg;
  *result = &env->last_error;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_version(node_api_basic_env env, uint32_t *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = env->version;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_node_version(node_api_basic_env env, const napi_node_version **version) {
  if (!env || !version) return Napi_InvalidArg(env);
  *version = &g_napi_node_version;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_run_script(napi_env env, napi_value script, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  if (vtype(Napi_Decode(script)) != kTypeString) return Ant_Napi_SetLastError(env, napi_string_expected, "script must be string");

  size_t len = 0;
  const char *src = js_getstr(env->js, Napi_Decode(script), &len);
  if (!src) return Ant_Napi_SetLastError(env, napi_string_expected, "script must be string");

  ant_value_t out = js_eval_bytecode_eval(env->js, src, len, false);
  if (Napi_Failed(env, out)) return Ant_Napi_CapturePending(env, out);
  *result = Ant_Napi_Pin(env, out);

  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_adjust_external_memory(node_api_basic_env env, int64_t change_in_bytes, int64_t *adjusted_value) {
  if (!env) return napi_invalid_arg;
  env->external_memory += change_in_bytes;
  if (adjusted_value) *adjusted_value = env->external_memory;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_add_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun, void *arg) {
  if (!env || !fun) return Napi_InvalidArg(env);

  napi_cleanup_hook_entry_t *entry = calloc(1, sizeof(*entry));
  if (!entry) return Napi_OutOfMemory(env);
  entry->hook = fun;
  entry->arg = arg;
  entry->next = env->cleanup_hooks;
  env->cleanup_hooks = entry;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun, void *arg) {
  if (!env || !fun) return Napi_InvalidArg(env);

  napi_cleanup_hook_entry_t **pp = &env->cleanup_hooks;
  while (*pp) {
    if ((*pp)->hook == fun && (*pp)->arg == arg) {
      napi_cleanup_hook_entry_t *victim = *pp;
      *pp = victim->next;
      free(victim);
      return Napi_Ok(env);
    }
    pp = &(*pp)->next;
  }
  return Ant_Napi_SetLastError(env, napi_invalid_arg, "cleanup hook not found");
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_add_async_cleanup_hook(
  node_api_basic_env env, napi_async_cleanup_hook hook, void *arg,
  napi_async_cleanup_hook_handle *remove_handle
) {
  if (!env || !hook) return Napi_InvalidArg(env);

  struct napi_async_cleanup_hook_handle__ *handle = calloc(1, sizeof(*handle));
  if (!handle) return Napi_OutOfMemory(env);

  handle->env = env;
  handle->hook = hook;
  handle->arg = arg;
  handle->next = env->async_cleanup_hooks;
  if (handle->next) handle->next->prev = handle;
  env->async_cleanup_hooks = handle;

  if (remove_handle) *remove_handle = handle;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle remove_handle) {
  struct napi_async_cleanup_hook_handle__ *handle = remove_handle;
  if (!handle) return napi_invalid_arg;

  if (handle->env) {
    if (handle->prev) handle->prev->next = handle->next;
    else handle->env->async_cleanup_hooks = handle->next;
    if (handle->next) handle->next->prev = handle->prev;
  }

  free(handle);
  return napi_ok;
}

typedef struct {
  uv_idle_t idle;
  napi_env env;
  napi_finalize finalize_cb;
  void *finalize_data;
  void *finalize_hint;
} napi_posted_finalizer_t;

static void napi_posted_finalizer_close_cb(uv_handle_t *handle) {
  free(handle->data);
}

static void napi_posted_finalizer_cb(uv_idle_t *idle) {
  napi_posted_finalizer_t *posted = (napi_posted_finalizer_t *)idle->data;
  uv_idle_stop(idle);
  posted->finalize_cb(posted->env, posted->finalize_data, posted->finalize_hint);
  uv_close((uv_handle_t *)idle, napi_posted_finalizer_close_cb);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_post_finalizer(
  node_api_basic_env env, napi_finalize finalize_cb, void *finalize_data, void *finalize_hint
) {
  if (!env || !env->js || !env->js->loop || !finalize_cb)
    return Napi_InvalidArg(env);

  napi_posted_finalizer_t *posted = calloc(1, sizeof(*posted));
  if (!posted) return Napi_OutOfMemory(env);

  posted->env = env;
  posted->finalize_cb = finalize_cb;
  posted->finalize_data = finalize_data;
  posted->finalize_hint = finalize_hint;
  posted->idle.data = posted;

  if (uv_idle_init(env->js->loop, &posted->idle) != 0) {
    free(posted);
    return Ant_Napi_SetLastError(env, napi_generic_failure, "failed to schedule finalizer");
  }

  uv_idle_start(&posted->idle, napi_posted_finalizer_cb);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_get_module_file_name(node_api_basic_env env, const char **result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = env->module_file_name;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_set_instance_data(napi_env env, void *data, node_api_basic_finalize finalize_cb, void *finalize_hint) {
  if (!env) return Ant_Napi_SetLastError(env, napi_invalid_arg, "invalid env");

  env->instance_data = data;
  env->instance_data_finalize_cb = finalize_cb;
  env->instance_data_finalize_hint = finalize_hint;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_instance_data(napi_env env, void **data) {
  if (!env || !data) return Napi_InvalidArg(env);

  *data = env->instance_data;
  return Napi_Ok(env);
}
