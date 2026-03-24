#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"

#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/collections.h"
#include "modules/domexception.h"
#include "modules/globals.h"

ant_value_t js_report_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "reportError requires 1 argument");
  
  ant_value_t error  = args[0];
  ant_value_t global = js_glob(js);

  const char *msg = "";
  uint8_t et = vtype(error);
  
  if (et == T_STR) {
    msg = js_str(js, error);
  } else if (et == T_OBJ) {
    const char *m = get_str_prop(js, error, "message", 7, NULL);
    if (m && *m) msg = m;
  }
  
  if (!*msg) {
    ant_value_t str = js_tostring_val(js, error);
    if (vtype(str) == T_STR) { const char *s = js_str(js, str); if (s) msg = s; }
  }

  const char *filename = "";
  int lineno = 1, colno = 1;
  js_get_call_location(js, &filename, &lineno, &colno);
  if (!filename) filename = "";

  ant_value_t loc = js_get(js, global, "location");
  if (vtype(loc) == T_OBJ) {
    const char *href = get_str_prop(js, loc, "href", 4, NULL);
    if (href && *href) filename = href;
  }

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",     js_mkstr(js, "error", 5));
  js_set(js, event, "message",  js_mkstr(js, msg, strlen(msg)));
  js_set(js, event, "filename", js_mkstr(js, filename, strlen(filename)));
  js_set(js, event, "lineno",   js_mknum(lineno));
  js_set(js, event, "colno",    js_mknum(colno));
  js_set(js, event, "error",    error);

  js_dispatch_global_event(js, event);
  ant_value_t handler = js_get(js, global, "onerror");
  
  uint8_t ht = vtype(handler);
  if (ht == T_FUNC || ht == T_CFUNC) {
    ant_value_t msg_str  = js_mkstr(js, msg, strlen(msg));
    ant_value_t file_str = js_mkstr(js, filename, strlen(filename));
    ant_value_t call_args[5] = { msg_str, file_str, js_mknum(lineno), js_mknum(colno), error };
    sv_vm_call(js->vm, js, handler, global, call_args, 5, NULL, false);
  }

  return js_mkundef();
}

bool js_fire_unhandled_rejection(ant_t *js, ant_value_t promise_val, ant_value_t reason) {
  ant_value_t global  = js_glob(js);
  ant_value_t handler = js_get(js, global, "onunhandledrejection");
  
  uint8_t ht = vtype(handler);
  if (ht != T_FUNC && ht != T_CFUNC) return false;

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",    js_mkstr(js, "unhandledrejection", 18));
  js_set(js, event, "reason",  reason);
  js_set(js, event, "promise", promise_val);
  
  ant_value_t call_args[1] = { event };
  sv_vm_call(js->vm, js, handler, global, call_args, 1, NULL, false);
  
  return true;
}

void js_fire_rejection_handled(ant_t *js, ant_value_t promise_val, ant_value_t reason) {
  ant_value_t global  = js_glob(js);
  ant_value_t handler = js_get(js, global, "onrejectionhandled");
  
  uint8_t ht = vtype(handler);
  if (ht != T_FUNC && ht != T_CFUNC) return;

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",    js_mkstr(js, "rejectionhandled", 16));
  js_set(js, event, "reason",  reason);
  js_set(js, event, "promise", promise_val);
  
  ant_value_t call_args[1] = { event };
  sv_vm_call(js->vm, js, handler, global, call_args, 1, NULL, false);
}

static void sc_add_seen(ant_t *js, ant_value_t seen, ant_value_t val, ant_value_t clone) {
  char key[24];
  snprintf(key, sizeof(key), "%" PRIxPTR, (uintptr_t)vdata(val));
  js_set(js, seen, key, clone);
}

static ant_value_t sc_lookup_seen(ant_t *js, ant_value_t seen, ant_value_t val) {
  char key[24];
  snprintf(key, sizeof(key), "%" PRIxPTR, (uintptr_t)vdata(val));
  ant_value_t found = js_get(js, seen, key);
  return (vtype(found) == T_UNDEF) ? js_mkundef() : found;
}

static const char *sc_ta_type_name(TypedArrayType type) {
  static const char *names[] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray",
    "Int16Array", "Uint16Array",
    "Int32Array", "Uint32Array",
    "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array"
  };
  int i = (int)type;
  if (i < 0 || i > 10) return "Uint8Array";
  return names[i];
}

