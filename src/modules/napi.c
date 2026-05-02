// TODO: split into multiple files

#include <compat.h> // IWYU pragma: keep

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NAPI_DLOPEN(name, flags) ((void*)LoadLibraryA(name))
#define NAPI_DLSYM(handle, name) ((void*)GetProcAddress((HMODULE)(handle), (name)))
#define NAPI_DLERROR() "LoadLibrary failed"
#define NAPI_RTLD_NOW 0
#define NAPI_RTLD_LOCAL 0
#define NAPI_RTLD_GLOBAL 0
#else
#include <dlfcn.h>
#define NAPI_DLOPEN(name, flags) dlopen((name), (flags))
#define NAPI_DLSYM(handle, name) dlsym((handle), (name))
#define NAPI_DLERROR() dlerror()
#define NAPI_RTLD_NOW RTLD_NOW
#define NAPI_RTLD_LOCAL RTLD_LOCAL
#define NAPI_RTLD_GLOBAL RTLD_GLOBAL
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <uthash.h>
#include <uv.h>

#if defined(__has_include)
#if __has_include(<uchar.h>)
#include <uchar.h>
#else
typedef uint16_t char16_t;
#endif
#else
typedef uint16_t char16_t;
#endif

#include "ant.h"
#include "ptr.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "silver/engine.h"

#include "modules/buffer.h"
#include "modules/date.h"
#include "modules/napi.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "utf8.h"

typedef struct napi_cleanup_hook_entry {
  napi_cleanup_hook hook;
  void *arg;
  struct napi_cleanup_hook_entry *next;
} napi_cleanup_hook_entry_t;

typedef struct ant_napi_env__ {
  ant_t *js;
  napi_extended_error_info last_error;
  char last_error_msg[256];
  bool has_pending_exception;
  napi_value pending_exception;
  uint32_t version;
  void *instance_data;
  node_api_basic_finalize instance_data_finalize_cb;
  void *instance_data_finalize_hint;
  napi_cleanup_hook_entry_t *cleanup_hooks;
  struct napi_ref__ *refs;
  struct napi_deferred__ *deferreds;
  struct napi_threadsafe_function__ *tsfns;
  int open_handle_scopes;
  ant_value_t *handle_slots;
  size_t handle_slots_len;
  size_t handle_slots_cap;
} ant_napi_env_t;

typedef struct napi_callback_binding {
  ant_napi_env_t *env;
  napi_callback cb;
  void *data;
} napi_callback_binding_t;

struct napi_callback_info__ {
  ant_napi_env_t *env;
  const napi_value *argv;
  size_t argc;
  napi_value this_arg;
  napi_value new_target;
  void *data;
};

struct napi_ref__ {
  ant_napi_env_t *env;
  ant_value_t ref_val;
  napi_value value;
  uint32_t refcount;
  struct napi_ref__ *next;
  struct napi_ref__ *prev;
};

struct napi_deferred__ {
  ant_napi_env_t *env;
  ant_value_t promise_val;
  bool settled;
  struct napi_deferred__ *next;
  struct napi_deferred__ *prev;
};

struct napi_handle_scope__ {
  ant_napi_env_t *env;
  size_t gc_root_mark;
  size_t handle_slots_mark;
};

struct napi_escapable_handle_scope__ {
  ant_napi_env_t *env;
  size_t gc_root_mark;
  size_t handle_slots_mark;
  bool escaped;
  ant_value_t escaped_val;
};

struct napi_async_context__ {
  ant_napi_env_t *env;
};

struct napi_callback_scope__ {
  ant_napi_env_t *env;
};

typedef struct napi_external_entry {
  uint64_t id;
  void *data;
  node_api_basic_finalize finalize_cb;
  void *finalize_hint;
  UT_hash_handle hh;
} napi_external_entry_t;

typedef struct napi_wrap_entry {
  uint64_t id;
  void *native_object;
  node_api_basic_finalize finalize_cb;
  void *finalize_hint;
  void *attached_data;
  node_api_basic_finalize attached_finalize_cb;
  void *attached_finalize_hint;
  bool has_wrap;
  UT_hash_handle hh;
} napi_wrap_entry_t;

typedef struct napi_async_work_impl {
  ant_napi_env_t *env;
  napi_async_execute_callback execute;
  napi_async_complete_callback complete;
  void *data;
  uv_work_t req;
  bool queued;
  bool delete_after_complete;
} napi_async_work_impl_t;

typedef struct napi_tsfn_item {
  void *data;
  struct napi_tsfn_item *next;
} napi_tsfn_item_t;

struct napi_threadsafe_function__ {
  ant_napi_env_t *env;
  ant_value_t func_val;
  napi_threadsafe_function_call_js call_js_cb;
  node_api_basic_finalize thread_finalize_cb;
  void *thread_finalize_data;
  void *context;
  size_t max_queue_size;
  size_t queue_size;
  size_t thread_count;
  bool closing;
  bool aborted;
  uv_async_t async;
  uv_mutex_t mutex;
  napi_tsfn_item_t *head;
  napi_tsfn_item_t *tail;
  struct napi_threadsafe_function__ *next;
  struct napi_threadsafe_function__ *prev;
};

typedef napi_value(NAPI_CDECL* napi_register_module_v1_fn)(
  napi_env env, napi_value exports
);

typedef struct napi_native_lib {
  void *handle;
  struct napi_native_lib *next;
} napi_native_lib_t;

typedef enum {
  napi_key_include_prototypes = 0,
  napi_key_own_only = 1,
} napi_key_collection_mode;

typedef enum {
  napi_key_all_properties = 0,
  napi_key_writable = 1 << 0,
  napi_key_enumerable = 1 << 1,
  napi_key_configurable = 1 << 2,
  napi_key_skip_strings = 1 << 3,
  napi_key_skip_symbols = 1 << 4,
} napi_key_filter;

typedef enum {
  napi_key_keep_numbers = 0,
  napi_key_numbers_to_strings = 1,
} napi_key_conversion;

typedef struct {
  uint8_t sign;
  uint8_t pad[3];
  uint32_t limb_count;
  uint32_t limbs[];
} napi_bigint_payload_t;

enum { NAPI_CALLBACK_NATIVE_TAG = 0x4e43424bu }; // NCBK

static ant_napi_env_t *g_napi_env = NULL;
static napi_external_entry_t *g_napi_externals = NULL;
static napi_wrap_entry_t *g_napi_wraps = NULL;

static uint64_t g_napi_external_next_id = 1;
static uint64_t g_napi_wrap_next_id = 1;
static int64_t g_napi_external_memory = 0;

static napi_native_lib_t *g_napi_native_libs = NULL;
static napi_module *g_pending_napi_module = NULL;

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
}}

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

static napi_status napi_set_last(napi_env env, napi_status status, const char *message) {
  return napi_set_last_raw(env, status, message);
}

static ant_napi_env_t *napi_get_or_create_env(ant_t *js) {
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
  return (napi_env)napi_get_or_create_env(js);
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

  for (struct napi_threadsafe_function__ *t = nenv->tsfns; t; t = t->next) mark(js, t->func_val);
  for (size_t i = 0; i < nenv->handle_slots_len; i++) mark(js, nenv->handle_slots[i]);
}

void gc_clear_napi_weak_refs(ant_t *js, bool minor) {
  if (!g_napi_env || g_napi_env->js != js) return;
  ant_napi_env_t *nenv = g_napi_env;
  
  for (struct napi_ref__ *r = nenv->refs; r; r = r->next) {
    if (r->refcount > 0 || !r->value) continue;
    ant_value_t value = (ant_value_t)r->value;
    ant_object_t *obj = is_object_type(value) ? js_obj_ptr(value) : NULL;
    if (obj && (!minor || obj->generation == 0) && !gc_obj_is_marked(obj)) r->value = 0;
  }
}

