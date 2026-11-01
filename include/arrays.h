#ifndef ANT_ARRAYS_H
#define ANT_ARRAYS_H

#include "internal.h"

#define PROXY_AWARE_LENGTH_OR_RETURN(obj_expr, out_var)                       \
  ant_offset_t out_var = 0;                                                   \
  do {                                                                        \
    ant_value_t _len_result = proxy_aware_length(js, (obj_expr), &(out_var)); \
    if (is_err(_len_result)) return _len_result;                              \
  } while (0)

static constexpr ant_offset_t ARRAY_DENSE_GAP = 1024;
static constexpr ant_value_t JS_SETPROP_ARRAY_NOT_HANDLED = ANT_SENTINEL_TAG | 0xA551ULL;

typedef enum {
  OBJECT_FILL_NONE,
  OBJECT_FILL_DONE,
  OBJECT_FILL_RESUME,
} object_fill_t;

ant_value_t array_method_has_index(ant_t *js, ant_value_t arr, ant_offset_t idx);
ant_value_t array_method_get_index(ant_t *js, ant_value_t arr, ant_offset_t idx);

bool is_array_index(const char *key, ant_offset_t klen);
bool parse_array_index(const char *key, size_t klen, ant_offset_t max_len, unsigned long *out_idx);

ant_object_t *array_obj_ptr(ant_value_t obj);
ant_object_t *real_array_obj_ptr(ant_value_t obj);

bool array_dense_storage(const ant_object_t *ptr);
bool array_dense_covers(const ant_object_t *ptr, ant_offset_t n);
bool array_dense_read(const ant_object_t *ptr, ant_offset_t idx, ant_value_t *out);
bool array_may_have_holes(ant_value_t obj);
bool array_may_have_dense_elements(ant_value_t obj);

void array_mark_may_have_holes(ant_value_t obj);
void array_mark_packed(ant_value_t obj);
void array_define_or_set_index(ant_t *js, ant_value_t obj, const char *key, size_t klen);

ant_offset_t get_array_length(ant_t *js, ant_value_t arr);
ant_value_t array_like_length_checked(ant_t *js, ant_value_t obj, ant_offset_t *out);

ant_offset_t get_dense_buf(ant_value_t arr);
ant_object_t *dense_obj(ant_offset_t doff);
ant_value_t *dense_data(ant_offset_t doff);
ant_offset_t dense_capacity(ant_offset_t doff);
ant_value_t dense_get(ant_offset_t doff, ant_offset_t idx);
object_fill_t arr_fill_object_values(ant_t *js, ant_object_t *ptr, ant_value_t *out, uint32_t *resume);

void dense_set(ant_t *js, ant_offset_t doff, ant_offset_t idx, ant_value_t val);
ant_offset_t dense_grow(ant_t *js, ant_value_t arr, ant_offset_t needed);

ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx);
void arr_set(ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t val);
bool arr_own_dense_element(ant_t *js, ant_value_t arr, const char *key, size_t klen, ant_value_t *out);
bool arr_has(ant_t *js, ant_value_t arr, ant_offset_t idx);

bool array_length_readonly(ant_t *js, ant_value_t arr);
bool array_result_extensible(ant_value_t result);

ant_value_t array_result_set(ant_t *js, ant_value_t result, bool fresh, ant_offset_t idx, ant_value_t val);
ant_value_t mkarr_dense_literal_capacity(ant_t *js, const ant_value_t *elements, uint32_t count, bool exact_capacity);

void arr_del(ant_t *js, ant_value_t arr, ant_offset_t idx);
ant_value_t array_alloc_intrinsic(ant_t *js, ant_value_t source, ant_offset_t length_hint);
ant_value_t array_alloc_from_ctor_with_length(ant_t *js, ant_value_t ctor, ant_offset_t length_hint);
ant_value_t array_species_create(ant_t *js, ant_value_t receiver, ant_offset_t length, bool *intrinsic);

__attribute__((noinline)) bool array_length_readonly_slow(ant_t *js, ant_value_t arr);
__attribute__((noinline, cold)) ant_value_t array_length_write_ignored(ant_t *js, ant_value_t value, bool *declined);
__attribute__((noinline)) bool array_store_reaches_proto_slow(ant_t *js, ant_value_t arr, uint64_t idx);

ant_value_t array_length_readonly_error(ant_t *js);
ant_value_t set_declined(bool *declined, ant_value_t value);
ant_value_t array_grow_readonly_result(ant_t *js, uint64_t idx, ant_value_t value, bool *declined);
ant_value_t validate_array_length(ant_t *js, ant_value_t v);

ant_value_t check_object_extensibility(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t check_object_extensibility_index(ant_t *js, ant_value_t obj, uint64_t idx);
ant_value_t check_object_extensibility_symbol(ant_t *js, ant_value_t obj, ant_value_t sym);

void array_len_set(ant_t *js, ant_value_t obj, ant_offset_t new_len);
bool array_length_has_guarded_elements(ant_object_t *ptr);
void array_length_set_unguarded(ant_t *js, ant_value_t obj, ant_offset_t new_len);
bool array_proto_chain_plain(ant_t *js, ant_object_t *arr);
bool array_elements_proto_plain(ant_t *js, ant_object_t *ptr);

ant_object_t *array_dense_source(ant_t *js, ant_value_t arr);
bool array_search_eq(ant_t *js, ant_value_t elem, ant_value_t search);
bool array_store_reaches_proto(ant_t *js, ant_value_t arr, ant_object_t *ptr, uint64_t idx);
bool own_key_is_array_index(const char *key, size_t key_len, unsigned long *out_idx);

ant_value_t array_truncate_length(ant_t *js, ant_value_t obj, ant_offset_t new_len, ant_offset_t *out_len);
ant_value_t array_truncate_stopped_error(ant_t *js, ant_offset_t final_len);

bool arr_fill_numeric_typed(ant_t *js, ant_value_t arr, const TypedArrayData *ta);
bool arr_fill_dense_values(ant_value_t arr, ant_offset_t doff, ant_offset_t len);

ant_value_t proxy_read_target(ant_t *js, ant_value_t obj);
ant_value_t proxy_aware_length(ant_t *js, ant_value_t obj, ant_offset_t *out_len);
ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
ant_value_t proxy_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx);

bool array_method_dense_element(ant_value_t arr, ant_offset_t idx, ant_value_t *out);
ant_object_t *array_method_elements(ant_value_t arr);

ant_value_t array_method_element(
  ant_t *js, ant_value_t arr, ant_object_t *elems, 
  ant_offset_t idx, ant_value_t *out
);

ant_value_t alloc_array_with_proto_capacity(
  ant_t *js, ant_value_t proto, uint32_t minimum_capacity,
  uint32_t overwritten_prefix, bool exact_capacity
);

#endif
