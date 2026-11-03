#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "utf8.h"
#include "arrays.h"
#include "keys.h"
#include "props.h"
#include "cfunc.h"
#include "stringify.h"
#include "protectors.h"
#include "modules/proxy.h"
#include "silver/engine.h"

ant_value_t map_to_entry(ant_t *js, ant_value_t key, ant_value_t val) {
  ant_value_t pair[2] = { key, val };
  return mkarr_dense_literal_capacity(js, pair, 2, true);
}

void own_index_list_init(own_index_list_t *list) {
  list->items = list->stack;
  list->count = 0;
  list->cap = (uint32_t)(sizeof(list->stack) / sizeof(list->stack[0]));
}

void own_index_list_free(own_index_list_t *list) {
  if (list->items != list->stack) free(list->items);
}

bool own_index_list_push(own_index_list_t *list, uint32_t slot, unsigned long index) {
  if (list->count == list->cap) {
    uint32_t cap = list->cap * 2;
    bool on_stack = list->items == list->stack;
    own_index_key_t *next = on_stack ? malloc(cap * sizeof(*next)) : realloc(list->items, cap * sizeof(*next));
    if (!next) return false;
    if (on_stack) memcpy(next, list->stack, list->count * sizeof(*next));
    list->items = next;
    list->cap = cap;
  }
  
  list->items[list->count++] = (own_index_key_t){ .slot = slot, .index = index };
  return true;
}

static int own_index_key_cmp(const void *a, const void *b) {
  unsigned long x = ((const own_index_key_t *)a)->index;
  unsigned long y = ((const own_index_key_t *)b)->index;
  return (x > y) - (x < y);
}

void own_index_keys_sort(own_index_key_t *keys, uint32_t count) {
  if (count <= 16) {
    for (uint32_t i = 1; i < count; i++) {
      own_index_key_t key = keys[i];
      uint32_t pos = i;
      for (; pos > 0 && keys[pos - 1].index > key.index; pos--) keys[pos] = keys[pos - 1];
      keys[pos] = key;
    }
    return;
  }
  
  own_index_key_t *tmp = malloc(count * sizeof(*tmp));
  if (!tmp) { qsort(keys, count, sizeof(*keys), own_index_key_cmp); return; }
  
  own_index_key_t *src = keys, *dst = tmp;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    uint32_t offsets[257] = {0};
    for (uint32_t i = 0; i < count; i++) offsets[((src[i].index >> shift) & 0xff) + 1]++;
    if (offsets[((src[0].index >> shift) & 0xff) + 1] == count) continue;
    
    for (unsigned d = 1; d < 257; d++) offsets[d] += offsets[d - 1];
    for (uint32_t i = 0; i < count; i++) dst[offsets[(src[i].index >> shift) & 0xff]++] = src[i];
    
    own_index_key_t *swap = src; src = dst; dst = swap;
  }
  
  if (src != keys) memcpy(keys, src, count * sizeof(*keys));
  free(tmp);
}

static bool own_slot_list_push(uint32_t **slots, uint32_t *count, uint32_t *cap, uint32_t *stack, uint32_t slot) {
  if (*count == *cap) {
    uint32_t next_cap = *cap * 2;
    uint32_t *next = NULL;
    if (*slots == stack) {
      next = calloc(next_cap, sizeof(*next));
      if (next) memcpy(next, *slots, *count * sizeof(*next));
    } else {
      next = realloc(*slots, next_cap * sizeof(*next));
    }
    if (!next) return false;
    *slots = next;
    *cap = next_cap;
  }
  (*slots)[(*count)++] = slot;
  return true;
}

static inline ant_value_t object_enum_item(ant_t *js, enum obj_enum_mode mode, const char *key, size_t key_len, ant_value_t val) {
  if (mode == OBJ_ENUM_VALUES) return val;
  ant_value_t key_val = js_mkstr(js, key, key_len);
  return mode == OBJ_ENUM_KEYS ? key_val : map_to_entry(js, key_val, val);
}

static inline ant_value_t object_enum_interned_item(ant_t *js, enum obj_enum_mode mode, const char *key, ant_value_t val) {
  if (mode == OBJ_ENUM_VALUES) return val;
  ant_value_t key_val = intern_value(js, key);
  return mode == OBJ_ENUM_KEYS ? key_val : map_to_entry(js, key_val, val);
}

static constexpr uint32_t OBJECT_ENUM_SINK_STACK = 32;

typedef struct {
  ant_value_t items[OBJECT_ENUM_SINK_STACK];
  ant_value_t arr;
  ant_value_t error;
  uint32_t count;
  uint32_t hint;
  bool spilled;
} object_enum_sink_t;

static void object_enum_spill(ant_t *js, object_enum_sink_t *sink) {
  uint32_t cap = sink->hint > sink->count ? sink->hint : sink->count + 1;
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, cap, sink->count, false);
  if (is_err(arr)) { sink->error = arr; return; }
  
  ant_object_t *dst = array_obj_ptr(arr);
  if (dst && dst->flags.fast_array && dst->u.array.data) {
    memcpy(dst->u.array.data, sink->items, sink->count * sizeof(*sink->items));
    dst->u.array.len = sink->count;
    dst->flags.may_have_dense_elements = sink->count > 0;
  } else for (uint32_t i = 0; i < sink->count; i++) js_arr_push(js, arr, sink->items[i]);
  
  sink->arr = arr;
  sink->spilled = true;
}

static __attribute__((noinline)) void object_enum_emit_spilled(ant_t *js, object_enum_sink_t *sink, ant_value_t val) {
  if (!sink->spilled) object_enum_spill(js, sink);
  if (is_err(sink->error)) return;
  
  ant_object_t *dst = array_obj_ptr(sink->arr);
  if (!dst || !dst->flags.fast_array || !dst->u.array.data || dst->u.array.len >= dst->u.array.cap) {
    js_arr_push(js, sink->arr, val);
    return;
  }
  
  uint32_t idx = dst->u.array.len++;
  dst->u.array.data[idx] = val;
  dst->flags.may_have_dense_elements = 1;
  gc_write_barrier_elem(js, dst, idx, val);
}

static inline __attribute__((always_inline)) void object_enum_emit(ant_t *js, object_enum_sink_t *sink, ant_value_t val) {
  if (__builtin_expect(!sink->spilled && sink->count < OBJECT_ENUM_SINK_STACK, 1)) sink->items[sink->count++] = val;
  else object_enum_emit_spilled(js, sink, val);
}

static ant_value_t object_enum_props_from(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink, uint32_t start);
static ant_value_t object_enum_props(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink);

