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

#define ANT_STRING(s)         js_mkstr(js, s, sizeof(s) - 1)
#define ANT_PTR(ptr)          js_mknum((double)(uintptr_t)(ptr))
#define ANT_COPY(buf, len, s) cpy(buf, len, s, sizeof(s) - 1)
#define REMAIN(n, len)        ((n) >= (len) ? 0 : (len) - (n))

#define JS_NAN     ((double)NAN)
#define JS_NEG_NAN ((double)(-NAN))
#define JS_INF     ((double)INFINITY)
#define JS_NEG_INF ((double)(-INFINITY))

#define js_true    (NANBOX_PREFIX | ((ant_value_t)T_BOOL << NANBOX_TYPE_SHIFT) | 1)
#define js_false   (NANBOX_PREFIX | ((ant_value_t)T_BOOL << NANBOX_TYPE_SHIFT))
#define js_bool(x) (js_false | (ant_value_t)!!(x))

ant_t *js_create(void *buf, size_t len);
ant_t *js_create_dynamic();

ant_value_t js_glob(ant_t *);
void js_mark_constructor(ant_value_t value, bool is_constructor);

// TODO: improve naming
ant_value_t js_eval_bytecode(ant_t *, const char *, size_t);
ant_value_t js_eval_bytecode_module(ant_t *, const char *, size_t);
ant_value_t js_eval_bytecode_eval(ant_t *, const char *, size_t);
ant_value_t js_eval_bytecode_eval_with_strict(ant_t *, const char *, size_t, bool);
ant_value_t js_eval_bytecode_repl(ant_t *, const char *, size_t);

void js_destroy(ant_t *);
bool js_truthy(ant_t *, ant_value_t);
void js_setstackbase(ant_t *, void *);
void js_setstacklimit(ant_t *, size_t);

uint32_t js_to_uint32(double d);
int32_t js_to_int32(double d);

bool js_chkargs(ant_value_t *, int, const char *);
void js_set_filename(ant_t *, const char *);
void js_stats(ant_t *, size_t *total, size_t *min, size_t *cstacksize);

ant_value_t js_mkundef(void);
ant_value_t js_mknull(void);
ant_value_t js_mknum(double);
ant_value_t js_mkpromise(ant_t *js);

ant_value_t js_getthis(ant_t *);
void js_setthis(ant_t *, ant_value_t);

ant_value_t js_getcurrentfunc(ant_t *);
ant_value_t js_get(ant_t *, ant_value_t, const char *);
ant_value_t js_getprop_proto(ant_t *, ant_value_t, const char *);
ant_value_t js_getprop_fallback(ant_t *js, ant_value_t obj, const char *name);
ant_value_t js_getprop_super(ant_t *js, ant_value_t super_obj, ant_value_t receiver, const char *name);

ant_offset_t js_arr_len(ant_t *js, ant_value_t arr);
ant_value_t js_arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx);

bool js_iter(
  ant_t *js, 
  ant_value_t iterable,
  bool (*callback)(
    ant_t *js,
    ant_value_t value,
    void *udata
  ), 
  void *udata
);

const char *js_sym_desc(ant_t *js, ant_value_t sym);
const char *js_sym_key(ant_value_t sym);

ant_value_t js_mksym_for(ant_t *, const char *key);
ant_value_t js_symbol_to_string(ant_t *js, ant_value_t sym);
ant_value_t js_get_sym(ant_t *, ant_value_t obj, ant_value_t sym);

ant_value_t js_mkobj(ant_t *);
ant_value_t js_mkobj_with_inobj_limit(ant_t *, uint8_t inobj_limit);
ant_value_t js_newobj(ant_t *);
ant_value_t js_mkarr(ant_t *);
ant_value_t js_mkstr(ant_t *, const void *, size_t);
ant_value_t js_mkstr_permanent(ant_t *, const void *, size_t);
ant_value_t js_mkbigint(ant_t *, const char *digits, size_t len, bool negative);
ant_value_t js_mksym(ant_t *, const char *desc);
ant_value_t js_mksym_well_known(ant_t *, const char *desc);
ant_value_t js_mkfun(ant_value_t (*fn)(ant_t *, ant_value_t *, int));
ant_value_t js_heavy_mkfun(ant_t *js, ant_value_t (*fn)(ant_t *, ant_value_t *, int), ant_value_t data);

