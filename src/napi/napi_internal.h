#ifndef ANT_NAPI_INTERNAL_H
#define ANT_NAPI_INTERNAL_H

#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uthash.h>
#include <uv.h>

#include "ant.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "ptr.h"
#include "silver/engine.h"

#include "gc/modules.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "modules/buffer.h"
#include "modules/date.h"
#include "modules/napi.h"
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
  napi_env env,
  napi_value exports
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

ant_napi_env_t *ant_napi_get_or_create_env(ant_t *js);
napi_status ant_napi_set_last(napi_env env, napi_status status, const char *message);
napi_status ant_napi_check_pending_from_result(napi_env env, ant_value_t result);
napi_status ant_napi_return_pending_if_any(napi_env env);
napi_value ant_napi_scope_pin(ant_napi_env_t *nenv, napi_value val);
bool ant_napi_is_external(ant_t *js, napi_value value);

/* Keep private cross-file helpers out of the exported napi_* symbol namespace. */
#define napi_set_last ant_napi_set_last
#define napi_check_pending_from_result ant_napi_check_pending_from_result

#define NAPI_RETURN(nenv, val) ant_napi_scope_pin((nenv), (napi_value)(val))

/* Pull every N-API archive member in with the loader. */
void ant_napi_link_async(void);
void ant_napi_link_objects(void);
void ant_napi_link_references(void);
void ant_napi_link_values(void);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(
  napi_env env,
  napi_value value,
  uint32_t initial_refcount,
  napi_ref *result
);

NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(
  napi_env env,
  napi_value recv,
  napi_value func,
  size_t argc,
  const napi_value *argv,
  napi_value *result
);

#endif
