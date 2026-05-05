#ifndef SV_COERCION_H
#define SV_COERCION_H

#include <string.h>

#include "globals.h"
#include "property.h"

#include "esm/loader.h"
#include "silver/engine.h"
#include "modules/symbol.h"

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
  ant_value_t import_fn = js_get_module_import_binding(js);
  
  if (vtype(import_fn) != T_FUNC && vtype(import_fn) != T_CFUNC)
    import_fn = js_getprop_fallback(js, js->global, "import");
    
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

static inline bool sv_module_namespace_has_export(
  ant_t *js,
  ant_value_t ns,
  const char *name,
  size_t len
) {
  if (!is_object_type(ns)) return false;

  ant_value_t as_obj = js_as_obj(ns);
  if (is_proxy(as_obj)) return false;
  if (lkp(js, as_obj, name, len) != 0) return true;

  prop_meta_t meta;
  return lookup_string_prop_meta(js, as_obj, name, len, &meta);
}

static inline const char *sv_module_namespace_display_name(ant_t *js, ant_value_t ns) {
  if (!is_object_type(ns)) return NULL;

  ant_value_t module_ctx = js_get_slot(ns, SLOT_MODULE_CTX);
  if (!is_object_type(module_ctx)) return NULL;

  ant_value_t display_name = js_get(js, module_ctx, "displayName");
  if (vtype(display_name) == T_STR) return js_getstr(js, display_name, NULL);

  ant_value_t filename = js_get(js, module_ctx, "filename");
  if (vtype(filename) != T_STR) return NULL;
  
  return js_getstr(js, filename, NULL);
}

static inline ant_value_t sv_missing_named_export_error(
  ant_t *js,
  ant_value_t ns,
  const char *name,
  size_t len
) {
  const char *display_name = sv_module_namespace_display_name(js, ns);
  if (!display_name) display_name = "<unknown>";
  return js_mkerr_typed(
    js, JS_ERR_SYNTAX,
    "The requested module '%s' does not provide an export named '%.*s'",
    display_name, (int)len, name
  );
}

static inline ant_value_t sv_op_import_named(
  sv_vm_t *vm,
  ant_t *js,
  sv_func_t *func,
  uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  if (atom_idx >= (uint32_t)func->atom_count)
    return js_mkerr(js, "invalid import atom index");

  ant_value_t ns = vm->stack[vm->sp - 1];
  sv_atom_t *a = &func->atoms[atom_idx];

  if (
    !sv_module_namespace_has_export(js, ns, a->str, a->len) &&
    js_get_slot(ns, SLOT_MODULE_LOADING) != js_true
  ) return sv_missing_named_export_error(js, ns, a->str, a->len);

  ant_value_t value = js_get(js, ns, a->str);
  if (is_err(value)) return value;
  vm->stack[vm->sp - 1] = value;
  
  return tov(0);
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
  WITH_FB_GLOBAL       = 0,
  WITH_FB_LOCAL        = 1,
  WITH_FB_ARG          = 2,
  WITH_FB_UPVAL        = 3,
  WITH_FB_GLOBAL_UNDEF = 4
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
  case WITH_FB_GLOBAL_UNDEF:
    return js_mkundef();
  default: return sv_global_get(js, NULL, 0);
}}

