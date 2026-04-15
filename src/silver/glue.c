#ifdef ANT_JIT

#include <math.h>
#include "silver/glue.h"

#include "utf8.h"
#include "internal.h"
#include "errors.h"

#include "ops/globals.h"
#include "ops/property.h"
#include "ops/iteration.h"
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
  if ((vtype(l) == T_NUM || vtype(l) == T_STR) && (vtype(r) == T_NUM || vtype(r) == T_STR)) {
    double ld = (vtype(l) == T_NUM) ? tod(l) : js_to_number(js, l);
    double rd = (vtype(r) == T_NUM) ? tod(r) : js_to_number(js, r);
    return tov(ld - rd);
  }
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
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx,
  ant_value_t rhs
) {
  if (!func)
    return SV_JIT_BAILOUT;

  sv_frame_t frame = {
    .func = func,
    .bp = args,
    .lp = locals,
    .argc = argc,
    .arguments_obj = js_mkundef(),
  };

  return sv_string_builder_append_slot(vm, js, &frame, func, slot_idx, rhs);
}

ant_value_t jit_helper_str_append_local_snapshot(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx,
  ant_value_t lhs, ant_value_t rhs
) {
  if (!func)
    return SV_JIT_BAILOUT;

  sv_frame_t frame = {
    .func = func,
    .bp = args,
    .lp = locals,
    .argc = argc,
    .arguments_obj = js_mkundef(),
  };

  return sv_string_builder_append_snapshot_slot(vm, js, &frame, func, slot_idx, lhs, rhs);
}

