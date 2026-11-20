#pragma once

#include "silver/engine.h"
#include "errors.h"
#include "protectors.h"
#include "shapes.h"
#include "eval_env.h"
#include "modules/regex.h"

bool sv_global_try_store_own_data(
  ant_t *js, ant_value_t obj, const char *interned, uint32_t len, ant_value_t val
);

__attribute__((noinline)) ant_value_t sv_global_put_slow(
  ant_t *js, const char *interned, uint32_t len, ant_value_t val, bool is_strict
);

static inline __attribute__((always_inline)) ant_value_t sv_global_put(
  ant_t *js, const char *interned, uint32_t len, ant_value_t val, bool is_strict
) {
  ant_global_lexical_t *lex = sv_global_lexical(js, interned);
  if (lex) return sv_global_lexical_set(js, lex, val, false);
  if (sv_global_try_store_own_data(js, js->global, interned, len, val)) return val;
  return sv_global_put_slow(js, interned, len, val, is_strict);
}

static __attribute__((noinline, cold)) ant_value_t sv_sandbox_global_delete(
  ant_t *js, const char *interned, uint32_t len
) {
  ant_value_t deleted = js_delete_prop(js, js->global_sandbox, interned, len);
  if (is_err(deleted) || !js_truthy(js, deleted)) return deleted;
  return js_delete_prop(js, js->global, interned, len);
}

static inline ant_value_t sv_global_delete(ant_t *js, const char *interned, uint32_t len) {
  if (sv_global_lexical(js, interned)) return Ant_Value_False;
  if (sv_global_sandboxed(js)) return sv_sandbox_global_delete(js, interned, len);
  return js_delete_prop(js, js->global, interned, len);
}

