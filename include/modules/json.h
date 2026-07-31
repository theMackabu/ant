#ifndef JSON_H
#define JSON_H

#include "types.h"

void init_json_module(ant_t *js);

ant_value_t js_json_parse(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_json_stringify(ant_t *js, ant_value_t *args, int nargs);

ant_value_t json_parse_value(ant_t *js, ant_value_t value);
ant_value_t json_stringify_value(ant_t *js, ant_value_t value);

#endif