ant_value_t object_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return Ant_Array_Create(js);
    obj = promoted;
  }
  
  bool is_arr = (vtype(obj) == kTypeArray);
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return Ant_Array_Create(js);
  
  TypedArrayData *typed = ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  bool wrapper = !is_arr && !typed && vtype(js_string_wrapper_value(obj)) == kTypeString;
  if (wrapper || (is_arr && ant_shape_may_have_index_keys(ptr->shape))) return object_enum_collect(js, obj, mode);
  
  object_enum_sink_t sink;
  sink.error = Ant_Value_Undefined;
  sink.count = 0;
  sink.spilled = false;
  
  if (mode == OBJ_ENUM_VALUES && !is_arr && !typed && !ptr->flags.is_exotic && !ptr->flags.arguments_object) {
    uint32_t resume = 0;
    object_fill_t fill = arr_fill_object_values(js, ptr, &sink.arr, &resume);
    if (fill == OBJECT_FILL_DONE) return sink.arr;
    
    if (fill == OBJECT_FILL_RESUME) {
      sink.spilled = true;
      ant_value_t result = object_enum_props_from(js, obj, ptr, mode, &sink, resume);
      if (is_err(result)) return result;
      return is_err(sink.error) ? sink.error : sink.arr;
    }
  }
  
  ant_offset_t doff = is_arr ? get_dense_buf(obj) : 0;
  bool typed_live = Ant_Buffer_ViewIsLive(typed);
  ant_offset_t elem_len = doff ? dense_iterable_length(js, obj) : typed_live ? (ant_offset_t)typed->length : 0;
  
  uint64_t want = (uint64_t)elem_len + ant_shape_count(ptr->shape);
  sink.hint = want > UINT32_MAX ? UINT32_MAX : (uint32_t)want;
  
  if (elem_len) {
    object_enum_spill(js, &sink);
    if (is_err(sink.error)) return sink.error;
  }
  
  bool filled = elem_len && mode == OBJ_ENUM_VALUES && (doff 
    ? arr_fill_dense_values(sink.arr, doff, elem_len) 
    : typed_live && arr_fill_numeric_typed(js, sink.arr, typed));
  
  for (ant_offset_t i = filled ? elem_len : 0; i < elem_len; i++) {
    ant_value_t v = Ant_Value_Undefined;
    if (doff) v = dense_get(doff, i);
    else if (!buffer_typedarray_data_read_index(js, typed, (size_t)i, &v)) {
      if (is_err(v)) return v;
      break;
    }
    if (is_empty_slot(v)) continue;
    
    char idxstr[24];
    size_t idxlen = mode == OBJ_ENUM_VALUES ? 0 : uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    object_enum_emit(js, &sink, object_enum_item(js, mode, idxstr, idxlen, v));
  }

  ant_value_t result = object_enum_props(js, obj, ptr, mode, &sink);
  if (is_err(result)) return result;
  if (is_err(sink.error)) return sink.error;
  
  return sink.spilled ? sink.arr : mkarr_dense_literal_capacity(js, sink.items, sink.count, false);
}

static bool own_key_is_dense_shadow(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return false;

  unsigned long idx = 0;
  if (!own_key_is_array_index(key, key_len, &idx)) return false;
  if (idx >= (unsigned long)dense_iterable_length(js, obj)) return false;
  return !is_empty_slot(dense_get(doff, (ant_offset_t)idx));
}

static bool own_key_is_enumerable(ant_t *js, ant_value_t obj, ant_object_t *ptr, uint32_t slot) {
  if (!ptr || !ptr->shape) return false;
  bool enumerable = (ant_shape_get_attrs(ptr->shape, slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, slot);
  if (!prop) return false;

  if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(js, obj, prop->key.sym_off, &meta)) enumerable = meta.enumerable;
    return enumerable;
  }

  if (ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(obj), prop->key.interned, intern_length(prop->key.interned));
    if (desc) enumerable = desc->enumerable;
  }
  return enumerable;
}

ant_value_t js_object_view(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == kTypeGenerator || t == kTypePromise) return mkval(kTypeObject, vdata(v));
  return v;
}

static __attribute__((noinline)) void own_keys_push_string_indexes(
  ant_t *js, ant_value_t arr, ant_offset_t *out_idx, ant_offset_t len
) {
  for (ant_offset_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    arr_set(js, arr, (*out_idx)++, js_mkstr(js, idxstr, idxlen));
  }
}

ant_value_t js_own_property_keys(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only) {
  obj = js_object_view(obj);
  
  if (is_proxy(obj)) 
    return proxy_own_keys_filtered(js, obj, include_symbols, enumerable_only);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t arr = Ant_Array_Create(js);
  GC_ROOT_PIN(js, arr);
  ant_offset_t out_idx = 0;

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray) goto done;

  ant_object_t *ptr = js_obj_ptr(obj);
  bool has_string_exotic = false;
  
  ant_offset_t string_exotic_len = 0;
  ant_value_t string_primitive = js_string_wrapper_value(obj);

  if (vtype(string_primitive) == kTypeString) {
    string_exotic_len = str_utf16_len(js, string_primitive);
    has_string_exotic = true;
    own_keys_push_string_indexes(js, arr, &out_idx, string_exotic_len);
    if (!enumerable_only) arr_set(js, arr, out_idx++, js->length_str);
  }

  TypedArrayData *typed = ptr && ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  if (Ant_Buffer_ViewIsLive(typed))
    own_keys_push_string_indexes(js, arr, &out_idx, (ant_offset_t)typed->length);

  if (!ptr || !ptr->shape) goto done;
  bool is_arr = (vtype(obj) == kTypeArray);
  uint32_t count = ant_shape_count(ptr->shape);

  if (!is_arr) {
    bool has_index_key = false;
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      
      if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
      if (i >= ptr->prop_count) continue;
      if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;

      const char *key = prop->key.interned;
      size_t key_len = intern_length(key);
      unsigned long string_idx = 0;
      
      if (
        has_string_exotic && (is_length_key(key, (ant_offset_t)key_len) || 
        parse_array_index(key, key_len, string_exotic_len, &string_idx))
      ) continue;
      
      if (own_key_is_array_index(key, key_len, NULL)) {
        has_index_key = true;
        break;
      }
      
      arr_set(js, arr, out_idx++, intern_value(js, key));
    }

    if (!has_index_key) {
      if (include_symbols) for (uint32_t i = 0; i < count; i++) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
        if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
        if (i >= ptr->prop_count) continue;
        if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;
        arr_set(js, arr, out_idx++, mkval(kTypeSymbol, prop->key.sym_off));
      }
      goto done;
    }

    arr = Ant_Array_Create(js);
    GC_ROOT_PIN(js, arr);
    out_idx = 0;
    if (has_string_exotic) own_keys_push_string_indexes(js, arr, &out_idx, string_exotic_len);
  }

  if (is_arr) {
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t dense_len = doff ? dense_iterable_length(js, obj) : 0;
    for (ant_offset_t i = 0; i < dense_len; i++) {
      if (is_empty_slot(dense_get(doff, i))) continue;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      arr_set(js, arr, out_idx++, js_mkstr(js, idxstr, idxlen));
    }
  }

  own_index_list_t indices;
  own_index_list_init(&indices);
  
  uint32_t stack_string_slots[64];
  uint32_t *string_slots = stack_string_slots;
  uint32_t string_count = 0;
  uint32_t string_cap = (uint32_t)(sizeof(stack_string_slots) / sizeof(stack_string_slots[0]));
  
  ant_offset_t dense_shadow_len = is_arr && get_dense_buf(obj) ? dense_iterable_length(js, obj) : 0;
  ant_offset_t dense_shadow_buf = is_arr ? get_dense_buf(obj) : 0;

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;
    if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    unsigned long string_idx = 0;
    
    if (
      has_string_exotic && (is_length_key(key, (ant_offset_t)key_len) || 
      parse_array_index(key, key_len, string_exotic_len, &string_idx))
    ) continue;
    
    unsigned long parsed = 0;
    if (!own_key_is_array_index(key, key_len, &parsed)) {
      if (!own_slot_list_push(&string_slots, &string_count, &string_cap, stack_string_slots, i)) {
        own_index_list_free(&indices);
        if (string_slots != stack_string_slots) free(string_slots);
        arr = js_mkerr(js, "oom");
        goto done;
      }
      continue;
    }

    if (
      dense_shadow_buf &&
      parsed < (unsigned long)dense_shadow_len &&
      !is_empty_slot(dense_get(dense_shadow_buf, (ant_offset_t)parsed))
    ) continue;

    if (!own_index_list_push(&indices, i, parsed)) {
      own_index_list_free(&indices);
      if (string_slots != stack_string_slots) free(string_slots);
      arr = js_mkerr(js, "oom");
      goto done;
    }
  }

  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t i = 0; i < indices.count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, indices.items[i].slot);
    if (!prop) continue;
    const char *key = prop->key.interned;
    arr_set(js, arr, out_idx++, js_mkstr(js, key, intern_length(key)));
  }

  if (!enumerable_only && ((is_arr && !ptr->flags.arguments_object) || has_string_exotic)) {
    arr_set(js, arr, out_idx++, js->length_str);
  }

  for (uint32_t i = 0; i < string_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, string_slots[i]);
    if (!prop) continue;
    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    arr_set(js, arr, out_idx++, js_mkstr(js, key, key_len));
  }

  if (include_symbols) {
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
      if (i >= ptr->prop_count) continue;
      if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;
      arr_set(js, arr, out_idx++, mkval(kTypeSymbol, prop->key.sym_off));
    }
  }

  own_index_list_free(&indices);
  if (string_slots != stack_string_slots) free(string_slots);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

