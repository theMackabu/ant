#ifdef ANT_JIT

#include <math.h>
#include "silver/glue.h"

#include "utf8.h"
#include "internal.h"
#include "errors.h"

#include "ops/globals.h"
#include "ops/property.h"
#include "ops/upvalues.h"
#include "ops/comparison.h"

bool jit_helper_stack_overflow(ant_t *js) {
  volatile char marker;
  uintptr_t curr = (uintptr_t)&marker;
  if (js->cstk.limit == 0 || js->cstk.base == NULL) return false;
  uintptr_t base = (uintptr_t)js->cstk.base;
  size_t used = (base > curr) ? (base - curr) : (curr - base);
  return used > js->cstk.limit;
}

ant_value_t jit_helper_stack_overflow_error(sv_vm_t *vm, ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum JIT call stack size exceeded");
}

ant_value_t jit_helper_add(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(tod(l) + tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_sub(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(tod(l) - tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_mul(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(tod(l) * tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_div(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(tod(l) / tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_mod(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(fmod(tod(l), tod(r)));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_lt(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return js_bool(tod(l) < tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_le(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) return js_bool(tod(l) <= tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_call(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc
) {
  return sv_vm_call(vm, js, func, this_val, args, argc, NULL, false);
}

ant_value_t jit_helper_get_global(ant_t *js, const char *str, uint32_t len) {
  return sv_global_get(js, str, len);
}

ant_value_t jit_helper_seq(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  return mkval(T_BOOL, strict_eq_values(js, l, r));
}

ant_value_t jit_helper_in(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  return do_in(js, l, r);
}

ant_value_t jit_helper_eq(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  return sv_abstract_eq(js, l, r);
}

ant_value_t jit_helper_throw(sv_vm_t *vm, ant_t *js, ant_value_t val) {
  return js_throw(js, val);
}

ant_value_t jit_helper_ne(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  ant_value_t eq = sv_abstract_eq(js, l, r);
  return mkval(T_BOOL, !vdata(eq));
}

ant_value_t jit_helper_sne(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  return mkval(T_BOOL, !strict_eq_values(js, l, r));
}

ant_value_t jit_helper_not(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  return mkval(T_BOOL, !js_truthy(js, v));
}

ant_value_t jit_helper_instanceof(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  return do_instanceof(js, l, r);
}

// TODO: dont bail out
ant_value_t jit_helper_typeof(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  return SV_JIT_BAILOUT;
}

int64_t jit_helper_is_truthy(ant_t *js, ant_value_t v) {
  return (int64_t)js_truthy(js, v);
}

sv_closure_t *jit_helper_get_closure(ant_value_t v) {
  if (vtype(v) != T_FUNC) return NULL;
  return js_func_closure(v);
}

static inline void jit_set_error_site_from_func(ant_t *js, sv_func_t *func, int32_t bc_off) {
  if (!func) return;
  js_set_error_site_from_bc(js, func, (int)bc_off, func->filename);
}

ant_value_t jit_helper_get_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj, 
  const char *str, uint32_t len, sv_func_t *func, int32_t bc_off
) {
  if (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)
    jit_set_error_site_from_func(js, func, bc_off);
  return sv_prop_get(js, obj, str, len);
}

ant_value_t jit_helper_to_propkey(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  if (vtype(v) == T_STR || vtype(v) == T_SYMBOL) return v;
  return coerce_to_str(js, v);
}

uint64_t jit_helper_vtype(ant_value_t v) { return vtype(v); }
double   jit_helper_tod(ant_value_t v)   { return tod(v); }
ant_value_t  jit_helper_tov(double d)    { return tov(d); }
ant_value_t  jit_helper_mkbool(int b)    { return js_bool(b); }

ant_value_t jit_helper_closure(
  sv_vm_t *vm, ant_t *js,
  sv_closure_t *parent_closure, ant_value_t this_val,
  ant_value_t *args, int argc,
  uint32_t const_idx, ant_value_t *locals, int n_locals
) {
  sv_func_t *parent_func = parent_closure->func;
  sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(parent_func->constants[const_idx]);

  sv_closure_t *closure = calloc(1, sizeof(sv_closure_t));
  if (!closure) return mkval(T_ERR, 0);
  
  closure->func = child;
  closure->bound_this = child->is_arrow ? this_val : js_mkundef();
  closure->bound_args = js_mkundef();
  closure->super_val = js_mkundef();
  closure->call_flags = child->is_arrow ? SV_CALL_IS_ARROW : 0;

  // TODO: reduce nesting
  if (child->upvalue_count > 0) {
    closure->upvalues = calloc((size_t)child->upvalue_count, sizeof(sv_upvalue_t *));
    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (desc->is_local) {
        int idx = (int)desc->index;
        if (idx < parent_func->param_count) {
          sv_upvalue_t *uv = calloc(1, sizeof(sv_upvalue_t));
          uv->closed = (idx < argc && args) ? args[idx] : js_mkundef();
          uv->location = &uv->closed;
          closure->upvalues[i] = uv;
        } else {
          int li = idx - parent_func->param_count;
          if (li >= 0 && li < n_locals && locals) {
            closure->upvalues[i] = sv_capture_upvalue(vm, &locals[li]);
          } else {
            sv_upvalue_t *uv = calloc(1, sizeof(sv_upvalue_t));
            uv->closed = js_mkundef();
            uv->location = &uv->closed;
            closure->upvalues[i] = uv;
          }
        }
      } else closure->upvalues[i] = parent_closure->upvalues[desc->index];
    }
  }

  ant_value_t func_obj = mkobj(js, 0);
  closure->func_obj = func_obj;
  ant_object_t *func_obj_ptr = js_obj_ptr(func_obj);
  if (func_obj_ptr)
    func_obj_ptr->is_constructor = (!child->is_arrow && !child->is_method);
  js_setprop(js, func_obj, js->length_str, tov((double)child->param_count));
  js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);

  ant_value_t func_val = mkval(T_FUNC, (uintptr_t)closure);
  if (!child->is_arrow && !child->is_method)
    sv_setup_function_prototype(js, func_obj, func_val);

  if (child->is_strict)
    js_set_slot(js, func_obj, SLOT_STRICT, js_true);
  
  if (child->is_async) {
    js_set_slot(js, func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = js_get_slot(js, js->global, SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC)
      js_set_proto(js, func_obj, async_proto);
  } else {
    ant_value_t func_proto = js_get_slot(js, js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC)
      js_set_proto(js, func_obj, func_proto);
  }

  return func_val;
}

void jit_helper_close_upval(sv_vm_t *vm, uint16_t slot_idx, ant_value_t *locals, int n_locals) {
  (void)slot_idx; // TODO: use value
  if (!locals || n_locals <= 0) return;

  ant_value_t *lo = locals;
  ant_value_t *hi = locals + n_locals;
  sv_upvalue_t **pp = &vm->open_upvalues;
  
  while (*pp) {
    sv_upvalue_t *uv = *pp;
    if (uv->location >= lo && uv->location < hi) {
      uv->closed = *uv->location;
      uv->location = &uv->closed;
      *pp = uv->next;
    } else pp = &uv->next;
  }
}

ant_value_t jit_helper_bailout_resume(
  sv_vm_t *vm, sv_closure_t *closure,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *vstack, int64_t vstack_sp,
  ant_value_t *locals, int64_t n_locals,
  int64_t bc_offset
) {
  if (!closure || !closure->func) return mkval(T_ERR, 0);
  sv_func_t *fn = closure->func;
  sv_jit_on_bailout(fn);

  vm->jit_resume.active     = true;
  vm->jit_resume.ip_offset  = (int)bc_offset;
  vm->jit_resume.locals     = locals;
  vm->jit_resume.n_locals   = n_locals;
  vm->jit_resume.vstack     = vstack;
  vm->jit_resume.vstack_sp  = vstack_sp;

  return sv_execute_closure_entry(
    vm, closure, closure->func_obj, js_mkundef(),
    this_val, args, argc, NULL
  );
}

void jit_helper_define_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const char *str, uint32_t len
) { js_define_own_prop(js, obj, str, len, val); }


void jit_helper_set_name(
  sv_vm_t *vm, ant_t *js, ant_value_t fn,
  const char *str, uint32_t len
) {
  ant_value_t name = js_mkstr(js, str, len);
  setprop_cstr(js, fn, "name", 4, name);
}

ant_value_t jit_helper_get_length(sv_vm_t *vm, ant_t *js, ant_value_t obj) {
  if (vtype(obj) == T_ARR)
    return tov((double)(uint32_t)js_arr_len(js, obj));
  if (vtype(obj) == T_STR) {
    ant_offset_t byte_len = 0;
    ant_offset_t off = vstr(js, obj, &byte_len);
    const char *str_data = (const char *)&js->mem[off];
    return tov((double)(uint32_t)utf16_strlen(str_data, byte_len));
  }
  return js_getprop_fallback(js, obj, "length");
}

ant_value_t jit_helper_put_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const char *str, uint32_t len
) {
  ant_value_t key = js_mkstr(js, str, len);
  return js_setprop(js, obj, key, val);
}

ant_value_t jit_helper_get_elem(
  sv_vm_t *vm, ant_t *js, ant_value_t obj, 
  ant_value_t key, sv_func_t *func, int32_t bc_off
) {
  uint8_t ot = vtype(obj);
  if (ot == T_NULL || ot == T_UNDEF) {
    jit_set_error_site_from_func(js, func, bc_off);
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot read properties of %s", ot == T_NULL ? "null" : "undefined");
  }
  if (vtype(obj) == T_ARR && vtype(key) == T_NUM) {
    double d = tod(key);
    if (d >= 0 && d == (uint32_t)d)
      return js_arr_get(js, obj, (uint32_t)d);
  }
  ant_value_t str_elem = js_mkundef();
  if (sv_try_string_index_get(js, obj, key, &str_elem))
    return str_elem;
  return sv_getprop_by_key(js, obj, key);
}

ant_value_t jit_helper_put_elem(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key, ant_value_t val
) {
  if (vtype(key) == T_SYMBOL) return js_setprop(js, obj, key, val);
  ant_value_t key_jv = sv_key_to_propstr(js, key);
  return js_setprop(js, obj, key_jv, val);
}

ant_value_t jit_helper_put_global(
  sv_vm_t *vm, ant_t *js, ant_value_t val,
  const char *str, uint32_t len, int is_strict
) {
  if (is_strict && lkp(js, js->global, str, len) == 0)
    return js_mkerr_typed(js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)len, str);
  ant_value_t key = js_mkstr(js, str, len);
  return js_setprop(js, js->global, key, val);
}

ant_value_t jit_helper_object(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  ant_value_t proto = js_get_ctor_proto(js, "Object", 6);
  if (vtype(proto) == T_OBJ) js_set_proto(js, obj, proto);
  return obj;
}

ant_value_t jit_helper_array(sv_vm_t *vm, ant_t *js, ant_value_t *elements, int count) {
  ant_value_t arr = js_mkarr(js);
  ant_handle_t h = js_root(js, arr);
  for (int i = 0; i < count; i++)
    js_arr_push(js, js_deref(js, h), elements[i]);
  arr = js_deref(js, h);
  js_unroot(js, h);
  return arr;
}

ant_value_t jit_helper_catch_value(sv_vm_t *vm, ant_t *js, ant_value_t err) {
  if (vtype(err) == T_ERR && js->thrown_exists &&
      vtype(js->thrown_value) != T_UNDEF) {
    ant_value_t caught = js->thrown_value;
    js->thrown_value = js_mkundef();
    js->thrown_exists = false;
    return caught;
  }
  return err;
}

ant_value_t jit_helper_throw_error(
  sv_vm_t *vm, ant_t *js,
  const char *str, uint32_t len, int err_type
) { return js_mkerr_typed(js, (js_err_type_t)err_type, "%.*s", (int)len, str); }

ant_value_t jit_helper_get_elem2(sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t key) {
  if (vtype(obj) == T_ARR && vtype(key) == T_NUM) {
    double d = tod(key);
    if (d >= 0 && d == (uint32_t)d)
      return js_arr_get(js, obj, (uint32_t)d);
  }
  ant_value_t str_elem = js_mkundef();
  if (sv_try_string_index_get(js, obj, key, &str_elem))
    return str_elem;
  return sv_getprop_by_key(js, obj, key);
}

ant_value_t jit_helper_set_proto(sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t proto) {
  uint8_t pt = vtype(proto);
  if (pt == T_OBJ || pt == T_NULL || pt == T_FUNC || pt == T_ARR)
    js_set_proto(js, obj, proto);
  return js_mkundef();
}

ant_value_t jit_helper_band(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32(tod(l));
  int32_t bi = js_to_int32(tod(r));
  return tov((double)(ai & bi));
}

ant_value_t jit_helper_bor(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32(tod(l));
  int32_t bi = js_to_int32(tod(r));
  return tov((double)(ai | bi));
}

ant_value_t jit_helper_bxor(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32(tod(l));
  int32_t bi = js_to_int32(tod(r));
  return tov((double)(ai ^ bi));
}

ant_value_t jit_helper_bnot(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  if (vtype(v) != T_NUM) return SV_JIT_BAILOUT;
  return tov((double)(~js_to_int32(tod(v))));
}

ant_value_t jit_helper_shl(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32(tod(l));
  uint32_t bi = js_to_uint32(tod(r));
  return tov((double)(ai << (bi & 0x1f)));
}

ant_value_t jit_helper_shr(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32(tod(l));
  uint32_t bi = js_to_uint32(tod(r));
  return tov((double)(ai >> (bi & 0x1f)));
}

ant_value_t jit_helper_ushr(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) != T_NUM || vtype(r) != T_NUM) return SV_JIT_BAILOUT;
  uint32_t ai = js_to_uint32(tod(l));
  uint32_t bi = js_to_uint32(tod(r));
  return tov((double)(ai >> (bi & 0x1f)));
}

ant_value_t jit_helper_gt(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM)
    return js_bool(tod(l) > tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_ge(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == T_NUM && vtype(r) == T_NUM)
    return js_bool(tod(l) >= tod(r));
  return SV_JIT_BAILOUT;
}

ant_value_t jit_helper_delete(sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t key) {
  ant_value_t key_str = js_mkundef();

  if (vtype(key) == T_SYMBOL) 
    return js_delete_sym_prop(js, obj, key);
  else key_str = coerce_to_str(js, key);

  if (!is_err(key_str) && vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)&js->mem[koff];
    return js_delete_prop(js, obj, kptr, klen);
  }
  return mkval(T_BOOL, 0);
}

ant_value_t jit_helper_new(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t new_target,
  ant_value_t *args, int argc
) {
  js->new_target = new_target;

  if (vtype(func) == T_OBJ && is_proxy(js, func))
    return js_proxy_construct(js, func, args, argc, new_target);
  if (!js_is_constructor(js, func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  ant_value_t obj = mkobj(js, 0);

  if (vtype(func) == T_FUNC) {
    ant_value_t proto_source = func;
    ant_value_t func_obj = js_func_obj(func);
    ant_value_t target_func = js_get_slot(js, func_obj, SLOT_TARGET_FUNC);
    if (vtype(target_func) == T_FUNC) proto_source = target_func;
    ant_value_t proto = js_getprop_fallback(js, proto_source, "prototype");
    if (is_object_type(proto)) js_set_proto(js, obj, proto);
  }

  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, args, argc, &ctor_this, true);
  
  if (is_err(result)) return result;
  return is_object_type(result) ? result : (is_object_type(ctor_this) ? ctor_this : obj);
}

#endif