static inline napi_value napi_scope_pin(ant_napi_env_t *nenv, napi_value val) {
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

#define NAPI_RETURN(nenv, val) napi_scope_pin((nenv), (napi_value)(val))

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

static napi_status napi_check_pending_from_result(napi_env env, ant_value_t result) {
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

static napi_status napi_return_pending_if_any(napi_env env) {
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

static bool napi_slot_get_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t *out) {
  ant_value_t value = js_get_slot(obj, slot);
  if (vtype(value) != T_NUM) return false;
  *out = (uint64_t)js_getnum(value);
  return true;
}

static void napi_slot_set_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t v) {
  js_set_slot(obj, slot, js_mknum((double)v));
}

static napi_external_entry_t *napi_find_external(ant_t *js, napi_value value) {
  if (!is_object_type((ant_value_t)value)) return NULL;
  uint64_t id = 0;
  if (!napi_slot_get_u64(js, (ant_value_t)value, SLOT_NAPI_EXTERNAL_ID, &id)) return NULL;
  napi_external_entry_t *entry = NULL;
  HASH_FIND(hh, g_napi_externals, &id, sizeof(id), entry);
  return entry;
}

static napi_wrap_entry_t *napi_find_wrap(ant_t *js, napi_value value) {
  if (!is_object_type((ant_value_t)value)) return NULL;
  uint64_t id = 0;
  if (!napi_slot_get_u64(js, (ant_value_t)value, SLOT_NAPI_WRAP_ID, &id)) return NULL;
  napi_wrap_entry_t *entry = NULL;
  HASH_FIND(hh, g_napi_wraps, &id, sizeof(id), entry);
  return entry;
}

static bool napi_get_typedarray_data(
  ant_t *js,
  napi_value value,
  TypedArrayData **out
) {
  TypedArrayData *ta = buffer_get_typedarray_data((ant_value_t)value);
  if (!ta || !ta->buffer || ta->buffer->is_detached) return false;
  *out = ta;
  return true;
}

static bool napi_to_ant_typedarray_type(
  napi_typedarray_type in,
  TypedArrayType *out
) {
  switch (in) {
    case napi_int8_array: *out = TYPED_ARRAY_INT8; return true;
    case napi_uint8_array: *out = TYPED_ARRAY_UINT8; return true;
    case napi_uint8_clamped_array: *out = TYPED_ARRAY_UINT8_CLAMPED; return true;
    case napi_int16_array: *out = TYPED_ARRAY_INT16; return true;
    case napi_uint16_array: *out = TYPED_ARRAY_UINT16; return true;
    case napi_int32_array: *out = TYPED_ARRAY_INT32; return true;
    case napi_uint32_array: *out = TYPED_ARRAY_UINT32; return true;
    case napi_float32_array: *out = TYPED_ARRAY_FLOAT32; return true;
    case napi_float64_array: *out = TYPED_ARRAY_FLOAT64; return true;
    case napi_bigint64_array: *out = TYPED_ARRAY_BIGINT64; return true;
    case napi_biguint64_array: *out = TYPED_ARRAY_BIGUINT64; return true;
    default: return false;
  }
}

static napi_typedarray_type napi_from_ant_typedarray_type(TypedArrayType in) {
  switch (in) {
    case TYPED_ARRAY_INT8: return napi_int8_array;
    case TYPED_ARRAY_UINT8: return napi_uint8_array;
    case TYPED_ARRAY_UINT8_CLAMPED: return napi_uint8_clamped_array;
    case TYPED_ARRAY_INT16: return napi_int16_array;
    case TYPED_ARRAY_UINT16: return napi_uint16_array;
    case TYPED_ARRAY_INT32: return napi_int32_array;
    case TYPED_ARRAY_UINT32: return napi_uint32_array;
    case TYPED_ARRAY_FLOAT32: return napi_float32_array;
    case TYPED_ARRAY_FLOAT64: return napi_float64_array;
    case TYPED_ARRAY_BIGINT64: return napi_bigint64_array;
    case TYPED_ARRAY_BIGUINT64: return napi_biguint64_array;
    default: return napi_uint8_array;
  }
}

static int napi_desc_flags(napi_property_attributes attributes) {
  int flags = 0;
  if (attributes & napi_writable) flags |= JS_DESC_W;
  if (attributes & napi_enumerable) flags |= JS_DESC_E;
  if (attributes & napi_configurable) flags |= JS_DESC_C;
  return flags;
}

static ant_value_t napi_callback_trampoline(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t current = js_getcurrentfunc(js);
  napi_callback_binding_t *binding = (napi_callback_binding_t *)js_get_native(current, NAPI_CALLBACK_NATIVE_TAG);
  if (!binding || !binding->cb) return js_mkundef();

  ant_napi_env_t *nenv = binding->env ? binding->env : napi_get_or_create_env(js);
  if (!nenv) return js_mkerr(js, "napi OOM");

  struct napi_callback_info__ info = {
    .env = nenv,
    .argv = (const napi_value *)args,
    .argc = (size_t)(nargs < 0 ? 0 : nargs),
    .this_arg = (napi_value)js_getthis(js),
    .new_target = (napi_value)sv_vm_get_new_target(js->vm, js),
    .data = binding->data,
  };

  napi_value ret = binding->cb((napi_env)nenv, (napi_callback_info)&info);
  if (nenv->has_pending_exception) {
    ant_value_t ex = (ant_value_t)nenv->pending_exception;
    nenv->has_pending_exception = false;
    nenv->pending_exception = (napi_value)js_mkundef();
    return js_throw(js, ex);
  }

  if (js->thrown_exists) {
    return js_throw(js, js->thrown_value);
  }

  if ((ant_value_t)ret == 0) return js_mkundef();
  return (ant_value_t)ret;
}

static napi_status napi_create_function_common(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback cb,
  void *data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !cb || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_callback_binding_t *binding = (napi_callback_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) return napi_set_last(env, napi_generic_failure, "out of memory");

  binding->env = nenv;
  binding->cb = cb;
  binding->data = data;

  ant_value_t fn = js_heavy_mkfun_native(nenv->js, napi_callback_trampoline, binding, NAPI_CALLBACK_NATIVE_TAG);
  js_mark_constructor(fn, true);
  if (utf8name && utf8name[0]) {
    size_t nlen = (length == NAPI_AUTO_LENGTH) ? strlen(utf8name) : length;
    js_set(nenv->js, fn, "name", js_mkstr(nenv->js, utf8name, nlen));
  }

  *result = NAPI_RETURN(nenv, fn);
  return napi_set_last(env, napi_ok, NULL);
}

static ant_value_t napi_make_string(ant_t *js, const char *s, size_t len) {
  if (!s) return js_mkstr(js, "", 0);
  if (len == NAPI_AUTO_LENGTH) len = strlen(s);
  return js_mkstr(js, s, len);
}

static bool napi_checked_add_size(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b) return false;
  *out = a + b;
  return true;
}

static bool napi_checked_mul_size(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) return false;
  *out = a * b;
  return true;
}

static bool napi_make_bigint_limbs(
  ant_t *js,
  const uint32_t *limbs,
  size_t count,
  bool negative,
  ant_value_t *out
) {
  uint32_t zero = 0;
  if (!out) return false;

  if (!limbs || count == 0) {
    limbs = &zero;
    count = 1;
  }

  while (count > 1 && limbs[count - 1] == 0) count--;
  if (count == 1 && limbs[0] == 0) negative = false;
  if (count > UINT32_MAX) return false;

  size_t limbs_bytes = 0;
  if (!napi_checked_mul_size(count, sizeof(uint32_t), &limbs_bytes)) return false;

  size_t payload_size = 0;
  if (!napi_checked_add_size(offsetof(napi_bigint_payload_t, limbs), limbs_bytes, &payload_size)) {
    return false;
  }

  napi_bigint_payload_t *payload = (napi_bigint_payload_t *)js_type_alloc(
    js, ANT_ALLOC_BIGINT, payload_size, 
    _Alignof(napi_bigint_payload_t)
  );
  
  if (!payload) return false;
  payload->sign = negative ? 1 : 0;
  payload->pad[0] = 0;
  payload->pad[1] = 0;
  payload->pad[2] = 0;
  payload->limb_count = (uint32_t)count;
  memcpy(payload->limbs, limbs, limbs_bytes);
  *out = mkval(T_BIGINT, (uint64_t)(uintptr_t)payload);
  
  return true;
}

static const napi_bigint_payload_t *napi_bigint_payload(napi_value value) {
  return (const napi_bigint_payload_t *)(uintptr_t)vdata((ant_value_t)value);
}

static const uint32_t *napi_bigint_limbs(napi_value value, size_t *count) {
  const napi_bigint_payload_t *payload = napi_bigint_payload(value);
  if (!payload) {
    if (count) *count = 0;
    return NULL;
  }

  size_t limb_count = payload->limb_count;
  if (limb_count == 0) limb_count = 1;
  
  while (limb_count > 1 && payload->limbs[limb_count - 1] == 0) limb_count--;
  if (count) *count = limb_count;
  
  return payload->limbs;
}

static bool napi_bigint_is_negative(napi_value value) {
  const napi_bigint_payload_t *payload = napi_bigint_payload(value);
  return payload && payload->sign == 1;
}

static bool napi_bigint_limbs_is_zero(const uint32_t *limbs, size_t count) {
  return count <= 1 && (!limbs || limbs[0] == 0);
}

static uint64_t napi_bigint_low_u64(const uint32_t *limbs, size_t count) {
  uint64_t out = 0;
  if (count > 0 && limbs) out |= (uint64_t)limbs[0];
  if (count > 1 && limbs) out |= ((uint64_t)limbs[1] << 32);
  return out;
}

static bool napi_parse_index_key(const char *str, size_t len, uint32_t *out) {
  if (!str || len == 0) return false;
  if (len > 1 && str[0] == '0') return false;

  uint64_t acc = 0;
  for (size_t i = 0; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') return false;
    acc = (acc * 10) + (uint64_t)(str[i] - '0');
    if (acc > UINT32_MAX) return false;
  }

  if (out) *out = (uint32_t)acc;
  return true;
}

