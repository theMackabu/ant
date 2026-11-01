#ifndef ANT_MODULES_FUNCTION_H
#define ANT_MODULES_FUNCTION_H

#include "internal.h"

ant_value_t builtin_Function(ant_params_t);
ant_value_t builtin_function_call(ant_params_t);
ant_value_t Ant_Function_ToString(ant_t *js, ant_value_t fn);
ant_value_t build_dynamic_function(ant_native_params_t, ant_value_t new_target, bool is_async, bool is_generator);

void js_function_init_prototype(ant_t *js, ant_value_t function_proto_obj);
void js_function_init_constructor(ant_t *js, ant_value_t func_ctor_obj, ant_value_t function_proto);
void js_function_init_kinds(ant_t *js, ant_value_t function_proto, ant_value_t func_ctor_func);

#endif
