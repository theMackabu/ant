#ifndef JSON_H
#define JSON_H
#define YYJSON_SKIP_VALUE ((yyjson_mut_val *)-1)

#include "types.h"

void init_json_module(void);

ant_value_t js_json_parse(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_json_stringify(ant_t *js, ant_value_t *args, int nargs);

#endif