static bool napi_seen_has_key(ant_t *js, ant_value_t seen, ant_value_t key) {
  if (vtype(key) == T_SYMBOL) {
    return lkp_sym(js, seen, (ant_offset_t)vdata(key)) != 0;
  }

  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return str && lkp(js, seen, str, len) != 0;
}

static bool napi_seen_add_key(ant_t *js, ant_value_t seen, ant_value_t key) {
  ant_value_t res = js_setprop(js, seen, key, js_true);
  return !is_err(res);
}

static bool napi_key_passes_filter(const ant_shape_prop_t *prop, napi_key_filter key_filter) {
  if (!prop) return false;
  if ((key_filter & napi_key_writable) && !(prop->attrs & ANT_PROP_ATTR_WRITABLE)) return false;
  if ((key_filter & napi_key_enumerable) && !(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) return false;
  if ((key_filter & napi_key_configurable) && !(prop->attrs & ANT_PROP_ATTR_CONFIGURABLE)) return false;
  return true;
}

static ant_value_t napi_convert_property_key(
  ant_t *js,
  ant_value_t key,
  napi_key_conversion key_conversion
) {
  if (key_conversion != napi_key_keep_numbers || vtype(key) != T_STR) return key;

  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  uint32_t idx = 0;
  if (!str || !napi_parse_index_key(str, len, &idx)) return key;
  return js_mknum((double)idx);
}

static napi_status napi_create_date_common(napi_env env, double time, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_t *js = nenv->js;
  ant_value_t ctor = js_get(js, js_glob(js), "Date");
  if (!is_callable(ctor)) return napi_set_last(env, napi_generic_failure, "Date constructor unavailable");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t argv[1] = {js_mknum(time)};
  ant_value_t saved = js->new_target;
  js->new_target = ctor;
  ant_value_t out = sv_vm_call(js->vm, js, ctor, obj, argv, 1, NULL, true);
  js->new_target = saved;

  if (is_err(out) || js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, is_object_type(out) ? out : obj);
  return napi_set_last(env, napi_ok, NULL);
}

static napi_status napi_make_error_object(
  napi_env env,
  const char *name,
  napi_value msg,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t message = (ant_value_t)msg;
  if (vtype(message) != T_STR) {
    message = coerce_to_str(js, message);
    if (is_err(message)) return napi_check_pending_from_result(env, message);
  }

  ant_value_t err = js_mkobj(js);
  js_set(js, err, "name", js_mkstr(js, name, strlen(name)));
  js_set(js, err, "message", message);

  ant_value_t proto = js_get_ctor_proto(js, name, strlen(name));
  if (is_object_type(proto)) js_set_proto_init(err, proto);

  *result = NAPI_RETURN(nenv, err);
  return napi_set_last(env, napi_ok, NULL);
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

static void napi_tsfn_maybe_finish(struct napi_threadsafe_function__ *tsfn);

static void napi_tsfn_async_cb(uv_async_t *handle) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)handle->data;
  if (!tsfn || !tsfn->env || !tsfn->env->js) return;

  ant_t *js = tsfn->env->js;
  for (;;) {
    napi_tsfn_item_t *item = NULL;

    uv_mutex_lock(&tsfn->mutex);
    if (tsfn->head) {
      item = tsfn->head;
      tsfn->head = item->next;
      if (!tsfn->head) tsfn->tail = NULL;
      tsfn->queue_size--;
    }
    bool done = tsfn->closing && tsfn->queue_size == 0;
    uv_mutex_unlock(&tsfn->mutex);

    if (!item) {
      if (done) napi_tsfn_maybe_finish(tsfn);
      break;
    }

    ant_value_t cb = tsfn->func_val;
    if (tsfn->call_js_cb) {
      tsfn->call_js_cb((napi_env)tsfn->env, (napi_value)cb, tsfn->context, item->data);
    } else if (is_callable(cb)) {
      sv_vm_call(js->vm, js, cb, js_mkundef(), NULL, 0, NULL, false);
    }

    free(item);
  }
}

static void napi_tsfn_close_cb(uv_handle_t *handle) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)handle->data;
  if (!tsfn) return;

  if (tsfn->thread_finalize_cb) {
    tsfn->thread_finalize_cb((napi_env)tsfn->env, tsfn->thread_finalize_data, NULL);
  }

  if (tsfn->env) {
    if (tsfn->prev) tsfn->prev->next = tsfn->next;
    else if (tsfn->env->tsfns == tsfn) tsfn->env->tsfns = tsfn->next;
    if (tsfn->next) tsfn->next->prev = tsfn->prev;
  }

  if (tsfn->env && tsfn->env->js) {
    tsfn->func_val = js_mkundef();
  }

  uv_mutex_destroy(&tsfn->mutex);
  free(tsfn);
}

static void napi_tsfn_maybe_finish(struct napi_threadsafe_function__ *tsfn) {
  if (!tsfn) return;
  uv_close((uv_handle_t *)&tsfn->async, napi_tsfn_close_cb);
}

static void napi_async_work_execute_cb(uv_work_t *req) {
  napi_async_work_impl_t *work = (napi_async_work_impl_t *)req->data;
  if (!work || !work->execute) return;
  work->execute((napi_env)work->env, work->data);
}

static void napi_async_work_after_cb(uv_work_t *req, int status) {
  napi_async_work_impl_t *work = (napi_async_work_impl_t *)req->data;
  if (!work) return;

  work->queued = false;
  if (work->complete) {
    napi_status st = (status == UV_ECANCELED) ? napi_cancelled : napi_ok;
    work->complete((napi_env)work->env, st, work->data);
  }

  if (work->delete_after_complete) free(work);
}

static ant_value_t napi_dlopen_common(ant_t *js, ant_value_t module_obj, const char *filename) {
  napi_env env = ant_napi_get_env(js);
  if (!env) return js_mkerr(js, "napi env allocation failed");

  if (!is_object_type(module_obj)) return js_mkerr(js, "process.dlopen module must be an object");
  if (!filename || !filename[0]) return js_mkerr(js, "process.dlopen filename must be a non-empty string");

  g_pending_napi_module = NULL;
  void *handle = NAPI_DLOPEN(filename, NAPI_RTLD_NOW | NAPI_RTLD_GLOBAL | NAPI_RTLD_LOCAL);
  if (!handle) {
    const char *msg = NAPI_DLERROR();
    return js_mkerr(js, "Failed to load native module '%s': %s", filename, msg ? msg : "unknown");
  }

  napi_register_module_v1_fn reg_fn = (napi_register_module_v1_fn)NAPI_DLSYM(handle, "napi_register_module_v1");
  ant_value_t exports = js_get(js, module_obj, "exports");
  
  if (!is_object_type(exports)) {
    exports = js_mkobj(js);
    js_set(js, module_obj, "exports", exports);
  }

  ant_value_t ret = js_mkundef();
  if (reg_fn) ret = (ant_value_t)reg_fn(env, (napi_value)exports);
  else if (g_pending_napi_module && g_pending_napi_module->nm_register_func) {
    ret = (ant_value_t)g_pending_napi_module->nm_register_func(env, (napi_value)exports);
  } else return js_mkerr(js, "No N-API registration entrypoint found in '%s'", filename);

  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (nenv->has_pending_exception || js->thrown_exists) {
    ant_value_t ex = nenv->has_pending_exception
      ? (ant_value_t)nenv->pending_exception
      : js->thrown_value;
    nenv->has_pending_exception = false;
    nenv->pending_exception = (napi_value)js_mkundef();
    return js_throw(js, ex);
  }

  if (is_object_type(ret)) exports = ret;
  js_set(js, module_obj, "exports", exports);
  js_set(js, module_obj, "loaded", js_true);

  napi_native_lib_t *node = (napi_native_lib_t *)calloc(1, sizeof(*node));
  if (node) {
    node->handle = handle;
    node->next = g_napi_native_libs;
    g_napi_native_libs = node;
  }

  return exports;
}

ant_value_t napi_process_dlopen_js(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.dlopen(module, filename) requires 2 arguments");
  if (!is_object_type(args[0])) return js_mkerr(js, "process.dlopen module must be an object");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "process.dlopen filename must be a string");

  size_t path_len = 0;
  const char *path = js_getstr(js, args[1], &path_len);
  if (!path || path_len == 0) return js_mkerr(js, "process.dlopen filename must be non-empty");

  ant_value_t loaded = napi_dlopen_common(js, args[0], path);
  if (is_err(loaded)) return loaded;
  return js_mkundef();
}

