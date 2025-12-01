#ifndef JSON_H
#define JSON_H

#include "ant.h"

void init_json_module();

jsval_t js_json_parse(struct js *js, jsval_t *args, int nargs);
jsval_t js_json_stringify(struct js *js, jsval_t *args, int nargs);

#endif
