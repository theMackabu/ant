#include "silver/call.h"
#include "napi_internal.h"

struct napi_tsfn_item {
  void *data;
  struct napi_tsfn_item *next;
};

static struct napi_tsfn_item *Queue_Pop(napi_threadsafe_function tsfn, bool *finished) {
  uv_mutex_lock(&tsfn->mutex);

  struct napi_tsfn_item *item = tsfn->head;
  if (item) {
    tsfn->head = item->next;
    if (!tsfn->head) tsfn->tail = NULL;
    tsfn->queue_size--;
  }

  *finished = tsfn->closing && tsfn->queue_size == 0;
  uv_mutex_unlock(&tsfn->mutex);
  return item;
}

static void Queue_Clear(napi_threadsafe_function tsfn) {
  for (struct napi_tsfn_item *item = tsfn->head, *next; item; item = next) {
    next = item->next;
    free(item);
  }

  tsfn->head = tsfn->tail = NULL;
  tsfn->queue_size = 0;
}

static void ThreadsafeFunction_OnClose(uv_handle_t *handle) {
  napi_threadsafe_function tsfn = handle->data;
  napi_env env = tsfn->env;

  if (tsfn->thread_finalize_cb) tsfn->thread_finalize_cb(env, tsfn->thread_finalize_data, tsfn->context);

  if (tsfn->prev) tsfn->prev->next = tsfn->next;
  else env->tsfns = tsfn->next;
  if (tsfn->next) tsfn->next->prev = tsfn->prev;

  uv_mutex_destroy(&tsfn->mutex);
  free(tsfn);
}

static void ThreadsafeFunction_Close(napi_threadsafe_function tsfn) {
  if (!uv_is_closing((uv_handle_t *)&tsfn->async)) uv_close((uv_handle_t *)&tsfn->async, ThreadsafeFunction_OnClose);
}

static void ThreadsafeFunction_Dispatch(napi_threadsafe_function tsfn, void *data) {
  ant_t *js = tsfn->env->js;
  ant_value_t callback = tsfn->func_val;

  if (tsfn->call_js_cb) tsfn->call_js_cb(tsfn->env, Napi_Encode(callback), tsfn->context, data);
  else if (is_callable(callback)) sv_vm_call(js->vm, js, callback, Ant_Value_Undefined, NULL, 0, NULL, Ant_Value_Undefined);
}

static void ThreadsafeFunction_OnAsync(uv_async_t *handle) {
  napi_threadsafe_function tsfn = handle->data;
  bool finished = false;

  for (struct napi_tsfn_item *item; (item = Queue_Pop(tsfn, &finished)); free(item))
    ThreadsafeFunction_Dispatch(tsfn, item->data);

  if (finished) ThreadsafeFunction_Close(tsfn);
}

void Ant_Napi_CloseThreadsafeFunctions(napi_env env) {
  for (napi_threadsafe_function tsfn = env->tsfns; tsfn; tsfn = tsfn->next) {
    uv_mutex_lock(&tsfn->mutex);
    tsfn->aborted = true;
    tsfn->closing = true;
    Queue_Clear(tsfn);
    uv_mutex_unlock(&tsfn->mutex);

    ThreadsafeFunction_Close(tsfn);
  }
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_threadsafe_function(
  napi_env env, napi_value func, napi_value async_resource, napi_value async_resource_name,
  size_t max_queue_size, size_t initial_thread_count, void *thread_finalize_data,
  napi_finalize thread_finalize_cb, void *context, napi_threadsafe_function_call_js call_js_cb,
  napi_threadsafe_function *result
) {
  if (!env || !env->js || !result || initial_thread_count == 0) return Napi_InvalidArg(env);
  if (Napi_IsEmpty(func) && !call_js_cb) return Napi_InvalidArg(env);

  napi_threadsafe_function tsfn = calloc(1, sizeof(*tsfn));
  if (!tsfn) return Napi_OutOfMemory(env);

  if (uv_async_init(env->js->loop, &tsfn->async, ThreadsafeFunction_OnAsync) != 0) {
    free(tsfn);
    return Ant_Napi_SetLastError(env, napi_generic_failure, "uv_async_init failed");
  }

  uv_mutex_init(&tsfn->mutex);
  tsfn->async.data = tsfn;
  tsfn->env = env;
  tsfn->func_val = Napi_IsEmpty(func) ? Ant_Value_Undefined : Napi_Decode(func);
  tsfn->call_js_cb = call_js_cb;
  tsfn->thread_finalize_cb = thread_finalize_cb;
  tsfn->thread_finalize_data = thread_finalize_data;
  tsfn->context = context;
  tsfn->max_queue_size = max_queue_size;
  tsfn->thread_count = initial_thread_count;

  tsfn->next = env->tsfns;
  if (tsfn->next) tsfn->next->prev = tsfn;
  env->tsfns = tsfn;

  *result = tsfn;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_threadsafe_function_context(napi_threadsafe_function func, void **result) {
  if (!func || !result) return napi_invalid_arg;
  *result = func->context;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_call_threadsafe_function(
  napi_threadsafe_function func, void *data, napi_threadsafe_function_call_mode mode
) {
  if (!func) return napi_invalid_arg;

  struct napi_tsfn_item *item = calloc(1, sizeof(*item));
  if (!item) return napi_generic_failure;
  item->data = data;

  uv_mutex_lock(&func->mutex);
  napi_status status = napi_ok;
  if (func->closing || func->aborted) status = napi_closing;
  else if (func->max_queue_size > 0 && func->queue_size >= func->max_queue_size) status = napi_queue_full;
  else {
    if (func->tail) func->tail->next = item;
    else func->head = item;
    func->tail = item;
    func->queue_size++;
  }
  uv_mutex_unlock(&func->mutex);

  if (status != napi_ok) free(item);
  else uv_async_send(&func->async);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_acquire_threadsafe_function(napi_threadsafe_function func) {
  if (!func) return napi_invalid_arg;

  uv_mutex_lock(&func->mutex);
  napi_status status = func->closing ? napi_closing : napi_ok;
  if (status == napi_ok) func->thread_count++;
  uv_mutex_unlock(&func->mutex);

  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_release_threadsafe_function(napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  if (!func) return napi_invalid_arg;

  uv_mutex_lock(&func->mutex);
  if (mode == napi_tsfn_abort) func->aborted = true;
  if (func->thread_count > 0) func->thread_count--;
  if (func->thread_count == 0 || func->aborted) func->closing = true;
  uv_mutex_unlock(&func->mutex);

  uv_async_send(&func->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_ref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function func) {
  if (!func) return napi_invalid_arg;
  uv_ref((uv_handle_t *)&func->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_unref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function func) {
  if (!func) return napi_invalid_arg;
  uv_unref((uv_handle_t *)&func->async);
  return napi_ok;
}
