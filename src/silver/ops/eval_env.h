#ifndef SV_EVAL_ENV_H
#define SV_EVAL_ENV_H

#include "silver/engine.h"
#include "ptr.h"

enum { SV_EVAL_ENV_NATIVE_TAG = 0x45564E56u }; // EVNV

typedef struct {
  sv_vm_t *vm;
  const sv_eval_scope_t *scope;
  int frame_index;
} sv_eval_env_state_t;

static inline bool sv_eval_binding_visible(const sv_runtime_binding_t *binding) {
  return binding && binding->name && binding->len > 0 && binding->name[0] != '\x01';
}

static inline sv_frame_t *sv_eval_env_frame(const sv_eval_env_state_t *state) {
  if (!state || !state->vm || state->frame_index < 0 ||
      state->frame_index > state->vm->fp) return NULL;
  return &state->vm->frames[state->frame_index];
}

static inline sv_eval_env_state_t *sv_eval_env_state(ant_value_t env) {
  if (!is_object_type(env)) return NULL;
  return (sv_eval_env_state_t *)js_get_native(
    js_as_obj(env), SV_EVAL_ENV_NATIVE_TAG);
}

static inline const sv_runtime_binding_t *sv_eval_env_find_binding(
  const sv_eval_env_state_t *state, const char *name, uint32_t len
) {
  if (!state || !state->scope || !name) return NULL;
  for (uint32_t i = 0; i < state->scope->count; i++) {
    const sv_runtime_binding_t *binding = &state->scope->bindings[i];
    if (sv_eval_binding_visible(binding) && binding->len == len &&
        memcmp(binding->name, name, len) == 0) return binding;
  }
  return NULL;
}

static inline bool sv_eval_binding_load(
  sv_frame_t *frame, const sv_runtime_binding_t *binding, ant_value_t *out
) {
  if (!frame || !binding || !out) return false;
  switch (binding->kind) {
    case SV_EVAL_BIND_PARAM:
      *out = sv_frame_get_arg_value(frame, binding->index);
      return true;
    case SV_EVAL_BIND_LOCAL:
      if (!frame->lp || binding->index >= (uint16_t)frame->func->max_locals)
        return false;
      *out = frame->lp[binding->index];
      return true;
    case SV_EVAL_BIND_UPVALUE: {
      if (!frame->upvalues || binding->index >= (uint16_t)frame->upvalue_count)
        return false;
      sv_upvalue_t *uv = frame->upvalues[binding->index];
      if (!uv) return false;
      *out = *uv->location;
      return true;
    }
    default:
      return false;
  }
}

static inline bool sv_eval_binding_store(
  ant_t *js, sv_frame_t *frame,
  const sv_runtime_binding_t *binding, ant_value_t value
) {
  if (!frame || !binding || binding->is_const) return false;
  switch (binding->kind) {
    case SV_EVAL_BIND_PARAM:
      sv_frame_set_arg_value(js, frame, binding->index, value);
      return true;
    case SV_EVAL_BIND_LOCAL:
      if (!frame->lp || binding->index >= (uint16_t)frame->func->max_locals)
        return false;
      frame->lp[binding->index] = value;
      return true;
    case SV_EVAL_BIND_UPVALUE: {
      if (!frame->upvalues || binding->index >= (uint16_t)frame->upvalue_count)
        return false;
      sv_upvalue_t *uv = frame->upvalues[binding->index];
      if (!uv) return false;
      *uv->location = value;
      gc_upvalue_write_barrier(js, uv, value);
      return true;
    }
    default:
      return false;
  }
}

static inline bool sv_eval_env_try_get(
  ant_t *js, ant_value_t env, const char *name, uint32_t len, ant_value_t *out
) {
  sv_eval_env_state_t *state = sv_eval_env_state(env);
  const sv_runtime_binding_t *binding =
    sv_eval_env_find_binding(state, name, len);
  if (!binding) return false;
  if (!sv_eval_binding_load(sv_eval_env_frame(state), binding, out))
    *out = js_mkundef();
  else if (is_empty_slot(*out))
    *out = js_mkerr_typed(
      js, JS_ERR_REFERENCE,
      "Cannot access '%.*s' before initialization", (int)len, name);
  return true;
}

#endif