static ant_value_t for_in_keys_add(ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t key) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  GC_ROOT_PIN(js, key);
  
  if (vtype(key) != kTypeString) return Ant_Value_Undefined;

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)(key_off);

  if (lkp(js, seen, key_ptr, key_len).obj) goto done;

  ant_value_t mark = setprop_cstr(js, seen, key_ptr, key_len, Ant_Value_True);
  if (is_err(mark)) {
    GC_ROOT_RESTORE(js, root_mark);
    return mark;
  }

  if (vtype(out) == kTypeArray) js_arr_push(js, out, key);
done:
  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_Undefined;
}

static ant_value_t for_in_keys_add_cstr(
  ant_t *js, ant_value_t out, ant_value_t seen, const char *key, size_t key_len, bool enumerable
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);

  const char *interned = intern_string(js, key, key_len);
  if (!interned) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "oom");
  }

  if (lkp_interned(seen, interned).obj) goto done;

  ant_value_t mark = mkprop_interned(js, seen, interned, Ant_Value_True, 0);
  if (is_err(mark)) {
    GC_ROOT_RESTORE(js, root_mark);
    return mark;
  }

  if (enumerable && vtype(out) == kTypeArray) {
    ant_value_t key_val = js_mkstr(js, key, key_len);
    GC_ROOT_PIN(js, key_val);
    js_arr_push(js, out, key_val);
  }

done:
  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_Undefined;
}

static ant_value_t for_in_keys_add_indices(ant_t *js, ant_value_t out, ant_value_t seen, ant_offset_t len) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  
  for (ant_offset_t i = 0; i < len; i++) {
    char idx[16];
    size_t idx_len = uint_to_str(idx, sizeof(idx), (uint64_t)i);
    
    ant_value_t key = js_mkstr(js, idx, idx_len);
    GC_ROOT_PIN(js, key);
    
    ant_value_t r = for_in_keys_add(js, out, seen, key);
    if (is_err(r)) { GC_ROOT_RESTORE(js, root_mark); return r; }
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_Undefined;
}

static ant_value_t for_in_keys_collect_ordinary_own(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj, ant_object_t *ptr
) {
  bool is_arr = (vtype(obj) == kTypeArray);
  uint32_t count = ant_shape_count(ptr->shape);
  ant_value_t r = Ant_Value_Undefined;

  if (!is_arr) {
    bool has_index_key = false;
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
      if (i >= ptr->prop_count) continue;
      const char *key = prop->key.interned;
      if (own_key_is_array_index(key, intern_length(key), NULL)) {
        has_index_key = true;
        break;
      }
    }

    if (!has_index_key) {
      for (uint32_t i = 0; i < count; i++) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
        if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
        if (i >= ptr->prop_count) continue;
        const char *key = prop->key.interned;
        bool enumerable = own_key_is_enumerable(js, obj, ptr, i);
        r = for_in_keys_add_cstr(js, out, seen, key, intern_length(key), enumerable);
        if (is_err(r)) return r;
      }
      return Ant_Value_Undefined;
    }
  }

  if (is_arr) {
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t dense_len = doff ? dense_iterable_length(js, obj) : 0;
    for (ant_offset_t i = 0; i < dense_len; i++) {
      if (is_empty_slot(dense_get(doff, i))) continue;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      r = for_in_keys_add_cstr(js, out, seen, idxstr, idxlen, true);
      if (is_err(r)) return r;
    }
  }

  own_index_list_t indices;
  own_index_list_init(&indices);

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (is_arr && own_key_is_dense_shadow(js, obj, key, key_len)) continue;

    unsigned long parsed = 0;
    if (!own_key_is_array_index(key, key_len, &parsed)) continue;
    if (!own_index_list_push(&indices, i, parsed)) {
      own_index_list_free(&indices);
      return js_mkerr(js, "oom");
    }
  }

  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t i = 0; i < indices.count && !is_err(r); i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, indices.items[i].slot);
    if (!prop) continue;
    const char *key = prop->key.interned;
    bool enumerable = own_key_is_enumerable(js, obj, ptr, indices.items[i].slot);
    r = for_in_keys_add_cstr(js, out, seen, key, intern_length(key), enumerable);
  }

  own_index_list_free(&indices);
  if (is_err(r)) return r;

  if (is_arr) {
    r = for_in_keys_add_cstr(js, out, seen, "length", 6, false);
    if (is_err(r)) return r;
  }

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (own_key_is_array_index(key, key_len, NULL)) continue;

    bool enumerable = own_key_is_enumerable(js, obj, ptr, i);
    r = for_in_keys_add_cstr(js, out, seen, key, key_len, enumerable);
    if (is_err(r)) return r;
  }

  return Ant_Value_Undefined;
}

static ant_value_t for_in_keys_add_exotic_indices(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj, ant_object_t *ptr
) {
  if (vtype(obj) != kTypeObject) return Ant_Value_Undefined;
  ant_value_t prim = get_slot(obj, SLOT_PRIMITIVE);
  if (vtype(prim) == kTypeString) return for_in_keys_add_indices(js, out, seen, str_utf16_len(js, prim));
  TypedArrayData *typed = ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  if (Ant_Buffer_ViewIsLive(typed))
    return for_in_keys_add_indices(js, out, seen, (ant_offset_t)typed->length);
  return Ant_Value_Undefined;
}

