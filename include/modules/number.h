#pragma once

#include "internal.h"

ant_value_t builtin_number_toLocaleString(ant_params_t);
size_t format_number_to_locale_string(ant_value_t num, char *buf, size_t capacity);

void js_number_init_prototype(ant_t *js, ant_value_t number_proto);
void js_number_init_globals(ant_t *js, ant_value_t glob);
void js_number_init_constructor(ant_t *js, ant_value_t number_ctor_obj, ant_value_t number_proto, ant_value_t *parse_int, ant_value_t *parse_float);
