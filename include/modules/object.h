#pragma once

#include "internal.h"

static constexpr uint32_t PROXY_PROTO_HOP_LIMIT = 100 * 1024;

ant_value_t builtin_Object(ant_params_t);
ant_value_t proxy_proto_hop(ant_t *js, ant_value_t proxy, uint32_t *hops);

ant_value_t builtin_object_toString(ant_params_t);
ant_value_t builtin_object_toLocaleString(ant_params_t);

void js_object_init_prototype(ant_t *js, ant_value_t object_proto);
void js_object_init_constructor(ant_t *js, ant_value_t obj_func_obj, ant_value_t object_proto);