static inline ant_value_t for_in_keys_collect_chain(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj
) {
  ant_value_t cur = obj;
  GC_ROOT_PIN(js, cur);

  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t as_cur = (vtype(cur) == kTypeFunction) ? js_func_obj(cur) : cur;
    
    ant_object_t *cur_ptr = js_obj_ptr(as_cur);
    ant_value_t r, proto;
    
    if (!cur_ptr) goto next_proto;
    r = for_in_keys_add_exotic_indices(js, out, seen, as_cur, cur_ptr);
    if (is_err(r)) goto err;
    
    if (!cur_ptr->shape) goto next_proto;
    r = for_in_keys_collect_ordinary_own(js, out, seen, as_cur, cur_ptr);
    
    if (is_err(r)) goto err;
    goto next_proto;

  next_proto:
    GC_ROOT_RESTORE(js, iter_mark);
    proto = js_get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = proto;
    continue;
  err:
    GC_ROOT_RESTORE(js, iter_mark);
    return r;
  }

  return out;
}

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  uint8_t t = vtype(obj);
  ant_value_t out = Ant_Array_Create(js);
  ant_value_t result = out;
  
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, out);
  
  if (t == kTypeNull || t == kTypeUndefined) goto done;

  ant_value_t seen = mkobj(js, 0);
  GC_ROOT_PIN(js, seen);

  if (t == kTypeString) {
    result = for_in_keys_add_indices(js, out, seen, str_utf16_len(js, obj));
    if (is_err(result)) goto done;
    result = out;
    goto done;
  }

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) goto done;
    obj = promoted;
    t = kTypeFunction;
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) goto done;
  
  if (is_proxy(obj)) {
    result = proxy_enum(js, obj, OBJ_ENUM_KEYS);
    goto done;
  }
  
  result = for_in_keys_collect_chain(js, out, seen, obj);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static bool copy_key_of(ant_t *js, ant_value_t key, copy_key_t *out) {
  if (vtype(key) == kTypeSymbol) {
    *out = (copy_key_t){ .sym = key };
    return true;
  }
  
  size_t len = 0;
  const char *str = vtype(key) == kTypeString ? js_getstr(js, key, &len) : NULL;
  *out = (copy_key_t){ .sym = Ant_Value_Undefined, .interned = str ? intern_string(js, str, len) : NULL };
  
  return out->interned != NULL;
}

static bool copy_excluded_key_of(ant_t *js, ant_value_t key, copy_key_t *out) {
  ant_flat_string_t *flat = ant_str_flat_ptr(key);
  const char *interned = flat ? intern_find_flat(js, flat->bytes, (size_t)flat->len) : NULL;
  if (!interned) return copy_key_of(js, key, out);
  *out = (copy_key_t){ .sym = Ant_Value_Undefined, .interned = interned };
  return true;
}

static inline bool copy_key_is_sym(copy_key_t key) {
  return vtype(key.sym) == kTypeSymbol;
}

static inline int32_t copy_key_slot(ant_shape_t *shape, copy_key_t key) {
  return copy_key_is_sym(key)
    ? ant_shape_lookup_symbol(shape, (ant_offset_t)vdata(key.sym))
    : ant_shape_lookup_interned(shape, key.interned);
}

static inline ant_value_t copy_key_get(ant_t *js, ant_value_t src, copy_key_t key) {
  return copy_key_is_sym(key) ? js_get_sym(js, src, key.sym) : js_get(js, src, key.interned);
}

static bool copy_key_excluded(const copy_ctx_t *cx, copy_key_t key) {
  for (int i = 0; i < cx->count; i++)
    if (cx->keys[i].sym == key.sym && cx->keys[i].interned == key.interned) return true;
  return false;
}

static ant_value_t copy_define_slow(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  
  ant_value_t key_val = copy_key_is_sym(key)
    ? key.sym 
    : js_mkstr(js, key.interned, intern_length(key.interned));
    
  GC_ROOT_PIN(js, key_val);
  
  ant_value_t desc = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, desc);
  
  js_set(js, desc, "value", val);
  js_set(js, desc, "writable", Ant_Value_True);
  js_set(js, desc, "enumerable", Ant_Value_True);
  js_set(js, desc, "configurable", Ant_Value_True);
  
  ant_value_t result = object_define_property(js, dst, key_val, desc);
  GC_ROOT_RESTORE(js, root_mark);
  
  return is_err(result) ? result : val;
}

static ant_value_t copy_define(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(dst);
  if (vtype(dst) != kTypeObject || !ptr || !ptr->shape || ptr->flags.is_exotic || !ptr->flags.extensible)
    return copy_define_slow(js, dst, key, val);
  
  bool is_sym = copy_key_is_sym(key);
  int32_t slot = copy_key_slot(ptr->shape, key);
  
  if (slot < 0) return is_sym
    ? mkprop_symbol_attrs_impl(js, dst, key.sym, val, 0, MKPROP_USE_DEFAULT_ATTRS)
    : mkprop_interned_exact(js, dst, key.interned, val, 0);
  
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
  
  bool plain = 
    prop && !prop->has_getter && !prop->has_setter &&
    ant_shape_get_attrs(ptr->shape, (uint32_t)slot) == ANT_PROP_ATTR_DEFAULT;
  
  if (!plain || (uint32_t)slot >= ptr->prop_count) return copy_define_slow(js, dst, key, val);
  
  ant_object_prop_set_unchecked(ptr, (uint32_t)slot, val);
  gc_write_barrier(js, ptr, val);
  
  if (is_sym) ant_symbol_property_mutation_invalidate(js, ptr, (ant_offset_t)vdata(key.sym));
  else ant_property_mutation_invalidate(js, ptr, key.interned);
  
  return val;
}

bool chain_lacks_plain_keys(ant_value_t start, const char *first, const char *second) {
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (!is_object_type(start)) return true;
    ant_object_t *ptr = js_obj_ptr(js_as_obj(start));
    if (!ptr || ptr->flags.is_exotic) return false;
    if (ptr->shape && ant_shape_lookup_interned(ptr->shape, first) >= 0) return false;
    if (second && ptr->shape && ant_shape_lookup_interned(ptr->shape, second) >= 0) return false;
    start = ant_object_proto(ptr);
  }
  
  return false;
}

static inline bool chain_lacks_plain_key(ant_value_t start, const char *interned) {
  return chain_lacks_plain_keys(start, interned, NULL);
}

bool is_plain_set_target(const ant_object_t *ptr) {
  return ptr && ptr->shape && !ptr->flags.is_exotic && ptr->type_tag == kTypeObject;
}

bool own_writable_data_slot(const ant_object_t *ptr, int32_t slot) {
  if (slot < 0 || (uint32_t)slot >= ptr->prop_count) return false;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
  return prop && !prop->has_getter && !prop->has_setter &&
    (ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_WRITABLE);
}

void store_own_data_slot(ant_t *js, ant_object_t *ptr, uint32_t slot, const char *interned, ant_value_t val) {
  ant_object_prop_set_unchecked(ptr, slot, val);
  gc_write_barrier(js, ptr, val);
  ant_property_mutation_invalidate(js, ptr, interned);
}

