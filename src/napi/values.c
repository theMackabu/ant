#include "silver/vm.h"
#include "modules/date.h"
#include "napi_internal.h"

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_undefined(napi_env env, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_Undefined);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_null(napi_env env, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_Null);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_global(napi_env env, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = Ant_Napi_Pin(env, js_glob(env->js));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_boolean(napi_env env, bool value, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_FromBool(value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_double(napi_env env, double value, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_FromNumber(value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_int32(napi_env env, int32_t value, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_FromNumber((double)value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_uint32(napi_env env, uint32_t value, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_FromNumber((double)value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_int64(napi_env env, int64_t value, napi_value *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = Napi_Encode(Ant_Value_FromNumber((double)value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_date(napi_env env, double time, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t date = js_mkdate(env->js, date_timeclip(time));
  if (Napi_Failed(env, date)) return Ant_Napi_CapturePending(env, date);

  *result = Ant_Napi_Pin(env, date);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_symbol(napi_env env, napi_value description, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  const char *text = NULL;
  ant_value_t value = Napi_IsEmpty(description) ? Ant_Value_Undefined : Napi_Decode(description);

  if (!is_undefined(value)) {
    if (vtype(value) != kTypeString) return Ant_Napi_SetLastError(env, napi_string_expected, NULL);
    text = js_getstr(env->js, value, NULL);
  }

  *result = Ant_Napi_Pin(env, js_mksym(env->js, text));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_symbol_for(napi_env env, const char *utf8description, size_t length, napi_value *result) {
  if (!env || !env->js || !result || (!utf8description && length != 0)) return Napi_InvalidArg(env);
  if (length == NAPI_AUTO_LENGTH) length = strlen(utf8description);

  char *key = strndup(utf8description ? utf8description : "", length);
  if (!key) return Napi_OutOfMemory(env);

  ant_value_t symbol = js_mksym_for(env->js, key);
  free(key);
  if (Napi_Failed(env, symbol)) return Ant_Napi_CapturePending(env, symbol);

  *result = Ant_Napi_Pin(env, symbol);
  return Napi_Ok(env);
}

static napi_status Number_Read(napi_env env, napi_value value, void *result, double *number) {
  if (!env || !result) return Napi_InvalidArg(env);
  if (vtype(Napi_Decode(value)) != kTypeNumber) return Ant_Napi_SetLastError(env, napi_number_expected, NULL);

  *number = Ant_Value_AsNumberUnchecked(Napi_Decode(value));
  return Napi_Ok(env);
}

static uint32_t Number_ToUint32(double number) {
  if (!isfinite(number)) return 0;
  double wrapped = fmod(trunc(number), 4294967296.0);
  return (uint32_t)(wrapped < 0 ? wrapped + 4294967296.0 : wrapped);
}

static int64_t Number_ToInt64(double number) {
  if (!isfinite(number)) return 0;
  if (number >= 9223372036854775808.0) return INT64_MAX;
  if (number <= -9223372036854775808.0) return INT64_MIN;
  return (int64_t)number;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_double(napi_env env, napi_value value, double *result) {
  double number = 0;
  napi_status status = Number_Read(env, value, result, &number);
  if (status == napi_ok) *result = number;
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_int32(napi_env env, napi_value value, int32_t *result) {
  double number = 0;
  napi_status status = Number_Read(env, value, result, &number);
  if (status == napi_ok) *result = (int32_t)Number_ToUint32(number);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_uint32(napi_env env, napi_value value, uint32_t *result) {
  double number = 0;
  napi_status status = Number_Read(env, value, result, &number);
  if (status == napi_ok) *result = Number_ToUint32(number);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_int64(napi_env env, napi_value value, int64_t *result) {
  double number = 0;
  napi_status status = Number_Read(env, value, result, &number);
  if (status == napi_ok) *result = Number_ToInt64(number);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_bool(napi_env env, napi_value value, bool *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  if (vtype(Napi_Decode(value)) != kTypeBool) return Ant_Napi_SetLastError(env, napi_boolean_expected, NULL);

  *result = Napi_Decode(value) == Ant_Value_True;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_date_value(napi_env env, napi_value value, double *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  if (!is_date_instance(Napi_Decode(value))) return Ant_Napi_SetLastError(env, napi_date_expected, NULL);

  ant_value_t time = js_get_slot(Napi_Decode(value), SLOT_DATA);
  *result = vtype(time) == kTypeNumber ? Ant_Value_AsNumberUnchecked(time) : ANT_NAN;
  return Napi_Ok(env);
}

static napi_valuetype Value_Type(ant_value_t value) {
  switch (vtype(value)) {
    case kTypeUndefined: return napi_undefined;
    case kTypeNull:      return napi_null;
    case kTypeBool:      return napi_boolean;
    case kTypeNumber:    return napi_number;
    case kTypeString:    return napi_string;
    case kTypeSymbol:    return napi_symbol;
    case kTypeBigInt:    return napi_bigint;
    case kTypeFunction:
    case kTypeBuiltin:   return napi_function;
    default:             return napi_object;
  }
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_typeof(napi_env env, napi_value value, napi_valuetype *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = Ant_Napi_IsExternal(env->js, value) ? napi_external : Value_Type(Napi_Decode(value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_array(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t checked = js_is_array_value_checked(env->js, Napi_Decode(value), result);
  return Napi_Failed(env, checked) ? Ant_Napi_CapturePending(env, checked) : Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_date(napi_env env, napi_value value, bool *result) {
  if (!env || !result) return Napi_InvalidArg(env);
  *result = is_date_instance(Napi_Decode(value));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_instanceof(napi_env env, napi_value object, napi_value constructor, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t matched = do_instanceof(env->js, Napi_Decode(object), Napi_Decode(constructor));
  if (Napi_Failed(env, matched)) return Ant_Napi_CapturePending(env, matched);

  *result = js_truthy(env->js, matched);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = strict_eq_values(env->js, Napi_Decode(lhs), Napi_Decode(rhs));
  return Napi_Ok(env);
}

static napi_status Coerce_Finish(napi_env env, ant_value_t value, napi_value *result) {
  if (Napi_Failed(env, value)) return Ant_Napi_CapturePending(env, value);
  *result = Ant_Napi_Pin(env, value);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_coerce_to_bool(napi_env env, napi_value value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  return Coerce_Finish(env, Ant_Value_FromBool(js_truthy(env->js, Napi_Decode(value))), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_coerce_to_number(napi_env env, napi_value value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  return Coerce_Finish(env, Ant_Value_FromNumber(js_to_number(env->js, Napi_Decode(value))), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_coerce_to_string(napi_env env, napi_value value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  return Coerce_Finish(env, coerce_to_str(env->js, Napi_Decode(value)), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_coerce_to_object(napi_env env, napi_value value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_t *js = env->js;
  ant_value_t input = Napi_Decode(value);
  if (is_object_type(input)) return Coerce_Finish(env, input, result);

  ant_value_t object_ctor = js_get(js, js_glob(js), "Object");
  if (Napi_Failed(env, object_ctor)) return Ant_Napi_CapturePending(env, object_ctor);
  if (!is_callable(object_ctor)) return Ant_Napi_SetLastError(env, napi_generic_failure, "Object constructor missing");

  ant_value_t object = sv_vm_call(js->vm, js, object_ctor, Ant_Value_Undefined, &input, 1, NULL, Ant_Value_Undefined);
  return Coerce_Finish(env, object, result);
}
