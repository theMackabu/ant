#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/symbol.h"
#include "streams/queuing.h"

static ant_value_t g_count_qs_proto;
static ant_value_t g_bytelength_qs_proto;

static ant_value_t js_count_size(ant_t *js, ant_value_t *args, int nargs) {
  return js_mknum(1);
}

static ant_value_t js_bytelength_size(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) == T_UNDEF || is_null(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot get property 'byteLength' of undefined or null");
  if (!is_object_type(args[0])) return js_mkundef();
  
  ant_value_t bl = js_get(js, args[0], "byteLength");
  if (is_err(bl)) return bl;
  if (vtype(bl) == T_NUM) return bl;
  
  return js_mkundef();
}

static ant_value_t js_qs_get_highwatermark(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t s = js_get_slot(js->this_val, SLOT_DATA);
  if (vtype(s) == T_NUM) return s;
  return js_mkundef();
}

static ant_value_t js_qs_get_size(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t proto = js_get_proto(js, js->this_val);
  return js_get_slot(proto, SLOT_DATA);
}

static ant_value_t qs_ctor(ant_t *js, ant_value_t *args, int nargs, ant_value_t proto, const char *name) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s constructor requires 'new'", name);

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || is_null(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct '%s': 1 argument required", name);

  if (!is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct '%s': argument is not an object", name);

  ant_value_t hv = js_get(js, args[0], "highWaterMark");
  if (is_err(hv)) return hv;
  if (vtype(hv) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct '%s': member highWaterMark is required", name);

  double hwm = js_to_number(js, hv);

  ant_value_t obj = js_mkobj(js);
  ant_value_t resolved = js_instance_proto_from_new_target(js, proto);
  
  if (is_object_type(resolved)) js_set_proto_init(obj, resolved);
  js_set_slot(obj, SLOT_DATA, js_mknum(hwm));

  return obj;
}

static ant_value_t js_count_qs_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return qs_ctor(js, args, nargs, g_count_qs_proto, "CountQueuingStrategy");
}

static ant_value_t js_bytelength_qs_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return qs_ctor(js, args, nargs, g_bytelength_qs_proto, "ByteLengthQueuingStrategy");
}

static ant_value_t make_size_fn(ant_t *js, ant_cfunc_t cfunc, int length) {
  ant_value_t obj = js_mkobj(js);
  
  js_set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(cfunc));
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, "size", 4));
  js_set_descriptor(js, obj, "name", 4, 0);
  js_mkprop_fast(js, obj, "length", 6, js_mknum(length));
  js_set_descriptor(js, obj, "length", 6, 0);
  
  return js_obj_to_func(obj);
}

void init_queuing_strategies_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_count_qs_proto = js_mkobj(js);
  js_set_slot(g_count_qs_proto, SLOT_DATA, make_size_fn(js, js_count_size, 0));
  js_set_getter_desc(js, g_count_qs_proto, "highWaterMark", 13, js_mkfun(js_qs_get_highwatermark), JS_DESC_C);
  js_set_getter_desc(js, g_count_qs_proto, "size", 4, js_mkfun(js_qs_get_size), JS_DESC_C);
  js_set_sym(js, g_count_qs_proto, get_toStringTag_sym(), js_mkstr(js, "CountQueuingStrategy", 20));

  ant_value_t cqs_ctor = js_make_ctor(js, js_count_qs_ctor, g_count_qs_proto, "CountQueuingStrategy", 20);
  js_set(js, g, "CountQueuingStrategy", cqs_ctor);
  js_set_descriptor(js, g, "CountQueuingStrategy", 20, JS_DESC_W | JS_DESC_C);

  g_bytelength_qs_proto = js_mkobj(js);
  js_set_slot(g_bytelength_qs_proto, SLOT_DATA, make_size_fn(js, js_bytelength_size, 1));
  js_set_getter_desc(js, g_bytelength_qs_proto, "highWaterMark", 13, js_mkfun(js_qs_get_highwatermark), JS_DESC_C);
  js_set_getter_desc(js, g_bytelength_qs_proto, "size", 4, js_mkfun(js_qs_get_size), JS_DESC_C);
  js_set_sym(js, g_bytelength_qs_proto, get_toStringTag_sym(), js_mkstr(js, "ByteLengthQueuingStrategy", 25));

  ant_value_t blqs_ctor = js_make_ctor(js, js_bytelength_qs_ctor, g_bytelength_qs_proto, "ByteLengthQueuingStrategy", 25);
  js_set(js, g, "ByteLengthQueuingStrategy", blqs_ctor);
  js_set_descriptor(js, g, "ByteLengthQueuingStrategy", 25, JS_DESC_W | JS_DESC_C);
}

void gc_mark_queuing_strategies(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, g_count_qs_proto);
  mark(js, g_bytelength_qs_proto);
}
