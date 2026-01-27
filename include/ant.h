#ifndef ANT_H
#define ANT_H

#pragma once
#define PCRE2_CODE_UNIT_WIDTH 8

#include <math.h>
#include <common.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#define STR_PROTO "__proto__"
#define STR_PROTO_LEN 9

#define ANT_STRING(s)         js_mkstr(js, s, sizeof(s) - 1)
#define ANT_PTR(ptr)          js_mknum((double)(uintptr_t)(ptr))
#define ANT_COPY(buf, len, s) cpy(buf, len, s, sizeof(s) - 1)

#define JS_NAN     ((double)NAN)
#define JS_NEG_NAN ((double)(-NAN))
#define JS_INF     ((double)INFINITY)
#define JS_NEG_INF ((double)(-INFINITY))

struct js;

typedef struct js ant_t;
typedef unsigned long long u64;

typedef int      jshdl_t;
typedef uint64_t jsoff_t;
typedef uint64_t jsval_t;

#define ANT_LIMIT_SIZE_CACHE 16384
#define CORO_PER_TICK_LIMIT 10000

#define GC_FWD_ARGS jsval_t (*fwd_val)(void *ctx, jsval_t old), void *ctx
#define GC_UPDATE_ARGS ant_t *js, jsoff_t (*fwd_off)(void *ctx, jsoff_t old), GC_FWD_ARGS

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
  JS_ERR_AGGREGATE,
  JS_ERR_GENERIC
} js_err_type_t;

#define JS_DESC_W (1 << 0)
#define JS_DESC_E (1 << 1)
#define JS_DESC_C (1 << 2)

#define js_mkerr(js, ...) js_create_error(js, JS_ERR_TYPE, js_mkundef(), __VA_ARGS__)
#define js_mkerr_typed(js, err_type, ...) js_create_error(js, err_type, js_mkundef(), __VA_ARGS__)
#define js_mkerr_props(js, err_type, props, ...) js_create_error(js, err_type, props, __VA_ARGS__)
jsval_t js_create_error(ant_t *js, js_err_type_t err_type, jsval_t props, const char *fmt, ...);

ant_t *js_create(void *buf, size_t len);
ant_t *js_create_dynamic(size_t initial_size, size_t max_size);

jsval_t js_glob(ant_t *);
jsval_t js_mkscope(ant_t *);
jsval_t js_getscope(ant_t *);
jsval_t js_eval(ant_t *, const char *, size_t);

void js_destroy(ant_t *);
void js_delscope(ant_t *);
bool js_truthy(ant_t *, jsval_t);
void js_setstacklimit(ant_t *, size_t);
void js_setstackbase(ant_t *, void *);

uint32_t js_to_uint32(double d);
int32_t js_to_int32(double d);

bool js_chkargs(jsval_t *, int, const char *);
void js_set_filename(ant_t *, const char *);
void js_stats(ant_t *, size_t *total, size_t *min, size_t *cstacksize);
size_t js_getbrk(ant_t *);

jshdl_t js_root(ant_t *, jsval_t);
jsval_t js_deref(ant_t *, jshdl_t);

void js_unroot(ant_t *, jshdl_t);
void js_root_update(ant_t *, jshdl_t, jsval_t);

jsval_t js_mkundef(void);
jsval_t js_mknull(void);
jsval_t js_mktrue(void);
jsval_t js_mkfalse(void);
jsval_t js_mknum(double);

jsval_t js_getthis(ant_t *);
void js_setthis(ant_t *, jsval_t);

jsval_t js_getcurrentfunc(ant_t *);
jsval_t js_get(ant_t *, jsval_t, const char *);
jsval_t js_getprop_proto(ant_t *, jsval_t, const char *);
bool js_iter(ant_t *js, jsval_t iterable, bool (*callback)(ant_t *js, jsval_t value, void *udata), void *udata);

uint64_t js_sym_id(jsval_t sym);
const char *js_sym_desc(ant_t *js, jsval_t sym);

jsval_t js_mksym_for(ant_t *, const char *key);
const char *js_sym_key(jsval_t sym);

jsval_t js_mkobj(ant_t *);
jsval_t js_newobj(ant_t *);
jsval_t js_mkarr(ant_t *);
void js_arr_push(ant_t *, jsval_t arr, jsval_t val);
jsval_t js_mkstr(ant_t *, const void *, size_t);
jsval_t js_mkbigint(ant_t *, const char *digits, size_t len, bool negative);
jsval_t js_mksym(ant_t *, const char *desc);
jsval_t js_mkfun(jsval_t (*fn)(ant_t *, jsval_t *, int));
jsval_t js_heavy_mkfun(ant_t *js, jsval_t (*fn)(ant_t *, jsval_t *, int), jsval_t data);

