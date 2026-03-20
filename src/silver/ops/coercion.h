#ifndef SV_COERCION_H
#define SV_COERCION_H

#include "silver/engine.h"
#include "modules/symbol.h"

#include "esm/loader.h"
#include <string.h>

#include "globals.h"
#include "property.h"

static inline ant_value_t sv_module_export_cstr(
  ant_t *js,
  const char *name, size_t len,
  ant_value_t value
) {
  ant_value_t module_ns = js_module_eval_active_ns(js);
  if (vtype(module_ns) != T_OBJ)
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "export used outside module");

  ant_value_t set_res = setprop_cstr(js, module_ns, name, len, value);
  if (is_err(set_res)) return set_res;

  if (len == 7 && memcmp(name, "default", 7) == 0)
    js_set_slot_wb(js, module_ns, SLOT_DEFAULT, value);

  return tov(0);
}

static inline ant_value_t sv_op_to_object(sv_vm_t *vm, ant_t *js) {
  ant_value_t v = vm->stack[vm->sp - 1];
  uint8_t t = vtype(v);
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) return tov(0);
  if (t == T_NULL || t == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot convert undefined or null to object");
  ant_value_t obj = mkobj(js, 0);
  ant_value_t obj_as_obj = js_as_obj(obj);
  js_set_slot(obj_as_obj, SLOT_PRIMITIVE, v);
  vm->stack[vm->sp - 1] = obj;
  return tov(0);
}

static inline void sv_op_to_propkey(sv_vm_t *vm, ant_t *js) {
  ant_value_t v = vm->stack[vm->sp - 1];
  if (vtype(v) != T_STR && vtype(v) != T_SYMBOL)
    vm->stack[vm->sp - 1] = coerce_to_str(js, v);
}

static inline void sv_op_is_undef(sv_vm_t *vm) {
  ant_value_t v = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = mkval(T_BOOL, vtype(v) == T_UNDEF);
}

static inline void sv_op_is_null(sv_vm_t *vm) {
  ant_value_t v = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = mkval(T_BOOL, vtype(v) == T_NULL);
}

static inline ant_value_t sv_op_import(sv_vm_t *vm, ant_t *js) {
  ant_value_t specifier = vm->stack[--vm->sp];
  ant_value_t import_fn = js_getprop_fallback(js, js->global, "import");
  if (vtype(import_fn) == T_FUNC || vtype(import_fn) == T_CFUNC) {
    ant_value_t result = sv_vm_call(vm, js, import_fn, js->global, &specifier, 1, NULL, false);
    if (!is_err(result)) vm->stack[vm->sp++] = result;
    return result;
  }
  vm->stack[vm->sp++] = mkval(T_UNDEF, 0);
  return tov(0);
}

static inline ant_value_t sv_op_import_sync(sv_vm_t *vm, ant_t *js) {
  ant_value_t specifier = vm->stack[--vm->sp];
  ant_value_t result = js_esm_import_sync(js, specifier);
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline void sv_op_import_default(sv_vm_t *vm, ant_t *js) {
  ant_value_t ns = vm->stack[vm->sp - 1];
  if (vtype(ns) == T_OBJ) {
    ant_value_t slot_val = js_get_slot(ns, SLOT_DEFAULT);
    if (vtype(slot_val) != T_UNDEF) { vm->stack[vm->sp - 1] = slot_val; return;  }
  }
}

static inline ant_value_t sv_op_export(sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  if (atom_idx >= (uint32_t)func->atom_count)
    return js_mkerr(js, "invalid export atom index");

  ant_value_t value = vm->stack[--vm->sp];
  sv_atom_t *a = &func->atoms[atom_idx];
  return sv_module_export_cstr(js, a->str, a->len, value);
}

static inline ant_value_t sv_op_export_all(sv_vm_t *vm, ant_t *js) {
  ant_value_t ns = vm->stack[--vm->sp];
  if (vtype(ns) != T_OBJ)
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "Cannot re-export from non-object module");

  ant_iter_t iter = js_prop_iter_begin(js, ns);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    ant_value_t export_res = sv_module_export_cstr(js, key, key_len, value);
    if (is_err(export_res)) { js_prop_iter_end(&iter); return export_res; }
  }
  
  js_prop_iter_end(&iter);
  return tov(0);
}

static inline ant_value_t sv_op_enter_with(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  if (sv_frame_is_strict(frame))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "with statement not allowed in strict mode");
  frame->with_obj = vm->stack[--vm->sp];
  return js_mkundef();
}

static inline void sv_op_exit_with(sv_vm_t *vm, sv_frame_t *frame) {
  frame->with_obj = js_mkundef();
}

enum { 
  WITH_FB_GLOBAL = 0,
  WITH_FB_LOCAL  = 1,
  WITH_FB_ARG    = 2,
  WITH_FB_UPVAL  = 3 
};

