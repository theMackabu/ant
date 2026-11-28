#include <string.h>
#include <stdlib.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "keys.h"
#include "gc/roots.h"
#include "gc/strings.h"
#include "descriptors.h"
#include "silver/call.h"
#include "arrays.h"

#include "modules/structured-clone.h"
#include "modules/blob.h"
#include "modules/date.h"
#include "modules/wire.h"
#include "modules/regex.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/domexception.h"
#include "modules/iterator.h"

typedef struct {
  collection_table_t *seen;
  collection_table_t *transfer;
  gc_temp_root_scope_t roots;
} sc_t;

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, sc_t *sc);

static ant_value_t sc_clone_error(ant_t *js, const char *message) {
  return js_throw(js, make_dom_exception(js, message, "DataCloneError"));
}

static ant_value_t sc_detached_error(ant_t *js) {
  return sc_clone_error(js, "An ArrayBuffer is detached and could not be cloned.");
}

static js_err_type_t sc_error_type(const char *name) {
  for (js_err_type_t type = JS_ERR_TYPE; name && type <= JS_ERR_URI; type++)
    if (strcmp(name, js_err_type_name(type)) == 0) return type;
  return JS_ERR_GENERIC;
}

static bool sc_store(ant_t *js, sc_t *sc, collection_table_t *table, ant_value_t key, ant_value_t value) {
  return
    gc_temp_root_push(&sc->roots, key) && gc_temp_root_push(&sc->roots, value) &&
    collections_map_store_cloned_entry(js, table, key, value);
}

static ant_value_t sc_lookup(ant_t *js, sc_t *sc, ant_value_t key) {
  map_entry_t *entry = Ant_Map_Find(js, sc->seen, key);
  return entry ? entry->value : Ant_Value_Undefined;
}

static ant_value_t sc_remember(ant_t *js, sc_t *sc, ant_value_t key, ant_value_t clone) {
  if (is_err(clone)) return clone;
  return sc_store(js, sc, sc->seen, key, clone) ? clone : Ant_Error_CreateOutOfMemory(js);
}

static map_entry_t *sc_transfer_entry(ant_t *js, sc_t *sc, ant_value_t key) {
  return sc->transfer ? Ant_Map_Find(js, sc->transfer, key) : NULL;
}

static ant_value_t sc_clone_view_buffer(ant_t *js, ant_value_t view, sc_t *sc, ArrayBufferData **out) {
  ant_value_t clone = sc_clone_rec(js, js_get_slot(view, SLOT_DATA), sc);
  if (is_err(clone)) return clone;

  *out = buffer_get_arraybuffer_data(clone);
  if (!*out) return sc_clone_error(js, "View buffer could not be cloned");

  return clone;
}

static ant_value_t sc_clone_typed_array(ant_t *js, ant_value_t val, TypedArrayData *ta_data, sc_t *sc) {
  if (!ta_data || !ta_data->buffer) return sc_clone_error(js, "TypedArray could not be cloned");
  if (!Ant_Buffer_ViewIsLive(ta_data)) return sc_detached_error(js);

  ArrayBufferData *buffer = NULL;
  ant_value_t ab_obj = sc_clone_view_buffer(js, val, sc, &buffer);
  if (is_err(ab_obj)) return ab_obj;

  return sc_remember(js, sc, val, create_typed_array_with_buffer(
    js, ta_data->type, buffer, ta_data->byte_offset, ta_data->length,
    js->builtins.typedarray_protos[ta_data->type], ab_obj, ta_data->length_tracking
  ));
}

static ant_value_t sc_clone_dataview(ant_t *js, ant_value_t val, sc_t *sc) {
  DataViewData *dv = buffer_get_dataview_data(val);
  if (!dv || !dv->buffer) return sc_clone_error(js, "DataView could not be cloned");
  if (!Ant_Buffer_ViewIsLive(dv)) return sc_detached_error(js);

  ArrayBufferData *buffer = NULL;
  ant_value_t ab_obj = sc_clone_view_buffer(js, val, sc, &buffer);
  if (is_err(ab_obj)) return ab_obj;

  return sc_remember(js, sc, val, create_dataview_with_buffer(
    js, buffer, dv->byte_offset, dv->byte_length, ab_obj, dv->length_tracking
  ));
}

