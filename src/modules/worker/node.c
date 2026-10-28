#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "silver/call.h"

#include "modules/events.h"
#include "modules/performance.h"
#include "modules/process.h"
#include "modules/timer.h"
#include "modules/worker_threads.h"

#include "worker_internal.h"

static void node_worker_emit(ant_t *js, ant_worker_t *w, const char *event, ant_value_t *args, int nargs) {
  if (!is_object_type(w->obj)) return;
  eventemitter_emit_args(js, w->obj, event, args, nargs);
  process_microtasks(js);
}

static void node_worker_on_message(ant_t *js, ant_value_t target, ant_value_t value, ant_value_t ports) {
  ant_worker_t *w = worker_data(target);
  if (!w) return;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);
  worker_flush_online(js, w);
  node_worker_emit(js, w, vtype(ports) == kTypeNull ? "messageerror" : "message", &value, 1);
  GC_ROOT_RESTORE(js, mark);
}

static void node_worker_online(ant_t *js, ant_worker_t *w) {
  node_worker_emit(js, w, "online", NULL, 0);
}

static void node_worker_error(ant_t *js, ant_worker_t *w, ant_value_t error) {
  if (is_object_type(w->obj) && eventemitter_listener_count(js, w->obj, "error") > 0) {
    node_worker_emit(js, w, "error", &error, 1);
    return;
  }
  js_throw(js, error);
  process_report_uncaught_exception(js);
}

static void node_worker_exit(ant_t *js, ant_worker_t *w, int code) {
  if (!is_object_type(w->obj)) return;
  ant_value_t code_val = js_mknum(code);
  js_set(js, w->obj, "threadId", js_mknum(-1));
  node_worker_emit(js, w, "exit", &code_val, 1);
}

static const worker_ops_t node_worker_ops = {
  .on_message = node_worker_on_message,
  .online = node_worker_online,
  .error = node_worker_error,
  .exit = node_worker_exit,
};

static ant_value_t js_worker_elu(ant_params_t) {
  ant_worker_t *w = worker_data(js_func_data(js->current_func));
  if (!w || w->exited) return ant_performance_elu_zero(js);

  ant_value_t pair;
  if (ant_performance_elu_pair(js, args, nargs, &pair)) return pair;

  ant_mutex_lock(&w->lock);
  uint64_t start = atomic_load_explicit(&w->loop_start_ns, memory_order_acquire);
  uint64_t idle_ns = w->child && start ? uv_metrics_idle_time(w->child->loop) : 0;
  ant_mutex_unlock(&w->lock);
  if (!start) return ant_performance_elu_zero(js);

  double idle = (double)idle_ns / 1e6;
  double active = (double)(uv_hrtime() - start) / 1e6 - idle;
  return ant_performance_elu_from(js, idle, active, args, nargs);
}

static bool node_worker_flag(ant_t *js, ant_value_t options, const char *name) {
  return is_object_type(options) && js_truthy(js, js_get(js, options, name));
}

static char *node_worker_entry(ant_t *js, ant_worker_t *w, ant_value_t spec, ant_value_t *error) {
  if (!w->is_eval) return worker_resolve_entry(js, spec, WORKER_ENTRY_NODE, error);
  if (vtype(spec) != kTypeString) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "The \"filename\" argument must be of type string when eval is true");
    return NULL;
  }
  size_t len = 0;
  const char *code = js_getstr(js, spec, &len);
  return strndup(code, len);
}

static void node_worker_define_props(ant_t *js, ant_worker_t *w) {
  ant_value_t obj = w->obj;
  worker_stdio_parent_init(js, w);

  js_set(js, obj, "threadId", js_mknum((double)w->thread_id));
  js_set(js, obj, "resourceLimits", worker_limits_object(js, w->requested_limits, -1));
  js_set(js, obj, "stdin", w->stdin_enabled ? w->stdin_obj : js_mknull());
  js_set(js, obj, "stdout", w->stdout_obj);
  js_set(js, obj, "stderr", w->stderr_obj);

  ant_value_t perf = Ant_Object_Create(js);
  js_set(js, perf, "eventLoopUtilization", js_heavy_mkfun(js, js_worker_elu, obj));
  js_set(js, obj, "performance", perf);
}

