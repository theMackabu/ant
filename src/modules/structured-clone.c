#include <string.h>
#include <stdlib.h>
#include <uthash.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/call.h"

#include "modules/date.h"
#include "modules/regex.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/domexception.h"
#include "modules/blob.h"
#include "modules/structured-clone.h"
#include "modules/wire.h"
#include "modules/iterator.h"

typedef struct sc_entry {
  ant_value_t key;
  ant_value_t value;
  UT_hash_handle hh;
} sc_entry_t;

static js_err_type_t sc_error_type(const char *name) {
  for (js_err_type_t type = JS_ERR_TYPE; name && type <= JS_ERR_URI; type++)
    if (strcmp(name, js_err_type_name(type)) == 0) return type;
  return JS_ERR_GENERIC;
}

static void sc_add(sc_entry_t **table, ant_value_t key, ant_value_t value) {
  sc_entry_t *e = calloc(1, sizeof(sc_entry_t));
  if (!e) return;
  e->key = key;
  e->value = value;
  HASH_ADD(hh, *table, key, sizeof(ant_value_t), e);
}

static sc_entry_t *sc_find(sc_entry_t **table, ant_value_t key) {
  sc_entry_t *e;
  HASH_FIND(hh, *table, &key, sizeof(ant_value_t), e);
  return e;
}

static ant_value_t sc_lookup(sc_entry_t **table, ant_value_t key) {
  sc_entry_t *e = sc_find(table, key);
  return e ? e->value : js_mkundef();
}

static ant_value_t sc_transfer_target(ant_t *js, sc_entry_t *entry) {
  ArrayBufferData *placeholder = create_array_buffer_data(js, 0);
  if (!placeholder) return Ant_Error_CreateOutOfMemory(js);
  
  entry->value = create_arraybuffer_obj(js, placeholder);
  free_array_buffer_data(js, placeholder);
  return entry->value;
}

static void sc_free(sc_entry_t **table) {
  sc_entry_t *e, *tmp;
  HASH_ITER(hh, *table, e, tmp) {
    HASH_DEL(*table, e);
    free(e);
  }
}

static ant_value_t sc_detached_error(ant_t *js) {
  return js_throw(js, make_dom_exception(js, "An ArrayBuffer is detached and could not be cloned.", "DataCloneError"));
}

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, sc_entry_t **seen, sc_entry_t **transfer);

static ant_value_t sc_clone_view_buffer(ant_t *js, ant_value_t view, sc_entry_t **seen, sc_entry_t **transfer, ArrayBufferData **out) {
  ant_value_t clone = sc_clone_rec(js, js_get_slot(view, SLOT_DATA), seen, transfer);
  if (is_err(clone)) return clone;
  
  *out = buffer_get_arraybuffer_data(clone);
  if (!*out) return js_throw(js, make_dom_exception(js, "View buffer could not be cloned", "DataCloneError"));
  
  return clone;
}

static ant_value_t sc_clone_typed_array(
  ant_t *js, ant_value_t key, TypedArrayData *ta_data, sc_entry_t **seen, sc_entry_t **transfer
) {
  ant_value_t existing = sc_lookup(seen, key);
  if (vtype(existing) != kTypeUndefined) return existing;
  
  if (!ta_data || !ta_data->buffer) return js_throw(
    js, make_dom_exception(js, "TypedArray could not be cloned", "DataCloneError")
  );
  
  if (!Ant_Buffer_ViewIsLive(ta_data)) return sc_detached_error(js);
  
  ArrayBufferData *buffer = NULL;
  ant_value_t ab_obj = sc_clone_view_buffer(js, key, seen, transfer, &buffer);
  if (is_err(ab_obj)) return ab_obj;
  
  ant_value_t clone = create_typed_array_with_buffer(
    js, ta_data->type, buffer, ta_data->byte_offset, ta_data->length,
    js->builtins.typedarray_protos[ta_data->type], ab_obj, ta_data->length_tracking
  );
  
  if (!is_err(clone)) sc_add(seen, key, clone);
  return clone;
}

