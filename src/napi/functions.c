#include "cfunc.h"
#include "silver/vm.h"
#include "silver/engine.h"
#include "napi_internal.h"

static ant_value_t Callback_Trampoline(ant_params_t) {
  napi_callback_binding_t *binding = js_get_native(call_func, NAPI_CALLBACK_NATIVE_TAG);
  if (!binding || !binding->cb) return Ant_Value_Undefined;

  napi_env env = binding->env;
  struct napi_callback_info__ info = {
    .env = env,
    .argv = args,
    .argc = nargs < 0 ? 0 : (size_t)nargs,
    .this_arg = call_this,
    .new_target = call_new_target,
    .data = binding->data,
  };

  napi_value returned = binding->cb(env, &info);
  if (is_err(env->exception)) {
    ant_value_t exception = env->exception;
    env->exception = Ant_Value_Undefined;
    return js_throw(js, exception);
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  return Napi_IsEmpty(returned) ? Ant_Value_Undefined : Napi_Decode(returned);
}

static napi_status Function_Invoke(
  napi_env env, ant_value_t func, ant_value_t this_value,
  const napi_value *argv, size_t argc,
  ant_value_t new_target, ant_value_t *out
) {
  napi_args_t call_args;
  if (!Napi_DecodeArgs(&call_args, argv, argc)) return Napi_OutOfMemory(env);

  *out = sv_vm_call(
    env->js->vm, env->js, func, this_value,
    (ant_value_t *)call_args.values, (int)argc,
    NULL, new_target
  );

  Napi_FreeArgs(&call_args);
  return Napi_Failed(env, *out) ? Ant_Napi_CapturePending(env, *out) : napi_ok;
}

napi_status Ant_Napi_CreateFunction(
  napi_env env, const char *utf8name, size_t length,
  napi_callback cb, void *data, napi_value *result
) {
  if (!env || !env->js || !cb || !result) return Napi_InvalidArg(env);

  napi_callback_binding_t *binding = calloc(1, sizeof(*binding));
  if (!binding) return Napi_OutOfMemory(env);

  binding->env = env;
  binding->cb = cb;
  binding->data = data;

  ant_value_t fn = js_heavy_mkfun_native(env->js, Callback_Trampoline, binding, NAPI_CALLBACK_NATIVE_TAG);
  js_mark_constructor(fn, true);

  if (utf8name && utf8name[0]) {
    if (length == NAPI_AUTO_LENGTH) length = strlen(utf8name);
    js_set(env->js, fn, "name", js_mkstr(env->js, utf8name, length));
  }

  *result = Ant_Napi_Pin(env, fn);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_function(
  napi_env env, const char *utf8name, size_t length, napi_callback cb, void *data,
  napi_value *result
) {
  return Ant_Napi_CreateFunction(env, utf8name, length, cb, data, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_define_class(
  napi_env env, const char *utf8name, size_t length, napi_callback constructor, void *data,
  size_t property_count, const napi_property_descriptor *properties, napi_value *result
) {
  if (!env || !env->js || !constructor || !result || (property_count > 0 && !properties))
    return Napi_InvalidArg(env);

  napi_status status = Ant_Napi_CreateFunction(env, utf8name, length, constructor, data, result);
  if (status != napi_ok) return status;

  ant_t *js = env->js;
  ant_value_t ctor = Napi_Decode(*result);
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (Napi_Failed(env, proto)) return Ant_Napi_CapturePending(env, proto);

  if (!is_object_type(proto)) {
    proto = Ant_Object_Create(js);
    js_set(js, ctor, "prototype", proto);
    if (Ant_Exception_Pending(js)) return Ant_Napi_CheckPending(env);
  }

  for (size_t i = 0; i < property_count; i++) {
    napi_property_descriptor property = properties[i];
    ant_value_t target = (property.attributes & napi_static) ? ctor : proto;
    property.attributes &= ~napi_static;

    status = napi_define_properties(env, Napi_Encode(target), 1, &property);
    if (status != napi_ok) return status;
  }

  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_cb_info(
  napi_env env, napi_callback_info info, size_t *argc, napi_value *argv, napi_value *this_arg,
  void **data
) {
  if (!env || !info) return Napi_InvalidArg(env);

  if (argc && argv) {
    size_t capacity = *argc;
    size_t provided = capacity < info->argc ? capacity : info->argc;
    for (size_t i = 0; i < provided; i++) argv[i] = Napi_Encode(info->argv[i]);
    for (size_t i = provided; i < capacity; i++) argv[i] = Napi_Encode(Ant_Value_Undefined);
  }

  if (argc) *argc = info->argc;
  if (this_arg) *this_arg = Napi_Encode(info->this_arg);
  if (data) *data = info->data;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_new_target(napi_env env, napi_callback_info info, napi_value *result) {
  if (!env || !info || !result) return Napi_InvalidArg(env);

  *result = is_undefined(info->new_target) ? (napi_value){0} : Napi_Encode(info->new_target);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_call_function(
  napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value *argv,
  napi_value *result
) {
  ant_value_t callee = Napi_Decode(func);
  ant_value_t this_value = Napi_IsEmpty(recv) ? Ant_Value_Undefined : Napi_Decode(recv);

  if (!env || !env->js || (argc > 0 && !argv)) return Napi_InvalidArg(env);
  if (!is_callable(callee)) return Ant_Napi_SetLastError(env, napi_function_expected, NULL);

  ant_value_t out = Ant_Value_Undefined;
  napi_status status = Function_Invoke(env, callee, this_value, argv, argc, Ant_Value_Undefined, &out);
  if (status != napi_ok) return status;

  if (result) *result = Ant_Napi_Pin(env, out);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_new_instance(
  napi_env env, napi_value constructor, size_t argc, const napi_value *argv, napi_value *result
) {
  ant_value_t ctor = Napi_Decode(constructor);
  if (!env || !env->js || !result || (argc > 0 && !argv)) return Napi_InvalidArg(env);
  if (!is_callable(ctor)) return Ant_Napi_SetLastError(env, napi_function_expected, NULL);

  ant_t *js = env->js;
  ant_value_t instance = Ant_Object_CreateBare(js);
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (Napi_Failed(env, proto)) return Ant_Napi_CapturePending(env, proto);
  if (is_object_type(proto)) js_set_proto_init(instance, proto);
  Ant_Napi_Pin(env, instance);

  ant_value_t out = Ant_Value_Undefined;
  napi_status status = Function_Invoke(env, ctor, instance, argv, argc, ctor, &out);
  if (status != napi_ok) return status;

  *result = Ant_Napi_Pin(env, is_object_type(out) ? out : instance);
  return Napi_Ok(env);
}