static ant_value_t copy_set(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(dst);
  
  bool plain =
    !copy_key_is_sym(key) && vtype(dst) == kTypeObject && is_plain_set_target(ptr) &&
    vtype(js_string_wrapper_value(dst)) != kTypeString;
  
  int32_t slot = plain ? ant_shape_lookup_interned(ptr->shape, key.interned) : -1;
  if (plain && own_writable_data_slot(ptr, slot)) {
    store_own_data_slot(js, ptr, (uint32_t)slot, key.interned, val);
    return val;
  }
  
  if (plain && slot < 0 && ptr->flags.extensible && chain_lacks_plain_key(ant_object_proto(ptr), key.interned))
    return mkprop_interned_exact(js, dst, key.interned, val, 0);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  
  ant_value_t key_val = copy_key_is_sym(key)
    ? key.sym
    : js_mkstr(js, key.interned, intern_length(key.interned));
  
  GC_ROOT_PIN(js, key_val);
  ant_value_t result = is_err(key_val) ? key_val : js_setprop_throw(js, dst, key_val, false, 0, val);
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static __attribute__((noinline)) ant_value_t copy_collect(ant_t *js, const copy_ctx_t *cx, ant_value_t arr, copy_key_t key, ant_value_t val) {
  if (!copy_key_is_sym(key)) js_arr_push(js, arr, object_enum_interned_item(js, cx->mode, key.interned, val));
  return Ant_Value_Undefined;
}

static inline ant_value_t copy_store(ant_t *js, const copy_ctx_t *cx, ant_value_t dst, copy_key_t key, ant_value_t val) {
  return cx->assign ? copy_set(js, dst, key, val) : copy_define(js, dst, key, val);
}

static inline ant_value_t copy_emit(ant_t *js, const copy_ctx_t *cx, ant_value_t dst, copy_key_t key, ant_value_t val) {
  return cx->collect ? copy_collect(js, cx, dst, key, val) : copy_store(js, cx, dst, key, val);
}

static ant_value_t copy_host_pairs(ant_t *js, ant_value_t dst, ant_proxy_state_t *data, const copy_ctx_t *cx) {
  ant_value_t pairs = proxy_host_pairs(js, data, true);
  if (is_err(pairs)) return pairs;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, pairs);
  ant_value_t result = Ant_Value_Undefined;
  
  ant_offset_t len = get_array_length(js, pairs);
  for (ant_offset_t i = 0; i + 1 < len && !is_err(result); i += 2) {
    copy_key_t ck;
    if (!copy_key_of(js, arr_get(js, pairs, i), &ck)) result = Ant_Error_CreateOutOfMemory(js);
    else if (!copy_key_excluded(cx, ck)) result = copy_store(js, cx, dst, ck, arr_get(js, pairs, i + 1));
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : Ant_Value_Undefined;
}

static ant_value_t copy_proxy_data_props(
  ant_t *js, ant_value_t dst, ant_value_t proxy, const copy_ctx_t *cx
) {
  ant_proxy_state_t *host_data = get_proxy_data(proxy);
  if (host_data && host_data->host && host_data->host->flat_entries) return copy_host_pairs(js, dst, host_data, cx);
  
  ant_value_t keys = proxy_own_keys_raw(js, proxy);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t result = Ant_Value_Undefined;

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t k = 0; k < key_count && !is_err(result); k++) {
    GC_ROOT_SAVE(key_mark, js);
    
    ant_value_t key = js_arr_get(js, keys, k);
    GC_ROOT_PIN(js, key);
    
    copy_key_t ck;
    if (!copy_key_of(js, key, &ck)) result = Ant_Error_CreateOutOfMemory(js);
    else if (!copy_key_excluded(cx, ck)) {
      ant_value_t enumerable = proxy_key_enumerable(js, proxy, key);
      if (is_err(enumerable)) result = enumerable;
      else if (js_truthy(js, enumerable)) {
        ant_value_t val = proxy_get_val(js, proxy, key);
        GC_ROOT_PIN(js, val);
        result = is_err(val) ? val : copy_store(js, cx, dst, ck, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : Ant_Value_Undefined;
}

static ant_value_t copy_string_props(ant_t *js, ant_value_t dst, ant_value_t str, const copy_ctx_t *cx) {
  ant_offset_t len = str_utf16_len(js, str);
  
  for (ant_offset_t i = 0; i < len; i++) {
    copy_key_t ck = { .sym = Ant_Value_Undefined, .interned = index_key_get_or_intern(js, (uint32_t)i) };
    if (!ck.interned) return Ant_Error_CreateOutOfMemory(js);
    if (copy_key_excluded(cx, ck)) continue;
    
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, str, &byte_len);
    ant_value_t val = js_string_from_utf16_code_unit(js, utf16_code_unit_at(data, (size_t)byte_len, (size_t)i));
    
    ant_value_t result = copy_emit(js, cx, dst, ck, val);
    if (is_err(result)) return result;
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t copy_dense_props_assign(ant_t *js, ant_value_t dst, ant_value_t arr, const copy_ctx_t *cx) {
  ant_offset_t doff = get_dense_buf(arr);
  ant_offset_t len = doff ? dense_iterable_length(js, arr) : 0;
  
  uint32_t stack_present[64];
  uint32_t *present = len <= 64 ? stack_present : malloc((size_t)len * sizeof(*present));
  if (!present) return Ant_Error_CreateOutOfMemory(js);

  uint32_t count = 0;
  for (ant_offset_t i = 0; i < len; i++) if (!is_empty_slot(dense_get(doff, i))) present[count++] = (uint32_t)i;

  ant_value_t result = Ant_Value_Undefined;
  for (uint32_t k = 0; k < count && !is_err(result); k++) {
    uint32_t i = present[k];
    copy_key_t ck = { .sym = Ant_Value_Undefined, .interned = index_key_get_or_intern(js, i) };
    if (!ck.interned) { result = Ant_Error_CreateOutOfMemory(js); break; }
    if (copy_key_excluded(cx, ck)) continue;

    ant_value_t val = Ant_Value_Undefined;
    doff = get_dense_buf(arr);
    bool dense_hit = doff && i < dense_iterable_length(js, arr) && !is_empty_slot(val = dense_get(doff, i));

    if (!dense_hit) {
      ant_object_t *ptr = js_obj_ptr(arr);
      int32_t slot = ptr && ptr->shape ? ant_shape_lookup_interned(ptr->shape, ck.interned) : -1;
      if (
        slot < 0 || (uint32_t)slot >= ptr->prop_count ||
        !(ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_ENUMERABLE)
      ) continue;
      val = copy_key_get(js, arr, ck);
      if (is_err(val)) { result = val; break; }
    }

    result = copy_store(js, cx, dst, ck, val);
  }

  if (present != stack_present) free(present);
  return is_err(result) ? result : Ant_Value_Undefined;
}

static bool copy_dense_index_layout(ant_t *js, ant_value_t dst, const copy_ctx_t *cx, ant_offset_t doff, ant_offset_t len) {
  if (cx->count || len > UINT32_MAX || vtype(dst) != kTypeObject) return false;

  ant_object_t *dst_ptr = js_obj_ptr(dst);
  if (
    !dst_ptr || !dst_ptr->shape || ant_shape_count(dst_ptr->shape) != 0 ||
    dst_ptr->flags.is_exotic || !dst_ptr->flags.extensible ||
    vtype(js_string_wrapper_value(dst)) == kTypeString
  ) return false;

  for (ant_offset_t i = 0; i < len; i++) if (is_empty_slot(dense_get(doff, i))) return false;

  ant_shape_t *shape = ant_shape_index_layout(js, &js->index_layout_shape, dst_ptr->shape, (uint32_t)len);
  if (!shape || !js_obj_ensure_prop_capacity(js, dst_ptr, (uint32_t)len)) return false;

  ant_shape_transition_existing(&dst_ptr->shape, shape);
  ant_object_invalidate_guarded_absence(js, dst_ptr);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = dense_get(doff, i);
    ant_object_prop_set_unchecked(dst_ptr, (uint32_t)i, val);
    gc_write_barrier(js, dst_ptr, val);
  }

  return true;
}

static ant_value_t copy_dense_props(ant_t *js, ant_value_t dst, ant_value_t arr, const copy_ctx_t *cx) {
  if (cx->assign) return copy_dense_props_assign(js, dst, arr, cx);
  
  ant_offset_t doff = get_dense_buf(arr);
  ant_offset_t len = doff ? dense_iterable_length(js, arr) : 0;
  
  if (doff && copy_dense_index_layout(js, dst, cx, doff, len)) return Ant_Value_Undefined;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = dense_get(doff, i);
    if (is_empty_slot(val)) continue;
    
    copy_key_t ck = { .sym = Ant_Value_Undefined, .interned = index_key_get_or_intern(js, (uint32_t)i) };
    if (!ck.interned) return Ant_Error_CreateOutOfMemory(js);
    if (copy_key_excluded(cx, ck)) continue;
    
    ant_value_t result = copy_store(js, cx, dst, ck, val);
    if (is_err(result)) return result;
  }
  
  return Ant_Value_Undefined;
}

static copy_key_t copy_key_at(const ant_shape_prop_t *prop) {
  return prop->type == ANT_SHAPE_KEY_SYMBOL
    ? (copy_key_t){ .sym = mkval(kTypeSymbol, prop->key.sym_off) }
    : (copy_key_t){ .sym = Ant_Value_Undefined, .interned = prop->key.interned };
}

static uint32_t copy_shape_keys_in_slot_order(ant_object_t *ptr, copy_key_t *keys, uint32_t *slots) {
  uint32_t count = ant_shape_count(ptr->shape), key_count = 0;
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_DELETED) continue;
    slots[key_count] = i;
    keys[key_count++] = copy_key_at(prop);
  }
  return key_count;
}

static uint32_t copy_shape_keys_ordered(ant_object_t *ptr, copy_key_t *keys, uint32_t *slots, own_index_key_t *indices) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  uint32_t index_count = 0, key_count = 0;
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    
    unsigned long index = 0;
    if (indices && own_key_is_array_index(prop->key.interned, intern_length(prop->key.interned), &index))
      indices[index_count++] = (own_index_key_t){ .slot = i, .index = index };
    else {
      slots[key_count] = i;
      keys[key_count++] = copy_key_at(prop);
    }
  }
  
  if (index_count) {
    own_index_keys_sort(indices, index_count);
    memmove(keys + index_count, keys, (size_t)key_count * sizeof(*keys));
    memmove(slots + index_count, slots, (size_t)key_count * sizeof(*slots));
    for (uint32_t k = 0; k < index_count; k++) {
      slots[k] = indices[k].slot;
      keys[k] = copy_key_at(ant_shape_prop_at(shape, indices[k].slot));
    }
    key_count += index_count;
  }
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
    slots[key_count] = i;
    keys[key_count++] = copy_key_at(prop);
  }
  
  return key_count;
}

