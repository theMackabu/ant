#ifndef SV_GLOBALS_H
#define SV_GLOBALS_H

#include "silver/engine.h"
#include "errors.h"
#include "shapes.h"
#include "eval_env.h"
#include "modules/regex.h"

static inline bool sv_global_try_store_own_data(
  ant_t *js, ant_value_t obj, const char *interned, uint32_t len,
  ant_value_t val
) {
  if (!is_object_type(obj) || !interned) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return false;
  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (slot < 0 || (uint32_t)slot >= ptr->prop_count) return false;

  const ant_shape_prop_t *prop =
    ant_shape_prop_at(ptr->shape, (uint32_t)slot);
  if (!prop || prop->has_getter || prop->has_setter ||
      (prop->attrs & ANT_PROP_ATTR_WRITABLE) == 0) return false;

  regexp_note_property_write(interned, len);
  ant_object_prop_set_unchecked(ptr, (uint32_t)slot, val);
  gc_write_barrier(js, ptr, val);
  return true;
}

static inline bool sv_env_try_get_interned(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len, ant_value_t *out
) {
  ant_value_t current = env;
  while (is_object_type(current)) {
    if (current == js->global) {
      if (lkp_proto(js, current, interned, len) == 0) return false;
      if (out) *out = js_getprop_fallback(js, current, interned);
      return true;
    }

    if (sv_eval_env_try_get(js, current, interned, len, out)) return true;

    ant_offset_t off = lkp_interned(js, current, interned);
    if (off != 0) {
      if (out) *out = js_propref_load(js, off);
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
  if (env == js->global) {
    if (is_strict && lkp_proto(js, js->global, str, len) == 0)
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)len, str);
    if (sv_global_try_store_own_data(
          js, js->global, str, len, val)) return val;
    return setprop_interned(js, js->global, str, len, val);
  }

  ant_value_t target = js->global;
  ant_value_t current = env;
  bool found = false;

  while (is_object_type(current)) {
    if (current == js->global) {
      found = lkp_proto(js, current, str, len) != 0;
      break;
    }

    ant_value_t binding_result;
    if (sv_eval_env_try_put(js, current, str, len, val, &binding_result)) return binding_result;
    if (lkp_interned(js, current, str) != 0) {
      target = current;
      found = true;
      break;
    }
    current = js_get_proto(js, current);
  }

  if (!found && is_strict)
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)len, str);

  if (sv_global_try_store_own_data(js, target, str, len, val)) return val;
  return setprop_interned(js, target, str, len, val);
}

static inline ant_value_t sv_env_delete(
  ant_t *js, ant_value_t env, const char *str, uint32_t len
) {
  if (env == js->global)
    return js_delete_prop(js, js->global, str, len);

  ant_value_t current = env;
  while (is_object_type(current)) {
    if (current == js->global) {
      if (lkp_proto(js, current, str, len) == 0) return js_true;
      return js_delete_prop(js, current, str, len);
    }
    if (sv_eval_env_has_binding(current, str, len)) return js_false;
    if (lkp_interned(js, current, str) != 0)
      return js_delete_prop(js, current, str, len);
    current = js_get_proto(js, current);
  }
  return js_true;
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

static inline ant_value_t sv_global_get_interned_ic(
  ant_t *js,
  const char *interned,
  sv_func_t *func,
  uint8_t *ip
) {
  ant_value_t out = js_mkundef();
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  ant_value_t target = js->global;
  
  if (sv_global_ic_try_get_hit(target, ic, interned, &out)) return out;
  if (sv_global_ic_try_fill(target, ic, interned, &out)) return out;

  if (sv_global_prop_is_accessor(target, interned))
    return js_getprop_fallback(js, target, interned);

  ant_value_t val = lkp_interned_val(js, target, interned);
  if (is_undefined(val)) val = js_getprop_fallback(js, target, interned);

  return val;
}

static inline ant_value_t sv_eval_global_get_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  sv_func_t *func, uint8_t *ip, bool *found
) {
  ant_value_t out = js_mkundef();
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  ant_value_t target = env;

  if (sv_global_ic_try_get_hit(target, ic, interned, &out) ||
      sv_global_ic_try_fill(target, ic, interned, &out)) {
    if (found) *found = true;
    return out;
  }

  bool resolved = sv_env_try_get_interned(
    js, env, interned, len, &out
  );
  if (found) *found = resolved;
  return out;
}

static inline ant_value_t sv_op_get_global(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t super_val = sv_vm_get_super_val(vm);
  if (a->len == 5 && memcmp(a->str, "super", 5) == 0 && vtype(super_val) != T_UNDEF) {
    ant_value_t sv = super_val;
    vm->stack[vm->sp++] = sv;
    return sv;
  }
  ant_value_t val = sv_global_get_interned_ic(js, a->str, func, ip);
  if (is_undefined(val) && lkp_interned(js, js->global, a->str) == 0) return js_mkerr_typed(
    js, JS_ERR_REFERENCE, "'%.*s' is not defined",
    (int)a->len, a->str
  );
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
  if (sv_frame_is_strict(frame) && lkp_interned(js, js->global, a->str) == 0)
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined",
      (int)a->len, a->str);
  ant_value_t val = vm->stack[--vm->sp];
  if (sv_global_try_store_own_data(
        js, js->global, a->str, a->len, val)) return val;
  return setprop_interned(js, js->global, a->str, a->len, val);
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
  if (!found)
    return js_mkerr_typed(
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
  return sv_env_put(
    js, sv_frame_eval_env(js, frame),
    a->str, a->len, vm->stack[--vm->sp], sv_frame_is_strict(frame));
}

#endif