static ant_value_t sc_transfer_placeholder(ant_t *js, sc_t *sc, ant_value_t key) {
  ArrayBufferData *placeholder = create_array_buffer_data(js, 0);
  if (!placeholder) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t target = create_arraybuffer_obj(js, placeholder);
  free_array_buffer_data(js, placeholder);
  if (is_err(target)) return target;

  return sc_store(js, sc, sc->transfer, key, target) ? target : Ant_Error_CreateOutOfMemory(js);
}

static ant_value_t sc_clone_arraybuffer(ant_t *js, ant_value_t val, ArrayBufferData *abd, sc_t *sc) {
  if (abd->is_detached) return sc_detached_error(js);
  if (sc_transfer_entry(js, sc, val)) return sc_remember(js, sc, val, sc_transfer_placeholder(js, sc, val));
  if (abd->is_shared) return sc_remember(js, sc, val, create_shared_arraybuffer_obj(js, abd));
  return sc_remember(js, sc, val, buffer_clone_arraybuffer(js, abd));
}

static ant_value_t sc_clone_wrapper(ant_t *js, ant_value_t val, ant_value_t prim, sc_t *sc) {
  if (vtype(prim) == kTypeSymbol) return sc_clone_error(js, "[object Symbol] could not be cloned.");
  return sc_remember(js, sc, val, js_normalize_sloppy_this(js, prim));
}

static bool sc_array_is_plain_packed(ant_value_t arr, ant_offset_t len) {
  ant_object_t *ptr = js_obj_ptr(arr);
  return ptr && !ptr->flags.may_have_holes && array_dense_covers(ptr, len) && ant_shape_count(ptr->shape) == 0;
}

static ant_value_t sc_clone_packed_array(ant_t *js, ant_value_t val, ant_offset_t len, sc_t *sc) {
  ant_value_t *data = NULL;
  ant_value_t clone = Ant_Array_CreateUninit(js, (uint32_t)len, (uint32_t)len, &data);
  if (is_err(clone)) return clone;

  for (ant_offset_t i = 0; i < len; i++) data[i] = Ant_Value_Undefined;
  clone = sc_remember(js, sc, val, clone);
  if (is_err(clone)) return clone;

  ant_object_t *dst = js_obj_ptr(clone);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = Ant_Value_Undefined;
    if (!array_dense_read(js_obj_ptr(val), i, &item)) item = js_arr_get(js, val, i);
    if (!is_clone_identity(item) && is_err(item = sc_clone_rec(js, item, sc))) return item;

    dst->u.array.data[i] = item;
    gc_write_barrier_elem(js, dst, (uint32_t)i, item);
  }

  return clone;
}

