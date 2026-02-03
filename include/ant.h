#ifndef ANT_H
#define ANT_H

#pragma once
#define PCRE2_CODE_UNIT_WIDTH 8

#include "types.h"
#include "common.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#define STR_PROTO "__proto__"
#define STR_PROTO_LEN 9

#define ANT_LIMIT_SIZE_CACHE 16384

#define ANT_STRING(s)         js_mkstr(js, s, sizeof(s) - 1)
#define ANT_PTR(ptr)          js_mknum((double)(uintptr_t)(ptr))
#define ANT_COPY(buf, len, s) cpy(buf, len, s, sizeof(s) - 1)
#define REMAIN(n, len)        ((n) >= (len) ? 0 : (len) - (n))

#define JS_NAN     ((double)NAN)
#define JS_NEG_NAN ((double)(-NAN))
#define JS_INF     ((double)INFINITY)
#define JS_NEG_INF ((double)(-INFINITY))

#define js_true    (NANBOX_PREFIX | ((jsval_t)T_BOOL << NANBOX_TYPE_SHIFT) | 1)
#define js_false   (NANBOX_PREFIX | ((jsval_t)T_BOOL << NANBOX_TYPE_SHIFT))
#define js_bool(x) (js_false | (jsval_t)!!(x))

#define JS_DESC_W (1 << 0)
#define JS_DESC_E (1 << 1)
#define JS_DESC_C (1 << 2)

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
jsval_t js_mknum(double);

jsval_t js_getthis(ant_t *);
void js_setthis(ant_t *, jsval_t);

jsval_t js_getcurrentfunc(ant_t *);
jsval_t js_get(ant_t *, jsval_t, const char *);
jsval_t js_getprop_proto(ant_t *, jsval_t, const char *);
jsval_t js_getprop_fallback(ant_t *js, jsval_t obj, const char *name);

jsoff_t js_arr_len(struct js *js, jsval_t arr);
jsval_t js_arr_get(struct js *js, jsval_t arr, jsoff_t idx);

bool js_iter(
  ant_t *js, 
  jsval_t iterable,
  bool (*callback)(
    ant_t *js,
    jsval_t value,
    void *udata
  ), 
  void *udata
);

uint64_t js_sym_id(jsval_t sym);
const char *js_sym_desc(ant_t *js, jsval_t sym);

jsval_t js_mksym_for(ant_t *, const char *key);
jsval_t js_symbol_to_string(struct js *js, jsval_t sym);
const char *js_sym_key(jsval_t sym);

jsval_t js_mkobj(ant_t *);
jsval_t js_newobj(ant_t *);
jsval_t js_mkarr(ant_t *);
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
void js_arr_push(ant_t *, jsval_t arr, jsval_t val);

jsval_t js_setprop(ant_t *, jsval_t obj, jsval_t key, jsval_t val);
jsval_t js_setprop_nonconfigurable(ant_t *, jsval_t obj, const char *key, size_t keylen, jsval_t val);
void js_set_proto(ant_t *, jsval_t obj, jsval_t proto);

jsval_t js_get_proto(ant_t *, jsval_t obj);
jsval_t js_get_ctor_proto(ant_t *, const char *name, size_t len);
jsval_t js_tostring_val(ant_t *js, jsval_t value);

uint8_t vtype(jsval_t val);
size_t vdata(jsval_t val);

double js_getnum(jsval_t val);
char *js_getstr(ant_t *js, jsval_t val, size_t *len);

const char *js_str(ant_t *, jsval_t val);
const char *get_str_prop(struct js *js, jsval_t obj, const char *key, jsoff_t klen, jsoff_t *out_len);

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

#endif