typedef struct copy_key_snapshot {
  copy_key_t *keys;
  uint32_t *slots;
  uint32_t count;
  copy_key_t stack[32];
  uint32_t stack_slots[32];
} copy_key_snapshot_t;

static inline bool copy_key_at_slot(const ant_object_t *ptr, uint32_t slot, copy_key_t key) {
  if (slot >= ptr->prop_count || slot >= ant_shape_count(ptr->shape)) return false;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, slot);
  return prop && (copy_key_is_sym(key)
    ? prop->type == ANT_SHAPE_KEY_SYMBOL && prop->key.sym_off == (ant_offset_t)vdata(key.sym)
    : prop->type == ANT_SHAPE_KEY_STRING && prop->key.interned == key.interned);
}

static void copy_key_snapshot_free(copy_key_snapshot_t *snap) {
  if (snap->keys != snap->stack) free(snap->keys);
  if (snap->slots != snap->stack_slots) free(snap->slots);
}

static bool copy_key_snapshot_take(ant_object_t *ptr, copy_key_snapshot_t *snap, bool spec_order) {
  uint32_t count = ant_shape_count(ptr->shape);
  
  bool small = count <= 32;
  bool order_indices = spec_order && ant_shape_may_have_index_keys(ptr->shape);
  
  own_index_key_t stack_indices[32];
  own_index_key_t *indices = !order_indices ? NULL
    : small ? stack_indices : malloc((size_t)count * sizeof(*indices));
  
  snap->keys = small ? snap->stack : malloc((size_t)count * sizeof(*snap->keys));
  snap->slots = small ? snap->stack_slots : malloc((size_t)count * sizeof(*snap->slots));
  
  bool ok = snap->keys && snap->slots && (indices || !order_indices);
  
  snap->count = !ok ? 0 : spec_order
    ? copy_shape_keys_ordered(ptr, snap->keys, snap->slots, indices)
    : copy_shape_keys_in_slot_order(ptr, snap->keys, snap->slots);
  
  if (indices != stack_indices) free(indices);
  if (!ok) copy_key_snapshot_free(snap);
  
  return ok;
}

static ant_value_t copy_key_snapshot_props(
  ant_t *js, ant_value_t dst, ant_value_t src, ant_object_t *ptr,
  const copy_ctx_t *cx, const copy_key_snapshot_t *snap
) {
  ant_value_t result = Ant_Value_Undefined;
  for (uint32_t k = 0; k < snap->count && !is_err(result); k++) {
    copy_key_t ck = snap->keys[k];
    if (copy_key_excluded(cx, ck)) continue;
    
    int32_t slot = copy_key_at_slot(ptr, snap->slots[k], ck) ? (int32_t)snap->slots[k] : copy_key_slot(ptr->shape, ck);
    if (slot < 0 || (uint32_t)slot >= ptr->prop_count) continue;
    if ((ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;
    
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    ant_value_t val = !(prop->has_getter || prop->has_setter) ? ant_object_prop_get_unchecked(ptr, (uint32_t)slot)
      : prop->has_getter ? js_call_getter(js, src, prop->getter) : Ant_Value_Undefined;
    
    result = is_err(val) ? val : copy_emit(js, cx, dst, ck, val);
  }
  
  return result;
}

static bool copy_adopt_shape(ant_t *js, ant_value_t dst, ant_object_t *src, bool via_set) {
  ant_object_t *dst_ptr = vtype(dst) == kTypeObject ? js_obj_ptr(dst) : NULL;
  ant_shape_t *shape = src->shape;
  uint32_t count = ant_shape_count(shape);
  
  if (
    count == 0 || count > src->prop_count || ant_shape_may_have_index_keys(shape) ||
    (src->protector_holder & ANT_PROTECTOR_HOLDER_WATCHED) || !dst_ptr ||
    ant_shape_proto(shape) != ant_object_proto(dst_ptr) || !dst_ptr->shape || ant_shape_count(dst_ptr->shape) != 0 || dst_ptr->prop_count != 0 ||
    dst_ptr->flags.is_exotic || !dst_ptr->flags.extensible || dst_ptr->type_tag != kTypeObject ||
    ant_object_inobj_limit(dst_ptr) != ant_shape_get_inobj_limit(shape) ||
    (via_set && vtype(js_string_wrapper_value(dst)) == kTypeString)
  ) return false;
  
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->has_getter || prop->has_setter) return false;
    if (ant_shape_get_attrs(shape, i) != ANT_PROP_ATTR_DEFAULT || ant_property_key_is_watched(js, prop->key.interned)) return false;
  }
  
  for (uint32_t i = 0; via_set && i < count; i++)
    if (!chain_lacks_plain_key(ant_object_proto(dst_ptr), ant_shape_prop_at(shape, i)->key.interned)) return false;
  
  if (!js_obj_ensure_prop_capacity(js, dst_ptr, count)) return false;
  ant_shape_retain(shape);
  ant_shape_release(dst_ptr->shape);
  
  dst_ptr->shape = shape;
  ant_object_invalidate_guarded_absence(js, dst_ptr);
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t val = ant_object_prop_get_unchecked(src, i);
    ant_object_prop_set_unchecked(dst_ptr, i, val);
    gc_write_barrier(js, dst_ptr, val);
  }
  
  return true;
}

