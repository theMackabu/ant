#include "ptr.h"
#include "cfunc.h"
#include "silver/vm.h"
#include "silver/engine.h"
#include "napi_internal.h"

void ant_napi_link_objects(void) {}

static bool napi_slot_get_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t *out) {
  ant_value_t value = js_get_slot(obj, slot);
  if (vtype(value) != kTypeNumber) return false;
  *out = (uint64_t)Ant_Value_AsNumberUnchecked(value);
  return true;
}

static void napi_slot_set_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t v) {
  js_set_slot(obj, slot, Ant_Value_FromNumber((double)v));
}

static napi_external_entry_t *napi_find_external(ant_t *js, napi_value value) {
  if (!is_object_type(napi_js(value))) return NULL;
  uint64_t id = 0;
  
  if (!napi_slot_get_u64(js, napi_js(value), SLOT_NAPI_EXTERNAL_ID, &id)) return NULL;
  napi_external_entry_t *entry = NULL;
  
  if (js->napi_env) HASH_FIND(hh, js->napi_env->externals, &id, sizeof(id), entry);
  
  return entry;
}

static napi_wrap_entry_t *napi_find_wrap(ant_t *js, napi_value value) {
  if (!is_object_type(napi_js(value))) return NULL;
  uint64_t id = 0;
  
  if (!napi_slot_get_u64(js, napi_js(value), SLOT_NAPI_WRAP_ID, &id)) return NULL;
  napi_wrap_entry_t *entry = NULL;
  
  if (js->napi_env) HASH_FIND(hh, js->napi_env->wraps, &id, sizeof(id), entry);
  
  return entry;
}

static napi_wrap_entry_t *napi_ensure_wrap(ant_napi_env_t *nenv, napi_value value) {
  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, value);
  if (entry) return entry;

  entry = (napi_wrap_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return NULL;

  entry->id = nenv->wrap_next_id++;
  HASH_ADD(hh, nenv->wraps, id, sizeof(entry->id), entry);
  napi_slot_set_u64(nenv->js, napi_js(value), SLOT_NAPI_WRAP_ID, entry->id);

  return entry;
}

bool ant_napi_is_external(ant_t *js, napi_value value) {
  return napi_find_external(js, value) != NULL;
}

static int napi_desc_flags(napi_property_attributes attributes) {
  int flags = 0;
  if (attributes & napi_writable) flags |= JS_DESC_W;
  if (attributes & napi_enumerable) flags |= JS_DESC_E;
  if (attributes & napi_configurable) flags |= JS_DESC_C;
  return flags;
}

static ant_value_t napi_callback_trampoline(ant_params_t) {
  napi_callback_binding_t *binding = (napi_callback_binding_t *)js_get_native(call_func, NAPI_CALLBACK_NATIVE_TAG);
  if (!binding || !binding->cb) return Ant_Value_Undefined;

  ant_napi_env_t *nenv = binding->env ? binding->env : ant_napi_get_or_create_env(js);
  if (!nenv) return js_mkerr(js, "napi OOM");

  struct napi_callback_info__ info = {
    .env = nenv,
    .argv = args,
    .argc = (size_t)(nargs < 0 ? 0 : nargs),
    .this_arg = call_this,
    .new_target = call_new_target,
    .data = binding->data,
  };

  napi_value ret = binding->cb((napi_env)nenv, (napi_callback_info)&info);
  if (is_err(nenv->exception)) {
    ant_value_t ex = nenv->exception;
    nenv->exception = Ant_Value_Undefined;
    return js_throw(js, ex);
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (napi_is_empty(ret)) return Ant_Value_Undefined;

  return napi_js(ret);
}

static napi_status napi_create_function_common(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback cb,
  void *data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !cb || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_callback_binding_t *binding = (napi_callback_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) return napi_set_last(env, napi_generic_failure, "out of memory");

  binding->env = nenv;
  binding->cb = cb;
  binding->data = data;

  ant_value_t fn = js_heavy_mkfun_native(nenv->js, napi_callback_trampoline, binding, NAPI_CALLBACK_NATIVE_TAG);
  js_mark_constructor(fn, true);
  if (utf8name && utf8name[0]) {
    size_t nlen = (length == NAPI_AUTO_LENGTH) ? strlen(utf8name) : length;
    js_set(nenv->js, fn, "name", js_mkstr(nenv->js, utf8name, nlen));
  }

  *result = NAPI_RETURN(nenv, fn);
  return napi_set_last(env, napi_ok, NULL);
}

static bool napi_parse_index_key(const char *str, size_t len, uint32_t *out) {
  if (!str || len == 0) return false;
  if (len > 1 && str[0] == '0') return false;

  uint64_t acc = 0;
  for (size_t i = 0; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') return false;
    acc = (acc * 10) + (uint64_t)(str[i] - '0');
    if (acc > UINT32_MAX) return false;
  }

  if (out) *out = (uint32_t)acc;
  return true;
}

static bool napi_seen_has_key(ant_t *js, ant_value_t seen, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) {
    return lkp_sym(seen, (ant_offset_t)vdata(key)).obj;
  }

  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return str && lkp(js, seen, str, len).obj;
}