static ant_value_t sc_clone_arraybuffer(
  ant_t *js, ant_value_t key, ArrayBufferData *abd, sc_entry_t **seen, sc_entry_t **transfer
) {
  if (abd->is_detached) return sc_detached_error(js);

  sc_entry_t *transferred = sc_find(transfer, key);
  ant_value_t clone;

  if (transferred) clone = sc_transfer_target(js, transferred);
  else if (abd->is_shared) clone = create_shared_arraybuffer_obj(js, abd);
  else clone = buffer_clone_arraybuffer(js, abd);

  if (!is_err(clone)) sc_add(seen, key, clone);
  return clone;
}

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, sc_entry_t **seen, sc_entry_t **transfer) {
  uint8_t t = vtype(val);
  TypedArrayData *ta_data = NULL;

  if (t == kTypeUndefined || t == kTypeNull || t == kTypeBool || t == kTypeNumber || t == kTypeBigInt || t == kTypeString) return val;
  if (t == kTypeSymbol) return js_throw(js, make_dom_exception(js, "Symbol cannot be serialized", "DataCloneError"));
  if (sv_check_c_stack_overflow(js)) return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");

  if (is_object_type(val)) ta_data = buffer_get_typedarray_data(val);
  if (ta_data) return sc_clone_typed_array(js, val, ta_data, seen, transfer);
  if (t == kTypeTypedArray) return sc_clone_typed_array(js, val, (TypedArrayData *)js_gettypedarray(val), seen, transfer);

  if (t == kTypeFunction || t == kTypeBuiltin)
    return js_throw(js, make_dom_exception(js, "() => {} could not be cloned", "DataCloneError"));
  if (t == kTypePromise || t == kTypeGenerator)
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));
  if (!is_object_type(val))
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));

  ant_value_t existing = sc_lookup(seen, val);
  if (vtype(existing) != kTypeUndefined) return existing;
  
  if (ant_wire_is_uncloneable(js, val))
    return js_throw(js, make_dom_exception(js, "Cannot clone object of unsupported type.", "DataCloneError"));

  ArrayBufferData *abd = buffer_get_arraybuffer_data(val);
  if (abd) return sc_clone_arraybuffer(js, val, abd, seen, transfer);

  if (buffer_is_dataview(val)) {
    DataViewData *dv = buffer_get_dataview_data(val);
    if (!dv || !dv->buffer)
      return js_throw(js, make_dom_exception(js, "DataView could not be cloned", "DataCloneError"));
    if (!Ant_Buffer_ViewIsLive(dv)) return sc_detached_error(js);
    
    ArrayBufferData *new_buf = NULL;
    ant_value_t ab_obj = sc_clone_view_buffer(js, val, seen, transfer, &new_buf);
    if (is_err(ab_obj)) return ab_obj;
    
    ant_value_t clone = create_dataview_with_buffer(js, new_buf, dv->byte_offset, dv->byte_length, ab_obj, dv->length_tracking);
    if (is_err(clone)) return clone;
    
    sc_add(seen, val, clone);
    return clone;
  }

  if (js_is_arguments(val))
    return js_throw(js, make_dom_exception(js, "#<Object> could not be cloned.", "DataCloneError"));

  if (t == kTypeArray) {
    ant_value_t clone = js_mkarr(js);
    sc_add(seen, val, clone);
    
    ant_offset_t len = js_arr_len(js, val);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t ic = sc_clone_rec(js, js_arr_get(js, val, i), seen, transfer);
      if (is_err(ic)) return ic;
      js_arr_push(js, clone, ic);
    }
    
    return clone;
  }

  ant_object_t *obj_ptr = js_obj_ptr(val);
  if (!obj_ptr)
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));

  if (obj_ptr->type_tag == kTypeWeakMap || obj_ptr->type_tag == kTypeWeakSet)
    return js_throw(js, make_dom_exception(js, "WeakMap/WeakSet could not be cloned", "DataCloneError"));

  if (obj_ptr->type_tag == kTypeMap) {
    ant_value_t clone = Ant_Object_CreateBare(js);
    js_obj_ptr(clone)->type_tag = kTypeMap;

    ant_value_t map_proto = js->builtins.map_proto;
    if (is_special_object(map_proto)) js_set_proto_init(clone, map_proto);
    
    map_entry_t **new_head = collections_map_store_new();
    if (!new_head) return Ant_Error_CreateOutOfMemory(js);
    
    *new_head = NULL;
    js_set_native(clone, new_head, MAP_NATIVE_TAG);
    sc_add(seen, val, clone);
    
    map_entry_t **src_head = get_map_from_obj(val);
    if (src_head && *src_head) {
    map_entry_t *e, *tmp;
    HASH_ITER(hh, *src_head, e, tmp) {
      ant_value_t kc = sc_clone_rec(js, e->key_val, seen, transfer);
      if (is_err(kc)) return kc;

      ant_value_t vc = sc_clone_rec(js, e->value, seen, transfer);
      if (is_err(vc)) return vc;

      if (!collections_map_store_cloned_entry(js, new_head, kc, vc))
        return Ant_Error_CreateOutOfMemory(js);
    }}
    
    return clone;
  }

  if (obj_ptr->type_tag == kTypeSet) {
    ant_value_t clone = Ant_Object_CreateBare(js);
    js_obj_ptr(clone)->type_tag = kTypeSet;
    
    ant_value_t set_proto = js->builtins.set_proto;
    if (is_special_object(set_proto)) js_set_proto_init(clone, set_proto);
    
    set_entry_t **new_head = collections_set_store_new();
    if (!new_head) return Ant_Error_CreateOutOfMemory(js);
    *new_head = NULL;
    
    js_set_native(clone, new_head, SET_NATIVE_TAG);
    sc_add(seen, val, clone);

    set_entry_t **src_head = get_set_from_obj(val);
    if (src_head && *src_head) {
      set_entry_t *e, *tmp;
      HASH_ITER(hh, *src_head, e, tmp) {
        ant_value_t vc = sc_clone_rec(js, e->value, seen, transfer);
        if (is_err(vc)) return vc;
        if (!collections_set_store_cloned_entry(js, new_head, vc)) return Ant_Error_CreateOutOfMemory(js);
      }
    }

    return clone;
  }

  if (js_get_slot(val, SLOT_ERROR_BRAND) == js_true) {
    const char *name = get_str_prop(js, val, "name", 4, NULL);
    js_err_type_t type = sc_error_type(name);
    
    ant_value_t clone = Ant_Object_CreateBare(js);
    js_set_proto_init(clone, js->builtins.error_protos[type]);
    sc_add(seen, val, clone);

    const char *msg   = get_str_prop(js, val, "message", 7, NULL);
    const char *stack = get_str_prop(js, val, "stack",   5, NULL);

    if (stack) defhidden(js, clone, "stack", 5, js_mkstr(js, stack, strlen(stack)));
    if (msg)   defhidden(js, clone, "message", 7, js_mkstr(js, msg, strlen(msg)));
    js_set_slot(clone, SLOT_ERROR_BRAND, js_true);
    js_set_slot(clone, SLOT_ERR_TYPE, js_mknum((double)type));

    ant_value_t cause = js_mkundef();
    if (js_try_get_own_data_prop(js, val, "cause", 5, &cause)) {
      ant_value_t cause_clone = sc_clone_rec(js, cause, seen, transfer);
      if (is_err(cause_clone)) return cause_clone;
      defhidden(js, clone, "cause", 5, cause_clone);
    }

    return clone;
  }

  if (is_date_instance(val)) {
    ant_value_t clone = Ant_Object_CreateBare(js);
    ant_value_t date_proto = js->builtins.date_proto;
    if (is_object_type(date_proto)) js_set_proto_init(clone, date_proto);
    
    js_set_slot(clone, SLOT_DATA, js_get_slot(val, SLOT_DATA));
    js_set_slot(clone, SLOT_BRAND, js_mknum(BRAND_DATE));
    sc_add(seen, val, clone);

    return clone;
  }

  if (obj_ptr->flags.regexp_brand) {
    ant_value_t clone = regexp_create_literal(js, js_get_slot(val, SLOT_DATA), regexp_flags_value(js, val));
    if (is_err(clone)) return clone;
    sc_add(seen, val, clone);
    return clone;
  }

  blob_data_t *bd = js_is_prototype_of(js, js->builtins.blob_proto, val) ? blob_get_data(val) : NULL;
  if (bd) {
    ant_value_t clone = blob_create(js, bd->data, bd->size, bd->type);
    if (is_err(clone)) return clone;
    sc_add(seen, val, clone);
    
    if (bd->name) {
      blob_data_t *nbd = blob_get_data(clone);
      if (nbd) {
        nbd->name = strdup(bd->name);
        nbd->last_modified = bd->last_modified;
      }
      
      js_set_proto_init(clone, js->builtins.file_proto);
    }
    
    return clone;
  }

  ant_value_t clone = Ant_Object_Create(js);
  sc_add(seen, val, clone);

  if (js_proxy_is_host(val)) {
    ant_value_t copied = js_copy_data_props(js, clone, val, NULL, 0);
    return is_err(copied) ? copied : clone;
  }

  ant_iter_t iter = js_prop_iter_begin(js, val);
  ant_iter_key_t key;
  ant_value_t pval;

  while (js_prop_iter_next_key(&iter, &key, &pval)) {
    if (key.is_symbol || !js_is_own_enumerable_prop(js, val, obj_ptr, &key)) continue;
    const ant_shape_prop_t *meta = ant_shape_prop_at(obj_ptr->shape, key.slot);
    
    if (obj_ptr->flags.is_exotic || !meta || meta->has_getter || meta->has_setter) {
      pval = js_get(js, val, key.str);
      if (is_err(pval)) { js_prop_iter_end(&iter); return pval; }
    }
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, pval);
    
    ant_value_t pc = sc_clone_rec(js, pval, seen, transfer);
    GC_ROOT_RESTORE(js, mark);
    
    if (is_err(pc)) { 
      js_prop_iter_end(&iter); 
      return pc;
    }
    
    js_set(js, clone, key.str, pc);
  }

  js_prop_iter_end(&iter);
  return clone;
}

