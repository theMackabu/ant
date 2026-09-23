#ifndef SV_GLOBALS_H
#define SV_GLOBALS_H

#include "silver/engine.h"
#include "errors.h"
#include "shapes.h"
#include "eval_env.h"
#include "modules/regex.h"

static inline bool sv_global_try_store_own_data(
  ant_t *js, ant_value_t obj, const char *interned, uint32_t len, ant_value_t val
) {
  if (!is_object_type(obj) || !interned) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return false;
  
  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (slot < 0 || (uint32_t)slot >= ptr->prop_count) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);

  if (
    !prop || prop->has_getter || prop->has_setter || 
    (prop->attrs & ANT_PROP_ATTR_WRITABLE) == 0) return false;

  regexp_note_property_write(js, interned, len);
  ant_object_prop_set_unchecked(ptr, (uint32_t)slot, val);
  
  gc_write_barrier(js, ptr, val);
  ant_property_mutation_invalidate(js, ptr, interned);
  
  return true;
}

static inline __attribute__((always_inline)) ant_value_t sv_global_put(
  ant_t *js, const char *interned, uint32_t len, ant_value_t val, bool is_strict
) {
  ant_global_lexical_t *lex = sv_global_lexical(js, interned);
  if (lex) return sv_global_lexical_set(js, lex, val, false);
  
  if (is_strict && !lkp_interned(js->global, interned).obj) 
    return js_mkerr_typed( js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)len, interned);
  
  if (sv_global_try_store_own_data(js, js->global, interned, len, val)) return val;
  return setprop_interned(js, js->global, interned, len, val);
}

static inline ant_value_t sv_global_delete(ant_t *js, const char *interned, uint32_t len) {
  if (sv_global_lexical(js, interned)) return js_false;
  return js_delete_prop(js, js->global, interned, len);
}

static inline bool sv_env_try_get_interned(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len, ant_value_t *out
) {
  ant_value_t current = env;
  
  while (is_object_type(current)) {
    if (current == js->global) {
      ant_global_lexical_t *lex = sv_global_lexical(js, interned);
    
      if (lex) {
        if (out) *out = sv_global_lexical_get(js, lex);
        return true;
      }
    
      if (!lkp_proto(js, current, interned, len).obj) return false;
      if (out) *out = js_getprop_fallback(js, current, interned);
    
      return true;
    }

    if (sv_eval_env_try_get(js, current, interned, len, out)) return true;
    ant_prop_loc_t off = lkp_interned(current, interned);
    
    if (off.obj) {
      if (out) *out = js_prop_load(off);
      return true;
    }

    current = js_get_proto(js, current);
  }

  return false;
}

static inline ant_value_t sv_env_get(
  ant_t *js, ant_value_t env, const char *str, uint32_t len
) {
  if (!str) return js_mkundef();

  const char *interned = intern_string(str, len);
  if (!interned) return js_mkundef();

  ant_value_t val = js_mkundef();
  (void)sv_env_try_get_interned(js, env, interned, len, &val);
  return val;
}

static inline ant_value_t sv_env_put(
  ant_t *js, ant_value_t env, const char *str, uint32_t len,
  ant_value_t val, bool is_strict
) {
  for (
    ant_value_t current = env; is_object_type(current) && 
    current != js->global; current = js_get_proto(js, current)
  ) {
    ant_value_t binding_result;
    if (sv_eval_env_try_put(js, current, str, len, val, &binding_result)) return binding_result;
    
    if (lkp_interned(current, str).obj) {
      if (sv_global_try_store_own_data(js, current, str, len, val)) return val;
      return setprop_interned(js, current, str, len, val);
    }
  }
  
  return sv_global_put(js, str, len, val, is_strict);
}

static inline ant_value_t sv_env_delete(
  ant_t *js, ant_value_t env, const char *str, uint32_t len
) {
  for (
    ant_value_t current = env; is_object_type(current) &&
    current != js->global; current = js_get_proto(js, current)
  ) {
    if (sv_eval_env_has_binding(current, str, len)) return js_false;
    if (lkp_interned(current, str).obj) return js_delete_prop(js, current, str, len);
  }
  
  return sv_global_delete(js, str, len);
}

static inline sv_ic_entry_t *sv_global_ic_slot_for_ip(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return NULL;
  uint16_t ic_idx = sv_get_u16(ip + 5);
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return NULL;
  return &func->ic_slots[ic_idx];
}

static inline ant_object_t *sv_global_obj_ptr(ant_value_t target) {
  if (!is_object_type(target)) return NULL;
  return js_obj_ptr(js_as_obj(target));
}

static inline bool sv_global_ic_try_get_hit(
  ant_value_t target,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
) {
  if (!ic || !interned) return false;

  ant_object_t *gptr = sv_global_obj_ptr(target);
  if (!gptr || gptr->flags.is_exotic || !gptr->shape) return false;
  if (ic->epoch != ant_ic_epoch_counter) return false;
  if (ic->cached_shape != gptr->shape) return false;
  if (ic->cached_index >= gptr->prop_count) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(gptr->shape, ic->cached_index);
  if (!prop) return false;
  if (prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != interned) return false;
  if (prop->has_getter || prop->has_setter) return false;

  *out = ant_object_prop_get_unchecked(gptr, ic->cached_index);
  return true;
}