ant_value_t napi_load_native_module(ant_t *js, const char *module_path, ant_value_t ns) {
  if (!module_path) return js_mkerr(js, "native module path is null");

  ant_value_t module_obj = js_mkobj(js);
  ant_value_t exports_obj = js_mkobj(js);
  js_set(js, module_obj, "exports", exports_obj);
  js_set(js, module_obj, "filename", js_mkstr(js, module_path, strlen(module_path)));
  js_set(js, module_obj, "id", js_mkstr(js, module_path, strlen(module_path)));
  js_set(js, module_obj, "loaded", js_false);

  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  ant_value_t dlopen_fn = is_object_type(process_obj) ? js_get(js, process_obj, "dlopen") : js_mkundef();

  if (is_callable(dlopen_fn)) {
    ant_value_t argv[2] = {module_obj, js_mkstr(js, module_path, strlen(module_path))};
    ant_value_t dl_res = sv_vm_call(js->vm, js, dlopen_fn, process_obj, argv, 2, NULL, false);
    if (is_err(dl_res) || js->thrown_exists) return js_throw(js, js->thrown_value);
  } else {
    ant_value_t load_res = napi_dlopen_common(js, module_obj, module_path);
    if (is_err(load_res)) return load_res;
  }

  ant_value_t exports_val = js_get(js, module_obj, "exports");
  if (!is_object_type(ns)) return exports_val;

  setprop_cstr(js, ns, "default", 7, exports_val);
  js_set_slot(ns, SLOT_DEFAULT, exports_val);

  if (!is_object_type(exports_val)) return exports_val;
  ant_iter_t iter = js_prop_iter_begin(js, exports_val);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (key_len == 7 && memcmp(key, "default", 7) == 0) continue;
    setprop_cstr(js, ns, key, key_len, value);
  }
  js_prop_iter_end(&iter);

  return exports_val;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_last_error_info(
  node_api_basic_env env,
  const napi_extended_error_info **result
) {
  if (!env || !result) return napi_invalid_arg;
  *result = &((ant_napi_env_t *)env)->last_error;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mkundef();
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknull();
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_glob(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_bool(value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_mkobj(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_mkarr(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array_with_length(
  napi_env env,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t arr = js_mkarr(nenv->js);
  ant_value_t r = js_setprop(
    nenv->js, arr,
    js_mkstr(nenv->js, "length", 6),
    js_mknum((double)length)
  );
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  *result = NAPI_RETURN(nenv, arr);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum(value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_latin1(
  napi_env env,
  const char *str,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !str) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, napi_make_string(nenv->js, str, length));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(
  napi_env env,
  const char *str,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !str) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, napi_make_string(nenv->js, str, length));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_date(
  napi_env env,
  double time,
  napi_value *result
) {
  return napi_create_date_common(env, time, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_int64(
  napi_env env,
  int64_t value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  uint64_t magnitude = value < 0
    ? (uint64_t)(-(value + 1)) + 1
    : (uint64_t)value;
    
  uint32_t limbs[2] = {
    (uint32_t)(magnitude & 0xffffffffu),
    (uint32_t)(magnitude >> 32)
  };
  
  size_t count = limbs[1] == 0 ? 1 : 2;
  ant_value_t out = js_mkundef();
  
  if (!napi_make_bigint_limbs(nenv->js, limbs, count, value < 0, &out)) {
    return napi_set_last(env, napi_generic_failure, "out of memory");
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_uint64(
  napi_env env,
  uint64_t value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  uint32_t limbs[2] = {
    (uint32_t)(value & 0xffffffffu),
    (uint32_t)(value >> 32)
  };
  size_t count = limbs[1] == 0 ? 1 : 2;

  ant_value_t out = js_mkundef();
  if (!napi_make_bigint_limbs(nenv->js, limbs, count, false, &out)) {
    return napi_set_last(env, napi_generic_failure, "out of memory");
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_words(
  napi_env env,
  int sign_bit,
  size_t word_count,
  const uint64_t *words,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || (word_count > 0 && !words)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  size_t limb_count = 1;
  if (word_count > 0 && !napi_checked_mul_size(word_count, 2, &limb_count)) {
    return napi_set_last(env, napi_invalid_arg, "word count overflow");
  }
  uint32_t *limbs = (uint32_t *)calloc(limb_count, sizeof(uint32_t));
  if (!limbs) return napi_set_last(env, napi_generic_failure, "out of memory");

  if (word_count == 0) limbs[0] = 0;
  else for (size_t i = 0; i < word_count; i++) {
    limbs[i * 2] = (uint32_t)(words[i] & 0xffffffffu);
    limbs[(i * 2) + 1] = (uint32_t)(words[i] >> 32);
  }

  ant_value_t out = js_mkundef();
  bool ok = napi_make_bigint_limbs(nenv->js, limbs, limb_count, sign_bit != 0, &out);
  free(limbs);
  if (!ok) return napi_set_last(env, napi_generic_failure, "out of memory");

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_symbol(
  napi_env env,
  napi_value description,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  const char *desc = NULL;
  if (vtype((ant_value_t)description) == T_STR) {
    desc = js_getstr(nenv->js, (ant_value_t)description, NULL);
  } else if (!is_undefined((ant_value_t)description) && !is_null((ant_value_t)description)) {
    ant_value_t s = coerce_to_str(nenv->js, (ant_value_t)description);
    if (is_err(s) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, s);
    desc = js_getstr(nenv->js, s, NULL);
  }

  *result = NAPI_RETURN(nenv, js_mksym(nenv->js, desc));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_function(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback cb,
  void *data,
  napi_value *result
) {
  return napi_create_function_common(env, utf8name, length, cb, data, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "Error", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_type_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "TypeError", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_range_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "RangeError", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external(
  napi_env env,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  napi_external_entry_t *entry = (napi_external_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");

  entry->id = g_napi_external_next_id++;
  entry->data = data;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  HASH_ADD(hh, g_napi_externals, id, sizeof(entry->id), entry);

  ant_value_t obj = js_mkobj(nenv->js);
  napi_slot_set_u64(nenv->js, obj, SLOT_NAPI_EXTERNAL_ID, entry->id);
  *result = NAPI_RETURN(nenv, obj);

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_promise(
  napi_env env,
  napi_deferred *deferred,
  napi_value *promise
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !deferred || !promise) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  struct napi_deferred__ *def = (struct napi_deferred__ *)calloc(1, sizeof(*def));
  if (!def) return napi_set_last(env, napi_generic_failure, "out of memory");

  ant_value_t p = js_mkpromise(nenv->js);
  def->env = nenv;
  def->promise_val = p;
  def->settled = false;

  def->prev = NULL;
  def->next = nenv->deferreds;
  if (nenv->deferreds) nenv->deferreds->prev = def;
  nenv->deferreds = def;

  *deferred = (napi_deferred)def;
  *promise = (napi_value)p;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_resolve_deferred(
  napi_env env,
  napi_deferred deferred,
  napi_value resolution
) {
  struct napi_deferred__ *def = (struct napi_deferred__ *)deferred;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !def || def->settled) {
    return napi_set_last(env, napi_invalid_arg, "invalid deferred");
  }

  ant_value_t promise = def->promise_val;
  js_resolve_promise(nenv->js, promise, (ant_value_t)resolution);
  def->settled = true;
  def->promise_val = js_mkundef();

  if (def->prev) def->prev->next = def->next;
  else if (nenv->deferreds == def) nenv->deferreds = def->next;
  if (def->next) def->next->prev = def->prev;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reject_deferred(
  napi_env env,
  napi_deferred deferred,
  napi_value rejection
) {
  struct napi_deferred__ *def = (struct napi_deferred__ *)deferred;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !def || def->settled) {
    return napi_set_last(env, napi_invalid_arg, "invalid deferred");
  }

  ant_value_t promise = def->promise_val;
  js_reject_promise(nenv->js, promise, (ant_value_t)rejection);
  def->settled = true;
  def->promise_val = js_mkundef();

  if (def->prev) def->prev->next = def->next;
  else if (nenv->deferreds == def) nenv->deferreds = def->next;
  if (def->next) def->next->prev = def->prev;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer(
  napi_env env,
  size_t length,
  void **data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ArrayBufferData *buf = create_array_buffer_data(length);
  if (!buf) return napi_set_last(env, napi_generic_failure, "allocation failed");

  ant_value_t value = create_typed_array(nenv->js, TYPED_ARRAY_UINT8, buf, 0, length, "Buffer");
  if (is_err(value)) return napi_check_pending_from_result(env, value);

  if (data) *data = buf->data;
  *result = NAPI_RETURN(nenv, value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer_copy(
  napi_env env,
  size_t length,
  const void *data,
  void **result_data,
  napi_value *result
) {
  void *buf_ptr = NULL;
  napi_status st = napi_create_buffer(env, length, &buf_ptr, result);
  if (st != napi_ok) return st;

  if (length > 0 && data && buf_ptr) memcpy(buf_ptr, data, length);
  if (result_data) *result_data = buf_ptr;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_buffer(
  napi_env env,
  size_t length,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  (void)finalize_cb;
  (void)finalize_hint;
  return napi_create_buffer_copy(env, length, data, NULL, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_arraybuffer(
  napi_env env,
  size_t byte_length,
  void **data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ArrayBufferData *ab = create_array_buffer_data(byte_length);
  if (!ab) return napi_set_last(env, napi_generic_failure, "allocation failed");

  ant_value_t ab_obj = create_arraybuffer_obj(nenv->js, ab);
  free_array_buffer_data(ab);

  if (data) *data = ab->data;
  *result = NAPI_RETURN(nenv, ab_obj);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_arraybuffer(
  napi_env env,
  void *external_data,
  size_t byte_length,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  (void)finalize_cb;
  (void)finalize_hint;
  void *out = NULL;
  napi_status st = napi_create_arraybuffer(env, byte_length, &out, result);
  if (st != napi_ok) return st;
  if (external_data && out && byte_length > 0) memcpy(out, external_data, byte_length);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_instance_data(
  napi_env env,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv) return napi_set_last(env, napi_invalid_arg, "invalid env");

  nenv->instance_data = data;
  nenv->instance_data_finalize_cb = finalize_cb;
  nenv->instance_data_finalize_hint = finalize_hint;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_instance_data(
  napi_env env,
  void **data
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !data) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  *data = nenv->instance_data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_typedarray(
  napi_env env,
  napi_typedarray_type type,
  size_t length,
  napi_value arraybuffer,
  size_t byte_offset,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)arraybuffer)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ArrayBufferData *ab = buffer_get_arraybuffer_data((ant_value_t)arraybuffer);
  if (!ab || ab->is_detached) return napi_set_last(env, napi_arraybuffer_expected, "invalid arraybuffer");

  TypedArrayType ta_type;
  if (!napi_to_ant_typedarray_type(type, &ta_type)) {
    return napi_set_last(env, napi_invalid_arg, "invalid typedarray type");
  }

  size_t element_size = 1;
  switch (ta_type) {
    case TYPED_ARRAY_INT16:
    case TYPED_ARRAY_UINT16: element_size = 2; break;
    case TYPED_ARRAY_INT32:
    case TYPED_ARRAY_UINT32:
    case TYPED_ARRAY_FLOAT32: element_size = 4; break;
    case TYPED_ARRAY_FLOAT64:
    case TYPED_ARRAY_BIGINT64:
    case TYPED_ARRAY_BIGUINT64: element_size = 8; break;
    default: break;
  }

  size_t byte_len = length * element_size;
  if (byte_offset + byte_len > ab->length) {
    return napi_set_last(env, napi_invalid_arg, "typedarray out of bounds");
  }

  ant_value_t out = create_typed_array_with_buffer(
    nenv->js,
    ta_type,
    ab,
    byte_offset,
    length,
    buffer_typedarray_type_name(ta_type),
    (ant_value_t)arraybuffer
  );

  if (is_err(out)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(
  napi_env env,
  napi_value value,
  uint32_t initial_refcount,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  struct napi_ref__ *ref = (struct napi_ref__ *)calloc(1, sizeof(*ref));
  if (!ref) return napi_set_last(env, napi_generic_failure, "out of memory");

  ref->env = nenv;
  ref->value = value;
  ref->refcount = initial_refcount;
  ref->ref_val = (initial_refcount > 0) ? (ant_value_t)value : js_mkundef();

  ref->prev = NULL;
  ref->next = nenv->refs;
  if (nenv->refs) nenv->refs->prev = ref;
  nenv->refs = ref;

  *result = (napi_ref)ref;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_reference(
  node_api_basic_env env,
  napi_ref ref
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");

  if (r->prev) r->prev->next = r->next;
  else if (nenv->refs == r) nenv->refs = r->next;
  if (r->next) r->next->prev = r->prev;

  r->ref_val = js_mkundef();
  free(r);
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_ref(
  napi_env env,
  napi_ref ref,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  
  if (!nenv || !nenv->js || !r) 
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  
  if (!r->value) {
    if (result) *result = 0;
    return napi_set_last(env, napi_ok, NULL);
  }

  if (r->refcount == 0) r->ref_val = (ant_value_t)r->value;
  r->refcount++;
  if (result) *result = r->refcount;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_unref(
  napi_env env,
  napi_ref ref,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (r->refcount == 0) return napi_set_last(env, napi_invalid_arg, "reference count already zero");

  r->refcount--;
  if (r->refcount == 0) r->ref_val = js_mkundef();
  if (result) *result = r->refcount;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_reference_value(
  napi_env env,
  napi_ref ref,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (r->refcount > 0) *result = NAPI_RETURN(nenv, r->ref_val);
  else *result = r->value ? NAPI_RETURN(nenv, r->value) : 0;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_double(
  napi_env env,
  napi_value value,
  double *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_NUM) return napi_set_last(env, napi_number_expected, "number expected");
  *result = js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int32(
  napi_env env,
  napi_value value,
  int32_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_NUM) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (int32_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_uint32(
  napi_env env,
  napi_value value,
  uint32_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_NUM) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (uint32_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int64(
  napi_env env,
  napi_value value,
  int64_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_NUM) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (int64_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bool(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_BOOL) return napi_set_last(env, napi_boolean_expected, "boolean expected");
  *result = ((ant_value_t)value == js_true);
  return napi_set_last(env, napi_ok, NULL);
}

static napi_status napi_get_string_common(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (vtype((ant_value_t)value) != T_STR) return napi_set_last(env, napi_string_expected, "string expected");

  size_t len = 0;
  const char *str = js_getstr(nenv->js, (ant_value_t)value, &len);
  if (!str) return napi_set_last(env, napi_string_expected, "string expected");

  if (result) *result = len;
  if (!buf || bufsize == 0) return napi_set_last(env, napi_ok, NULL);

  size_t n = (len < (bufsize - 1)) ? len : (bufsize - 1);
  memcpy(buf, str, n);
  buf[n] = '\0';
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf8(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  return napi_get_string_common(env, value, buf, bufsize, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_latin1(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  return napi_get_string_common(env, value, buf, bufsize, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf16(
  napi_env env,
  napi_value value,
  char16_t *buf,
  size_t bufsize,
  size_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (vtype((ant_value_t)value) != T_STR) return napi_set_last(env, napi_string_expected, "string expected");

  size_t byte_len = 0;
  const char *str = js_getstr(nenv->js, (ant_value_t)value, &byte_len);
  if (!str) return napi_set_last(env, napi_string_expected, "string expected");

  size_t utf16_len = utf16_strlen(str, byte_len);
  if (result) *result = utf16_len;
  if (!buf || bufsize == 0) return napi_set_last(env, napi_ok, NULL);

  size_t n = utf16_len < (bufsize - 1) ? utf16_len : (bufsize - 1);
  for (size_t i = 0; i < n; i++) buf[i] = (char16_t)utf16_code_unit_at(str, byte_len, i);
  buf[n] = 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_date_value(
  napi_env env,
  napi_value value,
  double *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_date_instance((ant_value_t)value)) return napi_set_last(env, napi_date_expected, "date expected");

  ant_value_t time_val = js_get_slot((ant_value_t)value, SLOT_DATA);
  *result = vtype(time_val) == T_NUM ? js_getnum(time_val) : JS_NAN;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_int64(
  napi_env env,
  napi_value value,
  int64_t *result,
  bool *lossless
) {
  if (!env || !result || !lossless) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_BIGINT) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  uint64_t magnitude = napi_bigint_low_u64(limbs, limb_count);
  bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);
  uint64_t bits = negative ? (uint64_t)(~magnitude + 1) : magnitude;

  *result = (int64_t)bits;
  *lossless = limb_count <= 2
    && ((!negative && magnitude <= (uint64_t)INT64_MAX)
      || (negative && magnitude <= (UINT64_C(1) << 63)));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_uint64(
  napi_env env,
  napi_value value,
  uint64_t *result,
  bool *lossless
) {
  if (!env || !result || !lossless) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_BIGINT) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  uint64_t magnitude = napi_bigint_low_u64(limbs, limb_count);
  bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);

  *result = negative ? (uint64_t)(~magnitude + 1) : magnitude;
  *lossless = !negative && limb_count <= 2;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_words(
  napi_env env,
  napi_value value,
  int *sign_bit,
  size_t *word_count,
  uint64_t *words
) {
  if (!env || !word_count) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != T_BIGINT) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  size_t actual_words = limb_count == 0 ? 0 : (limb_count + 1) / 2;
  size_t capacity = words ? *word_count : 0;

  if (sign_bit) {
    bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);
    *sign_bit = negative ? 1 : 0;
  }

  if (words) {
  size_t n = capacity < actual_words ? capacity : actual_words;
  for (size_t i = 0; i < n; i++) {
    uint64_t lo = i * 2 < limb_count ? (uint64_t)limbs[i * 2] : 0;
    uint64_t hi = (i * 2 + 1) < limb_count ? (uint64_t)limbs[i * 2 + 1] : 0;
    words[i] = lo | (hi << 32);
  }}

  *word_count = actual_words;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_external(
  napi_env env,
  napi_value value,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  napi_external_entry_t *entry = napi_find_external(nenv->js, value);
  if (!entry) return napi_set_last(env, napi_invalid_arg, "not an external");
  *result = entry->data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_cb_info(
  napi_env env,
  napi_callback_info cbinfo,
  size_t *argc,
  napi_value *argv,
  napi_value *this_arg,
  void **data
) {
  struct napi_callback_info__ *info = (struct napi_callback_info__ *)cbinfo;
  if (!env || !info) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (argc) {
    size_t requested = *argc;
    if (argv) {
      size_t ncopy = requested < info->argc ? requested : info->argc;
      if (ncopy > 0) memcpy(argv, info->argv, ncopy * sizeof(napi_value));
      for (size_t i = ncopy; i < requested; i++) argv[i] = (napi_value)js_mkundef();
    }
    *argc = info->argc;
  }

  if (this_arg) *this_arg = info->this_arg;
  if (data) *data = info->data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_new_target(
  napi_env env,
  napi_callback_info cbinfo,
  napi_value *result
) {
  struct napi_callback_info__ *info = (struct napi_callback_info__ *)cbinfo;
  if (!env || !info || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t nt = (ant_value_t)info->new_target;
  *result = is_undefined(nt) ? (napi_value)0 : info->new_target;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_array_length(
  napi_env env,
  napi_value value,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t v = (ant_value_t)value;
  if (vtype(v) == T_ARR) {
    *result = (uint32_t)js_arr_len(nenv->js, v);
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t len = js_get(nenv->js, v, "length");
  if (is_err(len) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, len);
  if (vtype(len) != T_NUM) return napi_set_last(env, napi_array_expected, "array expected");
  *result = (uint32_t)js_getnum(len);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_buffer_info(
  napi_env env,
  napi_value value,
  void **data,
  size_t *length
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, value, &ta)) {
    return napi_set_last(env, napi_invalid_arg, "not a buffer");
  }

  if (data) *data = ta->buffer->data + ta->byte_offset;
  if (length) *length = ta->byte_length;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_arraybuffer_info(
  napi_env env,
  napi_value arraybuffer,
  void **data,
  size_t *byte_length
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (!is_object_type((ant_value_t)arraybuffer)) return napi_set_last(env, napi_arraybuffer_expected, "arraybuffer expected");

  ArrayBufferData *ab = buffer_get_arraybuffer_data((ant_value_t)arraybuffer);
  if (!ab || ab->is_detached) return napi_set_last(env, napi_arraybuffer_expected, "arraybuffer expected");

  if (data) *data = ab->data;
  if (byte_length) *byte_length = ab->length;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_typedarray_info(
  napi_env env,
  napi_value typedarray,
  napi_typedarray_type *type,
  size_t *length,
  void **data,
  napi_value *arraybuffer,
  size_t *byte_offset
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, typedarray, &ta)) {
    return napi_set_last(env, napi_invalid_arg, "typedarray expected");
  }

  if (type) *type = napi_from_ant_typedarray_type(ta->type);
  if (length) *length = ta->length;
  if (data) *data = ta->buffer->data + ta->byte_offset;
  if (arraybuffer) {
    ant_value_t buffer = js_get(nenv->js, (ant_value_t)typedarray, "buffer");
    if (is_err(buffer) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, buffer);
    *arraybuffer = NAPI_RETURN(nenv, buffer);
  }
  if (byte_offset) *byte_offset = ta->byte_offset;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_prototype(
  napi_env env,
  napi_value object,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t proto = js_get_proto(nenv->js, (ant_value_t)object);
  if (is_err(proto) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, proto);
  *result = NAPI_RETURN(nenv, proto);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(
  napi_env env,
  napi_value object,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t out = js_mkarr(nenv->js);
  ant_iter_t iter = js_prop_iter_begin(nenv->js, (ant_value_t)object);
  const char *key = NULL;
  size_t key_len = 0;

  while (js_prop_iter_next(&iter, &key, &key_len, NULL)) {
    js_arr_push(nenv->js, out, js_mkstr(nenv->js, key, key_len));
  }
  js_prop_iter_end(&iter);

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_all_property_names(
  napi_env env,
  napi_value object,
  napi_key_collection_mode key_mode,
  napi_key_filter key_filter,
  napi_key_conversion key_conversion,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t out = js_mkarr(js);
  ant_value_t seen = js_mkobj(js);
  ant_value_t current = (ant_value_t)object;

  while (is_object_type(current)) {
    ant_iter_t iter = js_prop_iter_begin(js, current);
    ant_value_t key = js_mkundef();
    
    while (js_prop_iter_next_val(&iter, &key, NULL)) {
    ant_object_t *obj_ptr = js_obj_ptr(js_as_obj(current));
    if (!obj_ptr || !obj_ptr->shape || iter.off == 0) continue;
    
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj_ptr->shape, (uint32_t)(iter.off - 1));
    if (!napi_key_passes_filter(prop, key_filter)) continue;
    
    uint8_t key_type = vtype(key);
    if ((key_filter & napi_key_skip_strings) && key_type != T_SYMBOL) continue;
    if ((key_filter & napi_key_skip_symbols) && key_type == T_SYMBOL) continue;
    if (napi_seen_has_key(js, seen, key)) continue;
    if (!napi_seen_add_key(js, seen, key)) {
      js_prop_iter_end(&iter);
      return napi_set_last(env, napi_generic_failure, "failed to collect property names");
    }
    
    js_arr_push(js, out, napi_convert_property_key(js, key, key_conversion));
    if (js->thrown_exists) {
      js_prop_iter_end(&iter);
      return napi_check_pending_from_result(env, js_mkundef());
    }}
    
    js_prop_iter_end(&iter);
    if (key_mode == napi_key_own_only) break;
    current = js_get_proto(js, current);
    if (is_err(current) || js->thrown_exists) return napi_check_pending_from_result(env, current);
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
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

NAPI_EXTERN napi_status NAPI_CDECL napi_set_property(
  napi_env env,
  napi_value object,
  napi_value key,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t r = js_setprop(nenv->js, (ant_value_t)object, (ant_value_t)key, (ant_value_t)value);
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(
  napi_env env,
  napi_value object,
  napi_value key,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == T_SYMBOL) {
    ant_offset_t off = lkp_sym_proto(nenv->js, (ant_value_t)object, (ant_offset_t)vdata(k));
    ant_value_t out = off ? js_propref_load(nenv->js, off) : js_mkundef();
    if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
    *result = NAPI_RETURN(nenv, out);
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr)) return napi_check_pending_from_result(env, kstr);
  
  size_t klen = 0;
  const char *ks = js_getstr(nenv->js, kstr, &klen);
  if (!ks) return napi_set_last(env, napi_string_expected, "string expected");

  char *name = (char *)malloc(klen + 1);
  if (!name) return napi_set_last(env, napi_generic_failure, "out of memory");
  memcpy(name, ks, klen);
  name[klen] = '\0';
  
  ant_value_t out = js_getprop_fallback(nenv->js, (ant_value_t)object, name);
  free(name);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == T_SYMBOL) {
    *result = lkp_sym_proto(nenv->js, (ant_value_t)object, (ant_offset_t)vdata(k)) != 0;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp_proto(nenv->js, (ant_value_t)object, s, len) != 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  ant_value_t del_result = js_mkundef();

  if (vtype(k) == T_SYMBOL) {
    del_result = js_delete_sym_prop(nenv->js, (ant_value_t)object, k);
  } else {
    ant_value_t kstr = coerce_to_str(nenv->js, k);
    if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
    size_t len = 0;
    const char *s = js_getstr(nenv->js, kstr, &len);
    del_result = js_delete_prop(nenv->js, (ant_value_t)object, s, len);
  }

  if (is_err(del_result) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, del_result);
  *result = js_truthy(nenv->js, del_result);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  js_set(nenv->js, (ant_value_t)object, utf8name, (ant_value_t)value);
  return napi_return_pending_if_any(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t out = js_getprop_fallback(nenv->js, (ant_value_t)object, utf8name);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  *result = lkp_proto(nenv->js, (ant_value_t)object, utf8name, strlen(utf8name)) != 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  int n = snprintf(idx, sizeof(idx), "%u", index);
  if (n < 0) return napi_set_last(env, napi_generic_failure, "index conversion failed");
  ant_value_t key = js_mkstr(nenv->js, idx, (size_t)n);
  ant_value_t r = js_setprop(
    nenv->js, (ant_value_t)object,
    key, (ant_value_t)value
  );
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t out = js_get(nenv->js, (ant_value_t)object, idx);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  *result = lkp_proto(nenv->js, (ant_value_t)object, idx, strlen(idx)) != 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t del = js_delete_prop(nenv->js, (ant_value_t)object, idx, strlen(idx));
  if (is_err(del) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, del);
  *result = js_truthy(nenv->js, del);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_define_properties(
  napi_env env,
  napi_value object,
  size_t property_count,
  const napi_property_descriptor *properties
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object) || (property_count > 0 && !properties)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor *p = &properties[i];
    ant_value_t key = js_mkundef();
    const char *key_str = NULL;
    size_t key_len = 0;

    if (p->utf8name) {
      key_str = p->utf8name;
      key_len = strlen(p->utf8name);
      key = js_mkstr(nenv->js, key_str, key_len);
    } else key = (ant_value_t)p->name;

    bool is_symbol = (vtype(key) == T_SYMBOL);
    if (!is_symbol && !key_str) {
      if (vtype(key) != T_STR) continue;
      key_str = js_getstr(nenv->js, key, &key_len);
    }

    ant_value_t value = js_mkundef();
    if (p->method) {
      napi_value fn = 0;
      napi_status st = napi_create_function_common(
        env,  p->utf8name,
        NAPI_AUTO_LENGTH,
        p->method, p->data, &fn
      );
      if (st != napi_ok) return st;
      value = (ant_value_t)fn;
    } else if (p->getter || p->setter) {
      napi_value getter_fn = 0;
      napi_value setter_fn = 0;
      
      if (p->getter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->getter, p->data, &getter_fn);
        if (st != napi_ok) return st;
      }
      
      if (p->setter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->setter, p->data, &setter_fn);
        if (st != napi_ok) return st;
      }

      int flags = napi_desc_flags(p->attributes);
      ant_value_t desc_obj = js_as_obj((ant_value_t)object);
      
      if (is_symbol) {
        if (p->getter) js_set_sym_getter_desc(nenv->js, desc_obj, key, (ant_value_t)getter_fn, flags);
        if (p->setter) js_set_sym_setter_desc(nenv->js, desc_obj, key, (ant_value_t)setter_fn, flags);
      } else js_set_accessor_desc(
        nenv->js, desc_obj,
        key_str, key_len,
        p->getter ? (ant_value_t)getter_fn : js_mkundef(),
        p->setter ? (ant_value_t)setter_fn : js_mkundef(),
        flags
      );
      
      if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
      continue;
    } else value = (ant_value_t)p->value;

    if (is_symbol) js_set_sym(nenv->js, (ant_value_t)object, key, value);
    else {
      js_set(nenv->js, (ant_value_t)object, key_str, value);
      js_set_descriptor(nenv->js, js_as_obj((ant_value_t)object), key_str, key_len, napi_desc_flags(p->attributes));
    }
    
    if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_define_class(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback constructor,
  void *data,
  size_t property_count,
  const napi_property_descriptor *properties,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !constructor || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_status st = napi_create_function_common(
    env, utf8name, length, constructor, data, result
  );
  if (st != napi_ok) return st;

  ant_value_t ctor = (ant_value_t)*result;
  js_mark_constructor(ctor, true);

  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_err(proto) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, proto);
  if (!is_object_type(proto)) {
    proto = js_mkobj(nenv->js);
    js_set(nenv->js, ctor, "prototype", proto);
    if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  }

  for (size_t i = 0; i < property_count; i++) {
    napi_property_descriptor tmp = properties[i];
    bool is_static = (tmp.attributes & napi_static) != 0;
    tmp.attributes = (napi_property_attributes)(tmp.attributes & ~napi_static);
    st = napi_define_properties(env, (napi_value)(is_static ? ctor : proto), 1, &tmp);
    if (st != napi_ok) return st;
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_own_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == T_SYMBOL) {
    *result = lkp_sym(nenv->js, (ant_value_t)object, (ant_offset_t)vdata(k)) != 0;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp(nenv->js, (ant_value_t)object, s, len) != 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_typeof(
  napi_env env,
  napi_value value,
  napi_valuetype *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t v = (ant_value_t)value;
  uint8_t t = vtype(v);

  if (napi_find_external(nenv->js, value)) {
    *result = napi_external;
    return napi_set_last(env, napi_ok, NULL);
  }

  switch (t) {
    case T_UNDEF: *result = napi_undefined; break;
    case T_NULL: *result = napi_null; break;
    case T_BOOL: *result = napi_boolean; break;
    case T_NUM: *result = napi_number; break;
    case T_STR: *result = napi_string; break;
    case T_SYMBOL: *result = napi_symbol; break;
    case T_FUNC:
    case T_CFUNC: *result = napi_function; break;
    case T_BIGINT: *result = napi_bigint; break;
    default: *result = napi_object; break;
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_array(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = vtype((ant_value_t)value) == T_ARR;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_date(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = is_date_instance((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_arraybuffer(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) { *result = false; return napi_set_last(env, napi_ok, NULL); }

  *result = buffer_get_arraybuffer_data((ant_value_t)value) != NULL;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_buffer(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, value, &ta)) {
    *result = false;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t buffer_proto = js_get_ctor_proto(nenv->js, "Buffer", 6);
  *result = is_object_type(buffer_proto)
    && proto_chain_contains(nenv->js, (ant_value_t)value, buffer_proto);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_typedarray(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  TypedArrayData *ta = NULL;
  *result = napi_get_typedarray_data(nenv->js, value, &ta);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_dataview(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) {
    *result = false;
  } else {
    *result = buffer_get_dataview_data((ant_value_t)value) != NULL;
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_error(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) {
    *result = false;
  } else {
    ant_value_t et = js_get_slot((ant_value_t)value, SLOT_ERR_TYPE);
    *result = vtype(et) == T_NUM;
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_promise(
  napi_env env,
  napi_value value,
  bool *is_promise
) {
  if (!env || !is_promise) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *is_promise = vtype((ant_value_t)value) == T_PROMISE;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_instanceof(
  napi_env env,
  napi_value object,
  napi_value constructor,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t r = do_instanceof(nenv->js, (ant_value_t)object, (ant_value_t)constructor);
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  *result = js_truthy(nenv->js, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_strict_equals(
  napi_env env,
  napi_value lhs,
  napi_value rhs,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = strict_eq_values(nenv->js, (ant_value_t)lhs, (ant_value_t)rhs);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(
  napi_env env,
  napi_value recv,
  napi_value func,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_callable((ant_value_t)func)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    (ant_value_t)func,
    (ant_value_t)recv,
    (ant_value_t *)argv,
    (int)argc,
    NULL,
    false
  );

  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  if (result) *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_new_instance(
  napi_env env,
  napi_value constructor,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_callable((ant_value_t)constructor)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t ctor = (ant_value_t)constructor;
  ant_value_t obj = js_mkobj(nenv->js);
  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t saved = nenv->js->new_target;
  nenv->js->new_target = ctor;
  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    ctor,
    obj,
    (ant_value_t *)argv,
    (int)argc,
    NULL,
    true
  );
  nenv->js->new_target = saved;

  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, (is_object_type(out) ? out : obj));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_bool(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  bool truthy = js_truthy(nenv->js, (ant_value_t)value);
  if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  *result = NAPI_RETURN(nenv, js_bool(truthy));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_number(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  double num = js_to_number(nenv->js, (ant_value_t)value);
  if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  *result = NAPI_RETURN(nenv, js_mknum(num));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_object(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (is_object_type((ant_value_t)value)) {
    *result = value;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t obj_ctor = js_get(nenv->js, js_glob(nenv->js), "Object");
  if (is_err(obj_ctor) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, obj_ctor);
  if (!is_callable(obj_ctor)) return napi_set_last(env, napi_generic_failure, "Object constructor missing");
  ant_value_t arg = (ant_value_t)value;
  ant_value_t out = sv_vm_call(nenv->js->vm, nenv->js, obj_ctor, js_mkundef(), &arg, 1, NULL, false);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_string(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t out = coerce_to_str(nenv->js, (ant_value_t)value);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
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

NAPI_EXTERN napi_status NAPI_CDECL napi_wrap(
  napi_env env,
  napi_value js_object,
  void *native_object,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry) {
    entry = (napi_wrap_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");
    entry->id = g_napi_wrap_next_id++;
    HASH_ADD(hh, g_napi_wraps, id, sizeof(entry->id), entry);
    napi_slot_set_u64(nenv->js, (ant_value_t)js_object, SLOT_NAPI_WRAP_ID, entry->id);
  }

  entry->native_object = native_object;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  entry->has_wrap = true;

  if (result) {
    return napi_create_reference(env, js_object, 0, result);
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_add_finalizer(
  napi_env env,
  napi_value js_object,
  void *finalize_data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry) {
    entry = (napi_wrap_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");
    entry->id = g_napi_wrap_next_id++;
    HASH_ADD(hh, g_napi_wraps, id, sizeof(entry->id), entry);
    napi_slot_set_u64(nenv->js, (ant_value_t)js_object, SLOT_NAPI_WRAP_ID, entry->id);
  }

  entry->attached_data = finalize_data;
  entry->attached_finalize_cb = finalize_cb;
  entry->attached_finalize_hint = finalize_hint;

  if (result) return napi_create_reference(env, js_object, 0, result);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unwrap(
  napi_env env,
  napi_value js_object,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry || !entry->has_wrap) return napi_set_last(env, napi_invalid_arg, "object not wrapped");
  *result = entry->native_object;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_wrap(
  napi_env env,
  napi_value js_object,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry || !entry->has_wrap) return napi_set_last(env, napi_invalid_arg, "object not wrapped");

  if (result) *result = entry->native_object;
  entry->native_object = NULL;
  entry->finalize_cb = NULL;
  entry->finalize_hint = NULL;
  entry->has_wrap = false;

  if (!entry->attached_finalize_cb) {
    HASH_DEL(g_napi_wraps, entry);
    free(entry);
    js_set_slot((ant_value_t)js_object, SLOT_NAPI_WRAP_ID, js_mkundef());
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_handle_scope(
  napi_env env,
  napi_handle_scope *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_handle_scope__ *scope = (struct napi_handle_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = nenv;
  scope->gc_root_mark = gc_root_scope(nenv->js);
  scope->handle_slots_mark = nenv->handle_slots_len;
  nenv->open_handle_scopes++;
  *result = (napi_handle_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_handle_scope(
  napi_env env,
  napi_handle_scope scope
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_handle_scope__ *s = (struct napi_handle_scope__ *)scope;
  if (nenv->js) gc_pop_roots(nenv->js, s->gc_root_mark);
  nenv->handle_slots_len = s->handle_slots_mark;
  if (nenv->open_handle_scopes > 0) nenv->open_handle_scopes--;
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_escapable_handle_scope(
  napi_env env,
  napi_escapable_handle_scope *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_escapable_handle_scope__ *scope = (struct napi_escapable_handle_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = nenv;
  scope->gc_root_mark = gc_root_scope(nenv->js);
  scope->handle_slots_mark = nenv->handle_slots_len;
  scope->escaped = false;
  scope->escaped_val = js_mkundef();
  nenv->open_handle_scopes++;
  *result = (napi_escapable_handle_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_escapable_handle_scope(
  napi_env env,
  napi_escapable_handle_scope scope
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_escapable_handle_scope__ *s = (struct napi_escapable_handle_scope__ *)scope;
  if (nenv->js) gc_pop_roots(nenv->js, s->gc_root_mark);
  nenv->handle_slots_len = s->handle_slots_mark;
  if (nenv->open_handle_scopes > 0) nenv->open_handle_scopes--;
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_escape_handle(
  napi_env env,
  napi_escapable_handle_scope scope,
  napi_value escapee,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_escapable_handle_scope__ *esc = (struct napi_escapable_handle_scope__ *)scope;
  if (!nenv || !nenv->js || !esc || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (esc->escaped) return napi_set_last(env, napi_escape_called_twice, "escape already called");
  esc->escaped = true;
  esc->escaped_val = (ant_value_t)escapee;
  gc_push_root(nenv->js, &esc->escaped_val);
  *result = escapee;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_async_work(
  napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_execute_callback execute,
  napi_async_complete_callback complete,
  void *data,
  napi_async_work *result
) {
  (void)async_resource;
  (void)async_resource_name;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !execute || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_async_work_impl_t *work = (napi_async_work_impl_t *)calloc(1, sizeof(*work));
  if (!work) return napi_set_last(env, napi_generic_failure, "out of memory");

  work->env = nenv;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  work->req.data = work;

  *result = (napi_async_work)work;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_async_work(
  napi_env env,
  napi_async_work work
) {
  (void)env;
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!w) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (w->queued) {
    w->delete_after_complete = true;
    return napi_set_last(env, napi_ok, NULL);
  }
  free(w);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_queue_async_work(
  node_api_basic_env env,
  napi_async_work work
) {
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!env || !w) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  if (w->queued) return napi_set_last((napi_env)env, napi_invalid_arg, "already queued");

  int rc = uv_queue_work(uv_default_loop(), &w->req, napi_async_work_execute_cb, napi_async_work_after_cb);
  if (rc != 0) return napi_set_last((napi_env)env, napi_generic_failure, "uv_queue_work failed");
  w->queued = true;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_cancel_async_work(
  node_api_basic_env env,
  napi_async_work work
) {
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!env || !w) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  int rc = uv_cancel((uv_req_t *)&w->req);
  if (rc != 0) return napi_set_last((napi_env)env, napi_generic_failure, "uv_cancel failed");
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_threadsafe_function(
  napi_env env,
  napi_value func,
  napi_value async_resource,
  napi_value async_resource_name,
  size_t max_queue_size,
  size_t initial_thread_count,
  void *thread_finalize_data,
  napi_finalize thread_finalize_cb,
  void *context,
  napi_threadsafe_function_call_js call_js_cb,
  napi_threadsafe_function *result
) {
  (void)async_resource;
  (void)async_resource_name;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  struct napi_threadsafe_function__ *tsfn =
    (struct napi_threadsafe_function__ *)calloc(1, sizeof(*tsfn));
  if (!tsfn) return napi_set_last(env, napi_generic_failure, "out of memory");

  tsfn->env = nenv;
  tsfn->call_js_cb = call_js_cb;
  tsfn->thread_finalize_cb = thread_finalize_cb;
  tsfn->thread_finalize_data = thread_finalize_data;
  tsfn->context = context;
  tsfn->max_queue_size = max_queue_size;
  tsfn->thread_count = initial_thread_count > 0 ? initial_thread_count : 1;
  tsfn->func_val = func ? (ant_value_t)func : js_mkundef();

  uv_mutex_init(&tsfn->mutex);
  int rc = uv_async_init(uv_default_loop(), &tsfn->async, napi_tsfn_async_cb);
  if (rc != 0) {
    uv_mutex_destroy(&tsfn->mutex);
    free(tsfn);
    return napi_set_last(env, napi_generic_failure, "uv_async_init failed");
  }
  tsfn->async.data = tsfn;

  tsfn->prev = NULL;
  tsfn->next = nenv->tsfns;
  if (nenv->tsfns) nenv->tsfns->prev = tsfn;
  nenv->tsfns = tsfn;

  *result = (napi_threadsafe_function)tsfn;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_threadsafe_function(
  napi_threadsafe_function func,
  void *data,
  napi_threadsafe_function_call_mode is_blocking
) {
  (void)is_blocking;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;

  uv_mutex_lock(&tsfn->mutex);
  if (tsfn->closing || tsfn->aborted) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_closing;
  }
  if (tsfn->max_queue_size > 0 && tsfn->queue_size >= tsfn->max_queue_size) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_queue_full;
  }

  napi_tsfn_item_t *item = (napi_tsfn_item_t *)calloc(1, sizeof(*item));
  if (!item) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_generic_failure;
  }
  item->data = data;
  if (!tsfn->head) tsfn->head = item;
  else tsfn->tail->next = item;
  tsfn->tail = item;
  tsfn->queue_size++;
  uv_mutex_unlock(&tsfn->mutex);

  uv_async_send(&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_release_threadsafe_function(
  napi_threadsafe_function func,
  napi_threadsafe_function_release_mode mode
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;

  uv_mutex_lock(&tsfn->mutex);
  if (mode == napi_tsfn_abort) tsfn->aborted = true;
  if (tsfn->thread_count > 0) tsfn->thread_count--;
  if (tsfn->thread_count == 0 || tsfn->aborted) tsfn->closing = true;
  uv_mutex_unlock(&tsfn->mutex);

  uv_async_send(&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_acquire_threadsafe_function(
  napi_threadsafe_function func
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_mutex_lock(&tsfn->mutex);
  if (tsfn->closing) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_closing;
  }
  tsfn->thread_count++;
  uv_mutex_unlock(&tsfn->mutex);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_ref_threadsafe_function(
  node_api_basic_env env,
  napi_threadsafe_function func
) {
  (void)env;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_ref((uv_handle_t *)&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unref_threadsafe_function(
  node_api_basic_env env,
  napi_threadsafe_function func
) {
  (void)env;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_unref((uv_handle_t *)&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_threadsafe_function_context(
  napi_threadsafe_function func,
  void **result
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn || !result) return napi_invalid_arg;
  *result = tsfn->context;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_run_script(
  napi_env env,
  napi_value script,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)script) != T_STR) return napi_set_last(env, napi_string_expected, "script must be string");

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

NAPI_EXTERN napi_status NAPI_CDECL napi_open_callback_scope(
  napi_env env,
  napi_value resource_object,
  napi_async_context context,
  napi_callback_scope *result
) {
  (void)resource_object;
  (void)context;
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_callback_scope__ *scope = (struct napi_callback_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = (ant_napi_env_t *)env;
  *result = (napi_callback_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_callback_scope(
  napi_env env,
  napi_callback_scope scope
) {
  (void)env;
  if (!scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_init(
  napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_context *result
) {
  (void)async_resource;
  (void)async_resource_name;
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_async_context__ *ctx = (struct napi_async_context__ *)calloc(1, sizeof(*ctx));
  if (!ctx) return napi_set_last(env, napi_generic_failure, "out of memory");
  ctx->env = (ant_napi_env_t *)env;
  *result = (napi_async_context)ctx;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_destroy(
  napi_env env,
  napi_async_context async_context
) {
  (void)env;
  if (!async_context) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  free(async_context);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_make_callback(
  napi_env env,
  napi_async_context async_context,
  napi_value recv,
  napi_value func,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  (void)async_context;
  return napi_call_function(env, recv, func, argc, argv, result);
}

NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module *mod) {
  g_pending_napi_module = mod;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_uv_event_loop(
  node_api_basic_env env,
  struct uv_loop_s **loop
) {
  if (!env || !loop) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  *loop = uv_default_loop();
  return napi_set_last((napi_env)env, napi_ok, NULL);
}
