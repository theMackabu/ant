#include "napi_internal.h"

void ant_napi_link_async(void) {}

static void napi_tsfn_maybe_finish(struct napi_threadsafe_function__ *tsfn);

static void napi_tsfn_async_cb(uv_async_t *handle) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)handle->data;
  if (!tsfn || !tsfn->env || !tsfn->env->js) return;

  ant_t *js = tsfn->env->js;
  for (;;) {
    napi_tsfn_item_t *item = NULL;

    uv_mutex_lock(&tsfn->mutex);
    if (tsfn->head) {
      item = tsfn->head;
      tsfn->head = item->next;
      if (!tsfn->head) tsfn->tail = NULL;
      tsfn->queue_size--;
    }
    bool done = tsfn->closing && tsfn->queue_size == 0;
    uv_mutex_unlock(&tsfn->mutex);

    if (!item) {
      if (done) napi_tsfn_maybe_finish(tsfn);
      break;
    }

    ant_value_t cb = tsfn->func_val;
    if (tsfn->call_js_cb) {
      tsfn->call_js_cb((napi_env)tsfn->env, (napi_value)cb, tsfn->context, item->data);
    } else if (is_callable(cb)) {
      sv_vm_call(js->vm, js, cb, js_mkundef(), NULL, 0, NULL, false);
    }

    free(item);
  }
}

static void napi_tsfn_close_cb(uv_handle_t *handle) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)handle->data;
  if (!tsfn) return;

  if (tsfn->thread_finalize_cb) {
    tsfn->thread_finalize_cb((napi_env)tsfn->env, tsfn->thread_finalize_data, NULL);
  }

  if (tsfn->env) {
    if (tsfn->prev) tsfn->prev->next = tsfn->next;
    else if (tsfn->env->tsfns == tsfn) tsfn->env->tsfns = tsfn->next;
    if (tsfn->next) tsfn->next->prev = tsfn->prev;
  }

  if (tsfn->env && tsfn->env->js) {
    tsfn->func_val = js_mkundef();
  }

  uv_mutex_destroy(&tsfn->mutex);
  free(tsfn);
}

static void napi_tsfn_maybe_finish(struct napi_threadsafe_function__ *tsfn) {
  if (!tsfn) return;
  uv_close((uv_handle_t *)&tsfn->async, napi_tsfn_close_cb);
}

static void napi_async_work_execute_cb(uv_work_t *req) {
  napi_async_work_impl_t *work = (napi_async_work_impl_t *)req->data;
  if (!work || !work->execute) return;
  work->execute((napi_env)work->env, work->data);
}

