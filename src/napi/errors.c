#include "napi_internal.h"

static napi_status Exception_Report(napi_env env) {
  Ant_Napi_SetLastError(env, napi_pending_exception, NULL);
  return napi_pending_exception;
}

static bool Exception_IsPending(napi_env env) {
  return is_err(env->exception) || Ant_Exception_Pending(env->js);
}

napi_status Ant_Napi_CapturePending(napi_env env, ant_value_t result) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (!is_err(result) && !Ant_Exception_Pending(env->js)) return Napi_Ok(env);

  env->exception = is_err(result) ? result : Ant_Exception_Current(env->js);
  return Exception_Report(env);
}

napi_status Ant_Napi_CheckPending(napi_env env) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (is_err(env->exception)) return Exception_Report(env);
  return Ant_Napi_CapturePending(env, Ant_Value_Undefined);
}

static napi_status Error_SetCode(napi_env env, ant_value_t error, ant_value_t code) {
  if (code == 0 || is_undefined(code)) return Napi_Ok(env);
  if (vtype(code) != kTypeString) return Ant_Napi_SetLastError(env, napi_string_expected, "error code must be a string");

  js_set(env->js, error, "code", code);
  return Ant_Napi_CheckPending(env);
}

static napi_status Error_Create(
  napi_env env, js_err_type_t type,
  ant_value_t code, const char *message, ant_value_t *out
) {
  ant_value_t error = Ant_Error_Create(env->js, type, message);
  if (Napi_Failed(env, error)) return Ant_Napi_CapturePending(env, error);

  Ant_Napi_Pin(env, error);
  *out = error;
  return Error_SetCode(env, error, code);
}

napi_status Ant_Napi_ThrowMessage(napi_env env, js_err_type_t type, const char *code, const char *message) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (Exception_IsPending(env)) return Exception_Report(env);

  ant_t *js = env->js;
  ant_value_t error = Ant_Value_Undefined;
  ant_value_t code_value = code ? js_mkstr(js, code, strlen(code)) : Ant_Value_Undefined;

  napi_status status = Error_Create(env, type, code_value, message ? message : "", &error);
  if (status != napi_ok) return status;

  env->exception = js_throw(js, error);
  return Napi_Ok(env);
}

static napi_status Error_CreateFromValues(
  napi_env env, js_err_type_t type,
  napi_value code, napi_value message, napi_value *result
) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  if (vtype(Napi_Decode(message)) != kTypeString)
    return Ant_Napi_SetLastError(env, napi_string_expected, "error message must be a string");

  ant_value_t error = Ant_Value_Undefined;
  const char *text = js_getstr(env->js, Napi_Decode(message), NULL);
  ant_value_t code_value = Napi_IsEmpty(code) ? Ant_Value_Undefined : Napi_Decode(code);

  napi_status status = Error_Create(env, type, code_value, text ? text : "", &error);
  if (status != napi_ok) return status;

  *result = Napi_Encode(error);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_throw(napi_env env, napi_value error) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (Exception_IsPending(env)) return Exception_Report(env);

  env->exception = js_throw(env->js, Napi_Decode(error));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_throw_error(napi_env env, const char *code, const char *msg) {
  return Ant_Napi_ThrowMessage(env, JS_ERR_GENERIC, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_throw_type_error(napi_env env, const char *code, const char *msg) {
  return Ant_Napi_ThrowMessage(env, JS_ERR_TYPE, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_throw_range_error(napi_env env, const char *code, const char *msg) {
  return Ant_Napi_ThrowMessage(env, JS_ERR_RANGE, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_throw_syntax_error(napi_env env, const char *code, const char *msg) {
  return Ant_Napi_ThrowMessage(env, JS_ERR_SYNTAX, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
  return Error_CreateFromValues(env, JS_ERR_GENERIC, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
  return Error_CreateFromValues(env, JS_ERR_TYPE, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
  return Error_CreateFromValues(env, JS_ERR_RANGE, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_syntax_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
  return Error_CreateFromValues(env, JS_ERR_SYNTAX, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_error(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t object = Napi_Decode(value);
  *result = is_object_type(object) && js_get_slot(object, SLOT_ERROR_BRAND) == Ant_Value_True;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_exception_pending(napi_env env, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = Exception_IsPending(env);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_and_clear_last_exception(napi_env env, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t exception = is_err(env->exception)
    ? env->exception
    : Ant_Exception_Peek(env->js);

  *result = Ant_Napi_Pin(env, Ant_Exception_Value(env->js, exception));
  env->exception = Ant_Value_Undefined;
  js_take_thrown(env->js, exception);

  return Napi_Ok(env);
}

NAPI_EXTERN void NAPI_CDECL
napi_fatal_error(const char *location, size_t location_len, const char *message, size_t message_len) {
  if (!location) location_len = 0;
  else if (location_len == NAPI_AUTO_LENGTH) location_len = strlen(location);
  if (!message) message_len = 0;
  else if (message_len == NAPI_AUTO_LENGTH) message_len = strlen(message);

  fprintf(
    stderr, "N-API fatal error at %.*s: %.*s\n",
    (int)location_len, location ? location : "",
    (int)message_len, message ? message : ""
  );
  abort();
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_fatal_exception(napi_env env, napi_value err) {
  return napi_throw(env, err);
}
