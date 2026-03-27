#ifndef ANT_PTR_H
#define ANT_PTR_H

#include "types.h"
#include "internal.h" // IWYU pragma: keep

static inline void js_set_native_ptr(ant_value_t obj, void *ptr) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o) return;
  o->native.ptr = ptr;
}

static inline void *js_get_native_ptr(ant_value_t obj) {
  ant_object_t *o = js_obj_ptr(obj);
  return o ? o->native.ptr : NULL;
}

static inline void js_set_native_tag(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o) return;
  o->native.tag = tag;
}

static inline uint32_t js_get_native_tag(ant_value_t obj) {
  ant_object_t *o = js_obj_ptr(obj);
  return o ? o->native.tag : 0;
}


static inline bool js_check_native_tag(ant_value_t obj, uint32_t tag) {
  return js_get_native_tag(obj) == tag;
}

#endif