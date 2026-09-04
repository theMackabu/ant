#include "napi_internal.h"

void ant_napi_link_references(void) {}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(
  napi_env env,
  napi_value value,
  uint32_t initial_refcount,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  struct napi_ref__ *ref = (struct napi_ref__ *)calloc(1, sizeof(*ref));
  if (!ref) return napi_set_last(env, napi_generic_failure, "out of memory");

  ref->env = nenv;
  ref->value = value;
  ref->refcount = initial_refcount;
  ref->ref_val = (initial_refcount > 0) ? (ant_value_t)value : js_mkundef();

  ref->prev = NULL;
  ref->next = nenv->refs;
  if (nenv->refs) nenv->refs->prev = ref;
  nenv->refs = ref;

  *result = (napi_ref)ref;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_reference(
  node_api_basic_env env,
  napi_ref ref
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");

  if (r->prev) r->prev->next = r->next;
  else if (nenv->refs == r) nenv->refs = r->next;
  if (r->next) r->next->prev = r->prev;

  r->ref_val = js_mkundef();
  free(r);
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_ref(
  napi_env env,
  napi_ref ref,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;

  if (!nenv || !nenv->js || !r)
    return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (!r->value) {
    if (result) *result = 0;
    return napi_set_last(env, napi_ok, NULL);
  }

  if (r->refcount == 0) r->ref_val = (ant_value_t)r->value;
  r->refcount++;
  if (result) *result = r->refcount;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_unref(
  napi_env env,
  napi_ref ref,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (r->refcount == 0) return napi_set_last(env, napi_invalid_arg, "reference count already zero");

  r->refcount--;
  if (r->refcount == 0) r->ref_val = js_mkundef();
  if (result) *result = r->refcount;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_reference_value(
  napi_env env,
  napi_ref ref,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_ref__ *r = (struct napi_ref__ *)ref;
  if (!nenv || !nenv->js || !r || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (r->refcount > 0) *result = NAPI_RETURN(nenv, r->ref_val);
  else *result = r->value ? NAPI_RETURN(nenv, r->value) : 0;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_handle_scope(
  napi_env env,
  napi_handle_scope *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_handle_scope__ *scope = (struct napi_handle_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = nenv;
  scope->gc_root_mark = gc_root_scope(nenv->js);
  scope->handle_slots_mark = nenv->handle_slots_len;
  nenv->open_handle_scopes++;
  *result = (napi_handle_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_handle_scope(
  napi_env env,
  napi_handle_scope scope
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_handle_scope__ *s = (struct napi_handle_scope__ *)scope;
  if (nenv->js) gc_pop_roots(nenv->js, s->gc_root_mark);
  nenv->handle_slots_len = s->handle_slots_mark;
  if (nenv->open_handle_scopes > 0) nenv->open_handle_scopes--;
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_escapable_handle_scope(
  napi_env env,
  napi_escapable_handle_scope *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_escapable_handle_scope__ *scope = (struct napi_escapable_handle_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = nenv;
  scope->gc_root_mark = gc_root_scope(nenv->js);
  scope->handle_slots_mark = nenv->handle_slots_len;
  scope->escaped = false;
  scope->escaped_val = js_mkundef();
  nenv->open_handle_scopes++;
  *result = (napi_escapable_handle_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_escapable_handle_scope(
  napi_env env,
  napi_escapable_handle_scope scope
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_escapable_handle_scope__ *s = (struct napi_escapable_handle_scope__ *)scope;
  if (nenv->js) gc_pop_roots(nenv->js, s->gc_root_mark);
  nenv->handle_slots_len = s->handle_slots_mark;
  if (nenv->open_handle_scopes > 0) nenv->open_handle_scopes--;
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_escape_handle(
  napi_env env,
  napi_escapable_handle_scope scope,
  napi_value escapee,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  struct napi_escapable_handle_scope__ *esc = (struct napi_escapable_handle_scope__ *)scope;
  if (!nenv || !nenv->js || !esc || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (esc->escaped) return napi_set_last(env, napi_escape_called_twice, "escape already called");
  esc->escaped = true;
  esc->escaped_val = (ant_value_t)escapee;
  gc_push_root(nenv->js, &esc->escaped_val);
  *result = escapee;
  return napi_set_last(env, napi_ok, NULL);
}
