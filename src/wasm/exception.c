#include "wasm_internal.h"

static bool wasm_tag_kind_from_string(const char *name, size_t len, wasm_valkind_t *out) {
  bool ok = false;
  *out = wasm_valkind_from_string(name, len, &ok);
  if (ok) return true;
  if (len == 6 && !memcmp(name, "anyref", 6)) { *out = WASM_EXTERNREF; return true; }
  return false;
}

static bool wasm_is_tag(ant_value_t value) {
  return is_object_type(value) && js_check_brand(value, BRAND_WASM_TAG);
}

ant_value_t wasm_make_tag(ant_t *js, ant_value_t proto, ant_value_t kinds, bool js_tag) {
  ant_value_t tag = Ant_Object_CreateBare(js);
  if (is_err(tag)) return tag;
  js_set_proto_init(tag, proto);
  js_set_slot(tag, SLOT_BRAND, Ant_Value_FromNumber(BRAND_WASM_TAG));
  js_set_slot_wb(js, tag, SLOT_DATA, kinds);
  if (js_tag) js_set_slot(tag, SLOT_AUX, Ant_Value_True);
  return tag;
}

ant_value_t js_wasm_tag_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Tag constructor requires 'new'");
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Tag(): Argument 0 must be a tag type");

  GC_ROOT_SAVE(mark, js);
  ant_value_t params = js_get(js, args[0], "parameters");
  
  GC_ROOT_PIN(js, params);
  ant_value_t list = is_err(params) ? params : wasm_sequence_to_list(js, params, "WebAssembly.Tag(): Argument 0 parameters must be iterable");
  
  GC_ROOT_PIN(js, list);
  if (is_err(list)) { GC_ROOT_RESTORE(js, mark); return list; }

  ant_value_t kinds = Ant_Array_Create(js);
  GC_ROOT_PIN(js, kinds);
  
  ant_offset_t count = js_arr_len(js, list);
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t name = js_tostring_val(js, js_arr_get(js, list, i));
    if (is_err(name)) { GC_ROOT_RESTORE(js, mark); return name; }
    size_t len = 0;
    const char *str = js_getstr(js, name, &len);
    wasm_valkind_t kind;
    if (!str || !wasm_tag_kind_from_string(str, len, &kind)) {
      GC_ROOT_RESTORE(js, mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Tag(): Argument 0 parameter type at index #%u must be a value type", (unsigned)i);
    }
    js_arr_push(js, kinds, Ant_Value_FromNumber((double)kind));
  }

  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.wasm_tag_proto, call_new_target);
  ant_value_t tag = wasm_make_tag(js, proto, kinds, false);
  GC_ROOT_RESTORE(js, mark);
  
  return tag;
}

static ant_value_t wasm_exception_value(ant_t *js, wasm_valkind_t kind, ant_value_t value) {
  switch (kind) {
    case WASM_I32: {
      double d = js_to_number(js, value);
      return Ant_Value_FromNumber((double)js_to_int32(d));
    }
    case WASM_I64: {
      int64_t out = 0;
      if (vtype(value) != kTypeBigInt || !bigint_to_int64_wrapping(js, value, &out)) {
        ant_value_t text = js_tostring_val(js, value);
        size_t len = 0;
        const char *str = is_err(text) ? NULL : js_getstr(js, text, &len);
        return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert %.*s to a BigInt", (int)len, str ? str : "value");
      }
      return wasm_value_from_i64(js, out);
    }
    case WASM_F32: return Ant_Value_FromNumber((double)(float)js_to_number(js, value));
    case WASM_F64: return Ant_Value_FromNumber(js_to_number(js, value));
    case WASM_FUNCREF: {
      wasm_val_t tmp;
      if (!js_value_to_wasm(js, value, WASM_FUNCREF, &tmp))
        return js_mkerr_typed(js, JS_ERR_TYPE, "type incompatibility when transforming from/to JS");
      return value;
    }
    default: return value;
  }
}