static inline ant_value_t sv_with_fallback_get(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame,
  uint8_t kind, uint16_t idx
) {
switch (kind) {
  case WITH_FB_LOCAL: return frame->lp ? frame->lp[idx] : js_mkundef();
  case WITH_FB_ARG:   return sv_frame_get_arg_value(frame, idx);
  case WITH_FB_UPVAL: {
    if (!frame->upvalues || (int)idx >= frame->upvalue_count) return js_mkundef();
    sv_upvalue_t *uv = frame->upvalues[idx];
    return uv ? *uv->location : js_mkundef();
  }
  default: return sv_global_get(js, NULL, 0);
}}

static inline void sv_with_fallback_put(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame,
  uint8_t kind, uint16_t idx,
  ant_value_t val
) {
  switch (kind) {
    case WITH_FB_LOCAL:
      if (frame->lp) frame->lp[idx] = val;
      break;
    case WITH_FB_ARG:
      sv_frame_set_arg_value(frame, idx, val);
      break;
    case WITH_FB_UPVAL:
      if (frame->upvalues && (int)idx < frame->upvalue_count) {
        sv_upvalue_t *uv = frame->upvalues[idx];
        if (uv) *uv->location = val;
      }
      break;
    default: break;
  }
}

static inline ant_value_t sv_op_with_get_var(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t  fb_kind  = sv_get_u8(ip + 5);
  uint16_t fb_idx   = sv_get_u16(ip + 6);
  sv_atom_t *a = &func->atoms[atom_idx];

  if (vtype(frame->with_obj) != T_UNDEF) {
    if (lkp(js, frame->with_obj, a->str, a->len) != 0) {
      ant_value_t val = sv_getprop_fallback_len(
        js, frame->with_obj, a->str, (ant_offset_t)a->len);
      if (is_err(val)) return val;
      vm->stack[vm->sp++] = val;
      return js_mkundef();
    }
  }

  if (fb_kind == WITH_FB_GLOBAL) {
    ant_value_t val = sv_global_get(js, a->str, a->len);
    if (is_undefined(val))
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE, "'%.*s' is not defined",
        (int)a->len, a->str);
    vm->stack[vm->sp++] = val;
  } else vm->stack[vm->sp++] = sv_with_fallback_get(vm, js, frame, fb_kind, fb_idx);
  return js_mkundef();
}

static inline void sv_op_with_put_var(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t  fb_kind  = sv_get_u8(ip + 5);
  uint16_t fb_idx   = sv_get_u16(ip + 6);
  
  sv_atom_t *a = &func->atoms[atom_idx];
  ant_value_t val = vm->stack[--vm->sp];

  if (vtype(frame->with_obj) != T_UNDEF) {
    if (lkp(js, frame->with_obj, a->str, a->len) != 0) {
      ant_value_t key = js_mkstr(js, a->str, a->len);
      js_setprop(js, frame->with_obj, key, val);
      return;
    }
  }

  if (fb_kind == WITH_FB_GLOBAL) {
    setprop_interned(js, js->global, a->str, a->len, val);
  } else sv_with_fallback_put(vm, js, frame, fb_kind, fb_idx, val);
}

static inline void sv_op_with_del_var(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[atom_idx];
  
  if (vtype(frame->with_obj) != T_UNDEF) {
    if (lkp(js, frame->with_obj, a->str, a->len) != 0) {
      ant_value_t result = js_delete_prop(js, frame->with_obj, a->str, a->len);
      vm->stack[vm->sp++] = result;
      return;
    }
  }
  
  ant_value_t result = js_delete_prop(js, js->global, a->str, a->len);
  bool ok = !is_err(result) && js_truthy(js, result);
  vm->stack[vm->sp++] = mkval(T_BOOL, ok);
}

static inline void sv_op_special_obj(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, uint8_t *ip
) {
  uint8_t which = sv_get_u8(ip + 1);
  if (which == 1) {
    vm->stack[vm->sp++] = frame ? frame->new_target : js_mkundef();
    return;
  }
  if (which == 2) {
    vm->stack[vm->sp++] = frame ? frame->super_val : js_mkundef();
    return;
  }
  if (which != 0 || !frame) {
    vm->stack[vm->sp++] = js_mkundef();
    return;
  }

  ant_value_t arr = js_mkarr(js);
  
  if (frame->bp && frame->argc > 0) {
    for (int i = 0; i < frame->argc; i++)
      js_arr_push(js, arr, frame->bp[i]);
  }

  if (sv_frame_is_strict(frame))
    js_set_slot(arr, SLOT_STRICT_ARGS, tov(1));
  else if (vtype(frame->callee) == T_FUNC)
    setprop_cstr(js, arr, "callee", 6, frame->callee);

  js_set_sym(js, arr, get_toStringTag_sym(), js_mkstr(js, "Arguments", 9));
  ant_value_t array_proto = js_get_ctor_proto(js, "Array", 5);
  
  if (is_object_type(array_proto)) {
    ant_value_t iter_fn = js_get_sym(js, array_proto, get_iterator_sym());
    if (vtype(iter_fn) == T_FUNC || vtype(iter_fn) == T_CFUNC)
      js_set_sym(js, arr, get_iterator_sym(), iter_fn);
  }

  vm->stack[vm->sp++] = arr;
}

#endif
