#include "napi_internal.h"

struct napi_async_work__ {
  napi_env env;
  napi_async_execute_callback execute;
  napi_async_complete_callback complete;
  void *data;
  uv_work_t req;
  bool queued;
  bool delete_after_complete;
};

static void AsyncWork_Execute(uv_work_t *req) {
  napi_async_work work = req->data;
  work->execute(work->env, work->data);
}

static void AsyncWork_Complete(uv_work_t *req, int status) {
  napi_async_work work = req->data;
  work->queued = false;

  if (work->complete) work->complete(work->env, status == UV_ECANCELED ? napi_cancelled : napi_ok, work->data);
  if (work->delete_after_complete) free(work);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_async_work(
  napi_env env, napi_value async_resource, napi_value async_resource_name,
  napi_async_execute_callback execute, napi_async_complete_callback complete, void *data,
  napi_async_work *result
) {
  if (!env || !env->js || !execute || !result) return Napi_InvalidArg(env);

  napi_async_work work = calloc(1, sizeof(*work));
  if (!work) return Napi_OutOfMemory(env);

  work->env = env;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  work->req.data = work;

  *result = work;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_delete_async_work(napi_env env, napi_async_work work) {
  if (!work) return Napi_InvalidArg(env);

  if (work->queued) work->delete_after_complete = true;
  else free(work);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_queue_async_work(node_api_basic_env env, napi_async_work work) {
  if (!env || !work) return Napi_InvalidArg(env);
  if (work->queued) return Ant_Napi_SetLastError(env, napi_invalid_arg, "already queued");

  if (uv_queue_work(env->js->loop, &work->req, AsyncWork_Execute, AsyncWork_Complete) != 0)
    return Ant_Napi_SetLastError(env, napi_generic_failure, "uv_queue_work failed");

  work->queued = true;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_cancel_async_work(node_api_basic_env env, napi_async_work work) {
  if (!env || !work) return Napi_InvalidArg(env);

  if (uv_cancel((uv_req_t *)&work->req) != 0)
    return Ant_Napi_SetLastError(env, napi_generic_failure, "uv_cancel failed");
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_async_init(
  napi_env env, napi_value async_resource, napi_value async_resource_name,
  napi_async_context *result
) {
  if (!env || !result) return Napi_InvalidArg(env);

  napi_async_context context = calloc(1, sizeof(*context));
  if (!context) return Napi_OutOfMemory(env);

  context->env = env;
  *result = context;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_async_destroy(napi_env env, napi_async_context async_context) {
  if (!async_context) return Napi_InvalidArg(env);
  free(async_context);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_callback_scope(
  napi_env env, napi_value resource_object, napi_async_context context,
  napi_callback_scope *result
) {
  if (!env || !result) return Napi_InvalidArg(env);

  napi_callback_scope scope = calloc(1, sizeof(*scope));
  if (!scope) return Napi_OutOfMemory(env);

  scope->env = env;
  *result = scope;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  if (!scope) return Napi_InvalidArg(env);
  free(scope);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_make_callback(
  napi_env env, napi_async_context async_context, napi_value recv, napi_value func, size_t argc,
  const napi_value *argv, napi_value *result
) {
  return napi_call_function(env, recv, func, argc, argv, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s **loop) {
  if (!env || !env->js || !loop) return Napi_InvalidArg(env);
  *loop = env->js->loop;
  return Napi_Ok(env);
}
