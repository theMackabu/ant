#ifndef ANT_NATIVE_PTR_H
#define ANT_NATIVE_PTR_H

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

static inline void js_set_native(ant_value_t obj, void *ptr, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o) return;
  o->native.ptr = ptr;
  o->native.tag = tag;
}

static inline void js_clear_native(ant_value_t obj) {
  js_set_native(obj, NULL, 0);
}

static inline uint32_t js_get_native_tag(ant_value_t obj) {
  ant_object_t *o = js_obj_ptr(obj);
  return o ? o->native.tag : 0;
}


static inline bool js_check_native_tag(ant_value_t obj, uint32_t tag) {
  return js_get_native_tag(obj) == tag;
}

#endif
