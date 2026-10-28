// stub: minimal node:async_hooks implementation
// just enough for react to render tree

#include <stdlib.h>

#include "ant.h"
#include "cfunc.h"
#include "internal.h"
#include "silver/call.h"
#include "modules/async_hooks.h"

static ant_value_t async_hooks_call_with_tail_args(
  ant_t *js, ant_value_t fn, ant_value_t this_arg, ant_args_t, int start_idx
) {
  int call_nargs = nargs - start_idx;
  if (call_nargs <= 0) return sv_vm_call(js->vm, js, fn, this_arg, NULL, 0, NULL, js_mkundef());

  ant_value_t *call_args = (ant_value_t *)malloc((size_t)call_nargs * sizeof(ant_value_t));
  if (!call_args) return Ant_Error_CreateOutOfMemory(js);

  for (int i = 0; i < call_nargs; i++) call_args[i] = args[start_idx + i];
  ant_value_t result = sv_vm_call(js->vm, js, fn, this_arg, call_args, call_nargs, NULL, js_mkundef());
  free(call_args);
  return result;
}

static ant_value_t async_local_storage_run(ant_params_t) {
  if (nargs < 2 || !is_callable(args[1])) {
    return js_mkerr(js, "AsyncLocalStorage.run(store, callback, ...args) requires a callback");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.run() requires an AsyncLocalStorage instance");
  }

  ant_value_t prev = js_get_slot(this_obj, SLOT_DATA);
  js_set_slot_wb(js, this_obj, SLOT_DATA, args[0]);
  ant_value_t result = async_hooks_call_with_tail_args(js, args[1], js_mkundef(), args, nargs, 2);
  js_set_slot_wb(js, this_obj, SLOT_DATA, prev);
  return result;
}

static ant_value_t async_local_storage_exit(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0])) {
    return js_mkerr(js, "AsyncLocalStorage.exit(callback, ...args) requires a callback");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.exit() requires an AsyncLocalStorage instance");
  }

  ant_value_t prev = js_get_slot(this_obj, SLOT_DATA);
  js_set_slot_wb(js, this_obj, SLOT_DATA, js_mkundef());
  ant_value_t result = async_hooks_call_with_tail_args(js, args[0], js_mkundef(), args, nargs, 1);
  js_set_slot_wb(js, this_obj, SLOT_DATA, prev);
  return result;
}

static ant_value_t async_local_storage_enterWith(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.enterWith() requires an AsyncLocalStorage instance");
  }
  js_set_slot_wb(js, this_obj, SLOT_DATA, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_local_storage_getStore(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkundef();
  return js_get_slot(this_obj, SLOT_DATA);
}

static ant_value_t async_local_storage_disable(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (is_object_type(this_obj)) js_set_slot_wb(js, this_obj, SLOT_DATA, js_mkundef());
  return js_mkundef();
}

static ant_value_t async_resource_runInAsyncScope(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0])) {
    return js_mkerr(js, "AsyncResource.runInAsyncScope(fn[, thisArg, ...args]) requires a function");
  }
  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  return async_hooks_call_with_tail_args(js, args[0], this_arg, args, nargs, 2);
}

static ant_value_t async_resource_emitDestroy(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t async_resource_asyncId(ant_params_t) {
  return js_mknum(0);
}

static ant_value_t async_resource_triggerAsyncId(ant_params_t) {
  return js_mknum(0);
}

static ant_value_t async_hook_enable(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t async_hook_disable(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t async_hooks_createHook(ant_params_t) {
  ant_value_t hook = Ant_Object_CreateBare(js);
  js_set(js, hook, "enable", Ant_Function_Create(async_hook_enable));
  js_set(js, hook, "disable", Ant_Function_Create(async_hook_disable));
  return hook;
}

static ant_value_t async_hooks_executionAsyncId(ant_params_t) {
  return js_mknum(1);
}

static ant_value_t async_hooks_triggerAsyncId(ant_params_t) {
  return js_mknum(0);
}

static ant_value_t async_hooks_executionAsyncResource(ant_params_t) {
  return Ant_Object_CreateBare(js);
}

ant_value_t async_hooks_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  ant_value_t als_ctor = Ant_Object_CreateBare(js);
  ant_value_t als_proto = Ant_Object_CreateBare(js);
  js_set(js, als_proto, "run", Ant_Function_Create(async_local_storage_run));
  js_set(js, als_proto, "exit", Ant_Function_Create(async_local_storage_exit));
  js_set(js, als_proto, "enterWith", Ant_Function_Create(async_local_storage_enterWith));
  js_set(js, als_proto, "getStore", Ant_Function_Create(async_local_storage_getStore));
  js_set(js, als_proto, "disable", Ant_Function_Create(async_local_storage_disable));
  js_set_tostringtag(js, als_proto, ANT_STRING("AsyncLocalStorage"));
  js_mkprop_fast(js, als_ctor, "prototype", 9, als_proto);
  js_set_function_name(js, als_ctor, "AsyncLocalStorage", sizeof("AsyncLocalStorage") - 1);
  js_set(js, lib, "AsyncLocalStorage", js_obj_to_func_ex(js, als_ctor, SV_CALL_IS_DEFAULT_CTOR));

  ant_value_t resource_ctor = Ant_Object_CreateBare(js);
  ant_value_t resource_proto = Ant_Object_CreateBare(js);
  js_set(js, resource_proto, "runInAsyncScope", Ant_Function_CreateWithArity(async_resource_runInAsyncScope, 2));
  js_set(js, resource_proto, "emitDestroy", Ant_Function_Create(async_resource_emitDestroy));
  js_set(js, resource_proto, "asyncId", Ant_Function_Create(async_resource_asyncId));
  js_set(js, resource_proto, "triggerAsyncId", Ant_Function_Create(async_resource_triggerAsyncId));
  js_set_tostringtag(js, resource_proto, ANT_STRING("AsyncResource"));
  js_define_function_length(js, resource_ctor, 1);
  js_mkprop_fast(js, resource_ctor, "prototype", 9, resource_proto);
  js_set_function_name(js, resource_ctor, "AsyncResource", sizeof("AsyncResource") - 1);
  js_set(js, lib, "AsyncResource", js_obj_to_func_ex(js, resource_ctor, SV_CALL_IS_DEFAULT_CTOR));

  js_set(js, lib, "createHook", Ant_Function_CreateWithArity(async_hooks_createHook, 1));
  js_set(js, lib, "executionAsyncId", Ant_Function_Create(async_hooks_executionAsyncId));
  js_set(js, lib, "triggerAsyncId", Ant_Function_Create(async_hooks_triggerAsyncId));
  js_set(js, lib, "executionAsyncResource", Ant_Function_Create(async_hooks_executionAsyncResource));
  js_set(js, lib, "asyncWrapProviders", Ant_Object_CreateBare(js));
  js_set_tostringtag(js, lib, ANT_STRING("async_hooks"));

  return lib;
}
