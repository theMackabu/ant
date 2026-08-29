#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned long long u64;

struct ant_object;
struct ant_shape;
struct ant_isolate_t;

typedef struct ant_isolate_t      ant_t;
typedef struct ant_pool_block     ant_pool_block_t;
typedef struct ant_http_request_s ant_http_request_t;
typedef struct ant_process_state  ant_process_state_t;
typedef struct ant_events_state   ant_events_state_t;
typedef struct ant_esm_state      ant_esm_state_t;
typedef struct ant_regex_state    ant_regex_state_t;
typedef struct ant_string_builder ant_string_builder_t;
typedef struct js_async_entry     js_async_entry_t;

typedef struct ant_object ant_object_t;
typedef struct ant_shape  ant_shape_t;

typedef struct sv_vm      sv_vm_t;
typedef struct sv_ast     sv_ast_t;
typedef struct sv_func    sv_func_t;
typedef struct sv_upvalue sv_upvalue_t;
typedef struct sv_closure sv_closure_t;
typedef struct sv_frame   sv_frame_t;

typedef struct sv_eval_env_state    sv_eval_env_state_t;
typedef struct sv_map_template_desc sv_map_template_desc_t;

typedef struct server_runtime_s    server_runtime_t;
typedef struct server_request_s    server_request_t;
typedef struct server_conn_state_s server_conn_state_t;
typedef struct server_sse_state_s  server_sse_state_t;

typedef uint64_t ant_offset_t;
typedef uint64_t ant_value_t;

typedef struct {
  ant_object_t *obj;
  uint32_t slot;
} ant_prop_loc_t;

typedef ant_value_t 
  (*ant_cfunc_t) 
  (ant_t *, ant_value_t *, int);

typedef struct ant_cfunc_meta {
  ant_cfunc_t fn;
  const char *name;
  uint32_t length;
  uint8_t flags;
} ant_cfunc_meta_t;

constexpr uintptr_t ant_sidecar = 1u;
constexpr ant_prop_loc_t ANT_PROP_LOC_NONE = {NULL, 0};

#define ant_bind_t   ant_value_t func, ant_value_t this_val
#define ant_params_t ant_t *js, ant_value_t *args, int nargs

#endif