static void napi_async_work_after_cb(uv_work_t *req, int status) {
  napi_async_work_impl_t *work = (napi_async_work_impl_t *)req->data;
  if (!work) return;

  work->queued = false;
  if (work->complete) {
    napi_status st = (status == UV_ECANCELED) ? napi_cancelled : napi_ok;
    work->complete((napi_env)work->env, st, work->data);
  }

  if (work->delete_after_complete) free(work);
}
NAPI_EXTERN napi_status NAPI_CDECL napi_create_async_work(
  napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_execute_callback execute,
  napi_async_complete_callback complete,
  void *data,
  napi_async_work *result
) {
  (void)async_resource;
  (void)async_resource_name;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !execute || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_async_work_impl_t *work = (napi_async_work_impl_t *)calloc(1, sizeof(*work));
  if (!work) return napi_set_last(env, napi_generic_failure, "out of memory");

  work->env = nenv;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  work->req.data = work;

  *result = (napi_async_work)work;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_async_work(
  napi_env env,
  napi_async_work work
) {
  (void)env;
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!w) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (w->queued) {
    w->delete_after_complete = true;
    return napi_set_last(env, napi_ok, NULL);
  }
  free(w);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_queue_async_work(
  node_api_basic_env env,
  napi_async_work work
) {
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!env || !w) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  if (w->queued) return napi_set_last((napi_env)env, napi_invalid_arg, "already queued");

  int rc = uv_queue_work(uv_default_loop(), &w->req, napi_async_work_execute_cb, napi_async_work_after_cb);
  if (rc != 0) return napi_set_last((napi_env)env, napi_generic_failure, "uv_queue_work failed");
  w->queued = true;
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_cancel_async_work(
  node_api_basic_env env,
  napi_async_work work
) {
  napi_async_work_impl_t *w = (napi_async_work_impl_t *)work;
  if (!env || !w) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  int rc = uv_cancel((uv_req_t *)&w->req);
  if (rc != 0) return napi_set_last((napi_env)env, napi_generic_failure, "uv_cancel failed");
  return napi_set_last((napi_env)env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_threadsafe_function(
  napi_env env,
  napi_value func,
  napi_value async_resource,
  napi_value async_resource_name,
  size_t max_queue_size,
  size_t initial_thread_count,
  void *thread_finalize_data,
  napi_finalize thread_finalize_cb,
  void *context,
  napi_threadsafe_function_call_js call_js_cb,
  napi_threadsafe_function *result
) {
  (void)async_resource;
  (void)async_resource_name;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  struct napi_threadsafe_function__ *tsfn =
    (struct napi_threadsafe_function__ *)calloc(1, sizeof(*tsfn));
  if (!tsfn) return napi_set_last(env, napi_generic_failure, "out of memory");

  tsfn->env = nenv;
  tsfn->call_js_cb = call_js_cb;
  tsfn->thread_finalize_cb = thread_finalize_cb;
  tsfn->thread_finalize_data = thread_finalize_data;
  tsfn->context = context;
  tsfn->max_queue_size = max_queue_size;
  tsfn->thread_count = initial_thread_count > 0 ? initial_thread_count : 1;
  tsfn->func_val = func ? (ant_value_t)func : js_mkundef();

  uv_mutex_init(&tsfn->mutex);
  int rc = uv_async_init(uv_default_loop(), &tsfn->async, napi_tsfn_async_cb);
  if (rc != 0) {
    uv_mutex_destroy(&tsfn->mutex);
    free(tsfn);
    return napi_set_last(env, napi_generic_failure, "uv_async_init failed");
  }
  tsfn->async.data = tsfn;

  tsfn->prev = NULL;
  tsfn->next = nenv->tsfns;
  if (nenv->tsfns) nenv->tsfns->prev = tsfn;
  nenv->tsfns = tsfn;

  *result = (napi_threadsafe_function)tsfn;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_threadsafe_function(
  napi_threadsafe_function func,
  void *data,
  napi_threadsafe_function_call_mode is_blocking
) {
  (void)is_blocking;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;

  uv_mutex_lock(&tsfn->mutex);
  if (tsfn->closing || tsfn->aborted) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_closing;
  }
  if (tsfn->max_queue_size > 0 && tsfn->queue_size >= tsfn->max_queue_size) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_queue_full;
  }

  napi_tsfn_item_t *item = (napi_tsfn_item_t *)calloc(1, sizeof(*item));
  if (!item) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_generic_failure;
  }
  item->data = data;
  if (!tsfn->head) tsfn->head = item;
  else tsfn->tail->next = item;
  tsfn->tail = item;
  tsfn->queue_size++;
  uv_mutex_unlock(&tsfn->mutex);

  uv_async_send(&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_release_threadsafe_function(
  napi_threadsafe_function func,
  napi_threadsafe_function_release_mode mode
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;

  uv_mutex_lock(&tsfn->mutex);
  if (mode == napi_tsfn_abort) tsfn->aborted = true;
  if (tsfn->thread_count > 0) tsfn->thread_count--;
  if (tsfn->thread_count == 0 || tsfn->aborted) tsfn->closing = true;
  uv_mutex_unlock(&tsfn->mutex);

  uv_async_send(&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_acquire_threadsafe_function(
  napi_threadsafe_function func
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_mutex_lock(&tsfn->mutex);
  if (tsfn->closing) {
    uv_mutex_unlock(&tsfn->mutex);
    return napi_closing;
  }
  tsfn->thread_count++;
  uv_mutex_unlock(&tsfn->mutex);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_ref_threadsafe_function(
  node_api_basic_env env,
  napi_threadsafe_function func
) {
  (void)env;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_ref((uv_handle_t *)&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unref_threadsafe_function(
  node_api_basic_env env,
  napi_threadsafe_function func
) {
  (void)env;
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn) return napi_invalid_arg;
  uv_unref((uv_handle_t *)&tsfn->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_threadsafe_function_context(
  napi_threadsafe_function func,
  void **result
) {
  struct napi_threadsafe_function__ *tsfn = (struct napi_threadsafe_function__ *)func;
  if (!tsfn || !result) return napi_invalid_arg;
  *result = tsfn->context;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_callback_scope(
  napi_env env,
  napi_value resource_object,
  napi_async_context context,
  napi_callback_scope *result
) {
  (void)resource_object;
  (void)context;
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_callback_scope__ *scope = (struct napi_callback_scope__ *)calloc(1, sizeof(*scope));
  if (!scope) return napi_set_last(env, napi_generic_failure, "out of memory");
  scope->env = (ant_napi_env_t *)env;
  *result = (napi_callback_scope)scope;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_callback_scope(
  napi_env env,
  napi_callback_scope scope
) {
  (void)env;
  if (!scope) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  free(scope);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_init(
  napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_context *result
) {
  (void)async_resource;
  (void)async_resource_name;
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  struct napi_async_context__ *ctx = (struct napi_async_context__ *)calloc(1, sizeof(*ctx));
  if (!ctx) return napi_set_last(env, napi_generic_failure, "out of memory");
  ctx->env = (ant_napi_env_t *)env;
  *result = (napi_async_context)ctx;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_destroy(
  napi_env env,
  napi_async_context async_context
) {
  (void)env;
  if (!async_context) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  free(async_context);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_make_callback(
  napi_env env,
  napi_async_context async_context,
  napi_value recv,
  napi_value func,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  (void)async_context;
  return napi_call_function(env, recv, func, argc, argv, result);
}
NAPI_EXTERN napi_status NAPI_CDECL napi_get_uv_event_loop(
  node_api_basic_env env,
  struct uv_loop_s **loop
) {
  if (!env || !loop) return napi_set_last((napi_env)env, napi_invalid_arg, "invalid argument");
  *loop = uv_default_loop();
  return napi_set_last((napi_env)env, napi_ok, NULL);
}
