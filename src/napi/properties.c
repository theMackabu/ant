#include "ptr.h"
#include "descriptors.h"
#include "silver/vm.h"
#include "silver/engine.h"
#include "napi_internal.h"

static napi_status Key_Normalize(napi_env env, napi_value handle, ant_value_t *key) {
  *key = Napi_Decode(handle);
  if (vtype(*key) == kTypeSymbol || vtype(*key) == kTypeString) return napi_ok;

  *key = coerce_to_str(env->js, *key);
  return Napi_Failed(env, *key) ? Ant_Napi_CapturePending(env, *key) : napi_ok;
}

static ant_value_t Key_FromIndex(ant_t *js, uint32_t index) {
  char digits[16];
  int length = snprintf(digits, sizeof(digits), "%u", index);
  return js_mkstr(js, digits, (size_t)length);
}

static ant_value_t Key_FromName(ant_t *js, const char *utf8name) {
  return js_mkstr(js, utf8name, strlen(utf8name));
}

static bool Key_ParseIndex(const char *str, size_t len, uint32_t *out) {
  if (!str || len == 0) return false;
  if (len > 1 && str[0] == '0') return false;

  uint64_t index = 0;
  for (size_t i = 0; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') return false;
    index = index * 10 + (uint64_t)(str[i] - '0');
    if (index > UINT32_MAX) return false;
  }

  *out = (uint32_t)index;
  return true;
}

static ant_value_t Key_Convert(ant_t *js, ant_value_t key, napi_key_conversion conversion) {
  if (conversion != napi_key_keep_numbers || vtype(key) != kTypeString) return key;

  size_t len = 0;
  uint32_t index = 0;
  const char *str = js_getstr(js, key, &len);

  return Key_ParseIndex(str, len, &index) ? Ant_Value_FromNumber((double)index) : key;
}

static bool Key_PassesFilter(const ant_shape_prop_t *prop, ant_value_t key, napi_key_filter filter) {
  bool is_symbol = vtype(key) == kTypeSymbol;

  if (!prop) return false;
  if ((filter & napi_key_writable) && !(prop->attrs & ANT_PROP_ATTR_WRITABLE)) return false;
  if ((filter & napi_key_enumerable) && !(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) return false;
  if ((filter & napi_key_configurable) && !(prop->attrs & ANT_PROP_ATTR_CONFIGURABLE)) return false;
  if ((filter & napi_key_skip_strings) && !is_symbol) return false;
  if ((filter & napi_key_skip_symbols) && is_symbol) return false;

  return true;
}

static bool Key_IsOwn(ant_t *js, ant_value_t object, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) return lkp_sym(object, (ant_offset_t)vdata(key)).obj;

  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return str && lkp(js, object, str, len).obj;
}

static int Descriptor_Flags(napi_property_attributes attributes) {
  int flags = 0;
  if (attributes & napi_writable) flags |= JS_DESC_W;
  if (attributes & napi_enumerable) flags |= JS_DESC_E;
  if (attributes & napi_configurable) flags |= JS_DESC_C;
  return flags;
}

static napi_status Property_Get(napi_env env, napi_value object, ant_value_t key, napi_value *result) {
  ant_t *js = env->js;
  ant_value_t target = Napi_Decode(object);
  ant_value_t out = Ant_Value_Undefined;

  if (vtype(key) == kTypeSymbol) out = js_get_sym(js, target, key);
  else {
    size_t len = 0;
    const char *name = js_getstr(js, key, &len);
    out = js_getprop_fallback_len(js, target, name, len);
  }

  if (Napi_Failed(env, out)) return Ant_Napi_CapturePending(env, out);
  *result = Ant_Napi_Pin(env, out);
  return Napi_Ok(env);
}

static napi_status Property_Set(napi_env env, napi_value object, ant_value_t key, napi_value value) {
  ant_value_t stored = js_setprop(env->js, Napi_Decode(object), key, Napi_Decode(value));
  return Napi_Failed(env, stored) ? Ant_Napi_CapturePending(env, stored) : Napi_Ok(env);
}