static ant_value_t js_worker_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Worker cannot be invoked without 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "The \"filename\" argument must be of type string or an instance of URL");

  ant_value_t this_obj = js_getthis(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.worker_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  ant_value_t options = nargs > 1 && is_object_type(args[1]) ? args[1] : js_mkundef();
  ant_worker_t *w = worker_new(js, this_obj, &node_worker_ops);
  if (!w) return Ant_Error_CreateOutOfMemory(js);

  w->is_eval = node_worker_flag(js, options, "eval");
  w->stdin_enabled = node_worker_flag(js, options, "stdin");
  w->pipe_stdout = !node_worker_flag(js, options, "stdout");
  w->pipe_stderr = !node_worker_flag(js, options, "stderr");

  ant_value_t error = js_mkundef();
  w->entry = node_worker_entry(js, w, args[0], &error);
  if (!w->entry) {
    worker_release(w);
    return is_err(error) ? error : Ant_Error_CreateOutOfMemory(js);
  }

  ant_value_t r = worker_read_options(js, w, options);
  if (!is_err(r)) r = worker_start(js, w);
  if (is_err(r)) {
    worker_release(w);
    return r;
  }

  node_worker_define_props(js, w);
  return this_obj;
}

static ant_worker_t *worker_this(ant_t *js) {
  return worker_data(js_getthis(js));
}

static ant_value_t js_worker_post_message(ant_params_t) {
  ant_worker_t *w = worker_this(js);
  if (!w) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (w->exited || !w->binding) return js_mkundef();
  return ant_port_binding_post(js, w->binding, nargs > 0 ? args[0] : js_mkundef(), worker_transfer_arg(js, args, nargs));
}

static ant_value_t js_worker_terminate(ant_params_t) {
  ant_worker_t *w = worker_this(js);
  if (!w) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  ant_value_t promise = js_mkpromise(js);
  if (w->exited) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  if (vtype(w->exit_promise) == kTypePromise) return w->exit_promise;
  w->exit_promise = promise;
  worker_request_stop(w, 1);

  return promise;
}

static ant_value_t js_worker_ref(ant_params_t) {
  ant_worker_t *w = worker_this(js);
  if (w) worker_set_ref(w, true);
  return js_mkundef();
}

static ant_value_t js_worker_unref(ant_params_t) {
  ant_worker_t *w = worker_this(js);
  if (w) worker_set_ref(w, false);
  return js_mkundef();
}

static ant_value_t js_worker_has_ref(ant_params_t) {
  ant_worker_t *w = worker_this(js);
  return js_bool(w && !w->exited && w->refed);
}

static ant_value_t js_worker_get_heap_snapshot(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, js_mkerr(js, "Worker.getHeapSnapshot is not supported"));
  return promise;
}

static ant_value_t js_mark_as_untransferable(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  return ant_wire_mark_untransferable(js, args[0]);
}

static ant_value_t js_mark_as_uncloneable(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  return ant_wire_mark_uncloneable(js, args[0]);
}

static ant_value_t js_is_marked_as_untransferable(ant_params_t) {
  return js_bool(nargs > 0 && ant_wire_is_untransferable(js, args[0]));
}

