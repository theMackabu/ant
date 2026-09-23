#include "gc/verify.h"

#ifdef ANT_GC_VERIFY

#include "gc.h"
#include <stdlib.h>

void gc_verify_stress(ant_t *js) {
  static long every = -1;
  static long tick = 0;
  
  if (every < 0) {
    const char *v = getenv("ANT_GC_STRESS");
    every = v ? atol(v) : 0;
  }
  
  if (every <= 0 || gc_disabled || js->gc_running || js->gc_objects_running || !js->cstk.base) return;
  if (++tick % every != 0) return;
  
  gc_run(js);
}

void gc_verify_poison_object(ant_object_t *obj) {
  ant_value_t bad = mkval(kTypeObject, UINT64_C(0x7FFFFFFFF000));
  
  obj->proto = bad;
  obj->shape = NULL;
  obj->overflow_prop = (ant_value_t *)(uintptr_t)0xdead0001;
  
  for (size_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++) obj->inobj[i] = bad;
  
  obj->u.array.data = (ant_value_t *)(uintptr_t)0xdead0002;
  obj->u.array.len = 0x7fffffff;
  obj->prop_count = 0x7fffffff;
}

#endif