static napi_status Property_Has(napi_env env, napi_value object, ant_value_t key, bool *result) {
  ant_value_t found = do_in(env->js, key, Napi_Decode(object));
  if (Napi_Failed(env, found)) return Ant_Napi_CapturePending(env, found);

  *result = js_truthy(env->js, found);
  return Napi_Ok(env);
}

static napi_status Property_Delete(napi_env env, napi_value object, ant_value_t key, bool *result) {
  ant_value_t deleted = Ant_Object_DeleteProperty(env->js, Napi_Decode(object), key);
  if (Napi_Failed(env, deleted)) return Ant_Napi_CapturePending(env, deleted);

  if (result) *result = js_truthy(env->js, deleted);
  return Napi_Ok(env);
}

static bool Property_ArgsValid(napi_env env, napi_value object) {
  return env && env->js && is_object_type(Napi_Decode(object));
}

static napi_status Property_DefineAccessor(
  napi_env env, ant_value_t target, ant_value_t key,
  const napi_property_descriptor *property
) {
  ant_t *js = env->js;
  int flags = Descriptor_Flags(property->attributes);
  ant_value_t getter = Ant_Value_Undefined;
  ant_value_t setter = Ant_Value_Undefined;

  if (property->getter) {
    napi_value fn = {0};
    napi_status status = Ant_Napi_CreateFunction(env, property->utf8name, NAPI_AUTO_LENGTH, property->getter, property->data, &fn);
    if (status != napi_ok) return status;
    getter = Napi_Decode(fn);
  }

  if (property->setter) {
    napi_value fn = {0};
    napi_status status = Ant_Napi_CreateFunction(env, property->utf8name, NAPI_AUTO_LENGTH, property->setter, property->data, &fn);
    if (status != napi_ok) return status;
    setter = Napi_Decode(fn);
  }

  if (vtype(key) == kTypeSymbol) {
    if (property->getter) js_set_sym_getter_desc(js, target, key, getter, flags);
    if (property->setter) js_set_sym_setter_desc(js, target, key, setter, flags);
  } else {
    size_t len = 0;
    const char *name = js_getstr(js, key, &len);
    js_set_accessor_desc(js, target, name, len, getter, setter, flags);
  }

  return Ant_Napi_CheckPending(env);
}

