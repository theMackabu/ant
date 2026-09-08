#ifndef SV_EVAL_ENV_H
#define SV_EVAL_ENV_H

#include "silver/engine.h"
#include "gc/roots.h"
#include "silver/eval_env.h"

typedef struct sv_eval_env_state {
  const sv_eval_scope_t *scope;
  uint32_t cell_count;
  ant_value_t arguments_obj;
  bool is_variable;
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

  sv_upvalue_t *uv = js_upvalue_alloc(vm->js);
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
  switch (binding->kind & SV_EVAL_BIND_KIND_MASK) {
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
    (binding->kind & SV_EVAL_BIND_KIND_MASK) == SV_EVAL_BIND_PARAM &&
    vtype(state->arguments_obj) != kTypeUndefined
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
  
  else if (binding->kind & (SV_EVAL_BIND_IMPORT_DEFAULT | SV_EVAL_BIND_IMPORT_NAMED))
    *out = sv_eval_read_import(js, *out, binding->import_name, binding->import_len,
      (binding->kind & SV_EVAL_BIND_IMPORT_DEFAULT) != 0);

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

static inline ant_value_t sv_eval_arguments(ant_t *js, sv_vm_t *vm, sv_frame_t *frame) {
  if (vtype(frame->arguments_obj) == kTypeUndefined) {
    int mapped = sv_frame_is_strict(frame) ? 0 : frame->func->param_count;
    if (mapped > frame->argc) mapped = frame->argc;
    frame->arguments_obj = js_create_arguments_object(js, vm, frame->callee, frame,
      frame->argc, mapped, sv_frame_is_strict(frame));
  }
  return frame->arguments_obj;
}

static inline ant_value_t sv_eval_capture_env(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint32_t scope_index
) {
  const sv_eval_scope_t *scope = sv_func_eval_scope(frame->func, scope_index);
  ant_value_t parent = sv_frame_eval_env(js, frame);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, parent);
  ant_value_t env = js_mkobj(js);
  if (is_err(env)) { GC_ROOT_RESTORE(js, mark); return env; }
  GC_ROOT_PIN(js, env);
  js_set_proto_wb(js, env, parent);
  sv_eval_env_state_t *parent_state = sv_eval_env_state(parent);
  if (!frame->func->is_arrow && !frame->func->is_eval &&
      !(parent_state && parent_state->is_variable)) {
    ant_value_t arguments = sv_eval_arguments(js, vm, frame);
    if (is_err(arguments)) { GC_ROOT_RESTORE(js, mark); return arguments; }
    js_set(js, env, "arguments", arguments);
  }
  sv_eval_env_state_t *state = sv_eval_env_state_create(vm, frame, scope);
  if (!state || !sv_eval_env_state_attach(env, state)) {
    free(state);
    env = js_mkerr(js, "failed to capture eval environment");
  }
  GC_ROOT_RESTORE(js, mark);
  return env;
}

static inline ant_value_t sv_eval_init_variable_env(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  ant_value_t env = js_mkobj(js);
  if (is_err(env)) return env;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, env);
  js_set_proto_wb(js, env, sv_frame_eval_env(js, frame));
  sv_eval_env_state_t *state = calloc(1, sizeof(*state));
  if (!state || !sv_eval_env_state_attach(env, state)) {
    free(state);
    env = js_mkerr(js, "failed to create eval variable environment");
  } else {
    state->arguments_obj = js_mkundef();
    state->is_variable = true;
    frame->eval_env = env;
    if (!frame->func->is_arrow) {
      ant_value_t arguments = sv_eval_arguments(js, vm, frame);
      if (is_err(arguments)) { GC_ROOT_RESTORE(js, mark); return arguments; }
      js_set(js, env, "arguments", arguments);
    }
  }
  GC_ROOT_RESTORE(js, mark);
  return env;
}

static inline ant_value_t sv_eval_declare_vars(ant_t *js, sv_func_t *func, ant_value_t env) {
  sv_func_metadata_t *metadata = sv_func_metadata(func);
  if (!metadata || !metadata->eval_var_count) return js_mkundef();
  ant_value_t target = env;
  while (is_object_type(target) && target != js->global) {
    sv_eval_env_state_t *state = sv_eval_env_state(target);
    if (state && state->is_variable) break;
    target = js_get_proto(js, target);
  }
  if (!is_object_type(target)) return js_mkerr(js, "missing eval variable environment");

  // Check every declaration before publishing any names or executing eval code.
  for (uint32_t i = 0; i < metadata->eval_var_count; i++) {
    sv_eval_decl_t *name = &metadata->eval_vars[i];
    for (ant_value_t current = env; current != target; current = js_get_proto(js, current)) {
      const sv_runtime_binding_t *binding = sv_eval_env_find_binding(
        sv_eval_env_state(current), name->str, name->len);
      if (binding && (binding->kind & SV_EVAL_BIND_LEXICAL) && !name->annex_b)
        return js_mkerr_typed(js, JS_ERR_SYNTAX,
          "Identifier '%.*s' has already been declared", (int)name->len, name->str);
    }
  }
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, env);
  GC_ROOT_PIN(js, target);
  ant_value_t result = js_mkundef();
  for (uint32_t i = 0; i < metadata->eval_var_count; i++) {
    sv_eval_decl_t *name = &metadata->eval_vars[i];
    bool exists = false;
    for (ant_value_t current = env; current != target; current = js_get_proto(js, current)) {
      const sv_runtime_binding_t *binding = sv_eval_env_find_binding(
        sv_eval_env_state(current), name->str, name->len);
      if (binding && !(binding->kind & SV_EVAL_BIND_CATCH)) { exists = true; break; }
    }
    if (exists || lkp_interned(target, name->str).obj) continue;
    result = setprop_interned(js, target, name->str, name->len, js_mkundef());
    if (is_err(result)) break;
  }
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static inline ant_value_t sv_eval_store_function(
  ant_t *js, ant_value_t env, const char *name, uint32_t len, ant_value_t value
) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, env);
  GC_ROOT_PIN(js, value);
  ant_value_t result = js_mkundef();
  for (ant_value_t current = env; is_object_type(current); current = js_get_proto(js, current)) {
    sv_eval_env_state_t *state = sv_eval_env_state(current);
    if (current == js->global || (state && state->is_variable)) {
      result = setprop_interned(js, current, name, len, value);
      break;
    }
    const sv_runtime_binding_t *binding = sv_eval_env_find_binding(state, name, len);
    if (binding && (binding->kind & SV_EVAL_BIND_LEXICAL)) break;
    if (binding && !(binding->kind & SV_EVAL_BIND_CATCH) &&
        sv_eval_env_try_put(js, current, name, len, value, &result)) break;
  }
  GC_ROOT_RESTORE(js, mark);
  return result;
}

#endif
