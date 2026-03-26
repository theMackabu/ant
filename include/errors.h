#ifndef ERRORS_H
#define ERRORS_H

#include "types.h"
#include <stdio.h>
#include <stdbool.h>

typedef struct sv_func sv_func_t;

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

bool print_uncaught_throw(ant_t *js);
bool print_unhandled_promise_rejection(ant_t *js, ant_value_t value);

void js_clear_error_site(ant_t *js);
void js_print_stack_trace_vm(ant_t *js, FILE *stream);
void js_set_error_site_from_vm_top(ant_t *js);
void js_capture_stack(ant_t *js, ant_value_t err_obj);

void js_get_call_location(
  ant_t *js, const char **out_filename,
  int *out_line, int *out_col
);

void js_set_error_site_from_bc(
  ant_t *js, sv_func_t *func, 
  int bc_offset, const char *filename
);

void js_set_error_site(
  ant_t *js, const char *src,
  ant_offset_t src_len, const char *filename,
  ant_offset_t off, ant_offset_t span_len
);

__attribute__((format(printf, 4, 5)))
ant_value_t js_create_error(ant_t *js, js_err_type_t err_type, ant_value_t props, const char *fmt, ...);
ant_value_t js_make_error_silent(ant_t *js, js_err_type_t err_type, const char *message);

ant_value_t js_capture_raw_stack(ant_t *js);
ant_value_t js_throw(ant_t *js, ant_value_t value);

#define js_mkerr(js, ...) js_create_error(js, JS_ERR_TYPE, js_mkundef(), __VA_ARGS__)
#define js_mkerr_typed(js, err_type, ...) js_create_error(js, err_type, js_mkundef(), __VA_ARGS__)
#define js_mkerr_props(js, err_type, props, ...) js_create_error(js, err_type, props, __VA_ARGS__)

#endif