static inline bool sv_env_try_get_interned(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len, ant_value_t *out
) {
  ant_value_t current = env;
  
  while (is_object_type(current)) {
    if (current == js->global) {
      if (sv_global_sandboxed(js)) {
        ant_value_t value = Ant_Value_Undefined;
        bool found = sv_sandbox_global_lookup(js, interned, len, &value);
        if (found && out) *out = value;
        return found;
      }
      
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
  if (!str) return Ant_Value_Undefined;

  const char *interned = intern_string(js, str, len);
  if (!interned) return Ant_Value_Undefined;

  ant_value_t val = Ant_Value_Undefined;
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
    if (sv_eval_env_has_binding(current, str, len)) return Ant_Value_False;
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
  ant_t *js, ant_value_t target,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
) {
  if (!ic || !interned) return false;

  ant_object_t *gptr = sv_global_obj_ptr(target);
  if (!gptr || gptr->flags.is_exotic || !gptr->shape) return false;
  if (ic->epoch != js->ic.epoch) return false;
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

bool sv_global_ic_try_fill(
  ant_t *js, ant_value_t target,
  sv_ic_entry_t *ic,
  const char *interned,
  ant_value_t *out
);

static inline bool sv_global_ic_try_fill_unshadowed(
  ant_t *js, ant_value_t target, sv_ic_entry_t *ic,
  const char *interned, ant_value_t *out
) {
  if (target == js->global && sv_global_lexical(js, interned)) return false;
  return sv_global_ic_try_fill(js, target, ic, interned, out);
}

__attribute__((noinline)) ant_value_t sv_global_get_interned_miss(
  ant_t *js, const char *interned, sv_ic_entry_t *ic
);

static inline ant_value_t sv_global_get_interned_ic(
  ant_t *js, const char *interned, sv_func_t *func, uint8_t *ip
) {
  ant_value_t out = Ant_Value_Undefined;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  if (sv_global_ic_try_get_hit(js, js->global, ic, interned, &out)) return out;
  return sv_global_get_interned_miss(js, interned, ic);
}

ant_value_t sv_eval_global_get_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  sv_func_t *func, uint8_t *ip, bool *found
);

ant_value_t sv_eval_global_put_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  ant_value_t val, bool is_strict, sv_ic_entry_t *ic
);

static inline sv_eval_env_state_t *sv_eval_env_object_state(const ant_object_t *env) {
  return (sv_eval_env_state_t *)ant_object_eval_env_state(env);
}

static inline const sv_eval_scope_t *sv_eval_env_object_scope(const ant_object_t *env) {
  const sv_eval_env_state_t *state = sv_eval_env_object_state(env);
  return state ? state->scope : NULL;
}

static inline ant_object_t *sv_eval_env_object_parent(const ant_object_t *env) {
  ant_value_t parent = ant_shape_proto(env->shape);
  return vtype(parent) == kTypeObject ? (ant_object_t *)vptr(parent) : NULL;
}

static inline bool sv_eval_env_ic_live(ant_t *js, ant_value_t env, const sv_ic_entry_t *ic) {
  return ic && ic->epoch == js->ic.epoch && vtype(env) == kTypeObject;
}

static inline bool sv_eval_env_ic_hold(ant_t *js, sv_ic_entry_t *ic, ant_shape_t **slot, sv_ic_flags_t mask, ant_shape_t *shape) {
  if (!(ic->shape_ref_mask & mask)) {
    *slot = NULL;
    if (!shape) return true;
    if (!(ic->shape_ref_mask & SV_IC_UNIT_OWNED) && !sv_ic_shape_ref_register(js, slot)) return false;
    ic->shape_ref_mask |= mask;
  }

  if (*slot == shape) return true;
  if (shape) ant_shape_retain(shape);
  if (*slot) ant_shape_release(*slot);
  *slot = shape;

  return true;
}

static inline void sv_eval_env_ic_drop(ant_t *js, sv_ic_entry_t *ic) {
  if (!(ic->shape_ref_mask & (SV_IC_SHAPE_REF_ADD_FROM | SV_IC_SHAPE_REF_ADD_TO))) return;
  sv_eval_env_ic_hold(js, ic, &ic->guard.eval.absent_shape, SV_IC_SHAPE_REF_ADD_FROM, NULL);
  sv_eval_env_ic_hold(js, ic, &ic->guard.eval.holder_shape, SV_IC_SHAPE_REF_ADD_TO, NULL);
}

static inline __attribute__((always_inline)) sv_upvalue_t *sv_eval_env_ic_cell(ant_value_t env, const sv_ic_entry_t *ic) {
  const sv_eval_env_state_t *state = sv_eval_env_object_state((ant_object_t *)vptr(env));
  if (!state || (uintptr_t)state->scope != ic->cached_aux || ic->cached_index >= state->cell_count) return NULL;

  sv_upvalue_t *cell = state->cells[ic->cached_index];
  return cell && !is_empty_slot(*cell->location) ? cell : NULL;
}

static inline __attribute__((always_inline)) ant_object_t *sv_eval_env_ic_holder(ant_value_t env, const sv_ic_entry_t *ic) {
  if (ic->eval_depth > SV_EVAL_IC_MAX_DEPTH) return NULL;
  ant_object_t *holder = (ant_object_t *)vptr(env);
  const sv_eval_scope_t *scope = (const sv_eval_scope_t *)ic->cached_aux;

  for (uint8_t level = 0; level < ic->eval_depth; level++) {
    if (sv_eval_env_object_scope(holder) != scope) return NULL;

    bool absent = level == ic->eval_absent_level
      ? holder->shape == ic->guard.eval.absent_shape
      : holder->prop_count == 0;

    if (!absent || !(holder = sv_eval_env_object_parent(holder))) return NULL;
    scope = level ? ic->guard.eval.outer_scope : ic->eval_parent_scope;
  }

  if (holder->shape != ic->cached_shape || holder->flags.is_exotic || ic->cached_index >= holder->prop_count) return NULL;

  return holder;
}

static __attribute__((noinline)) ant_value_t sv_global_undefined_check(ant_t *js, sv_atom_t *a) {
  if (
    !lkp_interned(js->global, a->str).obj && !sv_global_lexical(js, a->str) &&
    !(sv_global_sandboxed(js) && sv_sandbox_has(js, a->str, a->len))
  ) return js_mkerr_typed(js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)a->len, a->str);
  return Ant_Value_Undefined;
}

static inline ant_value_t sv_global_require_defined(ant_t *js, sv_atom_t *a, ant_value_t val) {
  return is_undefined(val) ? sv_global_undefined_check(js, a) : val;
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
  
  ant_value_t val = sv_global_require_defined(js, a, sv_global_get_interned_ic(js, a->str, func, ip));
  if (is_err(val)) return val;

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
  ant_value_t env = sv_frame_eval_env(js, frame);
  ant_value_t val = vm->stack[vm->sp - 1];
  bool strict = sv_frame_is_strict(frame);
  ant_value_t result = env == js->global
    ? sv_global_put(js, a->str, a->len, val, strict)
    : sv_eval_global_put_interned_ic(js, env, a->str, a->len, val, strict, sv_global_ic_slot_for_ip(func, ip));
  vm->sp--;
  return result;
}

#ifdef SV_OPS_IMPLEMENTATION

