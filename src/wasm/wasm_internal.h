#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <wasm_c_api.h>
#include <wasm_export.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#include <wasm_c_api_internal.h>
#include <wasm_runtime.h>
#pragma clang diagnostic pop

#include "ant.h"
#include "list.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "gc/modules.h"
#include "silver/call.h"
#include "modules/buffer.h"
#include "wasm/wasm.h"
#include "wasm/wasi.h"
#include "modules/assert.h"
#include "modules/bigint.h"
#include "modules/iterator.h"

typedef struct {
  wasm_store_t *store;
  wasm_module_t *module;
  uint8_t *bytes;
  size_t bytes_len;
} wasm_module_handle_t;

typedef struct {
  wasm_instance_t *instance;
  wasm_extern_vec_t exports;
  wasm_func_t **host_funcs;
  size_t host_func_count;
  wasm_global_t **host_globals;
  size_t host_global_count;
} wasm_instance_handle_t;

typedef enum {
  WASM_EXTERN_WRAP_GLOBAL = 1,
  WASM_EXTERN_WRAP_MEMORY,
  WASM_EXTERN_WRAP_TABLE,
} wasm_extern_wrap_kind_t;

struct wasm_extern_handle_s {
  wasm_extern_handle_t *next;
  wasm_extern_handle_t **pprev;
  wasm_extern_wrap_kind_t kind;
  wasm_store_t *store;
  
  bool own_handle;
  bool use_cached_value;
  bool standalone_table;
  
  wasm_valkind_t standalone_table_element;
  ant_value_t memory_buffer;
  WASMMemoryInstance *view_memory;
  uint8_t *view_data;
  uint32_t view_pages;
  wasm_val_t cached_value;
  
  uint32_t standalone_table_size;
  uint32_t standalone_table_max_size;
  
  union {
    wasm_global_t *global;
    wasm_memory_t *memory;
    wasm_table_t *table;
  } as;
};

struct wasm_import_func_env_s {
  wasm_import_func_env_t *next;
  wasm_import_func_env_t **pprev;
  
  ant_t *js;
  wasm_store_t *store;
  ant_value_t owner;
  ant_value_t fn;
  wasm_valkind_t *result_kinds;
  size_t result_count;
};

typedef struct {
  wasm_func_t *func;
  bool own_func;
} wasm_func_handle_t;

static constexpr uint32_t WASM_FUNC_STATE_TAG = Ant_NativeTag("WFST");
static constexpr uint32_t WASM_MODULE_NATIVE_TAG = Ant_NativeTag("WMOD");
static constexpr uint32_t WASM_INSTANCE_NATIVE_TAG = Ant_NativeTag("WINS");
static constexpr uint32_t WASM_EXTERN_NATIVE_TAG = Ant_NativeTag("WEXT");

extern wasm_engine_t *g_wasm_engine;

void wasm_detach_moved_views(ant_t *js);
void wasm_clear_pending_import_throw(ant_t *js);
void wasm_extern_finalize(ant_t *js, ant_object_t *obj);
void wasm_set_pending_import_throw(ant_t *js, ant_value_t value);
void wasm_unregister_instance_owner(ant_t *js, wasm_instance_t *instance);
void wasm_reject_with_error(ant_t *js, ant_value_t promise, ant_value_t error);

bool ensure_wasm_engine(void);
bool wasm_to_page_count(ant_t *js, ant_value_t value, uint32_t *out);
bool js_value_to_wasm(ant_t *js, ant_value_t value, wasm_valkind_t kind, wasm_val_t *out);
bool wasm_register_instance_owner(ant_t *js, wasm_instance_t *instance, ant_value_t owner);

size_t wasm_name_len(const wasm_name_t *name);
const char *wasm_extern_kind_name(wasm_externkind_t kind);

wasm_valkind_t wasm_valkind_from_string(const char *name, size_t len, bool *ok);
wasm_module_handle_t *wasm_module_handle(ant_value_t value);
wasm_extern_handle_t *wasm_extern_handle(ant_value_t value, wasm_extern_wrap_kind_t kind);