static inline bool sv_global_prop_is_accessor(
  ant_value_t target,
  const char *interned
) {
  ant_object_t *gptr = sv_global_obj_ptr(target);
  if (!gptr || !gptr->shape) return false;

  int32_t slot = ant_shape_lookup_interned(gptr->shape, interned);
  if (slot < 0) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(gptr->shape, (uint32_t)slot);
  return prop && (prop->has_getter || prop->has_setter);
}

static inline bool sv_global_ic_try_fill(
  ant_value_t target,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
) {
  if (!ic || !interned) return false;

  ant_object_t *gptr = sv_global_obj_ptr(target);
  if (!gptr || gptr->flags.is_exotic || !gptr->shape) return false;

  int32_t slot = ant_shape_lookup_interned(gptr->shape, interned);
  if (slot < 0) return false;

  uint32_t idx = (uint32_t)slot;
  if (idx >= gptr->prop_count) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(gptr->shape, idx);
  if (!prop) return false;
  if (prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != interned) return false;
  if (prop->has_getter || prop->has_setter) return false;

  ic->cached_shape = gptr->shape;
  ic->cached_holder = gptr;
  ic->cached_index = idx;
  ic->epoch = ant_ic_epoch_counter;
  *out = ant_object_prop_get_unchecked(gptr, idx);
  return true;
}

static inline bool sv_global_ic_try_fill_unshadowed(
  ant_t *js, ant_value_t target, sv_ic_entry_t *ic,
  const char *interned, ant_value_t *out
) {
  if (target == js->global && sv_global_lexical(js, interned)) return false;
  return sv_global_ic_try_fill(target, ic, interned, out);
}

static __attribute__((noinline)) ant_value_t sv_global_get_interned_miss(
  ant_t *js, const char *interned, sv_ic_entry_t *ic
) {
  ant_value_t out = js_mkundef();
  ant_value_t target = js->global;

  ant_global_lexical_t *lex = sv_global_lexical(js, interned);
  if (lex) return sv_global_lexical_get(js, lex);
  if (sv_global_ic_try_fill(target, ic, interned, &out)) return out;

  if (sv_global_prop_is_accessor(target, interned))
    return js_getprop_fallback(js, target, interned);

  ant_value_t val = lkp_interned_val(js, target, interned);
  if (is_undefined(val)) val = js_getprop_fallback(js, target, interned);

  return val;
}

static inline ant_value_t sv_global_get_interned_ic(
  ant_t *js, const char *interned, sv_func_t *func, uint8_t *ip
) {
  ant_value_t out = js_mkundef();
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  if (sv_global_ic_try_get_hit(js->global, ic, interned, &out)) return out;
  return sv_global_get_interned_miss(js, interned, ic);
}

static inline ant_value_t sv_eval_global_get_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  sv_func_t *func, uint8_t *ip, bool *found
) {
  ant_value_t out = js_mkundef();
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  ant_value_t target = env;

  if (
    sv_global_ic_try_get_hit(target, ic, interned, &out) || 
    sv_global_ic_try_fill_unshadowed(js, target, ic, interned, &out)
  ) {
    if (found) *found = true;
    return out;
  }

  bool resolved = sv_env_try_get_interned(js, env, interned, len, &out);
  if (found) *found = resolved;

  return out;
}

static inline ant_value_t sv_op_get_global(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t super_val = sv_vm_get_super_val(vm);
  
  if (a->len == 5 && memcmp(a->str, "super", 5) == 0 && vtype(super_val) != kTypeUndefined) {
    ant_value_t sv = super_val;
    vm->stack[vm->sp++] = sv;
    return sv;
  }
  
  ant_value_t val = sv_global_get_interned_ic(js, a->str, func, ip);
  if (is_err(val)) return val;
  
  if (
    is_undefined(val) && !lkp_interned(js->global, a->str).obj &&
    !sv_global_lexical(js, a->str)
  ) return js_mkerr_typed(js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)a->len, a->str);

  vm->stack[vm->sp++] = val;
  return val;
}

static inline ant_value_t sv_op_get_global_undef(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t val = sv_global_get_interned_ic(js, a->str, func, ip);
  
  if (is_err(val)) return val;
  vm->stack[vm->sp++] = val;
  
  return val;
}

static inline ant_value_t sv_op_put_global(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t val = vm->stack[--vm->sp];
  return sv_global_put(js, a->str, a->len, val, sv_frame_is_strict(frame));
}

static inline ant_value_t sv_op_get_eval_global(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  bool found = false;
  ant_value_t val = sv_eval_global_get_interned_ic(
    js, sv_frame_eval_env(js, frame),
    a->str, a->len, func, ip, &found);
  if (!found) return js_mkerr_typed(
    js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)a->len, a->str);
  if (is_err(val)) return val;
  vm->stack[vm->sp++] = val;
  return val;
}

static inline ant_value_t sv_op_get_eval_global_undef(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t val = sv_eval_global_get_interned_ic(
    js, sv_frame_eval_env(js, frame),
    a->str, a->len, func, ip, NULL);
  if (is_err(val)) return val;
  vm->stack[vm->sp++] = val;
  return val;
}

static inline ant_value_t sv_op_put_eval_global(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t result = sv_env_put(
    js, sv_frame_eval_env(js, frame),
    a->str, a->len, vm->stack[vm->sp - 1], sv_frame_is_strict(frame));
  vm->sp--;
  return result;
}

#endif