ant_value_t js_mkprop_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_offset_t js_mkprop_fast_off(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

void js_set(ant_t *, ant_value_t, const char *, ant_value_t);
void js_set_sym(ant_t *, ant_value_t obj, ant_value_t sym, ant_value_t val);
void js_saveval(ant_t *js, ant_offset_t off, ant_value_t v);
void js_merge_obj(ant_t *, ant_value_t dst, ant_value_t src);
void js_arr_push(ant_t *, ant_value_t arr, ant_value_t val);
void js_set_proto(ant_value_t obj, ant_value_t proto);
void js_set_proto_wb(ant_t *js, ant_value_t obj, ant_value_t proto);
void js_set_proto_init(ant_value_t obj, ant_value_t proto);

ant_value_t js_propref_load(ant_t *js, ant_offset_t handle);
ant_value_t js_setprop(ant_t *, ant_value_t obj, ant_value_t key, ant_value_t val);
ant_value_t js_setprop_nonconfigurable(ant_t *, ant_value_t obj, const char *key, size_t keylen, ant_value_t val);

ant_value_t js_get_proto(ant_t *, ant_value_t obj);
ant_value_t js_get_ctor_proto(ant_t *, const char *name, size_t len);
ant_value_t js_tostring_val(ant_t *js, ant_value_t value);

uint8_t vtype(ant_value_t val);
size_t vdata(ant_value_t val);
ant_object_t *js_obj_ptr(ant_value_t val);
bool js_is_constructor(ant_t *js, ant_value_t value);

double js_getnum(ant_value_t val);
char *js_getstr(ant_t *js, ant_value_t val, size_t *len);

const char *js_str(ant_t *, ant_value_t val);
const char *get_str_prop(ant_t *js, ant_value_t obj, const char *key, ant_offset_t klen, ant_offset_t *out_len);

typedef struct {
  void *ctx;
  ant_offset_t off;
} ant_iter_t;

ant_iter_t js_prop_iter_begin(ant_t *js, ant_value_t obj);

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, ant_value_t *value);
void js_prop_iter_end(ant_iter_t *iter);

ant_value_t js_obj_to_func(ant_value_t obj);
ant_value_t js_obj_to_func_ex(ant_value_t obj, uint8_t flags);

ant_value_t js_mktypedarray(void *data);
void *js_gettypedarray(ant_value_t val);

ant_value_t js_mkffi(unsigned int index);
int js_getffi(ant_value_t val);

void js_check_unhandled_rejections(ant_t *js);
void js_setup_import_meta(ant_t *js, const char *filename);
void js_process_promise_handlers(ant_t *js, ant_value_t promise);
void js_mark_promise_trigger_dequeued(ant_t *js, ant_value_t promise);
bool js_mark_promise_trigger_queued(ant_t *js, ant_value_t promise);
void js_reject_promise(ant_t *js, ant_value_t promise, ant_value_t value);
void js_resolve_promise(ant_t *js, ant_value_t promise, ant_value_t value);

typedef ant_value_t (*js_getter_fn)(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
typedef ant_value_t (*js_keys_fn)(ant_t *js, ant_value_t obj);

typedef bool (*js_setter_fn)(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
typedef bool (*js_deleter_fn)(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
typedef void (*js_finalizer_fn)(ant_t *js, ant_object_t *obj);

void js_set_getter(ant_value_t obj, js_getter_fn getter);
void js_set_setter(ant_value_t obj, js_setter_fn setter);
void js_set_deleter(ant_value_t obj, js_deleter_fn deleter);
void js_set_keys(ant_value_t obj, js_keys_fn keys);
void js_set_finalizer(ant_value_t obj, js_finalizer_fn fn);

ant_value_t js_get_slot(ant_value_t obj, internal_slot_t slot);
ant_value_t js_promise_then(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected);

void js_set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value);
void js_set_slot_wb(ant_t *, ant_value_t obj, internal_slot_t slot, ant_value_t value);

#endif
