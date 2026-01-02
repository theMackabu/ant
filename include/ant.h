#ifndef ANT_H
#define ANT_H

#pragma once
#define PCRE2_CODE_UNIT_WIDTH 8

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <config.h>

#define STR_PROTO "__proto__"
#define STR_PROTO_LEN 9
#define ANT_LIMIT_SIZE_CACHE 16384

struct js;
extern bool executing_coro;

typedef uint32_t jsoff_t;
typedef uint64_t jsval_t;

enum { 
  JS_UNDEF, JS_NULL, JS_TRUE, JS_FALSE, JS_STR, JS_NUM,
  JS_ERR, JS_PRIV, JS_PROMISE, JS_OBJ, JS_FUNC, JS_SYMBOL
};

typedef enum {
  JS_ERR_TYPE,
  JS_ERR_SYNTAX,
  JS_ERR_REFERENCE,
  JS_ERR_RANGE,
  JS_ERR_EVAL,
  JS_ERR_URI,
  JS_ERR_INTERNAL,
  JS_ERR_GENERIC
} js_err_type_t;

#define JS_DESC_W (1 << 0)
#define JS_DESC_E (1 << 1)
#define JS_DESC_C (1 << 2)

struct js *js_create(void *buf, size_t len);
struct js *js_create_dynamic(size_t initial_size, size_t max_size);

jsval_t js_glob(struct js *);
jsval_t js_eval(struct js *, const char *, size_t);

void js_dump(struct js *);
void js_destroy(struct js *);
void js_mkscope(struct js *);
void js_delscope(struct js *);
bool js_truthy(struct js *, jsval_t);
void js_setmaxcss(struct js *, size_t);

bool js_chkargs(jsval_t *, int, const char *);
void js_set_filename(struct js *, const char *);
void js_stats(struct js *, size_t *total, size_t *min, size_t *cstacksize);
size_t js_getbrk(struct js *);

jsval_t js_mkundef(void);
jsval_t js_mknull(void);
jsval_t js_mktrue(void);
jsval_t js_mkfalse(void);
jsval_t js_mknum(double);

jsval_t js_getthis(struct js *);
void js_setthis(struct js *, jsval_t);

jsval_t js_getcurrentfunc(struct js *);
jsval_t js_get(struct js *, jsval_t, const char *);

uint64_t js_sym_id(jsval_t sym);
const char *js_sym_desc(struct js *js, jsval_t sym);

jsval_t js_mksym_for(struct js *, const char *key);
const char *js_sym_key(jsval_t sym);

jsval_t js_mkobj(struct js *);
jsval_t js_mkarr(struct js *);
void js_arr_push(struct js *, jsval_t arr, jsval_t val);
jsval_t js_mkstr(struct js *, const void *, size_t);
jsval_t js_mksym(struct js *, const char *desc);
jsval_t js_mkerr(struct js *js, const char *fmt, ...);
jsval_t js_mkerr_typed(struct js *js, js_err_type_t err_type, const char *fmt, ...);
jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int));
jsval_t js_call(struct js *js, jsval_t func, jsval_t *args, int nargs);
jsval_t js_call_with_this(struct js *js, jsval_t func, jsval_t this_val, jsval_t *args, int nargs);

void js_set(struct js *, jsval_t, const char *, jsval_t);
bool js_del(struct js *, jsval_t obj, const char *key);
void js_merge_obj(struct js *, jsval_t dst, jsval_t src);

jsval_t js_setprop(struct js *, jsval_t obj, jsval_t key, jsval_t val);
void js_set_proto(struct js *, jsval_t obj, jsval_t proto);

jsval_t js_get_proto(struct js *, jsval_t obj);
jsval_t js_get_ctor_proto(struct js *, const char *name, size_t len);
jsval_t js_tostring_val(struct js *js, jsval_t value);

int js_type(jsval_t val);
int js_type_ex(struct js *js, jsval_t val);

int js_getbool(jsval_t val);
uint8_t vtype(jsval_t val);
size_t vdata(jsval_t val);

double js_getnum(jsval_t val);
char *js_getstr(struct js *js, jsval_t val, size_t *len);

const char *js_str(struct js *, jsval_t val);

typedef struct {
  jsval_t obj;
  void *current;
  void *js_internal;
} js_prop_iter_t;

js_prop_iter_t js_prop_iter_begin(struct js *js, jsval_t obj);
bool js_prop_iter_next(js_prop_iter_t *iter, const char **key, size_t *key_len, jsval_t *value);
void js_prop_iter_end(js_prop_iter_t *iter);

jsval_t js_obj_to_func(jsval_t obj);
jsval_t js_mkpromise(struct js *js);

jsval_t js_mktypedarray(void *data);
void *js_gettypedarray(jsval_t val);

jsval_t js_mkffi(unsigned int index);
int js_getffi(jsval_t val);

void js_resolve_promise(struct js *js, jsval_t promise, jsval_t value);
void js_reject_promise(struct js *js, jsval_t promise, jsval_t value);

void js_run_event_loop(struct js *js);
void js_poll_events(struct js *js);
void js_setup_import_meta(struct js *js, const char *filename);

typedef jsval_t (*ant_library_init_fn)(struct js *js);
void ant_register_library(ant_library_init_fn init_fn, const char *name, ...);

#define ant_standard_library(name, lib) \
  ant_register_library(lib, name, "ant:" name, "node:" name, NULL)

typedef jsval_t (*js_getter_fn)(struct js *js, jsval_t obj, const char *key, size_t key_len);
typedef bool (*js_setter_fn)(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t value);

void js_set_getter(struct js *js, jsval_t obj, js_getter_fn getter);
void js_set_setter(struct js *js, jsval_t obj, js_setter_fn setter);

void js_set_descriptor(struct js *js, jsval_t obj, const char *key, size_t klen, int flags);
void js_set_getter_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, int flags);
void js_set_setter_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t setter, int flags);
void js_set_accessor_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, jsval_t setter, int flags);

jsval_t js_get_slot(struct js *js, jsval_t obj, internal_slot_t slot);
void js_set_slot(struct js *js, jsval_t obj, internal_slot_t slot, jsval_t value);

bool js_is_slot_prop(jsoff_t header);
jsoff_t js_next_prop(jsoff_t header);
jsoff_t js_loadoff(struct js *js, jsoff_t off);

void js_print_stack_trace(FILE *stream);

#endif