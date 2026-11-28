#pragma once

#include <compat.h> // IWYU pragma: keep
#include <uv.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "internal.h"
#include "errors.h"
#include "native_tag.h"
#include "modules/buffer.h"
#include "modules/napi.h"

#if defined(__has_include) && __has_include(<uchar.h>)
#include <uchar.h>
#else
typedef uint16_t char16_t;
#endif

typedef struct napi_cleanup_hook_entry {
  napi_cleanup_hook hook;
  void *arg;
  struct napi_cleanup_hook_entry *next;
} napi_cleanup_hook_entry_t;

struct napi_async_cleanup_hook_handle__ {
  napi_env env;
  napi_async_cleanup_hook hook;
  void *arg;
  struct napi_async_cleanup_hook_handle__ *next;
  struct napi_async_cleanup_hook_handle__ *prev;
};

typedef union {
  node_api_basic_finalize with_env;
  node_api_noenv_finalize without_env;
} napi_external_buffer_finalizer_t;

typedef struct napi_external_buffer {
  ArrayBufferData *data;
  void *bytes;
  napi_external_buffer_finalizer_t finalizer;
  void *finalize_hint;
  struct napi_external_buffer *next;
} napi_external_buffer_t;

struct ant_napi_env__ {
  ant_t *js;
  napi_extended_error_info last_error;
  char last_error_msg[256];
  ant_value_t exception;
  uint32_t version;
  void *instance_data;
  node_api_basic_finalize instance_data_finalize_cb;
  void *instance_data_finalize_hint;
  napi_cleanup_hook_entry_t *cleanup_hooks;
  struct napi_async_cleanup_hook_handle__ *async_cleanup_hooks;
  napi_external_buffer_t *external_buffers;
  const char *module_file_name;
  struct napi_ref__ *refs;
  struct napi_deferred__ *deferreds;
  struct napi_threadsafe_function__ *tsfns;
  int open_handle_scopes;
  ant_value_t *handle_slots;
  size_t handle_slots_len;
  size_t handle_slots_cap;
  struct napi_external_entry *externals;
  struct napi_wrap_entry *wraps;
  uint64_t external_next_id;
  uint64_t wrap_next_id;
  int64_t external_memory;
};

typedef struct napi_callback_binding {
  napi_env env;
  napi_callback cb;
  void *data;
} napi_callback_binding_t;

struct napi_callback_info__ {
  napi_env env;
  const ant_value_t *argv;
  size_t argc;
  ant_value_t this_arg;
  ant_value_t new_target;
  void *data;
};

struct napi_ref__ {
  napi_env env;
  ant_value_t ref_val;
  ant_value_t value;
  bool live;
  uint32_t refcount;
  struct napi_ref__ *next;
  struct napi_ref__ *prev;
};

struct napi_deferred__ {
  napi_env env;
  ant_value_t promise_val;
  struct napi_deferred__ *next;
  struct napi_deferred__ *prev;
};

struct napi_handle_scope__ {
  napi_env env;
  size_t gc_root_mark;
  size_t handle_slots_mark;
};

struct napi_escapable_handle_scope__ {
  struct napi_handle_scope__ base;
  bool escaped;
  ant_value_t escaped_val;
};

struct napi_async_context__ {
  napi_env env;
};

struct napi_callback_scope__ {
  napi_env env;
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
  bool has_type_tag;
  napi_type_tag type_tag;
  UT_hash_handle hh;
} napi_wrap_entry_t;

struct napi_threadsafe_function__ {
  napi_env env;
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
  struct napi_tsfn_item *head;
  struct napi_tsfn_item *tail;
  struct napi_threadsafe_function__ *next;
  struct napi_threadsafe_function__ *prev;
};

static constexpr uint32_t NAPI_CALLBACK_NATIVE_TAG = Ant_NativeTag("NCBK");
static constexpr ant_value_t NAPI_ZERO_HANDLE = Ant_SentinelTag("NZRO");
static constexpr size_t NAPI_INLINE_ARGS = 8;

