#ifndef ANT_ASSERT_MODULE_H
#define ANT_ASSERT_MODULE_H

#include "silver/engine.h"

ant_value_t assert_library(ant_t *js);

static inline bool promise_was_rejected(ant_value_t result) {
  if (vtype(result) != kTypePromise) return false;
  ant_object_t *obj = js_obj_ptr(js_as_obj(result));
  ant_promise_state_t *pd = ant_object_promise_state(obj);
  return pd && pd->state == 2;
}

static inline void promise_mark_handled(ant_value_t v) {
  if (vtype(v) != kTypePromise) return;
  ant_object_t *obj = js_obj_ptr(js_as_obj(v));
  ant_promise_state_t *pd = ant_object_promise_state(obj);
  if (pd) pd->has_rejection_handler = true;
}

#endif
