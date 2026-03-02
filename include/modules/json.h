#ifndef JSON_H
#define JSON_H
#define YYJSON_SKIP_VALUE ((yyjson_mut_val *)-1)

#include "types.h"

void init_json_module(void);

jsval_t js_json_parse(ant_t *js, jsval_t *args, int nargs);
jsval_t js_json_stringify(ant_t *js, jsval_t *args, int nargs);

#endif
