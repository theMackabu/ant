#include "internal.h"
#include "primordials.h"
#include "gc/roots.h"
#include "silver/engine.h"

typedef enum {
  ARRAY_PROTO,
  FUNCTION_PROTO,
  STRING_PROTO,
  ARRAY, JSON, GLOBAL
} primordial_owner_t;

static const struct {
  const char *name;
  primordial_owner_t owner;
  const char *property;
  bool uncurry;
} captures[] = {
#define PRIMORDIAL_DEF(name, owner, property, uncurry) {#name, owner, property, uncurry},
#include "primordial_list.h"
};

ant_value_t ant_capture_primordials(ant_t *js) {
  ant_value_t global = js_glob(js);
  
  ant_value_t owners[] = {
    [ARRAY_PROTO] = js->sym.array_proto,
    [FUNCTION_PROTO] = js->sym.function_proto,
    [STRING_PROTO] = js->sym.string_proto,
    [ARRAY] = js_get(js, global, "Array"),
    [JSON] = js_get(js, global, "JSON"),
    [GLOBAL] = global,
  };
  
  if (js->thrown_exists) return mkval(kTypeError, 0);
  for (size_t i = 0; i < ANT_PRIMORDIAL_EXPORT_COUNT; i++) {
    ant_value_t value = js_get(js, owners[captures[i].owner], captures[i].property);
    if (is_err(value)) return value;
    js->primordial_values[i] = value;
  }
  
  ant_value_t call = js_get(js, js->sym.function_proto, "call");
  if (is_err(call)) return call;
  js->primordial_values[ANT_PRIMORDIAL_CALL] = call;
  
  return js_mkundef();
}

ant_value_t primordial_library(ant_t *js) {
  if (!is_undefined(js->primordials)) return js->primordials;

  GC_ROOT_SAVE(mark, js);
  ant_value_t result = js_mkobj(js);
  
  GC_ROOT_PIN(js, result);
  ant_value_t captured = js_mkundef();
  
  GC_ROOT_PIN(js, captured);
  if (is_err(result)) goto done;
  js_set_proto_init(result, js_mknull());

  for (size_t i = 0; i < ANT_PRIMORDIAL_EXPORT_COUNT; i++) {
    captured = js->primordial_values[i];
    if (captures[i].uncurry) {
      ant_value_t target = captured;
      captured = sv_vm_call_explicit_this(
        js->vm, js, js->primordial_values[ANT_PRIMORDIAL_FunctionPrototypeBind],
        js->primordial_values[ANT_PRIMORDIAL_CALL], &target, 1);
      if (is_err(captured)) { result = captured; goto done; }
    }
    js_set(js, result, captures[i].name, captured);
    if (js->thrown_exists) { result = mkval(kTypeError, 0); goto done; }
  }

  captured = builtin_object_freeze(js, &result, 1);
  if (is_err(captured)) { result = captured; goto done; }
  js->primordials = result;
done:
  GC_ROOT_RESTORE(js, mark);
  return result;
}