ant_value_t jit_helper_str_flush_local(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx
) {
  if (!func)
    return SV_JIT_BAILOUT;

  sv_frame_t frame = {
    .func = func,
    .bp = args,
    .lp = locals,
    .argc = argc,
    .arguments_obj = js_mkundef(),
  };

  ant_value_t flush = sv_string_builder_flush_slot(vm, js, &frame, slot_idx);
  if (is_err(flush)) return flush;
  return js_mkundef();
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

ant_value_t jit_helper_call_method(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc,
  ant_value_t super_val, ant_value_t new_target,
  ant_value_t *out_this
) {
  bool is_super_call = (vtype(super_val) != T_UNDEF && func == super_val);
  ant_value_t call_this = this_val;

  if (is_super_call) js->new_target = new_target;

  ant_value_t super_this = call_this;
  ant_value_t result = sv_vm_call(
    vm, js, func, call_this, args, argc,
    is_super_call ? &super_this : NULL, is_super_call
  );

  if (out_this) {
    if (is_super_call && !is_err(result)) *out_this = is_object_type(result) ? result : super_this;
    else *out_this = call_this;
  }

  return result;
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

ant_value_t jit_helper_special_obj(sv_vm_t *vm, ant_t *js, uint32_t which) {
  if (which == 1) return sv_vm_get_new_target(vm, js);
  if (which == 2) return sv_vm_get_super_val(vm);
  if (which == 3) return js_get_module_import_binding(js);
  return js_mkundef();
}

void jit_helper_define_method_comp(
  ant_t *js,
  ant_value_t obj, ant_value_t key, ant_value_t fn, uint8_t flags
) {
  ant_value_t desc_obj = js_as_obj(obj);
  bool is_getter = (flags & 1) != 0;
  bool is_setter = (flags & 2) != 0;
  if (vtype(key) == T_SYMBOL) {
    if (is_getter) { js_set_sym_getter_desc(js, desc_obj, key, fn, JS_DESC_E | JS_DESC_C); return; }
    if (is_setter) { js_set_sym_setter_desc(js, desc_obj, key, fn, JS_DESC_E | JS_DESC_C); return; }
    js_set_sym(js, obj, key, fn);
    return;
  }
  ant_value_t key_str = sv_key_to_propstr(js, key);
  if ((is_getter || is_setter) && vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    if (is_getter) js_set_getter_desc(js, desc_obj, kptr, klen, fn, JS_DESC_E | JS_DESC_C);
    else js_set_setter_desc(js, desc_obj, kptr, klen, fn, JS_DESC_E | JS_DESC_C);
    return;
  }
  if (vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    js_define_own_prop(js, obj, kptr, (size_t)klen, fn);
  } else mkprop(js, obj, key_str, fn, 0);
}

static ant_value_t jit_iter_advance_from_buf(
  sv_vm_t *vm, ant_t *js, ant_value_t *iter_buf, int hint,
  ant_value_t *out_value, bool *out_done
) {
  GC_ROOT_SAVE(root_mark, js);

  int tag = hint ? hint : (int)js_getnum(iter_buf[2]);
  switch (tag) {
  case SV_ITER_ARRAY: {
    ant_value_t arr = iter_buf[0];
    GC_ROOT_PIN(js, arr);
    int idx = (int)js_getnum(iter_buf[1]);
    ant_offset_t len = js_arr_len(js, arr);
    if (idx >= (int)len) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      *out_value = js_arr_get(js, arr, (ant_offset_t)idx);
      *out_done = false;
      iter_buf[1] = tov(idx + 1);
    }
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  case SV_ITER_MAP: {
    map_iterator_state_t *st = (map_iterator_state_t *)(uintptr_t)js_getnum(iter_buf[0]);
    if (!st->current) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      map_entry_t *entry = st->current;
      ant_value_t value;
      switch (st->type) {
      case ITER_TYPE_MAP_VALUES:
        value = entry->value;
        break;
      case ITER_TYPE_MAP_KEYS:
        value = entry->key_val;
        break;
      case ITER_TYPE_MAP_ENTRIES: {
        ant_value_t pair = js_mkarr(js);
        js_arr_push(js, pair, entry->key_val);
        js_arr_push(js, pair, entry->value);
        value = pair;
        break;
      }
      default:
        value = js_mkundef();
      }
      st->current = entry->hh.next;
      *out_value = value;
      *out_done = false;
    }
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  case SV_ITER_SET: {
    set_iterator_state_t *st = (set_iterator_state_t *)(uintptr_t)js_getnum(iter_buf[0]);
    if (!st->current) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      set_entry_t *entry = st->current;
      ant_value_t value;
      if (st->type == ITER_TYPE_SET_ENTRIES) {
        ant_value_t pair = js_mkarr(js);
        js_arr_push(js, pair, entry->value);
        js_arr_push(js, pair, entry->value);
        value = pair;
      } else {
        value = entry->value;
      }
      st->current = entry->hh.next;
      *out_value = value;
      *out_done = false;
    }
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  case SV_ITER_STRING: {
    ant_value_t str = iter_buf[0];
    GC_ROOT_PIN(js, str);
    int idx = (int)js_getnum(iter_buf[1]);
    ant_offset_t slen = str_len_fast(js, str);
    if (idx >= (int)slen) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      ant_offset_t off = vstr(js, str, NULL);
      utf8proc_int32_t cp;
      ant_offset_t cb_len = (ant_offset_t)utf8_next(
        (const utf8proc_uint8_t *)(uintptr_t)(off + idx),
        (utf8proc_ssize_t)(slen - idx),
        &cp
      );
      *out_value = js_mkstr(js, (const void *)(uintptr_t)(off + idx), cb_len);
      *out_done = false;
      iter_buf[1] = tov(idx + (int)cb_len);
    }
    GC_ROOT_PIN(js, *out_value);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  default: {
    ant_value_t iterator = iter_buf[0];
    ant_value_t next_method = iter_buf[1];
    GC_ROOT_PIN(js, iterator);
    GC_ROOT_PIN(js, next_method);
    if (!is_callable(next_method)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "iterator.next is not a function");
    }
    ant_value_t result = sv_vm_call(vm, js, next_method, iterator, NULL, 0, NULL, false);
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
    GC_ROOT_PIN(js, result);
    if (!is_object_type(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
    }
    ant_value_t done = js_mkundef();
    GC_ROOT_PIN(js, done);
    sv_iter_result_unpack(js, result, &done, out_value);
    GC_ROOT_PIN(js, *out_value);
    if (is_err(done)) {
      GC_ROOT_RESTORE(js, root_mark);
      return done;
    }
    if (is_err(*out_value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return *out_value;
    }
    *out_done = js_truthy(js, done);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }}
}

void jit_helper_destructure_close(
  sv_vm_t *vm, ant_t *js, ant_value_t *iter_buf
) {
  GC_ROOT_SAVE(root_mark, js);

  int tag = (int)js_getnum(iter_buf[2]);
  if (tag == SV_ITER_GENERIC) {
    ant_value_t iterator = iter_buf[0];
    GC_ROOT_PIN(js, iterator);
    ant_value_t return_fn = js_getprop_fallback(js, iterator, "return");
    GC_ROOT_PIN(js, return_fn);
    if (is_callable(return_fn))
      sv_vm_call(vm, js, return_fn, iterator, NULL, 0, NULL, false);
  }

  GC_ROOT_RESTORE(js, root_mark);
}

ant_value_t jit_helper_for_of(
  sv_vm_t *vm, ant_t *js,
  ant_value_t iterable, ant_value_t *iter_buf
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterable);

  if (vtype(iterable) == T_ARR) {
    iter_buf[0] = iterable;
    iter_buf[1] = tov(0);
    iter_buf[2] = tov(SV_ITER_ARRAY);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  if (vtype(iterable) == T_STR) {
    if (str_is_heap_rope(iterable) || str_is_heap_builder(iterable)) {
      iterable = str_materialize(js, iterable);
      if (is_err(iterable)) {
        GC_ROOT_RESTORE(js, root_mark);
        return iterable;
      }
      GC_ROOT_PIN(js, iterable);
    }
    iter_buf[0] = iterable;
    iter_buf[1] = tov(0);
    iter_buf[2] = tov(SV_ITER_STRING);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  ant_value_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  GC_ROOT_PIN(js, iter_fn);
  if (!is_callable(iter_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "not iterable");
  }
  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iterator;
  }
  GC_ROOT_PIN(js, iterator);

  map_iterator_state_t *map_st;
  iter_type_t map_type;
  if (sv_is_map_iter(js, iterator, &map_st, &map_type)) {
    iter_buf[0] = ANT_PTR(map_st);
    iter_buf[1] = tov((double)map_type);
    iter_buf[2] = tov(SV_ITER_MAP);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  set_iterator_state_t *set_st;
  iter_type_t set_type;
  if (sv_is_set_iter(js, iterator, &set_st, &set_type)) {
    iter_buf[0] = ANT_PTR(set_st);
    iter_buf[1] = tov((double)set_type);
    iter_buf[2] = tov(SV_ITER_SET);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  GC_ROOT_PIN(js, next_method);
  iter_buf[0] = iterator;
  iter_buf[1] = next_method;
  iter_buf[2] = tov(SV_ITER_GENERIC);
  GC_ROOT_RESTORE(js, root_mark);
  return tov(0);
}

ant_value_t jit_helper_destructure_next(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *iter_buf
) {
  ant_value_t value = js_mkundef();
  bool done = false;
  ant_value_t status = jit_iter_advance_from_buf(vm, js, iter_buf, 0, &value, &done);
  if (is_err(status)) return status;

  iter_buf[3] = done ? js_mkundef() : value;
  return status;
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
  ant_value_t module_ctx = sv_get_current_closure_module_ctx(js, mkval(T_FUNC, (uintptr_t)parent_closure));
  
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
    ant_flat_string_t *flat = ant_str_flat_ptr(obj);
    if (flat) {
      const char *str_data = flat->bytes;
      ant_offset_t byte_len = flat->len;
      return tov((double)(uint32_t)(
        str_is_ascii(str_data) 
          ? byte_len 
          : utf16_strlen(str_data, byte_len)
      ));
    }
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
  if (!((vtype(l) == T_NUM || vtype(l) == T_STR) && (vtype(r) == T_NUM || vtype(r) == T_STR))) return SV_JIT_BAILOUT;
  int32_t ai = js_to_int32((vtype(l) == T_NUM) ? tod(l) : js_to_number(js, l));
  int32_t bi = js_to_int32((vtype(r) == T_NUM) ? tod(r) : js_to_number(js, r));
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