jsval_t js_mkprop_fast(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v);
jsoff_t js_mkprop_fast_off(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v);

jsval_t js_call(ant_t *js, jsval_t func, jsval_t *args, int nargs);
jsval_t js_call_with_this(ant_t *js, jsval_t func, jsval_t this_val, jsval_t *args, int nargs);

void js_set(ant_t *, jsval_t, const char *, jsval_t);
void js_saveval(ant_t *js, jsoff_t off, jsval_t v);
bool js_del(ant_t *, jsval_t obj, const char *key);
void js_merge_obj(ant_t *, jsval_t dst, jsval_t src);

jsval_t js_setprop(ant_t *, jsval_t obj, jsval_t key, jsval_t val);
jsval_t js_setprop_nonconfigurable(ant_t *, jsval_t obj, const char *key, size_t keylen, jsval_t val);
void js_set_proto(ant_t *, jsval_t obj, jsval_t proto);

jsval_t js_get_proto(ant_t *, jsval_t obj);
jsval_t js_get_ctor_proto(ant_t *, const char *name, size_t len);
jsval_t js_tostring_val(ant_t *js, jsval_t value);

int js_type(jsval_t val);
int js_type_ex(ant_t *js, jsval_t val);

int js_getbool(jsval_t val);
uint8_t vtype(jsval_t val);
size_t vdata(jsval_t val);

double js_getnum(jsval_t val);
char *js_getstr(ant_t *js, jsval_t val, size_t *len);

const char *js_str(ant_t *, jsval_t val);

typedef struct {
  void *ctx;
  jsoff_t off;
} ant_iter_t;

ant_iter_t js_prop_iter_begin(ant_t *js, jsval_t obj);

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, jsval_t *value);
void js_prop_iter_end(ant_iter_t *iter);

jsval_t js_obj_to_func(jsval_t obj);
jsval_t js_mkpromise(ant_t *js);

jsval_t js_mktypedarray(void *data);
void *js_gettypedarray(jsval_t val);

jsval_t js_mkffi(unsigned int index);
int js_getffi(jsval_t val);

void js_resolve_promise(ant_t *js, jsval_t promise, jsval_t value);
void js_reject_promise(ant_t *js, jsval_t promise, jsval_t value);
void js_check_unhandled_rejections(ant_t *js);
void js_process_promise_handlers(ant_t *js, uint32_t promise_id);

void js_run_event_loop(ant_t *js);
void js_poll_events(ant_t *js);
void js_setup_import_meta(ant_t *js, const char *filename);

typedef jsval_t (*ant_library_init_fn)(ant_t *js);
void ant_register_library(ant_library_init_fn init_fn, const char *name, ...);

#define ant_standard_library(name, lib) \
  ant_register_library(lib, name, "ant:" name, "node:" name, NULL)

typedef jsval_t (*js_getter_fn)(ant_t *js, jsval_t obj, const char *key, size_t key_len);
typedef bool (*js_setter_fn)(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t value);

void js_set_getter(ant_t *js, jsval_t obj, js_getter_fn getter);
void js_set_setter(ant_t *js, jsval_t obj, js_setter_fn setter);

void js_set_descriptor(ant_t *js, jsval_t obj, const char *key, size_t klen, int flags);
void js_set_getter_desc(ant_t *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, int flags);
void js_set_setter_desc(ant_t *js, jsval_t obj, const char *key, size_t klen, jsval_t setter, int flags);
void js_set_accessor_desc(ant_t *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, jsval_t setter, int flags);

jsval_t js_get_slot(ant_t *js, jsval_t obj, internal_slot_t slot);
void js_set_slot(ant_t *js, jsval_t obj, internal_slot_t slot, jsval_t value);

bool js_is_slot_prop(jsoff_t header);
jsoff_t js_next_prop(jsoff_t header);
jsoff_t js_loadoff(ant_t *js, jsoff_t off);

void js_print_stack_trace(FILE *stream);
void js_set_needs_gc(ant_t *js, bool needs);
void js_set_gc_suppress(ant_t *js, bool suppress);

size_t js_gc_compact(ant_t *js);

#endif