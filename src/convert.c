#include <compat.h> // IWYU pragma: keep
#include <string.h>
#include <math.h>

#include "numbers.h"
#include "convert.h"
#include "props.h"
#include "proto.h"
#include "silver/call.h"
#include "modules/bigint.h"

const char *typestr(ant_value_type_t t) {
  static const char *names[] = {
    [kTypeUndefined] = "undefined",
    [kTypeNull] = "object",
    [kTypeBool] = "boolean",
    [kTypeNumber] = "number",
    [kTypeBigInt] = "bigint",
    [kTypeString] = "string",
    [kTypeSymbol] = "symbol",
    [kTypeObject] = "object",
    [kTypeArray] = "object",
    [kTypeFunction] = "function",
    [kTypeBuiltin] = "function",
    [kTypePromise] = "object",
    [kTypeGenerator] = "object",
    [kTypeTypedArray] = "typedarray",
    [kTypeError] = "err",
    [kTypeFunctionInfo] = "functioninfo",
    [kTypeSourceCode] = "sourcecode"
  };

  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

static inline bool is_primitive(ant_value_t v) {
  uint8_t t = vtype(v);
  return is_boxable_primitive_type(t) || t == kTypeNull || t == kTypeUndefined;
}

static inline bool ordinary_primitive_result(ant_value_t result) {
  return is_err(result) || is_primitive(result);
}

bool is_unboxed_obj(ant_t *js, ant_value_t val, ant_value_t expected_proto) {
  if (vtype(val) != kTypeObject) return false;
  if (vtype(js_get_slot(val, SLOT_PRIMITIVE)) != kTypeUndefined) return false;
  ant_value_t proto = js_get_slot(val, SLOT_PROTO);
  return vdata(proto) == vdata(expected_proto);
}

uint32_t js_to_uint32(double d) {
  if (d >= 0.0 && d < 4294967296.0) return (uint32_t)d;
  if (!isfinite(d)) return 0;
  
  double sign = (d < 0) ? -1.0 : 1.0;
  double posInt = sign * floor(fabs(d));
  double val = fmod(posInt, 4294967296.0);
  
  if (val < 0) val += 4294967296.0;
  return (uint32_t) val;
}

int32_t js_to_int32(double d) {
  uint32_t uint32 = js_to_uint32(d);
  if (uint32 >= 2147483648U) return (int32_t)(uint32 - 4294967296.0);
  return (int32_t) uint32;
}

ant_value_t unwrap_primitive(ant_t *js, ant_value_t val) {
  if (__builtin_expect(vtype(val) != kTypeObject, 1)) return val;
  ant_value_t prim = js_get_slot(val, SLOT_PRIMITIVE);
  if (__builtin_expect(vtype(prim) == kTypeUndefined, 1)) return val;
  return prim;
}

bool js_truthy(ant_t *js, ant_value_t v) {
  static const void *dispatch[] = {
    [kTypeObject]    = &&l_true,
    [kTypeFunction]  = &&l_true,
    [kTypeBuiltin]   = &&l_true,
    [kTypeArray]     = &&l_true,
    [kTypePromise]   = &&l_true,
    [kTypeGenerator] = &&l_true,
    [kTypeSymbol]    = &&l_true,
    [kTypeBool]      = &&l_bool,
    [kTypeString]    = &&l_str,
    [kTypeBigInt]    = &&l_bigint,
    [kTypeNumber]    = &&l_num,
  };

  uint8_t t = vtype(v);
  if (t < sizeof(dispatch) / sizeof(*dispatch) && dispatch[t])
    goto *dispatch[t];
  return false;

  l_true:   return true;
  l_bool:   return vdata(v) != 0;
  l_str:    return vstrlen(js, v) > 0;
  l_bigint: return !bigint_is_zero(js, v);
  l_num: {
    double d = Ant_Value_AsNumberUnchecked(v);
    return d != 0.0 && !isnan(d);
  }
}

size_t uint_to_str(char *buf, size_t bufsize, uint64_t val) {
  if (bufsize == 0) return 0;
  
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return 1;
  }
  
  char temp[24];
  size_t len = 0;
  
  while (val > 0 && len < sizeof(temp)) {
    temp[len++] = '0' + (val % 10);
    val /= 10;
  }
  
  if (len >= bufsize) len = bufsize - 1;
  for (size_t i = 0; i < len; i++) buf[i] = temp[len - 1 - i];
  buf[len] = '\0';
  
  return len;
}