static bool copy_same_shape(ant_t *js, ant_value_t dst, ant_object_t *src) {
  ant_object_t *dst_ptr = vtype(dst) == kTypeObject ? js_obj_ptr(dst) : NULL;
  ant_shape_t *shape = src->shape;
  uint32_t count = ant_shape_count(shape);
  
  if (
    !dst_ptr || dst_ptr->shape != shape || count == 0 || count > src->prop_count || count > dst_ptr->prop_count ||
    ant_shape_may_have_index_keys(shape) || dst_ptr->flags.is_exotic || dst_ptr->type_tag != kTypeObject
  ) return false;
  
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->has_getter || prop->has_setter) return false;
    if (ant_shape_get_attrs(shape, i) != ANT_PROP_ATTR_DEFAULT || ant_property_key_is_watched(js, prop->key.interned)) return false;
  }
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t val = ant_object_prop_get_unchecked(src, i);
    ant_object_prop_set_unchecked(dst_ptr, i, val);
    gc_write_barrier(js, dst_ptr, val);
  }
  
  return true;
}

static bool copy_ordinary_data_props(
  ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx, ant_value_t *result
) {
  bool is_arr = vtype(src) == kTypeArray;
  ant_object_t *ptr = is_arr || vtype(src) == kTypeObject ? js_obj_ptr(src) : NULL;
  
  if (!ptr || !ptr->shape || ptr->flags.is_exotic || ptr->flags.arguments_object) return false;
  ant_value_t wrapped = js_string_wrapper_value(src);
  
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  
  if (count == 0 && !is_arr && vtype(wrapped) != kTypeString) {
    *result = Ant_Value_Undefined;
    return true;
  }
  
  bool has_accessor = false, symbol_accessor = false;
  for (uint32_t i = 0; i < count && !symbol_accessor; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || !(prop->has_getter || prop->has_setter)) continue;
    has_accessor = true;
    symbol_accessor = prop->type == ANT_SHAPE_KEY_SYMBOL;
  }
  
  bool index_keys = ant_shape_may_have_index_keys(shape);
  bool accessor_order_visible = symbol_accessor || (has_accessor && index_keys);
  if (is_arr && (has_accessor || cx->collect)) return false;
  
  bool indexed = is_arr || vtype(wrapped) == kTypeString;
  if (
    !has_accessor && !indexed && !cx->count && !cx->collect && !index_keys && 
    (copy_adopt_shape(js, dst, ptr, cx->assign) || copy_same_shape(js, dst, ptr))
  ) {
    *result = Ant_Value_Undefined;
    return true;
  }
  
  copy_key_snapshot_t snap;
  bool snapshot = has_accessor || cx->assign || cx->collect;
  
  if (snapshot && !copy_key_snapshot_take(ptr, &snap, cx->assign || cx->collect || accessor_order_visible)) {
    *result = Ant_Error_CreateOutOfMemory(js);
    return true;
  }
  
  *result = is_arr ? copy_dense_props(js, dst, src, cx)
    : vtype(wrapped) == kTypeString ? copy_string_props(js, dst, wrapped, cx)
    : Ant_Value_Undefined;
  
  if (snapshot) {
    if (!is_err(*result)) *result = copy_key_snapshot_props(js, dst, src, ptr, cx, &snap);
    copy_key_snapshot_free(&snap);
    return true;
  }
  
  if (is_err(*result)) return true;
  
  ant_object_t *dst_ptr = js_obj_ptr(dst);
  if (dst_ptr && dst_ptr->shape && !js_obj_ensure_prop_capacity(js, dst_ptr, ant_shape_count(dst_ptr->shape) + count)) {
    *result = Ant_Error_CreateOutOfMemory(js);
    return true;
  }
  
  for (uint32_t i = 0; i < count && !is_err(*result); i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || i >= ptr->prop_count) continue;
    if ((ant_shape_get_attrs(shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;
    
    copy_key_t ck = copy_key_at(prop);
    if (copy_key_excluded(cx, ck)) continue;
    *result = copy_define(js, dst, ck, ant_object_prop_get_unchecked(ptr, i));
  }
  
  return true;
}

static ant_value_t copy_object_data_props(ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx) {
  ant_value_t keys = js_own_property_keys(js, src, true, true);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t view = js_object_view(src);
  ant_value_t result = Ant_Value_Undefined;
  
  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t k = 0; k < key_count && !is_err(result); k++) {
    GC_ROOT_SAVE(key_mark, js);
    
    ant_value_t key = js_arr_get(js, keys, k);
    GC_ROOT_PIN(js, key);
    
    copy_key_t ck;
    if (!copy_key_of(js, key, &ck)) result = Ant_Error_CreateOutOfMemory(js);
    else if (!copy_key_excluded(cx, ck)) {
      ant_value_t own = Ant_Object_HasOwn(js, view, key);
      if (is_err(own)) result = own;
      else if (js_truthy(js, own)) {
        ant_value_t val = copy_key_get(js, src, ck);
        GC_ROOT_PIN(js, val);
        result = is_err(val) ? val : copy_emit(js, cx, dst, ck, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : Ant_Value_Undefined;
}

ant_value_t copy_data_props(ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx) {
  if (vtype(src) == kTypeString) return copy_string_props(js, dst, src, cx);
  if (!is_object_type(src)) return Ant_Value_Undefined;
  
  ant_value_t source = js_as_obj(src);
  if (is_proxy(source)) return copy_proxy_data_props(js, dst, source, cx);
  
  ant_value_t result;
  if (copy_ordinary_data_props(js, dst, src, cx, &result)) return result;
  return copy_object_data_props(js, dst, src, cx);
}

ant_value_t object_enum_collect(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  ant_value_t arr = Ant_Array_Create(js);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);
  GC_ROOT_PIN(js, obj);
  
  copy_ctx_t cx = { .collect = true, .mode = mode };
  ant_value_t result = copy_data_props(js, arr, obj, &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : mkval(kTypeArray, vdata(arr));
}

static inline bool object_enum_enumerable(ant_t *js, ant_value_t obj, ant_object_t *ptr, const ant_shape_prop_t *prop) {
  if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) return false;
  if (!ptr->flags.is_exotic) return true;
  descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(obj), prop->key.interned, intern_length(prop->key.interned));
  return !desc || desc->enumerable;
}

static ant_value_t object_enum_visit(
  ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode,
  object_enum_sink_t *sink, const copy_key_snapshot_t *snap
) {
  for (uint32_t k = 0; k < snap->count; k++) {
    copy_key_t ck = snap->keys[k];
    if (copy_key_is_sym(ck)) continue;
    
    int32_t slot = copy_key_at_slot(ptr, snap->slots[k], ck) ? (int32_t)snap->slots[k] : copy_key_slot(ptr->shape, ck);
    if (slot < 0 || (uint32_t)slot >= ptr->prop_count) continue;
    
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    ant_value_t val = !(prop->has_getter || prop->has_setter) ? ant_object_prop_get_unchecked(ptr, (uint32_t)slot)
      : prop->has_getter ? js_call_getter(js, obj, prop->getter) : Ant_Value_Undefined;
    
    if (is_err(val)) return val;
    object_enum_emit(js, sink, object_enum_interned_item(js, mode, ck.interned, val));
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t object_enum_props_from(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink, uint32_t start) {
  uint32_t count = ant_shape_count(ptr->shape);
  uint32_t limit = count < ptr->prop_count ? count : ptr->prop_count;
  uint32_t n = limit > start ? limit - start : 0;
  
  copy_key_snapshot_t snap;
  snap.count = 0;
  snap.keys = n <= 32 ? snap.stack : malloc((size_t)n * sizeof(*snap.keys));
  snap.slots = n <= 32 ? snap.stack_slots : malloc((size_t)n * sizeof(*snap.slots));
  
  if (!snap.keys || !snap.slots) {
    copy_key_snapshot_free(&snap);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  for (uint32_t i = start; i < limit; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    snap.keys[snap.count] = copy_key_at(prop);
    snap.slots[snap.count++] = i;
  }
  
  ant_value_t result = object_enum_visit(js, obj, ptr, mode, sink, &snap);
  copy_key_snapshot_free(&snap);
  
  return result;
}

static ant_value_t object_enum_props_ordered(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  
  own_index_list_t indices;
  own_index_list_init(&indices);
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      own_index_list_free(&indices);
      copy_key_snapshot_t snap;
      if (!copy_key_snapshot_take(ptr, &snap, true)) return Ant_Error_CreateOutOfMemory(js);
      ant_value_t result = object_enum_visit(js, obj, ptr, mode, sink, &snap);
      copy_key_snapshot_free(&snap);
      return result;
    }
    
    unsigned long index = 0;
    if (!own_key_is_array_index(prop->key.interned, intern_length(prop->key.interned), &index)) continue;
    if (!own_index_list_push(&indices, i, index)) {
      own_index_list_free(&indices);
      return Ant_Error_CreateOutOfMemory(js);
    }
  }
  
  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t k = 0; k < indices.count; k++) {
    uint32_t slot = indices.items[k].slot;
    const char *key = ant_shape_prop_at(shape, slot)->key.interned;
    object_enum_emit(js, sink, object_enum_interned_item(js, mode, key, ant_object_prop_get_unchecked(ptr, slot)));
  }
  own_index_list_free(&indices);
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    const char *key = prop->key.interned;
    if (own_key_is_array_index(key, intern_length(key), NULL)) continue;
    object_enum_emit(js, sink, object_enum_interned_item(js, mode, key, ant_object_prop_get_unchecked(ptr, i)));
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t object_enum_props(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink) {
  ant_shape_t *shape = ptr->shape;
  if (ant_shape_may_have_index_keys(shape)) return object_enum_props_ordered(js, obj, ptr, mode, sink);
  
  uint32_t count = ant_shape_count(shape);
  uint32_t n = sink->count;
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      sink->count = n;
      return object_enum_props_from(js, obj, ptr, mode, sink, i);
    }
    
    const char *key = prop->key.interned;
    ant_value_t item = object_enum_interned_item(js, mode, key, ant_object_prop_get_unchecked(ptr, i));
    if (!sink->spilled && n < OBJECT_ENUM_SINK_STACK) { sink->items[n++] = item; continue; }
    
    sink->count = n;
    object_enum_emit_spilled(js, sink, item);
    n = sink->count;
  }
  
  sink->count = n;
  return Ant_Value_Undefined;
}

ant_value_t js_copy_data_props(
  ant_t *js, ant_value_t dst, ant_value_t src, 
  const ant_value_t *excluded, int excluded_count
) {
  if (!is_object_type(dst)) return Ant_Value_Undefined;
  
  copy_key_t stack_keys[16];
  copy_key_t *keys = excluded_count <= 16 ? stack_keys : malloc((size_t)excluded_count * sizeof(*keys));
  if (!keys) return Ant_Error_CreateOutOfMemory(js);
  
  int count = 0;
  for (int i = 0; i < excluded_count; i++)
    if (copy_excluded_key_of(js, excluded[i], &keys[count])) count++;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, dst);
  GC_ROOT_PIN(js, src);
  
  copy_ctx_t cx = { .keys = keys, .count = count };
  ant_value_t result = copy_data_props(js, dst, src, &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  if (keys != stack_keys) free(keys);
  
  return is_err(result) ? result : Ant_Value_Undefined;
}

ant_iter_t js_prop_iter_begin(ant_t *js, ant_value_t obj) {
  ant_iter_t iter = {.js = NULL, .obj = NULL, .off = 0};
  
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return iter;

  iter.obj = js_obj_ptr(js_as_obj(obj));
  if (!iter.obj || !iter.obj->shape) {
    iter.obj = NULL;
    return iter;
  }
  
  iter.js = js;
  return iter;
}

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, ant_value_t *value) {
  ant_iter_key_t meta = {0};
  
  while (js_prop_iter_next_key(iter, &meta, value)) {
    if (meta.is_symbol) continue;
    if (key) *key = meta.str;
    if (key_len) *key_len = meta.key_len;
    return true;
  }

  return false;
}