static inline napi_value Napi_Encode(ant_value_t value) {
  return (napi_value){ value == 0 ? NAPI_ZERO_HANDLE : value };
}

static inline ant_value_t Napi_Decode(napi_value handle) {
  return handle.bits == NAPI_ZERO_HANDLE ? 0 : handle.bits;
}

static inline bool Napi_IsEmpty(napi_value handle) {
  return handle.bits == 0;
}

typedef struct {
  const ant_value_t *values;
  ant_value_t inline_values[NAPI_INLINE_ARGS];
  ant_value_t *heap_values;
} napi_args_t;

static inline bool Napi_DecodeArgs(napi_args_t *out, const napi_value *handles, size_t count) {
  out->heap_values = NULL;
  out->values = (const ant_value_t *)handles;

  size_t i = 0;
  while (i < count && handles[i].bits != NAPI_ZERO_HANDLE) i++;
  if (i == count) return true;

  ant_value_t *values = out->inline_values;
  if (count > NAPI_INLINE_ARGS && !(values = out->heap_values = malloc(count * sizeof(*values)))) return false;
  for (i = 0; i < count; i++) values[i] = Napi_Decode(handles[i]);
  out->values = values;
  return true;
}

static inline void Napi_FreeArgs(napi_args_t *args) {
  free(args->heap_values);
}

napi_status Ant_Napi_SetLastError(napi_env env, napi_status status, const char *message);

static inline napi_status Napi_Ok(napi_env env) {
  return Ant_Napi_SetLastError(env, napi_ok, NULL);
}

static inline napi_status Napi_InvalidArg(napi_env env) {
  return Ant_Napi_SetLastError(env, napi_invalid_arg, "invalid argument");
}

static inline napi_status Napi_OutOfMemory(napi_env env) {
  return Ant_Napi_SetLastError(env, napi_generic_failure, "out of memory");
}

static inline napi_status Napi_ObjectExpected(napi_env env) {
  return Ant_Napi_SetLastError(env, napi_object_expected, NULL);
}

static inline bool Napi_Failed(napi_env env, ant_value_t result) {
  return is_err(result) || Ant_Exception_Pending(env->js);
}

static inline ArrayBufferData *Napi_ArrayBufferData(napi_value value) {
  ant_value_t buffer = Napi_Decode(value);
  return is_object_type(buffer) ? buffer_get_arraybuffer_data(buffer) : NULL;
}

napi_value Ant_Napi_Pin(napi_env env, ant_value_t value);
napi_status Ant_Napi_CapturePending(napi_env env, ant_value_t result);
napi_status Ant_Napi_CheckPending(napi_env env);
napi_status Ant_Napi_ThrowMessage(napi_env env, js_err_type_t type, const char *code, const char *message);

napi_status Ant_Napi_CreateFunction(
  napi_env env, const char *utf8name, size_t length,
  napi_callback cb, void *data, napi_value *result
);

bool Ant_Napi_IsExternal(ant_t *js, napi_value value);
void Ant_Napi_CloseThreadsafeFunctions(napi_env env);
void Ant_Napi_SweepExternalBuffers(napi_env env, bool teardown);

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref *result);

NAPI_EXTERN napi_status NAPI_CDECL
napi_call_function(
  napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value *argv,
  napi_value *result
);

NAPI_EXTERN napi_status NAPI_CDECL
napi_define_properties(
  napi_env env, napi_value object, size_t property_count,
  const napi_property_descriptor *properties
);

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_bigint_int64(napi_env env, int64_t value, napi_value *result);

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_promise(napi_env env, napi_deferred *deferred, napi_value *promise);

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_string_utf8(napi_env env, const char *str, size_t length, napi_value *result);

NAPI_EXTERN napi_status NAPI_CDECL
napi_typeof(napi_env env, napi_value value, napi_valuetype *result);

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s **loop);