static ant_value_t sc_clone_error(ant_t *js, const char *message) {
  return js_throw(js, make_dom_exception(js, message, "DataCloneError"));
}

static ant_value_t sc_transfer_item_error(ant_t *js, sc_entry_t **transfer, ant_value_t item) {
  ArrayBufferData *data = is_object_type(item) ? buffer_get_arraybuffer_data(item) : NULL;
  if (!data || data->is_shared) return sc_clone_error(js, "Found invalid value in transferList.");
  if (sc_find(transfer, item)) return sc_clone_error(js, "Transfer list contains duplicate ArrayBuffer");
  if (!buffer_can_transfer(data) || data->untransferable || ant_wire_is_untransferable(js, item))
    return sc_clone_error(js, "Cannot transfer object of unsupported type.");
  return js_mkundef();
}

static ant_value_t sc_collect_transfer(ant_t *js, ant_value_t options, sc_entry_t **transfer) {
  if (vtype(options) == kTypeUndefined || vtype(options) == kTypeNull) return js_mkundef();
  if (!is_object_type(options)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Failed to execute 'structuredClone': Options cannot be converted to a dictionary"
  );
  
  ant_value_t list = js_getprop_fallback(js, options, "transfer");
  if (is_err(list) || vtype(list) == kTypeUndefined) return list;
  
  iterator_t it;
  if (!is_object_type(list) || !js_iter_open(js, list, &it)) return Ant_Exception_Pending(js)
    ? Ant_Exception_Current(js)
    : js_mkerr_typed(js, JS_ERR_TYPE, "Failed to execute 'structuredClone': transfer in Options cannot be converted to a sequence");
  
  ant_value_t item;
  while (js_iter_next(js, &it, &item)) {
    ant_value_t error = sc_transfer_item_error(js, transfer, item);
    if (is_err(error)) {
      js_iter_close(js, &it);
      return error;
    }
    sc_add(transfer, item, js_mkundef());
  }
  
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkundef();
}

static void sc_commit_transfer(ant_t *js, sc_entry_t **transfer) {
  sc_entry_t *e, *tmp;
  HASH_ITER(hh, *transfer, e, tmp) {
    ArrayBufferData *from = buffer_get_arraybuffer_data(e->key);
    ArrayBufferData *to = buffer_get_arraybuffer_data(e->value);
    if (!from) continue;
    if (to) buffer_transfer_storage(js, from, to);
    else buffer_detach(js, from);
  }
}

ant_value_t js_structured_clone(ant_params_t) {
  if (nargs < 1) return js_mkundef();

  sc_entry_t *transfer = NULL;
  sc_entry_t *seen = NULL;
  
  ant_value_t result = sc_collect_transfer(js, nargs > 1 ? args[1] : js_mkundef(), &transfer);
  if (!is_err(result)) result = sc_clone_rec(js, args[0], &seen, &transfer);
  if (!is_err(result)) sc_commit_transfer(js, &transfer);
  
  sc_free(&seen);
  sc_free(&transfer);
  
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
