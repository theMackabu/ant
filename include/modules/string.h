#pragma once

#include "isolate.h"

double string_number_to_integer(double number);

void js_string_init_prototype(ant_t *js, ant_value_t string_proto);
void js_string_init_constructor(ant_t *js, ant_value_t str_ctor_obj, ant_value_t string_proto);

ant_value_t builtin_string_toString(ant_params_t);
ant_value_t string_to_integer_or_infinity(ant_t *js, ant_value_t value, double *out);
