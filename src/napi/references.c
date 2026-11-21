#include "gc/roots.h"
#include "napi_internal.h"

static void Scope_Open(napi_env env, struct napi_handle_scope__ *scope) {
  scope->env = env;
  scope->gc_root_mark = gc_root_scope(env->js);
  scope->handle_slots_mark = env->handle_slots_len;
  env->open_handle_scopes++;
}

static void Scope_Close(napi_env env, const struct napi_handle_scope__ *scope) {
  if (env->js) gc_pop_roots(env->js, scope->gc_root_mark);
  env->handle_slots_len = scope->handle_slots_mark;
  if (env->open_handle_scopes > 0) env->open_handle_scopes--;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_ref ref = calloc(1, sizeof(*ref));
  if (!ref) return Napi_OutOfMemory(env);

  ref->env = env;
  ref->value = Napi_Decode(value);
  ref->live = true;
  ref->refcount = initial_refcount;
  ref->ref_val = initial_refcount > 0 ? ref->value : Ant_Value_Undefined;

  ref->next = env->refs;
  if (ref->next) ref->next->prev = ref;
  env->refs = ref;

  *result = ref;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_delete_reference(node_api_basic_env env, napi_ref ref) {
  if (!env || !env->js || !ref) return Napi_InvalidArg(env);

  if (ref->prev) ref->prev->next = ref->next;
  else env->refs = ref->next;
  if (ref->next) ref->next->prev = ref->prev;

  free(ref);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_reference_ref(napi_env env, napi_ref ref, uint32_t *result) {
  if (!env || !env->js || !ref) return Napi_InvalidArg(env);

  if (ref->live) {
    if (ref->refcount == 0) ref->ref_val = ref->value;
    ref->refcount++;
  }

  if (result) *result = ref->refcount;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_reference_unref(napi_env env, napi_ref ref, uint32_t *result) {
  if (!env || !env->js || !ref) return Napi_InvalidArg(env);
  if (ref->refcount == 0) return Ant_Napi_SetLastError(env, napi_generic_failure, "reference count already zero");

  if (--ref->refcount == 0) ref->ref_val = Ant_Value_Undefined;
  if (result) *result = ref->refcount;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_reference_value(napi_env env, napi_ref ref, napi_value *result) {
  if (!env || !env->js || !ref || !result) return Napi_InvalidArg(env);

  *result = ref->live ? Ant_Napi_Pin(env, ref->value) : (napi_value){0};
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_handle_scope(napi_env env, napi_handle_scope *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_handle_scope scope = calloc(1, sizeof(*scope));
  if (!scope) return Napi_OutOfMemory(env);

  Scope_Open(env, scope);
  *result = scope;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  if (!env || !scope) return Napi_InvalidArg(env);

  Scope_Close(env, scope);
  free(scope);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_escapable_handle_scope scope = calloc(1, sizeof(*scope));
  if (!scope) return Napi_OutOfMemory(env);

  Scope_Open(env, &scope->base);
  *result = scope;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
  if (!env || !scope) return Napi_InvalidArg(env);

  Scope_Close(env, &scope->base);
  if (scope->escaped) Ant_Napi_Pin(env, scope->escaped_val);

  free(scope);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value *result) {
  if (!env || !env->js || !scope || !result) return Napi_InvalidArg(env);
  if (scope->escaped) return Ant_Napi_SetLastError(env, napi_escape_called_twice, NULL);

  scope->escaped = true;
  scope->escaped_val = Napi_Decode(escapee);

  *result = escapee;
  return Napi_Ok(env);
}