static ant_value_t sc_clone_rec(ant_t *js, ant_value_t val, ant_value_t seen) {
  uint8_t t = vtype(val);

  if (t == T_UNDEF || t == T_NULL || t == T_BOOL || t == T_NUM || t == T_BIGINT || t == T_STR) return val;
  if (t == T_SYMBOL) return js_throw(js, make_dom_exception(js, "Symbol cannot be serialized", "DataCloneError"));

  if (t == T_TYPEDARRAY) {
    ant_value_t existing = sc_lookup_seen(js, seen, val);
    if (vtype(existing) != T_UNDEF) return existing;

    TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(val);
    if (!ta || !ta->buffer)
      return js_throw(js, make_dom_exception(js, "TypedArray could not be cloned", "DataCloneError"));

    ArrayBufferData *new_buf = create_array_buffer_data(ta->byte_length);
    if (!new_buf) return js_mkerr(js, "out of memory");
    if (ta->byte_length > 0) memcpy(new_buf->data, ta->buffer->data + ta->byte_offset, ta->byte_length);

    ant_value_t clone = create_typed_array(js, ta->type, new_buf, 0, ta->length, sc_ta_type_name(ta->type));
    sc_add_seen(js, seen, val, clone);
    
    return clone;
  }

  if (t == T_FUNC || t == T_CFUNC || t == T_CLOSURE) 
    return js_throw(js, make_dom_exception(js, "() => {} could not be cloned", "DataCloneError"));
  if (t == T_PROMISE || t == T_GENERATOR) 
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));
  if (!is_object_type(val)) 
    return js_throw(js, make_dom_exception(js, "Value could not be cloned", "DataCloneError"));

  ant_value_t existing = sc_lookup_seen(js, seen, val);
  if (vtype(existing) != T_UNDEF) return existing;

  if (t == T_ARR) {
    ant_value_t clone = js_mkarr(js);
    sc_add_seen(js, seen, val, clone);

    ant_offset_t len = js_arr_len(js, val);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t ic = sc_clone_rec(js, js_arr_get(js, val, i), seen);
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
    sc_add_seen(js, seen, val, clone);

    map_entry_t **src_head = get_map_from_obj(js, val);
    if (src_head && *src_head) {
    map_entry_t *e, *tmp;
    HASH_ITER(hh, *src_head, e, tmp) {
      ant_value_t vc = sc_clone_rec(js, e->value, seen);
      if (is_err(vc)) return vc;
      
      map_entry_t *ne = ant_calloc(sizeof(map_entry_t));
      if (!ne) return js_mkerr(js, "out of memory");
      ne->key   = strdup(e->key);
      ne->value = vc;
      HASH_ADD_STR(*new_head, key, ne);
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
    sc_add_seen(js, seen, val, clone);

    set_entry_t **src_head = get_set_from_obj(js, val);
    if (src_head && *src_head) {
    set_entry_t *e, *tmp;
    HASH_ITER(hh, *src_head, e, tmp) {
      ant_value_t vc = sc_clone_rec(js, e->value, seen);
      if (is_err(vc)) return vc;
      
      set_entry_t *ne = ant_calloc(sizeof(set_entry_t));
      if (!ne) return js_mkerr(js, "out of memory");
      ne->key   = strdup(e->key);
      ne->value = vc;
      HASH_ADD_STR(*new_head, key, ne);
    }}
    
    return clone;
  }

  if (js_get_slot(val, SLOT_ERROR_BRAND) == js_true) {
    ant_value_t clone = js_mkobj(js);
    sc_add_seen(js, seen, val, clone);

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

  ant_value_t date_proto = js_get_ctor_proto(js, "Date", 4);
  if (
    vtype(date_proto) != T_UNDEF 
    && is_object_type(date_proto)
    && js_is_prototype_of(js, date_proto, val)
  ) {
    ant_value_t clone = js_mkobj(js);
    js_set_proto_init(clone, date_proto);
    js_set_slot(clone, SLOT_DATA, js_get_slot(val, SLOT_DATA));
    sc_add_seen(js, seen, val, clone);
    
    return clone;
  }

  ant_value_t clone = js_mkobj(js);
  sc_add_seen(js, seen, val, clone);

  ant_iter_t iter = js_prop_iter_begin(js, val);
  const char *key;
  size_t key_len;
  ant_value_t pval;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &pval)) {
    ant_value_t pc = sc_clone_rec(js, pval, seen);
    if (is_err(pc)) { js_prop_iter_end(&iter); return pc; }
    js_set(js, clone, key, pc);
  }
  
  js_prop_iter_end(&iter);
  return clone;
}

ant_value_t js_structured_clone(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  // TODO: transfer option (args[1]) accepted but not yet implemented
  ant_value_t seen = js_mkobj(js);
  return sc_clone_rec(js, args[0], seen);
}

void init_globals_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  js_set(js, global, "reportError", js_mkfun(js_report_error));
  js_set_descriptor(js, global, "reportError", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "structuredClone", js_mkfun(js_structured_clone));
  js_set_descriptor(js, global, "structuredClone", 15, JS_DESC_W | JS_DESC_C);
}
