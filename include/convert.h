#pragma once

#include "internal.h"

ant_value_t coerce_to_str_hint(ant_t *js, ant_value_t v, int hint);

bool is_unboxed_obj(ant_t *js, ant_value_t val, ant_value_t expected_proto);
bool js_guard_proto_chain_absence(ant_t *js, ant_object_t *obj);

bool js_try_invoke(
  ant_t *js, ant_value_t obj, const char *method, 
  ant_args_t, ant_value_t *out_result
);