static ant_value_t sc_clone_array_keys(ant_t *js, ant_value_t val, ant_value_t clone, sc_t *sc) {
  ant_value_t keys = js_own_property_keys(js, val, false, true);
  if (is_err(keys)) return keys;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t result = Ant_Value_Undefined;
  ant_offset_t count = js_arr_len(js, keys);

  for (ant_offset_t i = 0; i < count && !is_err(result); i++) {
    size_t key_len = 0;
    const char *key = js_getstr(js, js_arr_get(js, keys, i), &key_len);
    if (!key) continue;

    ant_value_t item = js_getprop_fallback_len(js, val, key, key_len);
    if (is_err(item) || is_err(item = sc_clone_rec(js, item, sc))) { result = item; break; }

    key = js_getstr(js, js_arr_get(js, keys, i), &key_len);
    unsigned long index = 0;
    if (own_key_is_array_index(key, key_len, &index)) js_arr_set_index(js, clone, (ant_offset_t)index, item);
    else result = js_mkprop_fast(js, clone, key, key_len, item);
  }

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t sc_clone_array(ant_t *js, ant_value_t val, sc_t *sc) {
  ant_offset_t len = js_arr_len(js, val);
  if (sc_array_is_plain_packed(val, len)) return sc_clone_packed_array(js, val, len, sc);

  ant_value_t clone = sc_remember(js, sc, val, Ant_Array_Create(js));
  if (is_err(clone)) return clone;

  ant_value_t status = sc_clone_array_keys(js, val, clone, sc);
  if (is_err(status)) return status;

  if (js_arr_len(js, clone) < len) {
    ant_value_t set = js_setprop(js, clone, js->length_str, Ant_Value_FromNumber((double)len));
    if (is_err(set)) return set;
  }

  return clone;
}

static ant_value_t sc_clone_map(ant_t *js, ant_value_t val, sc_t *sc) {
  collection_table_t *dst = NULL;
  ant_value_t clone = sc_remember(js, sc, val, collections_map_create(js, &dst));
  if (is_err(clone)) return clone;

  collection_table_t *src = get_map_from_obj(val);
  if (!src) return clone;
  if (collections_is_primitive(src, true))
    return collections_copy(js, dst, src, true) ? clone : Ant_Error_CreateOutOfMemory(js);

  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  uint32_t len = 0;

  ant_value_t *items = collections_snapshot(&roots, (collections_snapshot_t){ .table = src, .map = true }, &len);
  ant_value_t result = items ? clone : Ant_Error_CreateOutOfMemory(js);

  for (uint32_t i = 0; i < len && !is_err(result); i += 2) {
    ant_value_t key = sc_clone_rec(js, items[i], sc);
    if (is_err(key)) { result = key; break; }
    ant_value_t value = sc_clone_rec(js, items[i + 1], sc);
    if (is_err(value)) result = value;
    else if (!collections_map_store_cloned_entry(js, dst, key, value)) result = Ant_Error_CreateOutOfMemory(js);
  }

  gc_temp_root_scope_end(&roots);
  return result;
}

static ant_value_t sc_clone_set(ant_t *js, ant_value_t val, sc_t *sc) {
  collection_table_t *dst = NULL;
  ant_value_t clone = sc_remember(js, sc, val, collections_set_create(js, &dst));
  if (is_err(clone)) return clone;

  collection_table_t *src = get_set_from_obj(val);
  if (!src) return clone;
  if (collections_is_primitive(src, false))
    return collections_copy(js, dst, src, false) ? clone : Ant_Error_CreateOutOfMemory(js);

  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  uint32_t len = 0;

  ant_value_t *items = collections_snapshot(&roots, (collections_snapshot_t){ .table = src }, &len);
  ant_value_t result = items ? clone : Ant_Error_CreateOutOfMemory(js);

  for (uint32_t i = 0; i < len && !is_err(result); i++) {
    ant_value_t value = sc_clone_rec(js, items[i], sc);
    if (is_err(value)) result = value;
    else if (!collections_set_store_cloned_entry(js, dst, value)) result = Ant_Error_CreateOutOfMemory(js);
  }

  gc_temp_root_scope_end(&roots);
  return result;
}

static ant_value_t sc_clone_error_object(ant_t *js, ant_value_t val, sc_t *sc) {
  js_err_type_t type = sc_error_type(get_str_prop(js, val, "name", 4, NULL));

  ant_value_t clone = Ant_Object_CreateBare(js);
  js_set_proto_init(clone, js->builtins.error_protos[type]);
  clone = sc_remember(js, sc, val, clone);
  if (is_err(clone)) return clone;

  const char *msg = get_str_prop(js, val, "message", 7, NULL);
  const char *stack = get_str_prop(js, val, "stack", 5, NULL);

  if (stack) defhidden(js, clone, "stack", 5, js_mkstr(js, stack, strlen(stack)));
  if (msg) defhidden(js, clone, "message", 7, js_mkstr(js, msg, strlen(msg)));
  js_set_slot(clone, SLOT_ERROR_BRAND, Ant_Value_True);
  js_set_slot(clone, SLOT_ERR_TYPE, Ant_Value_FromNumber((double)type));

  ant_value_t cause = Ant_Value_Undefined;
  if (!js_try_get_own_data_prop(js, val, "cause", 5, &cause)) return clone;

  ant_value_t cause_clone = sc_clone_rec(js, cause, sc);
  if (is_err(cause_clone)) return cause_clone;

  defhidden(js, clone, "cause", 5, cause_clone);
  return clone;
}

static ant_value_t sc_clone_date(ant_t *js, ant_value_t val, sc_t *sc) {
  ant_value_t clone = Ant_Object_CreateBare(js);
  ant_value_t date_proto = js->builtins.date_proto;
  if (is_object_type(date_proto)) js_set_proto_init(clone, date_proto);

  js_set_slot(clone, SLOT_DATA, js_get_slot(val, SLOT_DATA));
  js_set_slot(clone, SLOT_BRAND, Ant_Value_FromNumber(BRAND_DATE));

  return sc_remember(js, sc, val, clone);
}

static ant_value_t sc_clone_blob(ant_t *js, ant_value_t val, blob_data_t *bd, sc_t *sc) {
  ant_value_t clone = sc_remember(js, sc, val, blob_create(js, bd->data, bd->size, bd->type));
  if (is_err(clone) || !bd->name) return clone;

  blob_data_t *nbd = blob_get_data(clone);
  if (nbd) {
    nbd->name = strdup(bd->name);
    nbd->last_modified = bd->last_modified;
  }

  js_set_proto_init(clone, js->builtins.file_proto);
  return clone;
}

static bool sc_is_plain(ant_t *js, const ant_object_t *ptr) {
  return
    ptr->type_tag == kTypeObject && ptr->shape && !ptr->extra_slots && !ptr->native_ptr && !ptr->native_tag &&
    !ptr->finalizer && !ptr->flags.is_exotic && !ptr->flags.arguments_object && !ptr->flags.regexp_brand &&
    ant_shape_proto(ptr->shape) == js->sym.object_proto && ant_shape_count(ptr->shape) <= ptr->prop_count &&
    js_shape_is_plain_data(js, ptr->shape);
}

static ant_value_t sc_drop_missing(ant_t *js, ant_value_t clone, ant_shape_t *shape) {
  ant_object_t *dst = js_obj_ptr(clone);
  ant_value_t result = clone;
  ant_shape_retain(shape);

  for (uint32_t i = 0, count = ant_shape_count(shape); i < count && !is_err(result); i++) {
    const char *key = ant_shape_prop_at(shape, i)->key.interned;
    int32_t slot = ant_shape_lookup_interned(dst->shape, key);
    if (slot < 0 || ant_object_prop_get_unchecked(dst, (uint32_t)slot) != T_EMPTY) continue;
    ant_value_t deleted = js_delete_prop_ordinary(js, clone, key, intern_length(key));
    if (is_err(deleted)) result = deleted;
  }

  ant_shape_release(shape);
  return result;
}

static ant_value_t sc_clone_plain(ant_t *js, ant_value_t val, ant_object_t *src, sc_t *sc) {
  ant_shape_t *shape = src->shape;
  uint32_t count = ant_shape_count(shape);

  ant_shape_retain(shape);
  ant_object_t *dst = obj_alloc_shaped(js, kTypeObject, shape);
  if (!dst || !js_obj_ensure_prop_capacity(js, dst, count)) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t clone = sc_remember(js, sc, val, js_obj_from_ptr(dst));
  if (is_err(clone)) return clone;
  bool missing = false;

  for (uint32_t i = 0; i < count; i++) {
    ant_value_t item = ant_object_prop_get_unchecked(src, i);

    if (src->shape != shape) {
      const char *key = ant_shape_prop_at(shape, i)->key.interned;
      if (ant_shape_lookup_interned(src->shape, key) < 0) {
        ant_object_prop_set_unchecked(dst, i, T_EMPTY);
        missing = true;
        continue;
      }
      item = js_getprop_fallback_len(js, val, key, intern_length(key));
      if (is_err(item)) return item;
    }

    if (!is_clone_identity(item) && is_err(item = sc_clone_rec(js, item, sc))) return item;
    ant_object_prop_set_unchecked(dst, i, item);
    gc_write_barrier_prop(js, dst, item);
  }

  return missing ? sc_drop_missing(js, clone, shape) : clone;
}

static ant_value_t sc_clone_object(ant_t *js, ant_value_t val, ant_object_t *ptr, sc_t *sc) {
  ant_value_t clone = sc_remember(js, sc, val, Ant_Object_Create(js));
  if (is_err(clone)) return clone;

  if (js_proxy_is_host(val)) {
    ant_value_t copied = js_copy_data_props(js, clone, val, NULL, 0);
    return is_err(copied) ? copied : clone;
  }

  ant_iter_t iter = js_prop_iter_begin(js, val);
  ant_iter_key_t key;
  ant_value_t item;
  ant_value_t result = clone;

  while (!is_err(result) && js_prop_iter_next_key(&iter, &key, &item)) {
    if (key.is_symbol || !js_is_own_enumerable_prop(js, val, ptr, &key)) continue;
    const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, key.slot);

    if (ptr->flags.is_exotic || !meta || meta->has_getter || meta->has_setter)
      item = js_getprop_fallback_len(js, val, key.str, key.key_len);

    if (is_err(item) || is_err(item = sc_clone_rec(js, item, sc))) result = item;
    else if (is_err(item = js_mkprop_fast(js, clone, key.str, key.key_len, item))) result = item;
  }

  js_prop_iter_end(&iter);
  return result;
}

