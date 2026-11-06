#include <compat.h> // IWYU pragma: keep

#include "cfunc.h"

#include "modules/boolean.h"
#include "proto.h"

static ant_value_t builtin_Boolean(ant_params_t) {
  ant_value_t bval = mkval(kTypeBool, nargs > 0 && js_truthy(js, args[0]) ? 1 : 0);
  if (is_wrapper_ctor_target(js, call_this, call_new_target))
    js_set_slot(call_this, SLOT_PRIMITIVE, bval);
  return bval;
}

static ant_value_t builtin_boolean_valueOf(ant_params_t) {
  ant_value_t b = unwrap_primitive(js, call_this);
  if (vtype(b) != kTypeBool) return js_mkerr(js, "valueOf called on non-boolean");
  return b;
}

static ant_value_t builtin_boolean_toString(ant_params_t) {
  ant_value_t b = unwrap_primitive(js, call_this);
  if (vtype(b) != kTypeBool) return js_mkerr(js, "toString called on non-boolean");
  return vdata(b) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
}

void js_boolean_init_prototype(ant_t *js, ant_value_t boolean_proto) {
  js_set_slot(boolean_proto, SLOT_PRIMITIVE, Ant_Value_False);
  defmethod(js, boolean_proto, "valueOf", 7, Ant_Function_Create(builtin_boolean_valueOf));
  defmethod(js, boolean_proto, "toString", 8, Ant_Function_Create(builtin_boolean_toString));
}

void js_boolean_init_constructor(ant_t *js, ant_value_t bool_ctor_obj, ant_value_t boolean_proto) {
  js_set_slot(bool_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Boolean, 1));
  js_setprop_readonly_nonconfigurable(js, bool_ctor_obj, "prototype", 9, boolean_proto);
  js_set_function_name(js, bool_ctor_obj, "Boolean", sizeof("Boolean") - 1);
}