ant_value_t wasm_consume_pending_import_throw(ant_t *js);
ant_value_t wasm_value_from_i64(ant_t *js, int64_t value);
ant_value_t wasm_error_value(ant_t *js, ant_value_t value);
ant_value_t wasm_trap_to_error(ant_t *js, wasm_trap_t *trap);
ant_value_t wasm_make_link_error(ant_t *js, const char *message);
ant_value_t wasm_value_to_js(ant_t *js, const wasm_val_t *value);
ant_value_t wasm_make_compile_error(ant_t *js, const char *message);
ant_value_t wasm_make_runtime_error(ant_t *js, const char *message);
ant_value_t wasm_js_from_result_vec(ant_t *js, wasm_val_vec_t *results);
ant_value_t wasm_make_tag(ant_t *js, ant_value_t proto, ant_value_t kinds, bool js_tag);
ant_value_t wasm_instantiate_result(ant_t *js, ant_value_t module, ant_value_t instance);
ant_value_t wasm_sequence_to_list(ant_t *js, ant_value_t value, const char *not_iterable);
ant_value_t wasm_wrap_func(ant_t *js, wasm_func_t *func, ant_value_t owner, bool own_func);
ant_value_t wasm_find_instance_owner(ant_t *js, WASMModuleInstanceCommon *runtime_instance);

ant_value_t js_wasm_compile(ant_params_t);
ant_value_t js_wasm_tag_ctor(ant_params_t);
ant_value_t js_wasm_validate(ant_params_t);
ant_value_t js_wasm_table_get(ant_params_t);
ant_value_t js_wasm_table_set(ant_params_t);
ant_value_t js_wasm_table_ctor(ant_params_t);
ant_value_t js_wasm_table_grow(ant_params_t);
ant_value_t js_wasm_global_ctor(ant_params_t);
ant_value_t js_wasm_instantiate(ant_params_t);
ant_value_t js_wasm_memory_ctor(ant_params_t);
ant_value_t js_wasm_memory_grow(ant_params_t);
ant_value_t js_wasm_module_ctor(ant_params_t);
ant_value_t js_wasm_exception_is(ant_params_t);
ant_value_t js_wasm_instance_ctor(ant_params_t);
ant_value_t js_wasm_exception_ctor(ant_params_t);
ant_value_t js_wasm_module_exports(ant_params_t);
ant_value_t js_wasm_module_imports(ant_params_t);
ant_value_t js_wasm_global_value_of(ant_params_t);
ant_value_t js_wasm_compile_streaming(ant_params_t);
ant_value_t js_wasm_exception_get_arg(ant_params_t);
ant_value_t js_wasm_global_value_getter(ant_params_t);
ant_value_t js_wasm_global_value_setter(ant_params_t);
ant_value_t js_wasm_table_length_getter(ant_params_t);
ant_value_t js_wasm_memory_buffer_getter(ant_params_t);
ant_value_t js_wasm_instantiate_streaming(ant_params_t);
ant_value_t js_wasm_exception_stack_getter(ant_params_t);
ant_value_t js_wasm_module_custom_sections(ant_params_t);
ant_value_t js_wasm_instance_exports_getter(ant_params_t);
ant_value_t js_wasm_memory_to_fixed_length_buffer(ant_params_t);

ant_value_t wasm_module_from_bytes(
  ant_t *js,
  ant_value_t value,
  ant_value_t *out_module,
  const char *api
);

bool wasm_buffer_source_to_vec(
  ant_t *js,
  ant_value_t value,
  wasm_byte_vec_t *out,
  char *error_buf,
  size_t error_buf_len
);

ant_value_t wasm_instantiate_module(
  ant_t *js,
  ant_value_t module_obj,
  ant_value_t import_obj,
  ant_value_t *out_instance
);

ant_value_t wasm_wrap_extern_object(
  ant_t *js, wasm_extern_wrap_kind_t kind,
  ant_value_t proto, int brand,
  wasm_store_t *store, bool own_handle,
  void *ptr, ant_value_t owner
);

static inline bool wasm_view_moved(const wasm_extern_handle_t *handle) {
  const WASMMemoryInstance *memory = handle->view_memory;
  return memory->memory_data != handle->view_data || memory->cur_page_count != handle->view_pages;
}

static inline void wasm_sync_memory_views(ant_t *js) {
  wasm_extern_handle_t *handle = js->wasm_memory_views;
  while (handle && !wasm_view_moved(handle)) handle = handle->next;
  if (handle) wasm_detach_moved_views(js);
}
