#include "internal.h"
#include "errors.h"
#include "gc/roots.h"
#include "modules/proxy.h"
#include "silver/call.h"
#include "silver/glue.h"

#include "ops/property.h"
#include "ops/coercion.h"

static ant_value_t Super_Base(ant_t *js, sv_func_t *func, ant_value_t home, ant_value_t receiver) {
  return vtype(home) == kTypeFunction && vtype(receiver) != kTypeFunction && func && !func->is_static ? js_getprop_fallback(js, home, "prototype") : js_get_proto(js, home);
}

ant_value_t Silver_Super_Get(
  ant_t *js, sv_func_t *func, ant_value_t receiver,
  ant_value_t home, ant_value_t prop, ant_value_t *roots
) {
  ant_value_t base = roots[0] = Super_Base(js, func, home, receiver);
  if (is_err(base)) return base;
  if (vtype(prop) == kTypeSymbol) {
    if (!is_object_type(base)) return sv_mk_nullish_read_error_by_key(js, NULL, base, prop);
    return js_get_sym_with_receiver(js, base, prop, receiver);
  }
  
  ant_value_t key = roots[1] = coerce_to_str(js, prop);
  if (is_err(key)) return key;
  if (!is_object_type(base)) return sv_mk_nullish_read_error_by_key(js, NULL, base, key);
  
  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key, &klen);
  return js_getprop_super(js, base, receiver, (const char *)(uintptr_t)koff);
}

ant_value_t Silver_Super_Put(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home,
  ant_value_t prop, ant_value_t val, ant_value_t *roots
) {
  ant_value_t base = roots[0] = Super_Base(js, func, home, receiver);
  if (is_err(base)) return base;
  if (!is_object_type(base)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set properties of %s", vtype(base) == kTypeNull ? "null" : "undefined");
  
  ant_value_t key = roots[1] = vtype(prop) == kTypeSymbol ? prop : coerce_to_str(js, prop);
  if (is_err(key)) return key;
  
  ant_value_t stored = js_set_with_receiver(js, base, key, val, receiver);
  
  if (is_err(stored)) return stored;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (stored == Ant_Value_False && func && func->is_strict) return Ant_Error_CreateReadOnlyProperty(js, receiver, key);
  
  return val;
}

