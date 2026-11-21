#include "napi_internal.h"

static napi_status Deferred_Settle(napi_env env, napi_deferred deferred, napi_value value, bool fulfilled) {
  if (!env || !env->js || !deferred) return Napi_InvalidArg(env);

  if (deferred->prev) deferred->prev->next = deferred->next;
  else env->deferreds = deferred->next;
  if (deferred->next) deferred->next->prev = deferred->prev;

  ant_value_t promise = deferred->promise_val;
  free(deferred);

  if (fulfilled) js_resolve_promise(env->js, promise, Napi_Decode(value));
  else js_reject_promise(env->js, promise, Napi_Decode(value));

  return Ant_Napi_CheckPending(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_promise(napi_env env, napi_deferred *deferred, napi_value *promise) {
  if (!env || !env->js || !deferred || !promise) return Napi_InvalidArg(env);

  napi_deferred created = calloc(1, sizeof(*created));
  if (!created) return Napi_OutOfMemory(env);

  created->env = env;
  created->promise_val = js_mkpromise(env->js);
  created->next = env->deferreds;
  if (created->next) created->next->prev = created;
  env->deferreds = created;

  *deferred = created;
  *promise = Ant_Napi_Pin(env, created->promise_val);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_resolve_deferred(napi_env env, napi_deferred deferred, napi_value resolution) {
  return Deferred_Settle(env, deferred, resolution, true);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_reject_deferred(napi_env env, napi_deferred deferred, napi_value rejection) {
  return Deferred_Settle(env, deferred, rejection, false);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_promise(napi_env env, napi_value value, bool *is_promise) {
  if (!env || !is_promise) return Napi_InvalidArg(env);
  *is_promise = vtype(Napi_Decode(value)) == kTypePromise;
  return Napi_Ok(env);
}
