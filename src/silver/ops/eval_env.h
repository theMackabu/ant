#ifndef SV_EVAL_ENV_H
#define SV_EVAL_ENV_H

#include "silver/engine.h"

typedef struct sv_eval_env_state {
  const sv_eval_scope_t *scope;
  uint32_t cell_count;
  ant_value_t arguments_obj;
  sv_upvalue_t *cells[];
} sv_eval_env_state_t;

static inline bool sv_eval_binding_visible(const sv_runtime_binding_t *binding) {
  return binding && binding->name && binding->len > 0 && binding->name[0] != '\x01';
}

static inline sv_eval_env_state_t *sv_eval_env_state(ant_value_t env) {
  if (!is_object_type(env)) return NULL;
  return (sv_eval_env_state_t *)ant_object_eval_env_state(js_obj_ptr(js_as_obj(env)));
}

static inline bool sv_eval_env_state_attach(ant_value_t env, sv_eval_env_state_t *state) {
  ant_object_t *obj = js_obj_ptr(js_as_obj(env));
  if (!obj || !state) return false;

  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(obj);
  if (!sidecar) return false;
  sidecar->eval_env_state = state;

  return true;
}

static inline sv_upvalue_t *sv_eval_capture_upvalue(sv_vm_t *vm, ant_value_t *slot) {
  if (!vm || !slot) return NULL;

  sv_upvalue_t **pp = &vm->open_upvalues;
  while (*pp && (*pp)->location > slot) pp = &(*pp)->next;
  if (*pp && (*pp)->location == slot) return *pp;

  sv_upvalue_t *uv = js_upvalue_alloc();
  if (!uv) return NULL;

  uv->location = slot;
  uv->next = *pp;
  *pp = uv;

  return uv;
}

static inline sv_upvalue_t *sv_eval_capture_binding(
  sv_vm_t *vm, sv_frame_t *frame,
  const sv_runtime_binding_t *binding
) {
  if (!vm || !frame || !frame->func || !binding) return NULL;
  switch (binding->kind) {
    case SV_EVAL_BIND_PARAM:
      if (!frame->bp || (int)binding->index >= sv_frame_arg_slots(frame)) return NULL;
      return sv_eval_capture_upvalue(vm, &frame->bp[binding->index]);
    case SV_EVAL_BIND_LOCAL:
      if (!frame->lp || binding->index >= (uint16_t)frame->func->max_locals) return NULL;
      return sv_eval_capture_upvalue(vm, &frame->lp[binding->index]);
    case SV_EVAL_BIND_UPVALUE:
      if (!frame->upvalues || binding->index >= (uint16_t)frame->upvalue_count) return NULL;
      return frame->upvalues[binding->index];
    default: return NULL;
  }
}

static inline sv_eval_env_state_t *sv_eval_env_state_create(
  sv_vm_t *vm, sv_frame_t *frame, const sv_eval_scope_t *scope
) {
  if (!vm || !frame || !scope) return NULL;
  size_t size = sizeof(sv_eval_env_state_t) + (size_t)scope->count * sizeof(sv_upvalue_t *);
  
  sv_eval_env_state_t *state = calloc(1, size);
  if (!state) return NULL;

  state->scope = scope;
  state->cell_count = scope->count;
  state->arguments_obj = frame->arguments_obj;

  for (uint32_t i = 0; i < scope->count; i++) {
    state->cells[i] = sv_eval_capture_binding(vm, frame, &scope->bindings[i]);
    if (!state->cells[i]) { free(state); return NULL; }
  }
  
  return state;
}

static inline const sv_runtime_binding_t *sv_eval_env_find_binding(
  const sv_eval_env_state_t *state, const char *name, uint32_t len
) {
  if (!state || !state->scope || !name) return NULL;
  for (uint32_t i = 0; i < state->scope->count; i++) {
    const sv_runtime_binding_t *binding = &state->scope->bindings[i];
    if (
      sv_eval_binding_visible(binding) && binding->len == len &&
      memcmp(binding->name, name, len) == 0
    ) return binding;
  }
  return NULL;
}

static inline bool sv_eval_binding_load(
  const sv_eval_env_state_t *state,
  const sv_runtime_binding_t *binding, ant_value_t *out
) {
  if (!state || !binding || !out || !state->scope) return false;
  ptrdiff_t index = binding - state->scope->bindings;
  
  if (index < 0 || (uint32_t)index >= state->cell_count) return false;
  sv_upvalue_t *uv = state->cells[index];
  
  if (!uv) return false;
  *out = *uv->location;
  
  return true;
}

static inline bool sv_eval_binding_store(
  ant_t *js, const sv_eval_env_state_t *state,
  const sv_runtime_binding_t *binding, ant_value_t value
) {
  if (!state || !binding || binding->is_const || !state->scope) return false;
  ptrdiff_t index = binding - state->scope->bindings;
  
  if (index < 0 || (uint32_t)index >= state->cell_count) return false;
  sv_upvalue_t *uv = state->cells[index];
  
  if (!uv) return false;
  *uv->location = value;
  gc_upvalue_write_barrier(js, uv, value);
  
  if (
    binding->kind == SV_EVAL_BIND_PARAM &&
    vtype(state->arguments_obj) != T_UNDEF
  ) js_arguments_sync_slot(js, state->arguments_obj, binding->index, value);
    
  return true;
}

static inline bool sv_eval_env_try_get(
  ant_t *js, ant_value_t env, const char *name, uint32_t len, ant_value_t *out
) {
  sv_eval_env_state_t *state = sv_eval_env_state(env);
  const sv_runtime_binding_t *binding =
    sv_eval_env_find_binding(state, name, len);
    
  if (!binding) return false;
  if (!sv_eval_binding_load(state, binding, out)) *out = js_mkundef();
  
  else if (is_empty_slot(*out)) *out = js_mkerr_typed(
    js, JS_ERR_REFERENCE,
    "Cannot access '%.*s' before initialization", (int)len, name
  );
  
  return true;
}

static inline bool sv_eval_env_try_put(
  ant_t *js, ant_value_t env, const char *name, uint32_t len,
  ant_value_t value, ant_value_t *out
) {
  sv_eval_env_state_t *state = sv_eval_env_state(env);
  const sv_runtime_binding_t *binding =
    sv_eval_env_find_binding(state, name, len);
  if (!binding) return false;

  ant_value_t current;
  if (!sv_eval_binding_load(state, binding, &current))
    *out = js_mkerr(js, "invalid direct eval binding");
  else if (is_empty_slot(current)) *out = js_mkerr_typed(
    js, JS_ERR_REFERENCE,
    "Cannot access '%.*s' before initialization", (int)len, name
  );
  else if (binding->is_const)
    *out = js_mkerr_typed(js, JS_ERR_TYPE, "assignment to constant variable");
  else if (!sv_eval_binding_store(js, state, binding, value))
    *out = js_mkerr(js, "invalid direct eval binding");  
  else *out = value;

  return true;
}

static inline bool sv_eval_env_has_binding(ant_value_t env, const char *name, uint32_t len) {
  return sv_eval_env_find_binding(sv_eval_env_state(env), name, len) != NULL;
}

#endif
