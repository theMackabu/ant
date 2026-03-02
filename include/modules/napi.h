#ifndef ANT_NAPI_H
#define ANT_NAPI_H

#include "types.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef NAPI_AUTO_LENGTH
#define NAPI_AUTO_LENGTH SIZE_MAX
#endif

#ifndef NAPI_CDECL
#ifdef _WIN32
#define NAPI_CDECL __cdecl
#else
#define NAPI_CDECL
#endif
#endif

#ifndef NAPI_EXTERN
#if defined(_WIN32)
#define NAPI_EXTERN __declspec(dllexport)
#else
#define NAPI_EXTERN __attribute__((visibility("default"), used))
#endif
#endif

typedef jsval_t napi_value;
#define NAPI_MODULE_VERSION 1

typedef struct ant_napi_env__* napi_env;
typedef napi_env node_api_basic_env;

typedef struct napi_ref__* napi_ref;
typedef struct napi_handle_scope__* napi_handle_scope;
typedef struct napi_escapable_handle_scope__* napi_escapable_handle_scope;
typedef struct napi_callback_info__* napi_callback_info;
typedef struct napi_deferred__* napi_deferred;

typedef struct napi_callback_scope__* napi_callback_scope;
typedef struct napi_async_context__* napi_async_context;
typedef struct napi_async_work__* napi_async_work;
typedef struct napi_threadsafe_function__* napi_threadsafe_function;

typedef enum {
  napi_default = 0,
  napi_writable = 1 << 0,
  napi_enumerable = 1 << 1,
  napi_configurable = 1 << 2,
  napi_static = 1 << 10,
} napi_property_attributes;

typedef enum {
  napi_undefined,
  napi_null,
  napi_boolean,
  napi_number,
  napi_string,
  napi_symbol,
  napi_object,
  napi_function,
  napi_external,
  napi_bigint,
} napi_valuetype;

typedef enum {
  napi_int8_array,
  napi_uint8_array,
  napi_uint8_clamped_array,
  napi_int16_array,
  napi_uint16_array,
  napi_int32_array,
  napi_uint32_array,
  napi_float32_array,
  napi_float64_array,
  napi_bigint64_array,
  napi_biguint64_array,
} napi_typedarray_type;

typedef enum {
  napi_ok,
  napi_invalid_arg,
  napi_object_expected,
  napi_string_expected,
  napi_name_expected,
  napi_function_expected,
  napi_number_expected,
  napi_boolean_expected,
  napi_array_expected,
  napi_generic_failure,
  napi_pending_exception,
  napi_cancelled,
  napi_escape_called_twice,
  napi_handle_scope_mismatch,
  napi_callback_scope_mismatch,
  napi_queue_full,
  napi_closing,
  napi_bigint_expected,
  napi_date_expected,
  napi_arraybuffer_expected,
  napi_detachable_arraybuffer_expected,
  napi_would_deadlock,
  napi_no_external_buffers_allowed,
  napi_cannot_run_js,
} napi_status;

typedef napi_value(NAPI_CDECL* napi_callback)(napi_env env, napi_callback_info info);
typedef void(NAPI_CDECL* napi_finalize)(napi_env env, void* finalize_data, void* finalize_hint);
typedef napi_finalize node_api_basic_finalize;

typedef struct {
  const char* utf8name;
  napi_value name;
  napi_callback method;
  napi_callback getter;
  napi_callback setter;
  napi_value value;
  napi_property_attributes attributes;
  void* data;
} napi_property_descriptor;

typedef struct {
  const char* error_message;
  void* engine_reserved;
  uint32_t engine_error_code;
  napi_status error_code;
} napi_extended_error_info;

typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char* release;
} napi_node_version;

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);
typedef void(NAPI_CDECL* napi_async_execute_callback)(napi_env env, void* data);
typedef void(NAPI_CDECL* napi_async_complete_callback)(napi_env env, napi_status status, void* data);

typedef void(NAPI_CDECL* napi_threadsafe_function_call_js)(
  napi_env env,
  napi_value js_callback,
  void* context,
  void* data
);

typedef enum {
  napi_tsfn_release,
  napi_tsfn_abort
} napi_threadsafe_function_release_mode;

typedef enum {
  napi_tsfn_nonblocking,
  napi_tsfn_blocking
} napi_threadsafe_function_call_mode;

typedef napi_value(NAPI_CDECL* napi_addon_register_func)(napi_env env, napi_value exports);

typedef struct napi_module {
  int nm_version;
  unsigned int nm_flags;
  const char* nm_filename;
  napi_addon_register_func nm_register_func;
  const char* nm_modname;
  void* nm_priv;
  void* reserved[4];
} napi_module;

napi_env ant_napi_get_env(ant_t *js);
jsval_t napi_process_dlopen_js(ant_t *js, jsval_t *args, int nargs);
jsval_t napi_load_native_module(ant_t *js, const char *module_path, jsval_t ns);

#endif
