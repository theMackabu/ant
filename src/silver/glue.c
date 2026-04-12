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
#include "ops/calls.h"

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

ant_value_t jit_helper_str_append_local(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *locals, uint16_t local_idx, ant_value_t rhs
) {
  if (!func || !locals || local_idx >= (uint16_t)func->max_locals)
    return SV_JIT_BAILOUT;
    
  sv_frame_t frame = {
    .func = func,
    .lp = locals,
    .argc = func->param_count,
  };
  
  uint16_t slot_idx = (uint16_t)(func->param_count + local_idx);
  return sv_string_builder_append_slot(vm, js, &frame, func, slot_idx, rhs);
}

ant_value_t jit_helper_str_append_local_snapshot(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *locals, uint16_t local_idx,
  ant_value_t lhs, ant_value_t rhs
) {
  if (!func || !locals || local_idx >= (uint16_t)func->max_locals)
    return SV_JIT_BAILOUT;
    
  sv_frame_t frame = {
    .func = func,
    .lp = locals,
    .argc = func->param_count,
  };
  
  uint16_t slot_idx = (uint16_t)(func->param_count + local_idx);
  return sv_string_builder_append_snapshot_slot(vm, js, &frame, func, slot_idx, lhs, rhs);
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

ant_value_t jit_helper_apply(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc
) {
  sv_call_args_t call;
  sv_call_args_reset(&call, args, argc);
  ant_value_t norm = sv_apply_normalize_args(js, &call);
  if (is_err(norm)) return norm;
  ant_value_t result = sv_vm_call(vm, js, func, this_val, call.args, call.argc, NULL, false);
  sv_call_args_release(&call);
  return result;
}

ant_value_t jit_helper_rest(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *args, int argc, int start
) {
  ant_value_t arr = js_mkarr(js);
  for (int i = start; i < argc; i++)
    js_arr_push(js, arr, args[i]);
  return arr;
}

ant_value_t jit_helper_get_global(
  ant_t *js, const char *str,
  sv_func_t *func, int32_t bc_off
) {
  uint8_t *ip = NULL;
  if (func && bc_off >= 0 && bc_off < func->code_len) ip = func->code + bc_off;
  return sv_global_get_interned_ic(js, str, func, ip);
}

ant_value_t jit_helper_special_obj(ant_t *js, uint32_t which) {
  if (which == 3) return js_get_module_import_binding(js);
  return js_mkundef();
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

ant_value_t jit_helper_instanceof(
  sv_vm_t *vm, ant_t *js,
  ant_value_t l, ant_value_t r,
  sv_func_t *func, int32_t bc_off
) {
  (void)vm;
  uint8_t *ip = NULL;
  if (func && bc_off >= 0 && bc_off < func->code_len) ip = func->code + bc_off;
  return sv_instanceof_ic_eval(js, l, r, func, ip);
}

ant_value_t jit_helper_call_is_proto(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_this, ant_value_t call_func, ant_value_t arg,
  sv_func_t *func, int32_t bc_off
) {
  uint8_t *ip = NULL;
  if (func && bc_off >= 0 && bc_off < func->code_len) ip = func->code + bc_off;
  if (
    vtype(call_func) == T_CFUNC &&
    js_as_cfunc(call_func) == builtin_object_isPrototypeOf
  ) {
    return sv_isproto_ic_eval(js, call_this, arg, func, ip);
  }
  ant_value_t args[1] = { arg };
  return sv_vm_call(vm, js, call_func, call_this, args, 1, NULL, false);
}

// TODO: dont bail out
ant_value_t jit_helper_typeof(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  return SV_JIT_BAILOUT;
}

int64_t jit_helper_is_truthy(ant_t *js, ant_value_t v) {
  return (int64_t)js_truthy(js, v);
}

static inline void jit_set_error_site_from_func(ant_t *js, sv_func_t *func, int32_t bc_off) {
  if (!func) return;
  js_set_error_site_from_bc(js, func, (int)bc_off, func->filename);
}

ant_value_t jit_helper_get_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj, 
  const char *str, uint32_t len, sv_func_t *func, int32_t bc_off
) {
  (void)vm;
  uint8_t *ip = NULL;
  if (func && bc_off >= 0 && bc_off < func->code_len) ip = func->code + bc_off;
  sv_atom_t atom = { .str = str, .len = len };
  ant_value_t out = sv_prop_get_field_ic(js, obj, &atom, func, ip);
  if ((vtype(obj) == T_NULL || vtype(obj) == T_UNDEF) && is_err(out))
    jit_set_error_site_from_func(js, func, bc_off);
  return out;
}

ant_value_t jit_helper_to_propkey(sv_vm_t *vm, ant_t *js, ant_value_t v) {
  if (vtype(v) == T_STR || vtype(v) == T_SYMBOL) return v;
  return coerce_to_str(js, v);
}

static inline sv_upvalue_t *jit_make_undef_upvalue(void) {
  sv_upvalue_t *uv = js_upvalue_alloc();
  uv->closed = js_mkundef();
  uv->location = &uv->closed;
  return uv;
}

ant_value_t jit_helper_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *parent_closure,
  ant_value_t this_val, ant_value_t *slots,
  int slot_base, int slot_count, uint32_t const_idx
) {
  sv_func_t *parent_func = parent_closure->func;
  sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(parent_func->constants[const_idx]);

  sv_closure_t *closure = js_closure_alloc(js);
  if (!closure) return mkval(T_ERR, 0);

  closure->func = child;
  closure->bound_this = child->is_arrow ? this_val : js_mkundef();
  closure->bound_args = js_mkundef();
  closure->super_val = js_mkundef();
  closure->call_flags = child->is_arrow ? SV_CALL_IS_ARROW : 0;

  if (child->upvalue_count > 0)
    closure->upvalues = calloc((size_t)child->upvalue_count, sizeof(sv_upvalue_t *));

  for (int i = 0; i < child->upvalue_count; i++) {
    sv_upval_desc_t *desc = &child->upval_descs[i];
    if (!desc->is_local) {
      closure->upvalues[i] = parent_closure->upvalues[desc->index];
      continue;
    }
    
    int idx = (int)desc->index - slot_base;
    if (!slots || idx < 0 || idx >= slot_count) {
      closure->upvalues[i] = jit_make_undef_upvalue();
      continue;
    }
    
    closure->upvalues[i] = sv_capture_upvalue(vm, &slots[idx]);
  }

  ant_value_t func_obj = mkobj(js, 0);
  closure->func_obj = func_obj;
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  
  js_mark_constructor(func_obj, !child->is_arrow && !child->is_method && !child->is_generator);
  js_setprop(js, func_obj, js->length_str, tov((double)child->param_count));
  js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);
  
  if (is_object_type(module_ctx))
    js_set_slot_wb(js, func_obj, SLOT_MODULE_CTX, module_ctx);

  ant_value_t func_val = mkval(T_FUNC, (uintptr_t)closure);
  if (!child->is_arrow && !child->is_method) {
    ant_value_t parent_proto = child->is_generator ? js->sym.generator_proto : js->sym.object_proto;
    sv_setup_function_prototype_with_parent(js, func_obj, func_val, parent_proto);
  }
  
  if (child->is_async) {
    js_set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = js_get_slot(js->global, SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) js_set_proto_init(func_obj, async_proto);
  } else if (child->is_generator) {
    ant_value_t generator_proto = js_get_slot(js->global, SLOT_GENERATOR_PROTO);
    if (vtype(generator_proto) == T_FUNC) js_set_proto_init(func_obj, generator_proto);
  } else {
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_init(func_obj, func_proto);
  }

  return func_val;
}