static ant_value_t js_receive_message_on_port(ant_params_t) {
  if (nargs < 1 || !ant_message_port_is(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"port\" argument must be a MessagePort instance");
  return ant_message_port_receive(js, args[0]);
}

static ant_value_t js_move_message_port_to_context(ant_params_t) {
  if (nargs < 1 || !ant_message_port_is(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"port\" argument must be a MessagePort instance");
  return args[0];
}

static ant_value_t js_set_environment_data(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t map = worker_environment_map(js);
  if (is_err(map)) return map;
  bool remove = nargs < 2 || vtype(args[1]) == kTypeUndefined;
  ant_value_t method = js_get(js, map, remove ? "delete" : "set");
  return sv_vm_call(js->vm, js, method, map, args, remove ? 1 : 2, NULL, js_mkundef());
}

static ant_value_t js_get_environment_data(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t map = worker_environment_map(js);
  if (is_err(map)) return map;
  ant_value_t method = js_get(js, map, "get");
  return sv_vm_call(js->vm, js, method, map, args, 1, NULL, js_mkundef());
}

static ant_value_t js_post_message_to_thread(ant_params_t) {
  return worker_post_message_to_thread(js, args, nargs);
}

static void worker_setup_child_state(ant_t *js, ant_value_t lib) {
  ant_worker_t *w = js->worker;

  ant_value_t data = w->worker_data ? ant_wire_deserialize(js, w->worker_data, NULL) : js_mkundef();
  if (is_err(data)) data = js_take_thrown(js, data);
  js_set(js, lib, "workerData", data);

  if (w->environment_data) {
    ant_value_t map = ant_wire_deserialize(js, w->environment_data, NULL);
    if (is_err(map)) js_take_thrown(js, map);
    else js->mutable_roots.worker_environment_data = map;
  }

  js_set(js, lib, "parentPort", worker_parent_port(js));
  js_set(js, lib, "threadId", js_mknum((double)w->thread_id));
  js_set(js, lib, "resourceLimits", worker_limits_object(js, w->limits, 0));
  js_set(js, lib, "threadName", w->name ? js_mkstr(js, w->name, strlen(w->name)) : js_mkstr(js, "", 0));
}

static void worker_setup_main_state(ant_t *js, ant_value_t lib) {
  js_set(js, lib, "parentPort", js_mknull());
  js_set(js, lib, "workerData", js_mknull());
  js_set(js, lib, "threadId", js_mknum(0));
  js_set(js, lib, "threadName", js_mkstr(js, "", 0));
  js_set(js, lib, "resourceLimits", Ant_Object_Create(js));
}

static void node_worker_init_class(ant_t *js) {
  if (is_object_type(js->builtins.worker_proto)) return;

  ant_value_t proto = Ant_Object_CreateBare(js);
  js_set_proto_init(proto, js->builtins.eventemitter_proto);
  js_set(js, proto, "postMessage", Ant_Function_CreateWithArity(js_worker_post_message, 1));
  js_set(js, proto, "terminate", Ant_Function_Create(js_worker_terminate));
  js_set(js, proto, "ref", Ant_Function_Create(js_worker_ref));
  js_set(js, proto, "unref", Ant_Function_Create(js_worker_unref));
  js_set(js, proto, "hasRef", Ant_Function_Create(js_worker_has_ref));
  js_set(js, proto, "getHeapSnapshot", Ant_Function_Create(js_worker_get_heap_snapshot));
  js_set_tostringtag(js, proto, js_mkstr(js, "Worker", 6));

  js->builtins.worker_proto = proto;
  js->builtins.worker_ctor = Ant_Function_CreateConstructor(js, js_worker_ctor, proto, "Worker", 1);
}

ant_value_t worker_threads_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  bool is_worker = js->worker != NULL;
  if (!js->builtins.worker_share_env) js->builtins.worker_share_env = js_mksym(js, "nodejs.worker_threads.SHARE_ENV");
  node_worker_init_class(js);

  js_set(js, lib, "Worker", js->builtins.worker_ctor);
  js_set(js, lib, "MessageChannel", js_get(js, js->global, "MessageChannel"));
  js_set(js, lib, "MessagePort", js_get(js, js->global, "MessagePort"));
  js_set(js, lib, "BroadcastChannel", js_get(js, js->global, "BroadcastChannel"));
  js_set(js, lib, "isMainThread", js_bool(!is_worker));
  js_set(js, lib, "SHARE_ENV", js->builtins.worker_share_env);
  js_set(js, lib, "markAsUntransferable", Ant_Function_CreateWithArity(js_mark_as_untransferable, 1));
  js_set(js, lib, "isMarkedAsUntransferable", Ant_Function_CreateWithArity(js_is_marked_as_untransferable, 1));
  js_set(js, lib, "markAsUncloneable", Ant_Function_CreateWithArity(js_mark_as_uncloneable, 1));
  js_set(js, lib, "isInternalThread", js_false);
  js_set(js, lib, "receiveMessageOnPort", Ant_Function_CreateWithArity(js_receive_message_on_port, 1));
  js_set(js, lib, "moveMessagePortToContext", Ant_Function_CreateWithArity(js_move_message_port_to_context, 2));
  js_set(js, lib, "setEnvironmentData", Ant_Function_CreateWithArity(js_set_environment_data, 2));
  js_set(js, lib, "getEnvironmentData", Ant_Function_CreateWithArity(js_get_environment_data, 1));
  js_set(js, lib, "postMessageToThread", Ant_Function_CreateWithArity(js_post_message_to_thread, 4));

  if (is_worker) worker_setup_child_state(js, lib);
  else worker_setup_main_state(js, lib);

  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "worker_threads", 14));

  return lib;
}
