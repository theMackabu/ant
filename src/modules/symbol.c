#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "silver/call.h"
#include "modules/symbol.h"
#include "descriptors.h"
#include "gc/modules.h"

static ant_value_t builtin_Symbol(ant_params_t) {
  if (vtype(call_new_target) != kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol is not a constructor");

  if (nargs == 0 || vtype(args[0]) == kTypeUndefined) return js_mksym(js, NULL);
  
  ant_value_t desc = js_template_to_string(js, args[0]);
  if (is_err(desc)) return desc;
  
  return js_mksym(js, js_getstr(js, desc, NULL));
}

static ant_value_t builtin_Symbol_for(ant_params_t) {
  ant_value_t key_str = js_template_to_string(js, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(key_str)) return key_str;
  
  char *key = js_getstr(js, key_str, NULL);
  if (!key) return js_mkerr(js, "Invalid key");
  
  return js_mksym_for(js, key);
}

static ant_value_t builtin_Symbol_keyFor(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeSymbol) return js_mkundef();
  
  const char *key = js_sym_key(args[0]);
  if (!key) return js_mkundef();
  
  return js_mkstr(js, key, strlen(key));
}

static ant_value_t builtin_Symbol_toString(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  
  if (vtype(this_val) != kTypeSymbol && is_object_type(this_val)) {
    ant_value_t prim = js_get_slot(this_val, SLOT_PRIMITIVE);
    if (vtype(prim) == kTypeSymbol) this_val = prim;
  }

  if (vtype(this_val) != kTypeSymbol)
    return js_mkerr(js, "Symbol.prototype.toString requires a symbol");
  
  return js_symbol_to_string(js, this_val);
}

static ant_value_t builtin_Symbol_valueOf(ant_params_t) {
  ant_value_t this_val = js_getthis(js);

  if (vtype(this_val) != kTypeSymbol && is_object_type(this_val)) {
    ant_value_t prim = js_get_slot(this_val, SLOT_PRIMITIVE);
    if (vtype(prim) == kTypeSymbol) return prim;
  }

  if (vtype(this_val) == kTypeSymbol) return this_val;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.prototype.valueOf requires a symbol");
}

static ant_value_t builtin_Symbol_description(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t sym = this_val;

  if (vtype(sym) != kTypeSymbol && is_object_type(sym)) {
    ant_value_t prim = js_get_slot(sym, SLOT_PRIMITIVE);
    if (vtype(prim) == kTypeSymbol) sym = prim;
  }

  if (vtype(sym) != kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.prototype.description requires a symbol");

  return js_symbol_description_value(js, sym);
}

bool js_is_symbol_description_getter(ant_value_t getter) {
  return 
    vtype(getter) == kTypeBuiltin && 
    js_cfunc_same_entrypoint(getter, builtin_Symbol_description);
}

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target,
  ant_value_t sym,
  ant_value_t this_arg, ant_value_t *args,
  int nargs, bool *called
) {
  *called = false;
  if (vtype(sym) != kTypeSymbol || !is_object_type(target)) return js_mkundef();

  ant_value_t method = js_get_sym(js, target, sym);
  if (is_err(method)) return method;

  uint8_t mt = vtype(method);
  if (mt == kTypeUndefined || mt == kTypeNull) return js_mkundef();
  if (!is_callable(method)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol method is not callable");
  }

  *called = true;
  return sv_vm_call(js->vm, js, method, this_arg, args, nargs, NULL, js_mkundef());
}

void js_define_species_getter(ant_t *js, ant_value_t ctor) {
  if (!is_object_type(ctor) || vtype(js->sym.species_sym) != kTypeSymbol) return;
  ctor = js_as_obj(ctor);
  js_set_sym_getter_desc(js, ctor, js->sym.species_sym, Ant_Function_Create(sym_this_cb), JS_DESC_C);
}

void init_symbol_module(ant_t *js) {
  #define ANT_SYMBOL(name, desc) js->sym.name##_sym = js_mksym_well_known(js, desc);
  #include "symbol_list.h"
  init_symbol_ctor(js);
}

void init_symbol_ctor(ant_t *js) {
  ant_value_t proto = Ant_Object_CreateBare(js);
  js->sym.symbol_proto = proto;
  js_set_proto_init(proto, js->sym.object_proto);
  
  defmethod(js, proto, "toString", 8, Ant_Function_Create(builtin_Symbol_toString));
  defmethod(js, proto, "valueOf", 7, Ant_Function_Create(builtin_Symbol_valueOf));
  
  mkprop(js, proto, js->sym.toPrimitive_sym, Ant_Function_CreateWithMeta(builtin_Symbol_valueOf, "[Symbol.toPrimitive]", 1, 0), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, proto, js->sym.toStringTag_sym, ANT_STRING("Symbol"), ANT_PROP_ATTR_CONFIGURABLE);
  js_set_getter_desc(js, proto, "description", 11, Ant_Function_Create(builtin_Symbol_description), JS_DESC_C);

  ant_value_t ctor = Ant_Object_CreateBare(js);
  js_set_slot(ctor, SLOT_CFUNC, Ant_Function_Create(builtin_Symbol));
  js_set_function_name(js, ctor, "Symbol", 6);
  
  defmethod(js, ctor, "for", 3, Ant_Function_CreateWithArity(builtin_Symbol_for, 1));
  defmethod(js, ctor, "keyFor", 6, Ant_Function_CreateWithArity(builtin_Symbol_keyFor, 1));
  
  js_set(js, ctor, "prototype", proto);
  js_set_descriptor(js, ctor, "prototype", 9, 0);

  #define ANT_SYMBOL(name, _desc)                \
    js_set(js, ctor, #name, js->sym.name##_sym); \
    js_set_descriptor(js, ctor, #name, sizeof(#name) - 1, 0);
  #include "symbol_list.h"

  ant_value_t fn = js_obj_to_func(js, ctor);
  defmethod(js, proto, "constructor", 11, fn);
  js_set_global_builtin(js, "Symbol", fn);
}

void gc_mark_symbols(ant_t *js, gc_mark_fn mark) {
  #define ANT_SYMBOL(name, _desc) mark(js, js->sym.name##_sym);
  #include "symbol_list.h"
}