void jit_helper_close_upval(sv_vm_t *vm, uint16_t slot_idx, ant_value_t *slots, int slot_count) {
  if (!slots || slot_count <= 0) return;
  if ((int)slot_idx >= slot_count) return;

  ant_value_t *lo = slots + slot_idx;
  ant_value_t *hi = slots + slot_count;
  sv_upvalue_t **pp = &vm->open_upvalues;
  
  while (*pp) {
    sv_upvalue_t *uv = *pp;
    if (uv->location < lo) break;
    if (uv->location >= lo && uv->location < hi) {
      uv->closed = *uv->location;
      uv->location = &uv->closed;
      *pp = uv->next;
    } 
    else pp = &uv->next;
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
) {
  if (!sv_try_define_field_fast(js, obj, str, val))
    js_define_own_prop(js, obj, str, len, val);
}


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
    const char *str_data = (const char *)(uintptr_t)(off);
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
    return sv_mk_nullish_read_error_by_key(js, obj, key);
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
  ant_value_t proto = js->sym.object_proto;
  if (vtype(proto) == T_OBJ) js_set_proto_init(obj, proto);
  return obj;
}

ant_value_t jit_helper_array(sv_vm_t *vm, ant_t *js, ant_value_t *elements, int count) {
  ant_value_t arr = js_mkarr(js);
  for (int i = 0; i < count; i++)
    js_arr_push(js, arr, elements[i]);
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
  if (pt == T_OBJ || pt == T_NULL || pt == T_FUNC || pt == T_ARR) js_set_proto_wb(js, obj, proto);
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
    const char *kptr = (const char *)(uintptr_t)(koff);
    return js_delete_prop(js, obj, kptr, klen);
  }
  return mkval(T_BOOL, 0);
}

ant_value_t jit_helper_new(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t new_target,
  ant_value_t *args, int argc
) {
  ant_value_t record_func = func;
  js->new_target = new_target;

  if (vtype(func) == T_OBJ && is_proxy(func))
    return js_proxy_construct(js, func, args, argc, new_target);
  if (!js_is_constructor(js, func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  ant_value_t proto = js_mkundef();
  if (vtype(func) == T_FUNC) {
    ant_value_t proto_source = func;
    ant_value_t func_obj = js_func_obj(func);
    ant_value_t target_func = js_get_slot(func_obj, SLOT_TARGET_FUNC);
    if (vtype(target_func) == T_FUNC) {
      proto_source = target_func;
      record_func = target_func;
    }
    proto = js_getprop_fallback(js, proto_source, "prototype");
  }

  ant_value_t obj = js_mkobj_with_inobj_limit(js, sv_tfb_ctor_inobj_limit(record_func));
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, args, argc, &ctor_this, true);

  if (is_err(result)) return result;
  ant_value_t final_obj =
    is_object_type(result) ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
  sv_tfb_record_ctor_prop_count(record_func, final_obj);
  return final_obj;
}

#endif