bool sv_global_try_store_own_data(
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

ant_value_t sv_global_put_slow(
  ant_t *js, const char *interned, uint32_t len, ant_value_t val, bool is_strict
) {
  bool sandboxed = sv_global_sandboxed(js);
  bool declared = sandboxed
    ? sv_sandbox_has(js, interned, len) || lkp_proto(js, js->global, interned, len).obj
    : lkp_interned(js->global, interned).obj != NULL;
  
  if (!declared && (is_strict || sv_is_strict_context(js)))
    return js_mkerr_typed(js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int)len, interned);
  
  return setprop_interned(js, sandboxed ? js->global_sandbox : js->global, interned, len, val);
}

bool sv_global_ic_try_fill(
  ant_t *js, ant_value_t target,
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

  ic->eval_depth = SV_EVAL_IC_NONE;
  ic->cached_shape = gptr->shape;
  ic->cached_holder = gptr;
  ic->cached_index = idx;
  ic->epoch = js->ic.epoch;
  *out = ant_object_prop_get_unchecked(gptr, idx);
  return true;
}

ant_value_t sv_global_get_interned_miss(
  ant_t *js, const char *interned, sv_ic_entry_t *ic
) {
  ant_value_t out = Ant_Value_Undefined;
  ant_value_t target = js->global;

  if (sv_global_sandboxed(js)) {
    (void)sv_sandbox_global_lookup(js, interned, (uint32_t)strlen(interned), &out);
    return out;
  }

  ant_global_lexical_t *lex = sv_global_lexical(js, interned);
  if (lex) return sv_global_lexical_get(js, lex);
  if (sv_global_ic_try_fill(js, target, ic, interned, &out)) return out;

  if (sv_global_prop_is_accessor(target, interned))
    return js_getprop_fallback(js, target, interned);

  ant_value_t val = lkp_interned_val(js, target, interned);
  if (is_undefined(val)) val = js_getprop_fallback(js, target, interned);

  return val;
}

static bool sv_eval_env_ic_fill_binding(ant_t *js, ant_object_t *target, sv_ic_entry_t *ic, const char *interned, uint32_t len) {
  const sv_eval_env_state_t *state = sv_eval_env_object_state(target);
  const sv_runtime_binding_t *binding = sv_eval_env_find_binding(state, interned, len);
  if (!binding) return false;

  if (binding->kind & (SV_EVAL_BIND_IMPORT_DEFAULT | SV_EVAL_BIND_IMPORT_NAMED | SV_EVAL_BIND_ARGUMENTS)) return true;
  uint32_t index = (uint32_t)(binding - state->scope->bindings);
  if (index >= state->cell_count || !state->cells[index]) return true;

  sv_eval_env_ic_drop(js, ic);
  ic->eval_depth = SV_EVAL_IC_CELL;
  ic->cached_index = index;
  ic->cached_aux = (uintptr_t)state->scope;
  ic->epoch = js->ic.epoch;

  return true;
}

static void sv_eval_env_ic_fill(
  ant_t *js, ant_value_t env, sv_ic_entry_t *ic, const char *interned, uint32_t len, bool for_put
) {
  if (!ic || env == js->global || vtype(env) != kTypeObject) return;

  ant_object_t *global = sv_global_obj_ptr(js->global);
  ant_object_t *holder = (ant_object_t *)vptr(env);
  if (sv_eval_env_ic_fill_binding(js, holder, ic, interned, len)) return;

  const sv_eval_scope_t *scopes[SV_EVAL_IC_MAX_DEPTH] = {0};
  ant_shape_t *absent_shape = NULL;
  uint16_t absent_level = UINT16_MAX;
  uint8_t depth = 0;
  int32_t slot;

  for (;; depth++) {
    if (holder->flags.is_exotic || !holder->shape) return;
    if (depth > 0 && holder != global && sv_eval_env_find_binding(sv_eval_env_object_state(holder), interned, len)) return;
    if ((slot = ant_shape_lookup_interned(holder->shape, interned)) >= 0) break;
    if (holder == global || depth == SV_EVAL_IC_MAX_DEPTH) return;

    if (holder->prop_count) {
      if (absent_shape || ant_shape_is_dictionary(holder->shape)) return;
      absent_level = depth;
      absent_shape = holder->shape;
    }

    scopes[depth] = sv_eval_env_object_scope(holder);
    if (!(holder = sv_eval_env_object_parent(holder))) return;
  }

  if (holder == global && (sv_global_sandboxed(js) || sv_global_lexical(js, interned))) return;
  if (holder != global && sv_eval_env_object_scope(holder)) return;
  if ((uint32_t)slot >= holder->prop_count) return;

  const ant_shape_prop_t *prop = ant_shape_prop_at(holder->shape, (uint32_t)slot);
  if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->has_getter || prop->has_setter) return;
  if (for_put && !(prop->attrs & ANT_PROP_ATTR_WRITABLE)) return;

  bool tracked = holder == global || ant_shape_is_dictionary(holder->shape);
  if (tracked) ant_shape_mark_epoch_guarded(holder->shape);

  ic->eval_depth = SV_EVAL_IC_NONE;
  if (!sv_eval_env_ic_hold(js, ic, &ic->guard.eval.absent_shape, SV_IC_SHAPE_REF_ADD_FROM, absent_shape)) return;
  if (!sv_eval_env_ic_hold(js, ic, &ic->guard.eval.holder_shape, SV_IC_SHAPE_REF_ADD_TO, tracked ? NULL : holder->shape)) return;

  ic->eval_depth = depth;
  ic->eval_absent_level = absent_level;
  ic->cached_shape = holder->shape;
  ic->cached_index = (uint32_t)slot;
  ic->cached_aux = (uintptr_t)scopes[0];
  ic->eval_parent_scope = scopes[1];
  ic->guard.eval.outer_scope = scopes[2];
  ic->epoch = js->ic.epoch;
}