static inline bool sv_with_binding_is_unscopable(
  ant_t *js,
  ant_value_t with_obj,
  const sv_atom_t *a,
  ant_value_t *out,
  bool *abrupt
) {
  *abrupt = false;

  ant_value_t unscopables_sym = get_unscopables_sym();
  if (vtype(unscopables_sym) != T_SYMBOL) return false;

  bool is_proxy_obj = is_proxy(js_as_obj(with_obj));
  ant_value_t unscopables = js_mkundef();

  if (!is_proxy_obj) {
    ant_offset_t sym_off = (ant_offset_t)vdata(unscopables_sym);
    ant_object_t *base_ptr = js_obj_ptr(js_as_obj(with_obj));
    ant_value_t base_proto = (base_ptr && is_object_type(base_ptr->proto)) ? base_ptr->proto : js_mknull();
    ant_object_t *proto_ptr = is_object_type(base_proto) ? js_obj_ptr(js_as_obj(base_proto)) : NULL;

    static ant_object_t *cached_no_base = NULL;
    static void *cached_no_base_shape = NULL;
    static ant_value_t cached_no_proto = 0;
    static void *cached_no_proto_shape = NULL;

    if (
      base_ptr == cached_no_base &&
      (void *)(base_ptr ? base_ptr->shape : NULL) == cached_no_base_shape &&
      base_proto == cached_no_proto &&
      (void *)(proto_ptr ? proto_ptr->shape : NULL) == cached_no_proto_shape
    ) return false;

    ant_offset_t unscopables_off = lkp_sym_proto(js, with_obj, sym_off);
    bool has_unscopables = unscopables_off != 0;
    bool saw_exotic = base_ptr && base_ptr->is_exotic;

    if (!has_unscopables) {
      ant_value_t cur = with_obj;
      while (is_object_type(cur)) {
        ant_value_t cur_obj = js_as_obj(cur);
        ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
        if (cur_ptr && cur_ptr->is_exotic) saw_exotic = true;
        prop_meta_t meta;
        if (cur_ptr && cur_ptr->is_exotic && lookup_symbol_prop_meta(cur_obj, sym_off, &meta)) {
          has_unscopables = true;
          break;
        }

        ant_value_t proto = js_get_proto(js, cur_obj);
        if (!is_object_type(proto)) break;
        cur = proto;
      }
    }

    if (!has_unscopables) {
      ant_value_t proto_proto = (proto_ptr && is_object_type(proto_ptr->proto)) ? proto_ptr->proto : js_mknull();
      if (!saw_exotic && !is_object_type(proto_proto)) {
        cached_no_base = base_ptr;
        cached_no_base_shape = (void *)(base_ptr ? base_ptr->shape : NULL);
        cached_no_proto = base_proto;
        cached_no_proto_shape = (void *)(proto_ptr ? proto_ptr->shape : NULL);
      }
      return false;
    }
    if (unscopables_off != 0) unscopables = js_propref_load(js, unscopables_off);
  }

  if (is_proxy_obj || vtype(unscopables) == T_UNDEF)
    unscopables = js_get_sym(js, with_obj, unscopables_sym);
  if (is_err(unscopables)) {
    *out = unscopables;
    *abrupt = true;
    return false;
  }

  if (!is_object_type(unscopables)) return false;

  ant_value_t blocked = js_mkundef();
  bool got_blocked_fast = false;

  if (!is_proxy(js_as_obj(unscopables))) {
    ant_value_t cur = unscopables;
    while (is_object_type(cur)) {
      ant_value_t cur_obj = js_as_obj(cur);
      prop_meta_t meta;
      if (lookup_string_prop_meta(js, cur_obj, a->str, a->len, &meta)) {
        if (meta.has_getter || meta.has_setter) break;
        ant_offset_t off = lkp(js, cur_obj, a->str, a->len);
        blocked = off != 0 ? js_propref_load(js, off) : js_mkundef();
        got_blocked_fast = true;
        break;
      }

      ant_value_t proto = js_get_proto(js, cur_obj);
      if (!is_object_type(proto)) {
        got_blocked_fast = true;
        break;
      }
      cur = proto;
    }
  }

  if (!got_blocked_fast)
    blocked = js_getprop_fallback(js, unscopables, a->str);
  if (is_err(blocked)) {
    *out = blocked;
    *abrupt = true;
    return false;
  }

  return js_truthy(js, blocked);
}

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
      sv_frame_set_arg_value(js, frame, idx, val);
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

static inline bool sv_try_get_with_bound_value(
  ant_t *js,
  ant_value_t with_obj,
  const sv_atom_t *a,
  ant_value_t *out
) {
  ant_object_t *ptr = is_object_type(with_obj) ? js_obj_ptr(js_as_obj(with_obj)) : NULL;
  const char *interned = intern_string(a->str, a->len);

  if (ptr && is_proxy(js_as_obj(with_obj))) {
    bool abrupt = false;
    if (sv_with_binding_is_unscopable(js, with_obj, a, out, &abrupt)) return false;
    if (abrupt) return true;
    *out = js_getprop_fallback(js, with_obj, a->str);
    return true;
  }

  if (ptr && interned) {
    bool should_fallback = false;
    if (sv_try_get_shape_data_prop(js, ptr, interned, out, &should_fallback)) {
      bool abrupt = false;
      if (sv_with_binding_is_unscopable(js, with_obj, a, out, &abrupt)) return false;
      if (abrupt) return true;
      return true;
    }
    if (!should_fallback) return false;
  }

  if (lkp(js, with_obj, a->str, a->len) == 0) return false;
  bool abrupt = false;
  if (sv_with_binding_is_unscopable(js, with_obj, a, out, &abrupt)) return false;
  if (abrupt) return true;
  *out = sv_getprop_fallback_len(js, with_obj, a->str, (ant_offset_t)a->len);
  
  return true;
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
  ant_value_t val = js_mkundef();
  if (sv_try_get_with_bound_value(js, frame->with_obj, a, &val)) {
    if (is_err(val)) return val;
    vm->stack[vm->sp++] = val;
    return js_mkundef();
  }}

  if (fb_kind == WITH_FB_GLOBAL || fb_kind == WITH_FB_GLOBAL_UNDEF) {
    ant_value_t val = sv_global_get(js, a->str, a->len);
    if (is_undefined(val) && fb_kind == WITH_FB_GLOBAL)
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
  if (which == 3) {
    vm->stack[vm->sp++] = js_get_module_import_binding(js);
    return;
  }
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

  if (vtype(frame->arguments_obj) == T_UNDEF) {
    int mapped_count = sv_frame_is_strict(frame) || !frame->func ? 0 : frame->func->param_count;
    if (mapped_count > frame->argc) mapped_count = frame->argc;
    frame->arguments_obj = js_create_arguments_object(
      js, frame->callee, frame, frame->argc, 
      mapped_count, sv_frame_is_strict(frame)
    );
  }

  vm->stack[vm->sp++] = frame->arguments_obj;
}

#endif