size_t js_number_to_chars(ant_value_t value, char *buf, size_t len) {
  double dv = Ant_Value_AsNumberUnchecked(value);

  if (__builtin_expect(isnan(dv), 0)) 
    return cpy(buf, len, "NaN", 3);
    
  if (__builtin_expect(isinf(dv), 0)) 
    return cpy(buf, len, dv > 0 ? "Infinity" : "-Infinity", dv > 0 ? 8 : 9);

  return ant_number_to_shortest(dv, buf, len);
}

static inline ant_object_t *js_to_primitive_cache_object(ant_value_t value) {
  if (vtype(value) == kTypeFunction) return js_obj_ptr(js_func_obj(value));
  if (vtype(value) == kTypeObject || vtype(value) == kTypeArray) return js_obj_ptr(js_as_obj(value));
  return NULL;
}

static inline bool js_to_primitive_cache_hit(ant_t *js, ant_value_t value) {
  ant_object_t *obj = js_to_primitive_cache_object(value);
  const ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
  
  if (!obj || cache->ic_epoch != js->ic.epoch) return false;
  if (cache->object == obj) return true;
  
  return !obj->flags.is_exotic &&
    cache->shape == obj->shape &&
    cache->proto == ant_object_proto(obj);
}

static ant_value_t try_ordinary_to_primitive_number(ant_t *js, ant_value_t value) {
  ant_value_t result;
  if (js_try_invoke(js, value, "valueOf", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  if (js_try_invoke(js, value, "toString", NULL, 0, &result) &&  ordinary_primitive_result(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static ant_value_t try_ordinary_to_primitive_string(ant_t *js, ant_value_t value) {
  ant_value_t result;
  if (js_try_invoke(js, value, "toString", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  if (js_try_invoke(js, value, "valueOf", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static ant_value_t try_ordinary_to_primitive(ant_t *js, ant_value_t value, int hint) {
  return hint == 1
    ? try_ordinary_to_primitive_string(js, value)
    : try_ordinary_to_primitive_number(js, value);
}

static ant_value_t try_cached_ordinary_to_primitive_number(
  ant_t *js, ant_value_t value
) {
  const ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
  ant_object_t *obj = js_to_primitive_cache_object(value);
  
  uint32_t slot = cache->own_valueof_data_slot;
  ant_value_t result;

  if (obj && slot < obj->prop_count) {
    ant_value_t method = ant_object_prop_get_unchecked(obj, slot);
    if (is_callable(method)) {
      result = sv_call_value(js, method, value, NULL, 0);
      if (ordinary_primitive_result(result)) return result;
    }

    if (js_try_invoke(js, value, "toString", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
  }

  return try_ordinary_to_primitive_number(js, value);
}

double js_to_number(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == kTypeNull) return 0.0;
  
  if (vtype(arg) == kTypeSymbol) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a number");
    return ANT_NAN;
  }
  
  if (vtype(arg) == kTypeUndefined) return ANT_NAN;
  if (vtype(arg) == kTypeNumber) return Ant_Value_AsNumberUnchecked(arg);
  if (vtype(arg) == kTypeBool) return vdata(arg) ? 1.0 : 0.0;
  if (vtype(arg) == kTypeBigInt) return bigint_to_double(js, arg);

  if (vtype(arg) == kTypeString) {
    ant_flat_string_t *flat = ant_str_flat_ptr(arg);
    const char *base = NULL;
    const char *end = NULL;

    if (flat) {
      base = flat->bytes;
      end = base + flat->len;
    } else {
      ant_offset_t len = 0;
      ant_offset_t off = vstr(js, arg, &len);
      base = (const char *)(uintptr_t)off;
      end = base + len;
    }
    
    double val = ANT_NAN; if (
      ant_number_parse(base, (size_t)(end - base), 
      ANT_NUMBER_PARSE_JS_NUMBER, &val, NULL)
    ) return val;
    
    return ANT_NAN;
  }

  if (is_object_type(arg) || vtype(arg) == kTypeBuiltin) {
    ant_value_t primitive = js_to_primitive_cache_hit(js, arg)
      ? try_cached_ordinary_to_primitive_number(js, arg)
      : js_to_primitive(js, arg, 2);
    
    if (is_err(primitive)) return ANT_NAN;
    if (vtype(primitive) == kTypeNumber) return Ant_Value_AsNumberUnchecked(primitive);
    if (vtype(primitive) == kTypeBool) return vdata(primitive) ? 1.0 : 0.0;
    if (vtype(primitive) == kTypeNull) return 0.0;
    if (vtype(primitive) == kTypeUndefined) return ANT_NAN;
    if (vtype(primitive) == kTypeBigInt) return bigint_to_double(js, primitive);

    return js_to_number(js, primitive);
  }
  
  return ANT_NAN;
}

ant_value_t js_to_numeric(ant_t *js, ant_value_t arg) {
  uint8_t t = vtype(arg);
  if (t == kTypeNumber || t == kTypeBigInt) return arg;
  
  if (is_object_type(arg) || t == kTypeBuiltin) {
    arg = js_to_primitive_cache_hit(js, arg)
      ? try_cached_ordinary_to_primitive_number(js, arg)
      : js_to_primitive(js, arg, 2);
    if (is_err(arg)) return arg;
    t = vtype(arg);
    if (t == kTypeNumber || t == kTypeBigInt) return arg;
  }
  
  if (t == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a number");
  return Ant_Value_FromNumber(js_to_number(js, arg));
}

bool js_try_invoke(
  ant_t *js, ant_value_t obj, const char *method, 
  ant_args_t, ant_value_t *out_result
) {
  ant_value_t fn = js_getprop_fallback(js, obj, method);
  if (is_err(fn)) {
    *out_result = fn;
    return true;
  }

  if (!is_callable(fn)) return false;
  *out_result = sv_call_value(js, fn, obj, args, nargs);

  return true;
}

static bool js_try_exotic_to_primitive(
  ant_t *js, ant_value_t value, int hint,
  ant_value_t *out_result, bool *out_absent
) {
  *out_absent = false;
  ant_value_t tp_sym = js->sym.toPrimitive_sym;
  
  if (vtype(tp_sym) != kTypeSymbol) {
    *out_absent = true;
    return false;
  }

  ant_value_t tp_fn;
  if (!js_try_get_sym_with_receiver(js, value, tp_sym, value, &tp_fn)) {
    *out_absent = true;
    return false;
  }

  if (is_err(tp_fn)) {
    *out_result = tp_fn;
    return true;
  }
  
  if (vtype(tp_fn) == kTypeUndefined || vtype(tp_fn) == kTypeNull) return false;
  if (!is_callable(tp_fn)) {
    *out_result = js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.toPrimitive is not a function");
    return true;
  }
  
  const char *hint_str = hint == 1 ? "string" : (hint == 2 ? "number" : "default");
  ant_value_t hint_arg = js_mkstr(js, hint_str, strlen(hint_str));
  ant_value_t result = sv_vm_call(js->vm, js, tp_fn, value, &hint_arg, 1, NULL, Ant_Value_Undefined);

  *out_result = (is_err(result) || is_primitive(result))
    ? result
    : js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
  
  return true;
}

static bool js_guard_to_primitive_absence(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeFunction) value = js_func_obj(value);
  else if (vtype(value) != kTypeObject && vtype(value) != kTypeArray) return false;

  ant_value_t cur = js_as_obj(value);
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);

  while (is_object_type(cur)) {
    if (is_proxy(cur)) return false;
    ant_object_t *obj = js_obj_ptr(js_as_obj(cur));
    if (!obj) break;
    ant_object_guard_absence(obj);

    ant_value_t proto = get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = js_as_obj(proto);
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return true;
}

bool js_guard_proto_chain_absence(ant_t *js, ant_object_t *obj) {
  ant_value_t cur = ant_object_proto(obj);
  for (int depth = 0; is_object_type(cur); depth++) {
    if (depth >= MAX_PROTO_CHAIN_DEPTH || is_proxy(cur)) return false;
    ant_object_t *proto = js_obj_ptr(js_as_obj(cur));
    if (!proto || proto->flags.is_exotic) return false;
    ant_object_guard_absence(proto);
    cur = ant_object_proto(proto);
  }
  return vtype(cur) == kTypeNull;
}

ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint) {
  if (is_primitive(value)) return value;
  if (!is_object_type(value) && vtype(value) != kTypeBuiltin) return value;

  if (js_to_primitive_cache_hit(js, value))
    return try_ordinary_to_primitive(js, value, hint);

  ant_object_t *cache_obj = js_to_primitive_cache_object(value);
  bool cacheable = cache_obj && !cache_obj->flags.is_exotic;

  ant_value_t result;
  bool exotic_absent = false;
  if (js_try_exotic_to_primitive(js, value, hint, &result, &exotic_absent))
    return result;

  if (cacheable && exotic_absent && js_guard_to_primitive_absence(js, value)) {
    ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
    cache->object = cache_obj;
    cache->shape = cache_obj->shape;
    cache->proto = ant_object_proto(cache_obj);
    cache->own_valueof_data_slot = UINT32_MAX;

    ant_prop_loc_t valueof = lkp(js, value, "valueOf", 7);
    const ant_shape_prop_t *valueof_meta = prop_shape_meta(valueof);
    
    if (
      valueof.obj == cache_obj && valueof_meta &&
      !valueof_meta->has_getter && !valueof_meta->has_setter
    ) cache->own_valueof_data_slot = valueof.slot;
    cache->ic_epoch = js->ic.epoch;
  }
  
  return try_ordinary_to_primitive(js, value, hint);
}

bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == kTypeString) {
    ant_offset_t n1, n2, off1 = vstr(js, l, &n1), off2 = vstr(js, r, &n2);
    return n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
  }
  if (t == kTypeNumber) return Ant_Value_AsNumberUnchecked(l) == Ant_Value_AsNumberUnchecked(r);
  if (t == kTypeBigInt) return bigint_compare(js, l, r) == 0;
  return vdata(l) == vdata(r);
}

bool same_value_values(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == kTypeUndefined || t == kTypeNull) return true;
  if (t == kTypeNumber) {
    double dl = Ant_Value_AsNumberUnchecked(l);
    double dr = Ant_Value_AsNumberUnchecked(r);
    if (isnan(dl) && isnan(dr)) return true;
    if (dl == 0.0 && dr == 0.0) return !!signbit(dl) == !!signbit(dr);
    return dl == dr;
  }
  return strict_eq_values(js, l, r);
}

ant_value_t coerce_to_str(ant_t *js, ant_value_t v) {
  if (vtype(v) == kTypeString) return v;
  
  if (is_object_type(v) || vtype(v) == kTypeBuiltin) {
    ant_value_t prim = js_to_primitive(js, v, 1);
    if (is_err(prim)) return prim;
    if (vtype(prim) == kTypeString) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

ant_value_t coerce_to_str_hint(ant_t *js, ant_value_t v, int hint) {
  if (vtype(v) == kTypeString) return v;

  if (is_object_type(v) || vtype(v) == kTypeBuiltin) {
    v = js_to_primitive(js, v, hint);
    if (is_err(v) || vtype(v) == kTypeString) return v;
  }

  if (vtype(v) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a string");

  return js_tostring_val(js, v);
}

ant_value_t js_template_to_string(ant_t *js, ant_value_t v) {
  return coerce_to_str_hint(js, v, 1);
}

ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v) {
  return coerce_to_str_hint(js, v, 0);
}
