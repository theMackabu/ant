#ifndef ANT_PROPS_H
#define ANT_PROPS_H

#include "internal.h"

typedef enum {
  MKPROP_USE_DEFAULT_ATTRS = 1 << 0,
  MKPROP_EXPOSE_CFUNC = 1 << 1,
  MKPROP_KEYED_STORE = 1 << 2,
} mkprop_mode_t;

typedef struct {
  ant_value_t js_key;
  const char *bytes;
  size_t length;
  char *heap_bytes;
  const char *interned;
  bool is_symbol;
  bool requires_root;
  bool resolved;
  bool index_like;
} property_key_view_t;

bool is_const_prop(ant_prop_loc_t loc);
bool is_nonconfig_prop(ant_prop_loc_t loc);
bool index_keys_absent_from_protos(const ant_object_t *ptr);

void js_init_intern_cache(ant_t *js);
void property_key_view_dispose(property_key_view_t *key);
void prop_meta_from_shape(prop_meta_t *out, const ant_shape_prop_t *prop);

const ant_shape_prop_t *prop_shape_meta(ant_prop_loc_t loc);
size_t utf16_code_unit_to_wtf8(uint16_t code_unit, char out[4]);

ant_value_t mkprop_interned_attrs_impl(
  ant_t *js, ant_value_t obj, const char *interned_key, size_t key_len,
  ant_value_t v, uint8_t attrs, mkprop_mode_t mode
);

ant_value_t mkprop_bytes_exact_attrs(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
);

ant_value_t mkprop_bytes(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
);

ant_value_t mkprop_symbol_attrs_impl(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v,
  uint8_t attrs, mkprop_mode_t mode
);

ant_value_t call_proto_accessor(
  ant_t *js, ant_value_t prim, ant_value_t accessor, bool has_accessor,
  ant_value_t *arg, int arg_count, bool is_setter
);

ant_value_t property_key_view_init(
  ant_t *js, ant_value_t value, property_key_view_t *key,
  char *stack_buf, size_t stack_capacity
);

ant_value_t js_mk_ascii_char_cached(ant_t *js, unsigned char ch);
ant_value_t js_prop_load_from(ant_t *js, ant_value_t receiver, ant_prop_loc_t off);
ant_value_t property_key_view_materialize(ant_t *js, property_key_view_t *key);
ant_value_t object_has_own_key(ant_t *js, ant_value_t obj, property_key_view_t *key);

bool descriptor_has_own_key(ant_t *js, ant_value_t desc, const char *key, size_t key_len);
bool descriptor_bool_value(ant_t *js, ant_value_t desc, const char *key, size_t key_len, bool default_value);
bool string_wrapper_owns_key(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
bool array_own_value(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t *out);

ant_value_t exotic_own_get(ant_t *js, ant_value_t obj, const ant_object_t *ptr, const char *key, size_t len);
ant_value_t setprop_impl(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, bool keyed, bool *declined);
ant_value_t setprop_impl_receiver(ant_t *js, ant_value_t obj, ant_value_t receiver, ant_value_t k, ant_value_t v, bool keyed, bool *declined);

const char *index_key_find(ant_t *js, uint32_t idx, char *buf, size_t *len);
const char *index_key_intern(ant_t *js, uint32_t idx, char *buf, size_t *len);
const char *index_key_get_or_intern(ant_t *js, uint32_t idx);

ant_value_t js_setprop_throw(ant_t *js, ant_value_t obj, ant_value_t key, bool is_index, uint32_t idx, ant_value_t value);
ant_value_t js_setprop_index_throw(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value);

ant_value_t lkp_val(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_value_t lkp_sym_proto_val(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
ant_value_t mkprop_interned_keyed(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v);

uintptr_t lkp_with_getter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *getter_out, bool *has_getter_out);
uintptr_t lkp_with_setter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *setter_out, bool *has_setter_out);

bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
bool exotic_delete_allowed(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
bool try_accessor_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out);
bool property_key_view_requires_pin(const property_key_view_t *key, bool may_materialize);

bool js_try_get_sym_with_receiver(
  ant_t *js, ant_value_t obj, ant_value_t sym,
  ant_value_t receiver, ant_value_t *out
);

bool js_try_get_string_own_exotic(
  ant_t *js, ant_value_t obj, const char *key, 
  size_t key_len, ant_value_t *out, bool *is_length
);

#endif