ant_value_t js_wasm_exception_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception constructor requires 'new'");
  if (nargs < 1 || !wasm_is_tag(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception(): Argument 0 must be a WebAssembly tag");
  if (js_get_slot(args[0], SLOT_AUX) == Ant_Value_True)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception(): Argument 0 cannot be WebAssembly.JSTag");

  GC_ROOT_SAVE(mark, js);
  ant_value_t kinds = js_get_slot(args[0], SLOT_DATA);
  ant_value_t list = wasm_sequence_to_list(js, nargs > 1 ? args[1] : Ant_Value_Undefined, "WebAssembly.Exception(): Argument 1 must be iterable");
  GC_ROOT_PIN(js, list);
  if (is_err(list)) { GC_ROOT_RESTORE(js, mark); return list; }

  ant_offset_t count = js_arr_len(js, kinds);
  if (js_arr_len(js, list) != count) {
    GC_ROOT_RESTORE(js, mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception(): Number of exception values does not match signature length");
  }

  ant_value_t payload = Ant_Array_Create(js);
  GC_ROOT_PIN(js, payload);
  for (ant_offset_t i = 0; i < count; i++) {
    wasm_valkind_t kind = (wasm_valkind_t)Ant_Value_AsNumberUnchecked(js_arr_get(js, kinds, i));
    ant_value_t value = wasm_exception_value(js, kind, js_arr_get(js, list, i));
    if (is_err(value)) { GC_ROOT_RESTORE(js, mark); return value; }
    js_arr_push(js, payload, value);
  }

  ant_value_t stack = Ant_Value_Undefined;
  if (nargs > 2 && is_object_type(args[2])) {
    ant_value_t trace = js_get(js, args[2], "traceStack");
    if (is_err(trace)) { GC_ROOT_RESTORE(js, mark); return trace; }
    if (js_truthy(js, trace)) {
      ant_value_t holder = Ant_Error_Create(js, JS_ERR_GENERIC, "");
      GC_ROOT_PIN(js, holder);
      stack = is_object_type(holder) ? js_get(js, holder, "stack") : Ant_Value_Undefined;
      if (is_err(stack)) stack = Ant_Value_Undefined;
    }
  }
  GC_ROOT_PIN(js, stack);

  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.wasm_exception_proto, call_new_target);
  ant_value_t exception = Ant_Object_CreateBare(js);
  if (!is_err(exception)) {
    js_set_proto_init(exception, proto);
    js_set_slot(exception, SLOT_BRAND, Ant_Value_FromNumber(BRAND_WASM_EXCEPTION));
    js_set_slot_wb(js, exception, SLOT_DATA, args[0]);
    js_set_slot_wb(js, exception, SLOT_AUX, payload);
    js_set_slot_wb(js, exception, SLOT_ENTRIES, stack);
  }
  GC_ROOT_RESTORE(js, mark);
  return exception;
}

static bool wasm_is_exception(ant_value_t value) {
  return is_object_type(value) && js_check_brand(value, BRAND_WASM_EXCEPTION);
}

ant_value_t js_wasm_exception_get_arg(ant_params_t) {
  if (!wasm_is_exception(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Receiver is not a WebAssembly.Exception");
  if (nargs < 1 || !wasm_is_tag(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception.getArg(): Argument 0 must be a WebAssembly.Tag");
  if (vdata(js_get_slot(call_this, SLOT_DATA)) != vdata(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception.getArg(): First argument does not match the exception tag");

  double index = nargs > 1 ? js_to_number(js, args[1]) : 0;
  ant_value_t payload = js_get_slot(call_this, SLOT_AUX);
  ant_offset_t count = js_arr_len(js, payload);
  if (!isfinite(index) || index < 0 || index >= (double)count || index != floor(index))
    return js_mkerr_typed(js, JS_ERR_RANGE, "WebAssembly.Exception.getArg(): Index out of range");

  return js_arr_get(js, payload, (ant_offset_t)index);
}

ant_value_t js_wasm_exception_is(ant_params_t) {
  if (!wasm_is_exception(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Receiver is not a WebAssembly.Exception");
  if (nargs < 1 || !wasm_is_tag(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Exception.is(): Argument 0 must be a WebAssembly.Tag");
  return Ant_Value_FromBool(vdata(js_get_slot(call_this, SLOT_DATA)) == vdata(args[0]));
}

ant_value_t js_wasm_exception_stack_getter(ant_params_t) {
  if (!wasm_is_exception(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Receiver is not a WebAssembly.Exception");
  return js_get_slot(call_this, SLOT_ENTRIES);
}
