#pragma once

#include "internal.h"

ant_value_t builtin_Array(ant_params_t);
void js_array_init_prototype(ant_t *js, ant_value_t array_proto);
void js_array_init_constructor(ant_t *js, ant_value_t arr_ctor_obj, ant_value_t array_proto);
