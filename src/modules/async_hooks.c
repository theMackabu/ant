// stub: minimal node:async_hooks implementation
// just enough for react to render tree

#include <stdlib.h>

#include "ant.h"
#include "internal.h"
#include "silver/engine.h"

#include "descriptors.h"
#include "modules/async_hooks.h"
#include "modules/symbol.h"

static inline bool async_hooks_is_callable(ant_value_t v) {
  uint8_t t = vtype(v);
  return t == T_FUNC || t == T_CFUNC;
}

static ant_value_t async_hooks_call_with_tail_args(
  ant_t *js, ant_value_t fn, ant_value_t this_arg, ant_value_t *args, int nargs, int start_idx
) {
  int call_nargs = nargs - start_idx;
  if (call_nargs <= 0) return sv_vm_call(js->vm, js, fn, this_arg, NULL, 0, NULL, false);

  ant_value_t *call_args = (ant_value_t *)malloc((size_t)call_nargs * sizeof(ant_value_t));
  if (!call_args) return js_mkerr(js, "Out of memory");

  for (int i = 0; i < call_nargs; i++) call_args[i] = args[start_idx + i];
  ant_value_t result = sv_vm_call(js->vm, js, fn, this_arg, call_args, call_nargs, NULL, false);
  free(call_args);
  return result;
}

static ant_value_t async_local_storage_run(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || !async_hooks_is_callable(args[1])) {
    return js_mkerr(js, "AsyncLocalStorage.run(store, callback, ...args) requires a callback");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.run() requires an AsyncLocalStorage instance");
  }

  ant_value_t prev = js_get_slot(js, this_obj, SLOT_DATA);
  js_set_slot(js, this_obj, SLOT_DATA, args[0]);
  ant_value_t result = async_hooks_call_with_tail_args(js, args[1], js_mkundef(), args, nargs, 2);
  js_set_slot(js, this_obj, SLOT_DATA, prev);
  return result;
}

static ant_value_t async_local_storage_exit(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !async_hooks_is_callable(args[0])) {
    return js_mkerr(js, "AsyncLocalStorage.exit(callback, ...args) requires a callback");
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.exit() requires an AsyncLocalStorage instance");
  }

  ant_value_t prev = js_get_slot(js, this_obj, SLOT_DATA);
  js_set_slot(js, this_obj, SLOT_DATA, js_mkundef());
  ant_value_t result = async_hooks_call_with_tail_args(js, args[0], js_mkundef(), args, nargs, 1);
  js_set_slot(js, this_obj, SLOT_DATA, prev);
  return result;
}

static ant_value_t async_local_storage_enterWith(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) {
    return js_mkerr(js, "AsyncLocalStorage.enterWith() requires an AsyncLocalStorage instance");
  }
  js_set_slot(js, this_obj, SLOT_DATA, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_local_storage_getStore(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkundef();
  return js_get_slot(js, this_obj, SLOT_DATA);
}

static ant_value_t async_local_storage_disable(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_obj = js_getthis(js);
  if (is_object_type(this_obj)) js_set_slot(js, this_obj, SLOT_DATA, js_mkundef());
  return js_mkundef();
}

static ant_value_t async_resource_runInAsyncScope(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !async_hooks_is_callable(args[0])) {
    return js_mkerr(js, "AsyncResource.runInAsyncScope(fn[, thisArg, ...args]) requires a function");
  }
  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  return async_hooks_call_with_tail_args(js, args[0], this_arg, args, nargs, 2);
}

static ant_value_t async_resource_emitDestroy(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

static ant_value_t async_resource_asyncId(ant_t *js, ant_value_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mknum(0);
}

static ant_value_t async_resource_triggerAsyncId(ant_t *js, ant_value_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mknum(0);
}

static ant_value_t async_hook_enable(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

static ant_value_t async_hook_disable(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

static ant_value_t async_hooks_createHook(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t hook = js_mkobj(js);
  js_set(js, hook, "enable", js_mkfun(async_hook_enable));
  js_set(js, hook, "disable", js_mkfun(async_hook_disable));
  return hook;
}

static ant_value_t async_hooks_executionAsyncId(ant_t *js, ant_value_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mknum(1);
}

static ant_value_t async_hooks_triggerAsyncId(ant_t *js, ant_value_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mknum(0);
}

static ant_value_t async_hooks_executionAsyncResource(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkobj(js);
}

ant_value_t async_hooks_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  ant_value_t als_ctor = js_mkobj(js);
  ant_value_t als_proto = js_mkobj(js);
  js_set(js, als_proto, "run", js_mkfun(async_local_storage_run));
  js_set(js, als_proto, "exit", js_mkfun(async_local_storage_exit));
  js_set(js, als_proto, "enterWith", js_mkfun(async_local_storage_enterWith));
  js_set(js, als_proto, "getStore", js_mkfun(async_local_storage_getStore));
  js_set(js, als_proto, "disable", js_mkfun(async_local_storage_disable));
  js_set_sym(js, als_proto, get_toStringTag_sym(), ANT_STRING("AsyncLocalStorage"));
  js_mkprop_fast(js, als_ctor, "prototype", 9, als_proto);
  js_mkprop_fast(js, als_ctor, "name", 4, ANT_STRING("AsyncLocalStorage"));
  js_set_descriptor(js, als_ctor, "name", 4, 0);
  js_set(js, lib, "AsyncLocalStorage", js_obj_to_func_ex(als_ctor, SV_CALL_IS_DEFAULT_CTOR));

  ant_value_t resource_ctor = js_mkobj(js);
  ant_value_t resource_proto = js_mkobj(js);
  js_set(js, resource_proto, "runInAsyncScope", js_mkfun(async_resource_runInAsyncScope));
  js_set(js, resource_proto, "emitDestroy", js_mkfun(async_resource_emitDestroy));
  js_set(js, resource_proto, "asyncId", js_mkfun(async_resource_asyncId));
  js_set(js, resource_proto, "triggerAsyncId", js_mkfun(async_resource_triggerAsyncId));
  js_set_sym(js, resource_proto, get_toStringTag_sym(), ANT_STRING("AsyncResource"));
  js_mkprop_fast(js, resource_ctor, "prototype", 9, resource_proto);
  js_mkprop_fast(js, resource_ctor, "name", 4, ANT_STRING("AsyncResource"));
  js_set_descriptor(js, resource_ctor, "name", 4, 0);
  js_set(js, lib, "AsyncResource", js_obj_to_func_ex(resource_ctor, SV_CALL_IS_DEFAULT_CTOR));

  js_set(js, lib, "createHook", js_mkfun(async_hooks_createHook));
  js_set(js, lib, "executionAsyncId", js_mkfun(async_hooks_executionAsyncId));
  js_set(js, lib, "triggerAsyncId", js_mkfun(async_hooks_triggerAsyncId));
  js_set(js, lib, "executionAsyncResource", js_mkfun(async_hooks_executionAsyncResource));
  js_set(js, lib, "asyncWrapProviders", js_mkobj(js));
  js_set_sym(js, lib, get_toStringTag_sym(), ANT_STRING("async_hooks"));

  return lib;
}
