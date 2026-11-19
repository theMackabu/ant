#include <stdlib.h>
#include "gc/objects.h"
#include "ops/eval_env.h"
#include "silver/eval_env.h"
#include "ops/coercion.h"

ant_value_t sv_eval_read_import(
  ant_t *js, ant_value_t value, const char *name, uint32_t len, bool is_default
) {
  return is_default 
    ? sv_import_default_value(value) 
    : sv_import_named_value(js, value, name, len);
}

void sv_eval_env_gc_mark(ant_t *js, ant_object_t *obj) {
  if (!obj) return;
  
  sv_eval_env_state_t *state = sv_eval_env_state(js_obj_from_ptr(obj));
  if (!state) return;
  
  if (state->func) gc_mark_value(js, mkref(kTypeFunctionInfo, state->func));
  gc_mark_value(js, state->arguments_obj);
  gc_mark_upvalue_cells(js, state->cells, state->cell_count);
}

void sv_eval_env_gc_free(ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  if (!sidecar || !sidecar->eval_env_state) return;
  
  free(sidecar->eval_env_state);
  sidecar->eval_env_state = NULL;
}

bool sv_global_lexical_lookup(
  ant_t *js, const char *name, size_t len, ant_value_t *out
) {
  if (!js || !name || !js->global_lexical_count) return false;
  const char *interned = intern_find(js, name, len);
  
  if (!interned) return false;
  const ant_global_lexical_t *lex = sv_global_lexical(js, interned);
  
  if (!lex) return false;
  if (out) *out = lex->value;
  
  return true;
}

void sv_eval_env_each_with(
  ant_t *js, ant_value_t env, const char *name, uint32_t len,
  bool declared_by_eval, sv_eval_with_marker_fn fn, void *ctx
) {
  const char *interned = intern_find(js, name, len);
  for (ant_value_t cur = env; is_object_type(cur) && cur != js->global; cur = js_get_proto(js, cur)) {
    const sv_eval_env_state_t *state = sv_eval_env_state(cur);
    if (declared_by_eval && state && state->is_variable) return;
    for (uint32_t i = 0; state && state->scope && i < state->scope->count; i++) {
      const sv_runtime_binding_t *binding = &state->scope->bindings[i];
      if (!binding->name || binding->len == 0) continue;
      if (sv_eval_name_is_with_marker(binding->name, binding->len)) fn(ctx, binding->name, binding->len);
      else if (binding->len == len && memcmp(binding->name, name, len) == 0) return;
    }
    if (interned && lkp_interned(cur, interned).obj) return;
  }
}

const sv_eval_scope_flags_t *sv_eval_env_flags(ant_value_t env) {
  static const sv_eval_scope_flags_t none = {0};
  if (!is_object_type(env)) return &none;
  const sv_eval_env_state_t *state = sv_eval_env_state(env);
  return state && state->scope ? &state->scope->flags : &none;
}

bool sv_eval_env_shadows_global(ant_t *js, ant_value_t env, const char *name, uint32_t len) {
  const char *interned = intern_string(js, name, len);
  
  for (ant_value_t cur = env; is_object_type(cur) && cur != js->global; cur = js_get_proto(js, cur)) {
    if (sv_eval_env_find_binding(sv_eval_env_state(cur), name, len)) return true;
    if (interned && lkp_interned(cur, interned).obj) return true;
  }
  
  return false;
}