bool js_prop_iter_next_key(ant_iter_t *iter, ant_iter_key_t *key_out, ant_value_t *value) {
  if (!iter || !iter->obj) return false;
  ant_object_t *obj = iter->obj;
  
  if (!obj || !obj->shape) return false;
  uint32_t count = ant_shape_count(obj->shape);
  
  while (iter->off < count) {
    uint32_t i = (uint32_t)iter->off++;
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, i);
    if (!prop) continue;
    if (i >= obj->prop_count) continue;
    if (key_out) {
      key_out->slot = i;
      key_out->is_symbol = (prop->type == ANT_SHAPE_KEY_SYMBOL);
      if (key_out->is_symbol) {
        key_out->str = NULL;
        key_out->key_len = 0;
        key_out->sym_off = prop->key.sym_off;
      } else {
        key_out->str = prop->key.interned;
        key_out->key_len = intern_length(prop->key.interned);
        key_out->sym_off = 0;
      }
    }
    
    if (value) *value = ant_object_prop_get_unchecked(obj, i);
    return true;
  }

  return false;
}

bool js_prop_iter_next_val(ant_iter_t *iter, ant_value_t *key_out, ant_value_t *value) {
  ant_iter_key_t meta = {0};

  if (!iter || !iter->obj) return false;
  if (!js_prop_iter_next_key(iter, &meta, value)) return false;

  if (key_out) {
    if (meta.is_symbol) *key_out = mkval(kTypeSymbol, meta.sym_off);
    else *key_out = js_mkstr(iter->js, meta.str, meta.key_len);
  }

  return true;
}

void js_prop_iter_end(ant_iter_t *iter) {
  if (!iter) return;
  iter->js = NULL;
  iter->obj = NULL;
  iter->off = 0;
}
