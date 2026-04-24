#include <string.h>
#include <stdlib.h>

#include <uthash.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/date.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/domexception.h"
#include "modules/blob.h"
#include "modules/structured-clone.h"

typedef struct sc_entry {
  ant_value_t key;
  ant_value_t value;
  UT_hash_handle hh;
} sc_entry_t;

static void sc_add(sc_entry_t **table, ant_value_t key, ant_value_t value) {
  sc_entry_t *e = calloc(1, sizeof(sc_entry_t));
  if (!e) return;
  e->key = key;
  e->value = value;
  HASH_ADD(hh, *table, key, sizeof(ant_value_t), e);
}

static ant_value_t sc_lookup(sc_entry_t **table, ant_value_t key) {
  sc_entry_t *e;
  HASH_FIND(hh, *table, &key, sizeof(ant_value_t), e);
  return e ? e->value : js_mkundef();
}

static bool sc_has(sc_entry_t **table, ant_value_t key) {
  sc_entry_t *e;
  HASH_FIND(hh, *table, &key, sizeof(ant_value_t), e);
  return e != NULL;
}

static void sc_free(sc_entry_t **table) {
  sc_entry_t *e, *tmp;
  HASH_ITER(hh, *table, e, tmp) {
    HASH_DEL(*table, e);
    free(e);
  }
}

