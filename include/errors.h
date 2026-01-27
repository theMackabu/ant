#ifndef ERRORS_H
#define ERRORS_H

#include "types.h"
#include <stdio.h>

#define ERR_FMT "\x1b[31m%.*s\x1b[0m: \x1b[1m%.*s\x1b[0m"
#define ERR_NAME_ONLY "\x1b[31m%.*s\x1b[0m"

typedef enum {
  JS_ERR_GENERIC = 0,
  JS_ERR_TYPE,
  JS_ERR_SYNTAX,
  JS_ERR_REFERENCE,
  JS_ERR_RANGE,
  JS_ERR_EVAL,
  JS_ERR_URI,
  JS_ERR_INTERNAL,
  JS_ERR_AGGREGATE,
} js_err_type_t;

js_err_type_t get_error_type(ant_t *js);
void js_print_stack_trace(FILE *stream);

__attribute__((format(printf, 4, 5)))
jsval_t js_create_error(ant_t *js, js_err_type_t err_type, jsval_t props, const char *fmt, ...);
jsval_t js_throw(ant_t *js, jsval_t value);

#define js_mkerr(js, ...) js_create_error(js, JS_ERR_TYPE, js_mkundef(), __VA_ARGS__)
#define js_mkerr_typed(js, err_type, ...) js_create_error(js, err_type, js_mkundef(), __VA_ARGS__)
#define js_mkerr_props(js, err_type, props, ...) js_create_error(js, err_type, props, __VA_ARGS__)

#endif