static bool napi_seen_add_key(ant_t *js, ant_value_t seen, ant_value_t key) {
  ant_value_t res = js_setprop(js, seen, key, Ant_Value_True);
  return !is_err(res);
}

static bool napi_key_passes_filter(const ant_shape_prop_t *prop, napi_key_filter key_filter) {
  if (!prop) return false;
  if ((key_filter & napi_key_writable) && !(prop->attrs & ANT_PROP_ATTR_WRITABLE)) return false;
  if ((key_filter & napi_key_enumerable) && !(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) return false;
  if ((key_filter & napi_key_configurable) && !(prop->attrs & ANT_PROP_ATTR_CONFIGURABLE)) return false;
  return true;
}

static ant_value_t napi_convert_property_key(
  ant_t *js,
  ant_value_t key,
  napi_key_conversion key_conversion
) {
  if (key_conversion != napi_key_keep_numbers || vtype(key) != kTypeString) return key;

  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  uint32_t idx = 0;
  if (!str || !napi_parse_index_key(str, len, &idx)) return key;
  return Ant_Value_FromNumber((double)idx);
}


NAPI_EXTERN napi_status NAPI_CDECL napi_create_function(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback cb,
  void *data,
  napi_value *result
) {
  return napi_create_function_common(env, utf8name, length, cb, data, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external(
  napi_env env,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  napi_external_entry_t *entry = (napi_external_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");

  entry->id = nenv->external_next_id++;
  entry->data = data;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  HASH_ADD(hh, nenv->externals, id, sizeof(entry->id), entry);

  ant_value_t obj = Ant_Object_CreateBare(nenv->js);
  napi_slot_set_u64(nenv->js, obj, SLOT_NAPI_EXTERNAL_ID, entry->id);
  *result = NAPI_RETURN(nenv, obj);

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_external(
  napi_env env,
  napi_value value,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  napi_external_entry_t *entry = napi_find_external(nenv->js, value);
  if (!entry) return napi_set_last(env, napi_invalid_arg, "not an external");
  *result = entry->data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_cb_info(
  napi_env env,
  napi_callback_info cbinfo,
  size_t *argc,
  napi_value *argv,
  napi_value *this_arg,
  void **data
) {
  struct napi_callback_info__ *info = (struct napi_callback_info__ *)cbinfo;
  if (!env || !info) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (argc) {
    size_t requested = *argc;
    if (argv) {
      size_t ncopy = requested < info->argc ? requested : info->argc;
      for (size_t i = 0; i < ncopy; i++) argv[i] = napi_handle(info->argv[i]);
      for (size_t i = ncopy; i < requested; i++) argv[i] = napi_handle(Ant_Value_Undefined);
    }
    *argc = info->argc;
  }

  if (this_arg) *this_arg = napi_handle(info->this_arg);
  if (data) *data = info->data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_new_target(
  napi_env env,
  napi_callback_info cbinfo,
  napi_value *result
) {
  struct napi_callback_info__ *info = (struct napi_callback_info__ *)cbinfo;
  if (!env || !info || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t nt = info->new_target;
  *result = is_undefined(nt) ? (napi_value){0} : napi_handle(nt);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(
  napi_env env,
  napi_value object,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t out = Ant_Array_Create(nenv->js);
  ant_iter_t iter = js_prop_iter_begin(nenv->js, napi_js(object));
  const char *key = NULL;
  size_t key_len = 0;

  while (js_prop_iter_next(&iter, &key, &key_len, NULL)) {
    js_arr_push(nenv->js, out, js_mkstr(nenv->js, key, key_len));
  }
  js_prop_iter_end(&iter);

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_all_property_names(
  napi_env env,
  napi_value object,
  napi_key_collection_mode key_mode,
  napi_key_filter key_filter,
  napi_key_conversion key_conversion,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t out = Ant_Array_Create(js);
  ant_value_t seen = Ant_Object_CreateBare(js);
  ant_value_t current = napi_js(object);

  while (is_object_type(current)) {
    ant_iter_t iter = js_prop_iter_begin(js, current);
    ant_value_t key = Ant_Value_Undefined;

    while (js_prop_iter_next_val(&iter, &key, NULL)) {
    ant_object_t *obj_ptr = js_obj_ptr(js_as_obj(current));
    if (!obj_ptr || !obj_ptr->shape || iter.off == 0) continue;

    const ant_shape_prop_t *prop = ant_shape_prop_at(obj_ptr->shape, (uint32_t)(iter.off - 1));
    if (!napi_key_passes_filter(prop, key_filter)) continue;

    uint8_t key_type = vtype(key);
    if ((key_filter & napi_key_skip_strings) && key_type != kTypeSymbol) continue;
    if ((key_filter & napi_key_skip_symbols) && key_type == kTypeSymbol) continue;
    if (napi_seen_has_key(js, seen, key)) continue;
    if (!napi_seen_add_key(js, seen, key)) {
      js_prop_iter_end(&iter);
      return napi_set_last(env, napi_generic_failure, "failed to collect property names");
    }

    js_arr_push(js, out, napi_convert_property_key(js, key, key_conversion));
    if (Ant_Exception_Pending(js)) {
      js_prop_iter_end(&iter);
      return napi_check_pending_from_result(env, Ant_Value_Undefined);
    }}

    js_prop_iter_end(&iter);
    if (key_mode == napi_key_own_only) break;
    current = js_get_proto(js, current);
    if (is_err(current) || Ant_Exception_Pending(js)) return napi_check_pending_from_result(env, current);
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_property(
  napi_env env,
  napi_value object,
  napi_value key,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t r = js_setprop(nenv->js, napi_js(object), napi_js(key), napi_js(value));
  if (is_err(r) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(
  napi_env env,
  napi_value object,
  napi_value key,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = napi_js(key);
  if (vtype(k) == kTypeSymbol) {
    ant_prop_loc_t off = lkp_sym_proto(nenv->js, napi_js(object), (ant_offset_t)vdata(k));
    ant_value_t out = off.obj ? js_prop_load(off) : Ant_Value_Undefined;
    if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
    *result = NAPI_RETURN(nenv, out);
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr)) return napi_check_pending_from_result(env, kstr);

  size_t klen = 0;
  const char *ks = js_getstr(nenv->js, kstr, &klen);
  if (!ks) return napi_set_last(env, napi_string_expected, "string expected");

  char *name = (char *)malloc(klen + 1);
  if (!name) return napi_set_last(env, napi_generic_failure, "out of memory");
  memcpy(name, ks, klen);
  name[klen] = '\0';

  ant_value_t out = js_getprop_fallback(nenv->js, napi_js(object), name);
  free(name);
  if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = napi_js(key);
  if (vtype(k) == kTypeSymbol) {
    *result = lkp_sym_proto(nenv->js, napi_js(object), (ant_offset_t)vdata(k)).obj;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp_proto(nenv->js, napi_js(object), s, len).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = napi_js(key);
  ant_value_t del_result = Ant_Value_Undefined;

  if (vtype(k) == kTypeSymbol) {
    del_result = js_delete_sym_prop(nenv->js, napi_js(object), k);
  } else {
    ant_value_t kstr = coerce_to_str(nenv->js, k);
    if (is_err(kstr) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, kstr);
    size_t len = 0;
    const char *s = js_getstr(nenv->js, kstr, &len);
    del_result = js_delete_prop(nenv->js, napi_js(object), s, len);
  }

  if (is_err(del_result) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, del_result);
  *result = js_truthy(nenv->js, del_result);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !utf8name || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  js_set(nenv->js, napi_js(object), utf8name, napi_js(value));
  return ant_napi_return_pending_if_any(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t out = js_getprop_fallback(nenv->js, napi_js(object), utf8name);
  if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  *result = lkp_proto(nenv->js, napi_js(object), utf8name, strlen(utf8name)).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  int n = snprintf(idx, sizeof(idx), "%u", index);
  if (n < 0) return napi_set_last(env, napi_generic_failure, "index conversion failed");
  ant_value_t key = js_mkstr(nenv->js, idx, (size_t)n);
  ant_value_t r = js_setprop(
    nenv->js, napi_js(object),
    key, napi_js(value)
  );
  if (is_err(r) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t out = js_get(nenv->js, napi_js(object), idx);
  if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  *result = lkp_proto(nenv->js, napi_js(object), idx, strlen(idx)).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t del = js_delete_prop(nenv->js, napi_js(object), idx, strlen(idx));
  if (is_err(del) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, del);
  *result = js_truthy(nenv->js, del);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_define_properties(
  napi_env env,
  napi_value object,
  size_t property_count,
  const napi_property_descriptor *properties
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(object)) || (property_count > 0 && !properties)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor *p = &properties[i];
    ant_value_t key = Ant_Value_Undefined;
    const char *key_str = NULL;
    size_t key_len = 0;

    if (p->utf8name) {
      key_str = p->utf8name;
      key_len = strlen(p->utf8name);
      key = js_mkstr(nenv->js, key_str, key_len);
    } else key = napi_js(p->name);

    bool is_symbol = (vtype(key) == kTypeSymbol);
    if (!is_symbol && !key_str) {
      if (vtype(key) != kTypeString) continue;
      key_str = js_getstr(nenv->js, key, &key_len);
    }

    ant_value_t value = Ant_Value_Undefined;
    if (p->method) {
      napi_value fn = {0};
      napi_status st = napi_create_function_common(
        env,  p->utf8name,
        NAPI_AUTO_LENGTH,
        p->method, p->data, &fn
      );
      if (st != napi_ok) return st;
      value = napi_js(fn);
    } else if (p->getter || p->setter) {
      napi_value getter_fn = {0};
      napi_value setter_fn = {0};

      if (p->getter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->getter, p->data, &getter_fn);
        if (st != napi_ok) return st;
      }

      if (p->setter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->setter, p->data, &setter_fn);
        if (st != napi_ok) return st;
      }

      int flags = napi_desc_flags(p->attributes);
      ant_value_t desc_obj = js_as_obj(napi_js(object));

      if (is_symbol) {
        if (p->getter) js_set_sym_getter_desc(nenv->js, desc_obj, key, napi_js(getter_fn), flags);
        if (p->setter) js_set_sym_setter_desc(nenv->js, desc_obj, key, napi_js(setter_fn), flags);
      } else js_set_accessor_desc(
        nenv->js, desc_obj,
        key_str, key_len,
        p->getter ? napi_js(getter_fn) : Ant_Value_Undefined,
        p->setter ? napi_js(setter_fn) : Ant_Value_Undefined,
        flags
      );

      if (Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, Ant_Value_Undefined);
      continue;
    } else value = napi_js(p->value);

    if (is_symbol) js_set_sym(nenv->js, napi_js(object), key, value);
    else {
      js_set(nenv->js, napi_js(object), key_str, value);
      js_set_descriptor(nenv->js, js_as_obj(napi_js(object)), key_str, key_len, napi_desc_flags(p->attributes));
    }

    if (Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, Ant_Value_Undefined);
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_define_class(
  napi_env env,
  const char *utf8name,
  size_t length,
  napi_callback constructor,
  void *data,
  size_t property_count,
  const napi_property_descriptor *properties,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !constructor || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_status st = napi_create_function_common(
    env, utf8name, length, constructor, data, result
  );
  if (st != napi_ok) return st;

  ant_value_t ctor = napi_js(*result);
  js_mark_constructor(ctor, true);

  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_err(proto) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, proto);
  if (!is_object_type(proto)) {
    proto = Ant_Object_CreateBare(nenv->js);
    js_set(nenv->js, ctor, "prototype", proto);
    if (Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, Ant_Value_Undefined);
  }

  for (size_t i = 0; i < property_count; i++) {
    napi_property_descriptor tmp = properties[i];
    bool is_static = (tmp.attributes & napi_static) != 0;
    tmp.attributes = (napi_property_attributes)(tmp.attributes & ~napi_static);
    st = napi_define_properties(env, napi_handle(is_static ? ctor : proto), 1, &tmp);
    if (st != napi_ok) return st;
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_own_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = napi_js(key);
  if (vtype(k) == kTypeSymbol) {
    *result = lkp_sym(napi_js(object), (ant_offset_t)vdata(k)).obj;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp(nenv->js, napi_js(object), s, len).obj;
  return napi_set_last(env, napi_ok, NULL);
}


NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(
  napi_env env,
  napi_value recv,
  napi_value func,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_callable(napi_js(func))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_args_t call_args;
  if (!napi_args_decode(&call_args, argv, argc)) return napi_set_last(env, napi_generic_failure, "out of memory");

  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    napi_js(func),
    napi_js(recv),
    (ant_value_t *)call_args.values,
    (int)argc,
    NULL,
    Ant_Value_Undefined
  );
  napi_args_free(&call_args);

  if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
  if (result) *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_new_instance(
  napi_env env,
  napi_value constructor,
  size_t argc,
  const napi_value *argv,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_callable(napi_js(constructor))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t ctor = napi_js(constructor);
  ant_value_t obj = Ant_Object_CreateBare(nenv->js);
  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  napi_args_t call_args;
  if (!napi_args_decode(&call_args, argv, argc)) return napi_set_last(env, napi_generic_failure, "out of memory");

  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    ctor,
    obj,
    (ant_value_t *)call_args.values,
    (int)argc,
    NULL,
    ctor
  );
  napi_args_free(&call_args);

  if (is_err(out) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, (is_object_type(out) ? out : obj));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_wrap(
  napi_env env,
  napi_value js_object,
  void *native_object,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(js_object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_ensure_wrap(nenv, js_object);
  if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");

  entry->native_object = native_object;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  entry->has_wrap = true;

  if (result) {
    return napi_create_reference(env, js_object, 0, result);
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_add_finalizer(
  napi_env env,
  napi_value js_object,
  void *finalize_data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_ref *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(js_object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_ensure_wrap(nenv, js_object);
  if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");

  entry->attached_data = finalize_data;
  entry->attached_finalize_cb = finalize_cb;
  entry->attached_finalize_hint = finalize_hint;

  if (result) return napi_create_reference(env, js_object, 0, result);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unwrap(
  napi_env env,
  napi_value js_object,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type(napi_js(js_object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry || !entry->has_wrap) return napi_set_last(env, napi_invalid_arg, "object not wrapped");
  *result = entry->native_object;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_wrap(
  napi_env env,
  napi_value js_object,
  void **result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type(napi_js(js_object))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry || !entry->has_wrap) return napi_set_last(env, napi_invalid_arg, "object not wrapped");

  if (result) *result = entry->native_object;
  entry->native_object = NULL;
  entry->finalize_cb = NULL;
  entry->finalize_hint = NULL;
  entry->has_wrap = false;

  if (!entry->attached_finalize_cb && !entry->has_type_tag) {
    HASH_DEL(nenv->wraps, entry);
    free(entry);
    js_set_slot(napi_js(js_object), SLOT_NAPI_WRAP_ID, Ant_Value_Undefined);
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_type_tag_object(
  napi_env env, napi_value value,
  const napi_type_tag *type_tag
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !type_tag) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type(napi_js(value))) return napi_set_last(env, napi_object_expected, "object expected");

  napi_wrap_entry_t *entry = napi_ensure_wrap(nenv, value);
  if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");
  if (entry->has_type_tag) return napi_set_last(env, napi_invalid_arg, "object already has a type tag");

  entry->type_tag = *type_tag;
  entry->has_type_tag = true;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_check_object_type_tag(
  napi_env env, napi_value value,
  const napi_type_tag *type_tag, bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !type_tag || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type(napi_js(value))) return napi_set_last(env, napi_object_expected, "object expected");

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, value);
  *result = entry && entry->has_type_tag
    && entry->type_tag.lower == type_tag->lower
    && entry->type_tag.upper == type_tag->upper;
  return napi_set_last(env, napi_ok, NULL);
}

static napi_status napi_object_builtin_call(
  napi_env env,
  ant_value_t (*builtin)(ant_params_t),
  ant_value_t *args, int nargs
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  ant_value_t r = builtin(nenv->js, args, nargs, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  if (is_err(r) || Ant_Exception_Pending(nenv->js)) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type(napi_js(object))) return napi_set_last(env, napi_object_expected, "object expected");
  ant_value_t args[1] = {napi_js(object)};
  return napi_object_builtin_call(env, builtin_object_freeze, args, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type(napi_js(object))) return napi_set_last(env, napi_object_expected, "object expected");
  ant_value_t args[1] = {napi_js(object)};
  return napi_object_builtin_call(env, builtin_object_seal, args, 1);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_set_prototype(
  napi_env env, napi_value object, napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type(napi_js(object))) return napi_set_last(env, napi_object_expected, "object expected");
  if (!is_object_type(napi_js(value)) && !is_null(napi_js(value)))
    return napi_set_last(env, napi_invalid_arg, "prototype must be an object or null");
  ant_value_t args[2] = {napi_js(object), napi_js(value)};
  return napi_object_builtin_call(env, builtin_object_setPrototypeOf, args, 2);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_object_with_properties(
  napi_env env,
  napi_value prototype_or_null,
  const napi_value *property_names,
  const napi_value *property_values,
  size_t property_count,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || (property_count > 0 && (!property_names || !property_values))) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t proto = napi_js(prototype_or_null);
  bool null_proto = proto == 0 || is_null(proto);
  if (!null_proto && !is_object_type(proto))
    return napi_set_last(env, napi_invalid_arg, "prototype must be an object or null");

  for (size_t i = 0; i < property_count; i++) {
    uint8_t kt = vtype(napi_js(property_names[i]));
    if (kt != kTypeString && kt != kTypeSymbol) return napi_set_last(env, napi_name_expected, "name expected");
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return napi_check_pending_from_result(env, obj);
  if (!null_proto) js_set_proto_init(obj, proto);
  NAPI_RETURN(nenv, obj);

  for (size_t i = 0; i < property_count; i++) {
    ant_value_t r = js_create_data_property_or_throw(js, obj, napi_js(property_names[i]), napi_js(property_values[i]));
    if (is_err(r) || Ant_Exception_Pending(js)) return napi_check_pending_from_result(env, r);
  }

  *result = napi_handle(obj);
  return napi_set_last(env, napi_ok, NULL);
}
