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

typedef struct ant_object ant_object_t;
typedef struct ant_shape  ant_shape_t;

typedef struct sv_vm      sv_vm_t;
typedef struct sv_func    sv_func_t;
typedef struct sv_closure sv_closure_t;
typedef struct sv_frame   sv_frame_t;

typedef size_t   ant_handle_t;
typedef uint64_t ant_offset_t;
typedef uint64_t ant_value_t;

typedef ant_value_t 
  (*ant_cfunc_t) 
  (ant_t *, ant_value_t *, int);

typedef struct ant_cfunc_meta {
  ant_cfunc_t fn;
  const char *name;
  uint32_t length;
  uint8_t flags;
} ant_cfunc_meta_t;

#define ant_sidecar  ((uintptr_t)1u)
#define ant_bind_t   ant_value_t func, ant_value_t this_val
#define ant_params_t ant_t *js, ant_value_t *args, int nargs

#endif