static ant_value_t sc_clone_special(ant_t *js, ant_value_t val, ant_object_t *ptr, sc_t *sc) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(val);
  if (ta_data) return sc_clone_typed_array(js, val, ta_data, sc);

  if (ant_wire_is_uncloneable(js, val)) return sc_clone_error(js, "Cannot clone object of unsupported type.");

  ArrayBufferData *abd = buffer_get_arraybuffer_data(val);
  if (abd) return sc_clone_arraybuffer(js, val, abd, sc);
  if (buffer_is_dataview(val)) return sc_clone_dataview(js, val, sc);

  if (js_is_arguments(val)) return sc_clone_error(js, "#<Object> could not be cloned.");
  if (vtype(val) == kTypeArray) return sc_clone_array(js, val, sc);

  ant_value_t prim = js_get_slot(val, SLOT_PRIMITIVE);
  if ((1u << vtype(prim)) & T_BOXABLE_PRIMITIVE_MASK) return sc_clone_wrapper(js, val, prim, sc);

  switch (ptr->type_tag) {
    case kTypeWeakMap:
    case kTypeWeakSet: return sc_clone_error(js, "WeakMap/WeakSet could not be cloned");
    case kTypeMap: return sc_clone_map(js, val, sc);
    case kTypeSet: return sc_clone_set(js, val, sc);
    default: break;
  }

  if (js_get_slot(val, SLOT_ERROR_BRAND) == Ant_Value_True) return sc_clone_error_object(js, val, sc);
  if (is_date_instance(val)) return sc_clone_date(js, val, sc);
  if (ptr->flags.regexp_brand) return sc_remember(js, sc, val, regexp_create_literal(js, js_get_slot(val, SLOT_DATA), regexp_flags_value(js, val)));

  blob_data_t *bd = js_is_prototype_of(js, js->builtins.blob_proto, val) ? blob_get_data(val) : NULL;
  if (bd) return sc_clone_blob(js, val, bd, sc);

  return sc_clone_object(js, val, ptr, sc);
}

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, sc_t *sc) {
  if (is_clone_identity(val)) return val;

  uint8_t t = vtype(val);
  if (t == kTypeSymbol) return sc_clone_error(js, "Symbol cannot be serialized");
  if (t == kTypeFunction || t == kTypeBuiltin) return sc_clone_error(js, "() => {} could not be cloned");
  if (t != kTypeObject && t != kTypeArray && t != kTypeTypedArray) return sc_clone_error(js, "Value could not be cloned");
  if (sv_check_c_stack_overflow(js)) return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");

  ant_value_t existing = sc_lookup(js, sc, val);
  if (vtype(existing) != kTypeUndefined) return existing;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, val);

  ant_object_t *ptr = t == kTypeTypedArray ? NULL : js_obj_ptr(val);
  ant_value_t clone =
    t == kTypeTypedArray ? sc_clone_typed_array(js, val, (TypedArrayData *)js_gettypedarray(val), sc) :
    !ptr ? sc_clone_error(js, "Value could not be cloned") :
    sc_is_plain(js, ptr) ? sc_clone_plain(js, val, ptr, sc) :
    sc_clone_special(js, val, ptr, sc);

  GC_ROOT_RESTORE(js, mark);
  return clone;
}