static napi_status Property_DefineData(
  napi_env env, ant_value_t target, ant_value_t key,
  const napi_property_descriptor *property
) {
  ant_t *js = env->js;
  ant_value_t value = Napi_Decode(property->value);

  if (property->method) {
    napi_value fn = {0};
    napi_status status = Ant_Napi_CreateFunction(env, property->utf8name, NAPI_AUTO_LENGTH, property->method, property->data, &fn);
    if (status != napi_ok) return status;
    value = Napi_Decode(fn);
  }

  if (vtype(key) == kTypeSymbol) js_set_sym(js, target, key, value);
  else {
    size_t len = 0;
    const char *name = js_getstr(js, key, &len);
    js_set(js, target, name, value);
    js_set_descriptor(js, target, name, len, Descriptor_Flags(property->attributes));
  }

  return Ant_Napi_CheckPending(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_object(napi_env env, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = Ant_Napi_Pin(env, Ant_Object_Create(env->js));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_array(napi_env env, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = Ant_Napi_Pin(env, Ant_Array_Create(env->js));
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_array_with_length(napi_env env, size_t length, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_t *js = env->js;
  ant_value_t array = Ant_Array_Create(js);
  ant_value_t stored = js_setprop(js, array, js->length_str, Ant_Value_FromNumber((double)length));
  if (Napi_Failed(env, stored)) return Ant_Napi_CapturePending(env, stored);

  *result = Ant_Napi_Pin(env, array);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_array_length(napi_env env, napi_value value, uint32_t *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t array = Napi_Decode(value);
  if (!js_is_array(array)) return Ant_Napi_SetLastError(env, napi_array_expected, NULL);

  *result = (uint32_t)js_arr_len(env->js, array);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_object_with_properties(
  napi_env env, napi_value prototype_or_null, const napi_value *property_names,
  const napi_value *property_values, size_t property_count, napi_value *result
) {
  if (!env || !env->js || !result || (property_count > 0 && (!property_names || !property_values)))
    return Napi_InvalidArg(env);

  ant_t *js = env->js;
  ant_value_t proto = Napi_Decode(prototype_or_null);
  bool null_proto = Napi_IsEmpty(prototype_or_null) || is_null(proto);
  if (!null_proto && !is_object_type(proto))
    return Ant_Napi_SetLastError(env, napi_invalid_arg, "prototype must be an object or null");

  for (size_t i = 0; i < property_count; i++) {
    uint8_t type = vtype(Napi_Decode(property_names[i]));
    if (type != kTypeString && type != kTypeSymbol) return Ant_Napi_SetLastError(env, napi_name_expected, NULL);
  }

  ant_value_t object = Ant_Object_CreateBare(js);
  if (is_err(object)) return Ant_Napi_CapturePending(env, object);
  if (!null_proto) js_set_proto_init(object, proto);
  *result = Ant_Napi_Pin(env, object);

  for (size_t i = 0; i < property_count; i++) {
    ant_value_t defined = js_create_data_property_or_throw(
      js, object, Napi_Decode(property_names[i]), Napi_Decode(property_values[i])
    );
    if (Napi_Failed(env, defined)) return Ant_Napi_CapturePending(env, defined);
  }

  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_prototype(napi_env env, napi_value object, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t proto = js_get_proto(env->js, Napi_Decode(object));
  if (Napi_Failed(env, proto)) return Ant_Napi_CapturePending(env, proto);

  *result = Ant_Napi_Pin(env, proto);
  return Napi_Ok(env);
}

static napi_status Object_CallBuiltin(
  napi_env env, ant_value_t (*builtin)(ant_params_t),
  ant_value_t *args, int nargs
) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (!is_object_type(args[0])) return Napi_ObjectExpected(env);

  ant_value_t out = builtin(env->js, args, nargs, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  return Napi_Failed(env, out) ? Ant_Napi_CapturePending(env, out) : Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_object_freeze(napi_env env, napi_value object) {
  ant_value_t args[] = { Napi_Decode(object) };
  return Object_CallBuiltin(env, builtin_object_freeze, args, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_object_seal(napi_env env, napi_value object) {
  ant_value_t args[] = { Napi_Decode(object) };
  return Object_CallBuiltin(env, builtin_object_seal, args, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_set_prototype(napi_env env, napi_value object, napi_value value) {
  ant_value_t args[] = { Napi_Decode(object), Napi_Decode(value) };
  if (!is_object_type(args[1]) && !is_null(args[1]))
    return Ant_Napi_SetLastError(env, napi_invalid_arg, "prototype must be an object or null");
  return Object_CallBuiltin(env, builtin_object_setPrototypeOf, args, 2);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_all_property_names(
  napi_env env, napi_value object, napi_key_collection_mode key_mode, napi_key_filter key_filter,
  napi_key_conversion key_conversion, napi_value *result
) {
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);

  ant_t *js = env->js;
  ant_value_t names = Ant_Array_Create(js);
  ant_value_t seen = Ant_Object_CreateBare(js);
  ant_value_t current = Napi_Decode(object);

  Ant_Napi_Pin(env, names);
  Ant_Napi_Pin(env, seen);

  while (is_object_type(current)) {
    ant_object_t *holder = js_obj_ptr(js_as_obj(current));
    ant_iter_t iter = js_prop_iter_begin(js, current);
    ant_value_t key = Ant_Value_Undefined;
    bool failed = false;

    while (!failed && js_prop_iter_next_val(&iter, &key, NULL)) {
      if (!holder || !holder->shape || iter.off == 0) continue;

      const ant_shape_prop_t *prop = ant_shape_prop_at(holder->shape, (uint32_t)(iter.off - 1));
      if (!Key_PassesFilter(prop, key, key_filter) || Key_IsOwn(js, seen, key)) continue;

      js_setprop(js, seen, key, Ant_Value_True);
      js_arr_push(js, names, Key_Convert(js, key, key_conversion));
      failed = Ant_Exception_Pending(js);
    }

    js_prop_iter_end(&iter);
    if (failed) return Ant_Napi_CheckPending(env);
    if (key_mode == napi_key_own_only) break;

    current = js_get_proto(js, current);
    if (Napi_Failed(env, current)) return Ant_Napi_CapturePending(env, current);
  }

  *result = Napi_Encode(names);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_property_names(napi_env env, napi_value object, napi_value *result) {
  return napi_get_all_property_names(
    env, object, napi_key_include_prototypes,
    napi_key_enumerable | napi_key_skip_symbols,
    napi_key_numbers_to_strings, result
  );
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
  if (!Property_ArgsValid(env, object)) return Napi_InvalidArg(env);
  return Property_Set(env, object, Napi_Decode(key), value);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_property(napi_env env, napi_value object, napi_value key, napi_value *result) {
  ant_value_t name = Ant_Value_Undefined;
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);

  napi_status status = Key_Normalize(env, key, &name);
  return status == napi_ok ? Property_Get(env, object, name, result) : status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_has_property(napi_env env, napi_value object, napi_value key, bool *result) {
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);
  return Property_Has(env, object, Napi_Decode(key), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_has_own_property(napi_env env, napi_value object, napi_value key, bool *result) {
  ant_value_t name = Napi_Decode(key);
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);
  if (vtype(name) != kTypeString && vtype(name) != kTypeSymbol)
    return Ant_Napi_SetLastError(env, napi_name_expected, NULL);

  ant_value_t found = Ant_Object_HasOwn(env->js, Napi_Decode(object), name);
  if (Napi_Failed(env, found)) return Ant_Napi_CapturePending(env, found);

  *result = js_truthy(env->js, found);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_delete_property(napi_env env, napi_value object, napi_value key, bool *result) {
  if (!Property_ArgsValid(env, object)) return Napi_InvalidArg(env);
  return Property_Delete(env, object, Napi_Decode(key), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_set_named_property(napi_env env, napi_value object, const char *utf8name, napi_value value) {
  if (!Property_ArgsValid(env, object) || !utf8name) return Napi_InvalidArg(env);
  return Property_Set(env, object, Key_FromName(env->js, utf8name), value);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_named_property(napi_env env, napi_value object, const char *utf8name, napi_value *result) {
  if (!Property_ArgsValid(env, object) || !utf8name || !result) return Napi_InvalidArg(env);
  return Property_Get(env, object, Key_FromName(env->js, utf8name), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_has_named_property(napi_env env, napi_value object, const char *utf8name, bool *result) {
  if (!Property_ArgsValid(env, object) || !utf8name || !result) return Napi_InvalidArg(env);
  return Property_Has(env, object, Key_FromName(env->js, utf8name), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value) {
  if (!Property_ArgsValid(env, object)) return Napi_InvalidArg(env);
  return Property_Set(env, object, Key_FromIndex(env->js, index), value);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value *result) {
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);
  return Property_Get(env, object, Key_FromIndex(env->js, index), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_has_element(napi_env env, napi_value object, uint32_t index, bool *result) {
  if (!Property_ArgsValid(env, object) || !result) return Napi_InvalidArg(env);
  return Property_Has(env, object, Key_FromIndex(env->js, index), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_delete_element(napi_env env, napi_value object, uint32_t index, bool *result) {
  if (!Property_ArgsValid(env, object)) return Napi_InvalidArg(env);
  return Property_Delete(env, object, Key_FromIndex(env->js, index), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_define_properties(
  napi_env env, napi_value object, size_t property_count,
  const napi_property_descriptor *properties
) {
  if (!Property_ArgsValid(env, object) || (property_count > 0 && !properties)) return Napi_InvalidArg(env);

  ant_t *js = env->js;
  ant_value_t target = js_as_obj(Napi_Decode(object));

  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor *property = &properties[i];
    ant_value_t key = property->utf8name ? Key_FromName(js, property->utf8name) : Napi_Decode(property->name);
    if (vtype(key) != kTypeString && vtype(key) != kTypeSymbol)
      return Ant_Napi_SetLastError(env, napi_name_expected, NULL);

    napi_status status = property->getter || property->setter
      ? Property_DefineAccessor(env, target, key, property)
      : Property_DefineData(env, target, key, property);
    if (status != napi_ok) return status;
  }

  return Napi_Ok(env);
}
