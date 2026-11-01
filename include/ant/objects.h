#ifndef ANT_API_OBJECTS_H
#define ANT_API_OBJECTS_H

#include "ant/base.h"

void js_mark_constructor(ant_value_t value, bool is_constructor);
const char *get_str_prop(ant_t *js, ant_value_t obj, const char *key, ant_offset_t klen, ant_offset_t *out_len);

ant_value_t js_get(ant_t *, ant_value_t, const char *);
ant_value_t js_getprop_proto(ant_t *, ant_value_t, const char *);
ant_value_t js_getprop_fallback(ant_t *js, ant_value_t obj, const char *name);
ant_value_t js_getprop_fallback_len(ant_t *js, ant_value_t obj, const char *name, size_t key_len);
ant_value_t js_getprop_super(ant_t *js, ant_value_t super_obj, ant_value_t receiver, const char *name);

ant_value_t js_get_sym(ant_t *, ant_value_t obj, ant_value_t sym);
ant_value_t js_get_symbol(ant_t *, ant_value_t obj, const char *key);
ant_value_t js_get_sym_with_receiver(ant_t *, ant_value_t obj, ant_value_t sym, ant_value_t receiver);

ant_value_t Ant_Object_Create(ant_t *);
ant_value_t Ant_Object_DeleteProperty(ant_t *js, ant_value_t obj, ant_value_t key);
ant_value_t Ant_Delete(ant_t *js, ant_value_t obj, ant_value_t key);
ant_value_t js_mkprop_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

void js_set(ant_t *, ant_value_t, const char *, ant_value_t);
void js_set_exact(ant_t *, ant_value_t, const char *, ant_value_t);
void js_set_sym(ant_t *, ant_value_t obj, ant_value_t sym, ant_value_t val);
void js_set_tostringtag(ant_t *, ant_value_t obj, ant_value_t tag);
void js_set_symbol(ant_t *, ant_value_t obj, const char *key, ant_value_t val);
bool js_prop_store(ant_t *js, ant_prop_loc_t loc, ant_value_t value);
void js_merge_obj(ant_t *, ant_value_t dst, ant_value_t src);

void js_set_proto(ant_t *, ant_value_t obj, ant_value_t proto);
void js_set_proto_wb(ant_t *, ant_value_t obj, ant_value_t proto);
void js_set_literal_proto(ant_t *, ant_value_t obj, ant_value_t proto);
void js_set_proto_init(ant_value_t obj, ant_value_t proto);

ant_value_t js_get_proto(ant_t *, ant_value_t obj);
ant_value_t js_prop_load(ant_prop_loc_t loc);
ant_value_t js_setprop(ant_t *, ant_value_t obj, ant_value_t key, ant_value_t val);
ant_value_t js_setprop_keyed(ant_t *, ant_value_t obj, ant_value_t key, ant_value_t val);
ant_value_t js_setprop_readonly_nonconfigurable(ant_t *, ant_value_t obj, const char *key, size_t keylen, ant_value_t val);

ant_object_t *js_obj_ptr(ant_value_t val);
ant_value_t js_obj_from_ptr(ant_object_t *obj);

typedef struct {
  ant_t *js;
  ant_object_t *obj;
  ant_offset_t off;
} ant_iter_t;

typedef struct {
  uint32_t slot;
  bool is_symbol;
  const char *str;
  size_t key_len;
  ant_offset_t sym_off;
} ant_iter_key_t;

ant_iter_t js_prop_iter_begin(ant_t *js, ant_value_t obj);
void js_prop_iter_end(ant_iter_t *iter);
bool js_is_constructor(ant_value_t value);

bool js_prop_iter_next_val(ant_iter_t *iter, ant_value_t *key_out, ant_value_t *value);
bool js_prop_iter_next_key(ant_iter_t *iter, ant_iter_key_t *key_out, ant_value_t *value);
bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, ant_value_t *value);
bool js_try_get_own_data_prop(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out);
bool js_is_own_enumerable_prop(ant_t *js, ant_value_t source, ant_object_t *source_ptr, const ant_iter_key_t *key);

ant_value_t js_obj_to_func(ant_t *js, ant_value_t obj);
ant_value_t js_obj_to_func_ex(ant_t *js, ant_value_t obj, uint8_t flags);

ant_value_t js_mktypedarray(void *data);
void *js_gettypedarray(ant_value_t val);

typedef void (*js_finalizer_fn)(ant_t *js, ant_object_t *obj);
typedef bool (*js_setter_fn)(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
typedef bool (*js_deleter_fn)(ant_t *js, ant_value_t obj, const char *key, size_t key_len);

typedef ant_value_t (*js_definer_fn)(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t value, bool has_value, bool plain_data
);

typedef ant_value_t (*js_getter_fn)(
  ant_t *js, ant_value_t obj, 
  const char *key, size_t key_len
);

void js_set_getter(ant_value_t obj, js_getter_fn getter);
void js_set_setter(ant_value_t obj, js_setter_fn setter);
void js_set_deleter(ant_value_t obj, js_deleter_fn deleter);
void js_set_definer(ant_value_t obj, js_definer_fn definer);
void js_set_finalizer(ant_value_t obj, js_finalizer_fn fn);

ant_value_t js_get_slot(ant_value_t obj, internal_slot_t slot);
void js_set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value);
void js_set_slot_wb(ant_t *, ant_value_t obj, internal_slot_t slot, ant_value_t value);
bool js_reserve_slots(ant_value_t obj, uint8_t capacity);

#endif