static ant_value_t sc_transfer_item_error(ant_t *js, sc_t *sc, ant_value_t item) {
  ArrayBufferData *data = is_object_type(item) ? buffer_get_arraybuffer_data(item) : NULL;
  if (!data || data->is_shared) return sc_clone_error(js, "Found invalid value in transferList.");
  if (sc_transfer_entry(js, sc, item)) return sc_clone_error(js, "Transfer list contains duplicate ArrayBuffer");
  if (!buffer_can_transfer(data) || data->untransferable || ant_wire_is_untransferable(js, item))
    return sc_clone_error(js, "Cannot transfer object of unsupported type.");
  return Ant_Value_Undefined;
}

static ant_value_t sc_collect_transfer(ant_t *js, sc_t *sc, ant_value_t options) {
  if (vtype(options) == kTypeUndefined || vtype(options) == kTypeNull) return Ant_Value_Undefined;
  if (!is_object_type(options)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Failed to execute 'structuredClone': Options cannot be converted to a dictionary"
  );

  ant_value_t list = js_getprop_fallback(js, options, "transfer");
  if (is_err(list) || vtype(list) == kTypeUndefined) return list;

  iterator_t it;
  if (!is_object_type(list) || !js_iter_open(js, list, &it)) return Ant_Exception_Pending(js)
    ? Ant_Exception_Current(js)
    : js_mkerr_typed(js, JS_ERR_TYPE, "Failed to execute 'structuredClone': transfer in Options cannot be converted to a sequence");

  sc->transfer = collections_table_create();
  if (!sc->transfer) {
    js_iter_close(js, &it);
    return Ant_Error_CreateOutOfMemory(js);
  }

  ant_value_t item;
  while (js_iter_next(js, &it, &item)) {
    ant_value_t error = sc_transfer_item_error(js, sc, item);
    if (!is_err(error) && !sc_store(js, sc, sc->transfer, item, Ant_Value_Undefined))
      error = Ant_Error_CreateOutOfMemory(js);
    if (is_err(error)) {
      js_iter_close(js, &it);
      return error;
    }
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_Undefined;
}

static void sc_commit_transfer(ant_t *js, sc_t *sc) {
  if (!sc->transfer) return;
  for (uint32_t index = 0;;) {
    map_entry_t *entry = collections_map_next(sc->transfer, &index);
    if (!entry) break;
    ArrayBufferData *from = buffer_get_arraybuffer_data(entry->key);
    ArrayBufferData *to = buffer_get_arraybuffer_data(entry->value);
    if (!from) continue;
    if (to) buffer_transfer_storage(js, from, to);
    else buffer_detach(js, from);
  }
}

ant_value_t js_structured_clone(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;

  sc_t sc = {0};
  gc_temp_root_scope_begin(js, &sc.roots);

  ant_value_t result = sc_collect_transfer(js, &sc, nargs > 1 ? args[1] : Ant_Value_Undefined);
  if (!is_err(result) && !is_clone_identity(args[0]) && !(sc.seen = collections_table_create())) result = Ant_Error_CreateOutOfMemory(js);
  if (!is_err(result)) result = sc_clone_rec(js, args[0], &sc);
  if (!is_err(result)) sc_commit_transfer(js, &sc);

  collections_table_free(sc.seen);
  collections_table_free(sc.transfer);
  gc_temp_root_scope_end(&sc.roots);
  
  return result;
}

void init_structured_clone_module(ant_t *js) {
  ant_value_t global = js_glob(js);

  js_set(js, global, "structuredClone", Ant_Function_CreateWithArity(js_structured_clone, 2));
  js_set_descriptor(
    js, global, "structuredClone", 15,
    JS_DESC_W | JS_DESC_E | JS_DESC_C
  );
}
