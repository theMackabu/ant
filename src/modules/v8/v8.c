#include <uv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "cfunc.h"
#include "internal.h"
#include "v8_internal.h"
#include "gc/object_arenas.h"

static ant_value_t v8_get_heap_statistics(ant_params_t) {
  size_t rss = 0;
  uv_resident_set_memory(&rss);

  size_t arena_committed  = ant_object_committed(js);
  size_t arena_reserved   = js->obj_arena.limit + js->obj_wide_arena.limit + js->obj_large_arena.limit;
  size_t arena_live_bytes = ant_object_live_bytes(js);

  size_t closure_committed = js->closure_arena.committed;
  size_t closure_reserved  = js->closure_arena.limit;
  size_t closure_live      = js->closure_arena.live_count * js->closure_arena.elem_size;

  size_t pool_live  = js->gc.pool_last_live;
  size_t pool_alloc = js->gc.pool_alloc;
  size_t pool_total = pool_live + pool_alloc;

  size_t extra_alloc = js->alloc_bytes.closures + js->alloc_bytes.upvalues;

  size_t used_heap  = arena_live_bytes + closure_live + pool_total + extra_alloc;
  size_t total_heap = arena_committed + closure_committed + pool_total + extra_alloc;
  size_t heap_limit = arena_reserved + closure_reserved;

  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "total_heap_size",             Ant_Value_FromNumber((double)total_heap));
  js_set(js, obj, "total_heap_size_executable",  Ant_Value_FromNumber(0));
  js_set(js, obj, "total_physical_size",         Ant_Value_FromNumber((double)rss));
  js_set(js, obj, "total_available_size",        Ant_Value_FromNumber((double)(heap_limit > used_heap ? heap_limit - used_heap : 0)));
  js_set(js, obj, "total_global_handles_size",   Ant_Value_FromNumber(0));
  js_set(js, obj, "used_global_handles_size",    Ant_Value_FromNumber(0));
  js_set(js, obj, "used_heap_size",              Ant_Value_FromNumber((double)used_heap));
  js_set(js, obj, "heap_size_limit",             Ant_Value_FromNumber((double)heap_limit));
  js_set(js, obj, "malloced_memory",             Ant_Value_FromNumber((double)extra_alloc));
  js_set(js, obj, "peak_malloced_memory",        Ant_Value_FromNumber((double)extra_alloc));
  js_set(js, obj, "does_zap_garbage",            Ant_Value_FromNumber(0));
  js_set(js, obj, "number_of_native_contexts",   Ant_Value_FromNumber(1));
  js_set(js, obj, "number_of_detached_contexts", Ant_Value_FromNumber(0));
  js_set(js, obj, "total_heap_blinded_size",     Ant_Value_FromNumber(0));
  
  return obj;
}

static ant_value_t v8_get_heap_space_statistics(ant_params_t) {
  ant_value_t nursery = Ant_Object_Create(js);
  
  size_t arena_committed  = ant_object_committed(js);
  size_t arena_live_bytes = ant_object_live_bytes(js);
  double arena_available = (double)(arena_committed / 2 > arena_live_bytes / 2 ? arena_committed / 2 - arena_live_bytes / 2 : 0);

  js_set(js, nursery, "space_name",            js_mkstr(js, "new_space", 9));
  js_set(js, nursery, "space_size",            Ant_Value_FromNumber((double)arena_committed / 2));
  js_set(js, nursery, "space_used_size",       Ant_Value_FromNumber((double)arena_live_bytes / 2));
  js_set(js, nursery, "space_available_size",  Ant_Value_FromNumber(arena_available));
  js_set(js, nursery, "physical_space_size",   Ant_Value_FromNumber((double)arena_committed / 2));

  ant_value_t oldspace = Ant_Object_Create(js);
  size_t old_live = js->old_live_count * js->obj_arena.elem_size;
  double old_size = (double)(arena_committed / 2 > old_live ? arena_committed / 2 - old_live : 0);
  
  js_set(js, oldspace, "space_name",           js_mkstr(js, "old_space", 9));
  js_set(js, oldspace, "space_size",           Ant_Value_FromNumber((double)arena_committed / 2));
  js_set(js, oldspace, "space_used_size",      Ant_Value_FromNumber((double)old_live));
  js_set(js, oldspace, "space_available_size", Ant_Value_FromNumber(old_size));
  js_set(js, oldspace, "physical_space_size",  Ant_Value_FromNumber((double)arena_committed / 2));

  ant_value_t arr = Ant_Array_Create(js);
  js_arr_push(js, arr, nursery);
  js_arr_push(js, arr, oldspace);
  
  return arr;
}

static ant_value_t v8_get_heap_code_statistics(ant_params_t) {
  size_t closure_structs = js->closure_arena.live_count * js->closure_arena.elem_size;
  size_t bytecode_alloc  = js->alloc_bytes.closures;

  ant_pool_stats_t rope_stats = js_rope_pool_stats(js);
  ant_value_t obj = Ant_Object_Create(js);
  
  js_set(js, obj, "code_and_metadata_size",      Ant_Value_FromNumber((double)(closure_structs + bytecode_alloc)));
  js_set(js, obj, "bytecode_and_metadata_size",  Ant_Value_FromNumber((double)bytecode_alloc));
  js_set(js, obj, "external_script_source_size", Ant_Value_FromNumber((double)rope_stats.used));
  js_set(js, obj, "cpu_profiler_metadata_size",  Ant_Value_FromNumber(0));
  
  return obj;
}

ant_value_t v8_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  js_set(js, lib, "Serializer", v8_serializer_constructor(js));
  js_set(js, lib, "Deserializer", v8_deserializer_constructor(js));
  js_set(js, lib, "getHeapStatistics", Ant_Function_Create(v8_get_heap_statistics));
  js_set(js, lib, "getHeapSpaceStatistics", Ant_Function_Create(v8_get_heap_space_statistics));
  js_set(js, lib, "getHeapCodeStatistics", Ant_Function_Create(v8_get_heap_code_statistics));

  return lib;
}