static inline bool sv_eval_env_ic_get(ant_t *js, ant_value_t env, const sv_ic_entry_t *ic, ant_value_t *out) {
  if (!sv_eval_env_ic_live(js, env, ic)) return false;

  if (ic->eval_depth == SV_EVAL_IC_CELL) {
    sv_upvalue_t *cell = sv_eval_env_ic_cell(env, ic);
    if (cell) *out = *cell->location;
    return cell != NULL;
  }

  ant_object_t *holder = sv_eval_env_ic_holder(env, ic);
  if (holder) *out = ant_object_prop_get_unchecked(holder, ic->cached_index);

  return holder != NULL;
}

static inline bool sv_eval_env_ic_put(
  ant_t *js, ant_value_t env, const sv_ic_entry_t *ic, const char *interned, uint32_t len, ant_value_t val
) {
  if (!sv_eval_env_ic_live(js, env, ic)) return false;

  if (ic->eval_depth == SV_EVAL_IC_CELL) {
    if (!sv_eval_env_ic_cell(env, ic)) return false;
    const sv_eval_env_state_t *state = sv_eval_env_object_state((ant_object_t *)vptr(env));
    return sv_eval_binding_store(js, state, &state->scope->bindings[ic->cached_index], val);
  }

  ant_object_t *holder = sv_eval_env_ic_holder(env, ic);
  if (!holder) return false;

  regexp_note_property_write(js, interned, len);
  ant_object_prop_set_unchecked(holder, ic->cached_index, val);
  gc_write_barrier(js, holder, val);
  ant_property_mutation_invalidate(js, holder, interned);

  return true;
}

static __attribute__((noinline)) ant_value_t sv_eval_env_get_interned_miss(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  sv_ic_entry_t *ic, bool *found
) {
  ant_value_t out = Ant_Value_Undefined;
  bool resolved = sv_env_try_get_interned(js, env, interned, len, &out);
  if (found) *found = resolved;
  if (resolved && !is_err(out)) sv_eval_env_ic_fill(js, env, ic, interned, len, false);

  return out;
}

static __attribute__((noinline)) ant_value_t sv_eval_global_get_interned_miss(
  ant_t *js, const char *interned, uint32_t len,
  sv_ic_entry_t *ic, bool *found
) {
  ant_value_t out = Ant_Value_Undefined;
  if (ic) sv_eval_env_ic_drop(js, ic);

  bool resolved =
    sv_global_ic_try_fill_unshadowed(js, js->global, ic, interned, &out) ||
    sv_env_try_get_interned(js, js->global, interned, len, &out);
  if (found) *found = resolved;

  return out;
}

ant_value_t sv_eval_global_get_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  sv_func_t *func, uint8_t *ip, bool *found
) {
  ant_value_t out = Ant_Value_Undefined;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);

  bool hit = env != js->global
    ? sv_eval_env_ic_get(js, env, ic, &out)
    : sv_global_ic_try_get_hit(js, env, ic, interned, &out);

  if (!hit) return env != js->global
    ? sv_eval_env_get_interned_miss(js, env, interned, len, ic, found)
    : sv_eval_global_get_interned_miss(js, interned, len, ic, found);

  if (found) *found = true;
  return out;
}

ant_value_t sv_eval_global_put_interned_ic(
  ant_t *js, ant_value_t env,
  const char *interned, uint32_t len,
  ant_value_t val, bool is_strict, sv_ic_entry_t *ic
) {
  if (sv_eval_env_ic_put(js, env, ic, interned, len, val)) return val;

  ant_value_t result = sv_env_put(js, env, interned, len, val, is_strict);
  if (!is_err(result)) sv_eval_env_ic_fill(js, env, ic, interned, len, true);

  return result;
}

#endif