static ant_value_t sc_clone_typed_array(
  ant_t *js, ant_value_t key, TypedArrayData *ta_data, sc_entry_t **seen
) {
  ant_value_t existing = sc_lookup(seen, key);
  if (vtype(existing) != T_UNDEF) return existing;
  
  if (!ta_data || !ta_data->buffer) return js_throw(
    js, make_dom_exception(js, "TypedArray could not be cloned", "DataCloneError")
  );

  ArrayBufferData *new_buf = create_array_buffer_data(ta_data->byte_length);
  if (!new_buf) return js_mkerr(js, "out of memory");
  if (ta_data->byte_length > 0) {
    memcpy(new_buf->data, ta_data->buffer->data + ta_data->byte_offset, ta_data->byte_length);
  }

  ant_value_t clone = create_typed_array(
    js, ta_data->type, new_buf, 0, 
    ta_data->length, buffer_typedarray_type_name(ta_data->type)
  );
  
  sc_add(seen, key, clone);
  return clone;
}

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, sc_entry_t **seen, sc_entry_t **transfer) {
  uint8_t t = vtype(val);
  TypedArrayData *ta_data = NULL;

  if (t == T_UNDEF || t == T_NULL || t == T_BOOL || t == T_NUM || t == T_BIGINT || t == T_STR) return val;
  if (t == T_SYMBOL) return js_throw(js, make_dom_exception(js, "Symbol cannot be serialized", "DataCloneError"));

  if (is_object_type(val)) {
    ta_data = buffer_get_typedarray_data(val);
  }

  if (ta_data) {
    return sc_clone_typed_array(js, val, ta_data, seen);
  }

  if (t == T_TYPEDARRAY) {
    return sc_clone_typed_array(js, val, (TypedArrayData *)js_gettypedarray(val), seen);
  }

  if (t == T_FUNC || t == T_CFUNC)
    return js_throw(js, make_dom_exception(js, "() => {} could not be cloned", "DataCloneError"));
  if (t == T_PROMISE || t == T_GENERATOR)
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));
  if (!is_object_type(val))
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));

  ant_value_t existing = sc_lookup(seen, val);
  if (vtype(existing) != T_UNDEF) return existing;

  ArrayBufferData *abd = buffer_get_arraybuffer_data(val);
  if (abd) {
    if (abd && !abd->is_detached) {
    if (transfer && sc_has(transfer, val)) {
      ArrayBufferData *new_abd = calloc(1, sizeof(ArrayBufferData));
      if (!new_abd) return js_mkerr(js, "out of memory");
      new_abd->data      = abd->data;
      new_abd->length    = abd->length;
      new_abd->capacity  = abd->capacity;
      new_abd->ref_count = 1;
      abd->data        = NULL;
      abd->length      = 0;
      abd->capacity    = 0;
      abd->is_detached = 1;
      js_set(js, val, "byteLength", js_mknum(0));
      ant_value_t clone = create_arraybuffer_obj(js, new_abd);
      sc_add(seen, val, clone);
      return clone;
    }
    ArrayBufferData *new_abd = create_array_buffer_data(abd->length);
    if (!new_abd) return js_mkerr(js, "out of memory");
    if (abd->length > 0) memcpy(new_abd->data, abd->data, abd->length);
    ant_value_t clone = create_arraybuffer_obj(js, new_abd);
    sc_add(seen, val, clone);
    return clone;
  }}

  if (buffer_is_dataview(val)) {
    DataViewData *dv = buffer_get_dataview_data(val);
    if (!dv || !dv->buffer)
      return js_throw(js, make_dom_exception(js, "DataView could not be cloned", "DataCloneError"));
      
    ArrayBufferData *new_buf = create_array_buffer_data(dv->buffer->length);
    if (!new_buf) return js_mkerr(js, "out of memory");
    if (dv->buffer->length > 0) memcpy(new_buf->data, dv->buffer->data, dv->buffer->length);

    ant_value_t ab_obj = create_arraybuffer_obj(js, new_buf);
    ant_value_t clone = create_dataview_with_buffer(
      js, new_buf, dv->byte_offset, dv->byte_length, ab_obj
    );
    
    if (is_err(clone)) {
      free_array_buffer_data(new_buf);
      return clone;
    }

    js_set(js, clone, "byteOffset", js_mknum((double)dv->byte_offset));
    js_set(js, clone, "byteLength", js_mknum((double)dv->byte_length));
    
    sc_add(seen, val, clone);
    free_array_buffer_data(new_buf);
    return clone;
  }

  if (t == T_ARR) {
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

  if (obj_ptr->type_tag == T_WEAKMAP || obj_ptr->type_tag == T_WEAKSET)
    return js_throw(js, make_dom_exception(js, "WeakMap/WeakSet could not be cloned", "DataCloneError"));

  if (obj_ptr->type_tag == T_MAP) {
    ant_value_t clone = js_mkobj(js);
    js_obj_ptr(clone)->type_tag = T_MAP;

    ant_value_t map_proto = js_get_ctor_proto(js, "Map", 3);
    if (is_special_object(map_proto)) js_set_proto_init(clone, map_proto);
    
    map_entry_t **new_head = ant_calloc(sizeof(map_entry_t *));
    if (!new_head) return js_mkerr(js, "out of memory");
    
    *new_head = NULL;
    js_set_slot(clone, SLOT_DATA, ANT_PTR(new_head));
    sc_add(seen, val, clone);
    
    map_entry_t **src_head = get_map_from_obj(val);
    if (src_head && *src_head) {
    map_entry_t *e, *tmp;
    HASH_ITER(hh, *src_head, e, tmp) {
      ant_value_t vc = sc_clone_rec(js, e->value, seen, transfer);
      if (is_err(vc)) return vc;
      
      map_entry_t *ne = ant_calloc(sizeof(map_entry_t));
      if (!ne) return js_mkerr(js, "out of memory");
      
      ne->key = (unsigned char *)strdup((const char *)e->key);
      ne->key_val = e->key_val;
      ne->value = vc;
      
      HASH_ADD_KEYPTR(
        hh, *new_head, ne->key, 
        (unsigned)strlen((const char *)ne->key), ne
      );
    }}
    
    return clone;
  }

  if (obj_ptr->type_tag == T_SET) {
    ant_value_t clone = js_mkobj(js);
    js_obj_ptr(clone)->type_tag = T_SET;
    
    ant_value_t set_proto = js_get_ctor_proto(js, "Set", 3);
    if (is_special_object(set_proto)) js_set_proto_init(clone, set_proto);
    
    set_entry_t **new_head = ant_calloc(sizeof(set_entry_t *));
    if (!new_head) return js_mkerr(js, "out of memory");
    *new_head = NULL;
    js_set_slot(clone, SLOT_DATA, ANT_PTR(new_head));
    sc_add(seen, val, clone);

    set_entry_t **src_head = get_set_from_obj(val);
    if (src_head && *src_head) {
    set_entry_t *e, *tmp;
    HASH_ITER(hh, *src_head, e, tmp) {
      ant_value_t vc = sc_clone_rec(js, e->value, seen, transfer);
      if (is_err(vc)) return vc;

      set_entry_t *ne = ant_calloc(sizeof(set_entry_t));
      if (!ne) return js_mkerr(js, "out of memory");
      
      ne->key = (unsigned char *)strdup((const char *)e->key);
      ne->value = vc;
      
      HASH_ADD_KEYPTR(
        hh, *new_head, ne->key,
        (unsigned)strlen((const char *)ne->key), ne
      );
    }}

    return clone;
  }

  if (js_get_slot(val, SLOT_ERROR_BRAND) == js_true) {
    ant_value_t clone = js_mkobj(js);
    sc_add(seen, val, clone);

    const char *msg   = get_str_prop(js, val, "message", 7, NULL);
    const char *name  = get_str_prop(js, val, "name",    4, NULL);
    const char *stack = get_str_prop(js, val, "stack",   5, NULL);

    if (msg)   js_set(js, clone, "message", js_mkstr(js, msg,   strlen(msg)));
    if (name)  js_set(js, clone, "name",    js_mkstr(js, name,  strlen(name)));
    if (stack) js_set(js, clone, "stack",   js_mkstr(js, stack, strlen(stack)));
    js_set_slot(clone, SLOT_ERROR_BRAND, js_true);

    ant_value_t err_type = js_get_slot(val, SLOT_ERR_TYPE);
    if (vtype(err_type) != T_UNDEF) js_set_slot(clone, SLOT_ERR_TYPE, err_type);

    return clone;
  }

  if (is_date_instance(val)) {
    ant_value_t clone = js_mkobj(js);
    ant_value_t date_proto = js_get_ctor_proto(js, "Date", 4);
    if (is_object_type(date_proto)) js_set_proto_init(clone, date_proto);
    
    js_set_slot(clone, SLOT_DATA, js_get_slot(val, SLOT_DATA));
    js_set_slot(clone, SLOT_BRAND, js_mknum(BRAND_DATE));
    sc_add(seen, val, clone);

    return clone;
  }

  blob_data_t *bd = js_is_prototype_of(js, g_blob_proto, val) ? blob_get_data(val) : NULL;
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
      js_set_proto_init(clone, g_file_proto);
    }
    return clone;
  }

  ant_value_t clone = js_mkobj(js);
  sc_add(seen, val, clone);

  ant_iter_t iter = js_prop_iter_begin(js, val);
  const char *key;
  size_t key_len;
  ant_value_t pval;

  while (js_prop_iter_next(&iter, &key, &key_len, &pval)) {
    ant_value_t pc = sc_clone_rec(js, pval, seen, transfer);
    if (is_err(pc)) { js_prop_iter_end(&iter); return pc; }
    js_set(js, clone, key, pc);
  }

  js_prop_iter_end(&iter);
  return clone;
}

ant_value_t js_structured_clone(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();

  sc_entry_t *transfer = NULL;

  if (nargs < 2 || !is_object_type(args[1])) goto clone;
  ant_value_t xfer_arr = js_get(js, args[1], "transfer");
  if (vtype(xfer_arr) != T_ARR) goto clone;

  ant_offset_t xfer_len = js_arr_len(js, xfer_arr);
  for (ant_offset_t i = 0; i < xfer_len; i++) {
    ant_value_t item = js_arr_get(js, xfer_arr, i);
    if (is_object_type(item)) sc_add(&transfer, item, js_true);
  }

clone:;

  sc_entry_t *seen = NULL;
  ant_value_t result = sc_clone_rec(js, args[0], &seen, &transfer);
  sc_free(&seen);
  sc_free(&transfer);
  return result;
}

void init_structured_clone_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  js_set(js, global, "structuredClone", js_mkfun(js_structured_clone));
  js_set_descriptor(js, global, "structuredClone", 15, JS_DESC_W | JS_DESC_C);
}
