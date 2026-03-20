#ifndef SV_GLOBALS_H
#define SV_GLOBALS_H

#include "silver/engine.h"
#include "errors.h"
#include "shapes.h"

static inline ant_value_t sv_global_get(ant_t *js, const char *str, uint32_t len) {
  if (!str) return js_mkundef();
  const char *interned = intern_string(str, len);
  if (!interned) return js_mkundef();
  return lkp_interned_val(js, js->global, interned);
}

static inline sv_ic_entry_t *sv_global_ic_slot_for_ip(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return NULL;
  uint16_t ic_idx = sv_get_u16(ip + 5);
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return NULL;
  return &func->ic_slots[ic_idx];
}

static inline ant_object_t *sv_global_obj_ptr(ant_t *js) {
  if (!is_object_type(js->global)) return NULL;
  return js_obj_ptr(js_as_obj(js->global));
}

static inline bool sv_global_ic_try_get_hit(
  ant_t *js,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
) {
  if (!ic || !interned) return false;

  ant_object_t *gptr = sv_global_obj_ptr(js);
  if (!gptr || gptr->is_exotic || !gptr->shape) return false;
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

static inline bool sv_global_ic_try_fill(
  ant_t *js,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
) {
  if (!ic || !interned) return false;

  ant_object_t *gptr = sv_global_obj_ptr(js);
  if (!gptr || gptr->is_exotic || !gptr->shape) return false;

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
  if (sv_global_ic_try_get_hit(js, ic, interned, &out)) return out;
  if (sv_global_ic_try_fill(js, ic, interned, &out)) return out;
  return lkp_interned_val(js, js->global, interned);
}

static inline ant_value_t sv_op_get_global(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t super_val = sv_vm_get_super_val(vm);
  if (a->len == 5 && memcmp(a->str, "super", 5) == 0 &&
      vtype(super_val) != T_UNDEF) {
    ant_value_t sv = super_val;
    vm->stack[vm->sp++] = sv;
    return sv;
  }
  ant_value_t val = sv_global_get_interned_ic(js, a->str, func, ip);
  if (is_undefined(val))
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined",
      (int)a->len, a->str);
  vm->stack[vm->sp++] = val;
  return val;
}

static inline void sv_op_get_global_undef(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  vm->stack[vm->sp++] = sv_global_get_interned_ic(js, a->str, func, ip);
}

static inline ant_value_t sv_op_put_global(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  if (sv_frame_is_strict(frame) && lkp_interned(js, js->global, a->str, a->len) == 0)
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined",
      (int)a->len, a->str);
  ant_value_t key = js_mkstr(js, a->str, a->len);
  return js_setprop(js, js->global, key, vm->stack[--vm->sp]);
}

#endif
