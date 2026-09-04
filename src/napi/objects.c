#include "napi_internal.h"

static napi_external_entry_t *g_napi_externals = NULL;
static napi_wrap_entry_t *g_napi_wraps = NULL;

static uint64_t g_napi_external_next_id = 1;
static uint64_t g_napi_wrap_next_id = 1;

void ant_napi_link_objects(void) {}

static bool napi_slot_get_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t *out) {
  ant_value_t value = js_get_slot(obj, slot);
  if (vtype(value) != kTypeNumber) return false;
  *out = (uint64_t)js_getnum(value);
  return true;
}

static void napi_slot_set_u64(ant_t *js, ant_value_t obj, internal_slot_t slot, uint64_t v) {
  js_set_slot(obj, slot, js_mknum((double)v));
}

static napi_external_entry_t *napi_find_external(ant_t *js, napi_value value) {
  if (!is_object_type((ant_value_t)value)) return NULL;
  uint64_t id = 0;
  if (!napi_slot_get_u64(js, (ant_value_t)value, SLOT_NAPI_EXTERNAL_ID, &id)) return NULL;
  napi_external_entry_t *entry = NULL;
  HASH_FIND(hh, g_napi_externals, &id, sizeof(id), entry);
  return entry;
}

static napi_wrap_entry_t *napi_find_wrap(ant_t *js, napi_value value) {
  if (!is_object_type((ant_value_t)value)) return NULL;
  uint64_t id = 0;
  if (!napi_slot_get_u64(js, (ant_value_t)value, SLOT_NAPI_WRAP_ID, &id)) return NULL;
  napi_wrap_entry_t *entry = NULL;
  HASH_FIND(hh, g_napi_wraps, &id, sizeof(id), entry);
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

static ant_value_t napi_callback_trampoline(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t current = js_getcurrentfunc(js);
  napi_callback_binding_t *binding = (napi_callback_binding_t *)js_get_native(current, NAPI_CALLBACK_NATIVE_TAG);
  if (!binding || !binding->cb) return js_mkundef();

  ant_napi_env_t *nenv = binding->env ? binding->env : ant_napi_get_or_create_env(js);
  if (!nenv) return js_mkerr(js, "napi OOM");

  struct napi_callback_info__ info = {
    .env = nenv,
    .argv = (const napi_value *)args,
    .argc = (size_t)(nargs < 0 ? 0 : nargs),
    .this_arg = (napi_value)js_getthis(js),
    .new_target = (napi_value)sv_vm_get_new_target(js->vm, js),
    .data = binding->data,
  };

  napi_value ret = binding->cb((napi_env)nenv, (napi_callback_info)&info);
  if (nenv->has_pending_exception) {
    ant_value_t ex = (ant_value_t)nenv->pending_exception;
    nenv->has_pending_exception = false;
    nenv->pending_exception = (napi_value)js_mkundef();
    return js_throw(js, ex);
  }

  if (js->thrown_exists) {
    return js_throw(js, js->thrown_value);
  }

  if ((ant_value_t)ret == 0) return js_mkundef();
  return (ant_value_t)ret;
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
  ant_value_t res = js_setprop(js, seen, key, js_true);
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
  return js_mknum((double)idx);
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

  entry->id = g_napi_external_next_id++;
  entry->data = data;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  HASH_ADD(hh, g_napi_externals, id, sizeof(entry->id), entry);

  ant_value_t obj = js_mkobj(nenv->js);
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
      if (ncopy > 0) memcpy(argv, info->argv, ncopy * sizeof(napi_value));
      for (size_t i = ncopy; i < requested; i++) argv[i] = (napi_value)js_mkundef();
    }
    *argc = info->argc;
  }

  if (this_arg) *this_arg = info->this_arg;
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

  ant_value_t nt = (ant_value_t)info->new_target;
  *result = is_undefined(nt) ? (napi_value)0 : info->new_target;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(
  napi_env env,
  napi_value object,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t out = js_mkarr(nenv->js);
  ant_iter_t iter = js_prop_iter_begin(nenv->js, (ant_value_t)object);
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
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t out = js_mkarr(js);
  ant_value_t seen = js_mkobj(js);
  ant_value_t current = (ant_value_t)object;

  while (is_object_type(current)) {
    ant_iter_t iter = js_prop_iter_begin(js, current);
    ant_value_t key = js_mkundef();

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
    if (js->thrown_exists) {
      js_prop_iter_end(&iter);
      return napi_check_pending_from_result(env, js_mkundef());
    }}

    js_prop_iter_end(&iter);
    if (key_mode == napi_key_own_only) break;
    current = js_get_proto(js, current);
    if (is_err(current) || js->thrown_exists) return napi_check_pending_from_result(env, current);
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
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t r = js_setprop(nenv->js, (ant_value_t)object, (ant_value_t)key, (ant_value_t)value);
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(
  napi_env env,
  napi_value object,
  napi_value key,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == kTypeSymbol) {
    ant_prop_loc_t off = lkp_sym_proto(nenv->js, (ant_value_t)object, (ant_offset_t)vdata(k));
    ant_value_t out = off.obj ? js_prop_load(off) : js_mkundef();
    if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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

  ant_value_t out = js_getprop_fallback(nenv->js, (ant_value_t)object, name);
  free(name);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == kTypeSymbol) {
    *result = lkp_sym_proto(nenv->js, (ant_value_t)object, (ant_offset_t)vdata(k)).obj;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp_proto(nenv->js, (ant_value_t)object, s, len).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(
  napi_env env,
  napi_value object,
  napi_value key,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  ant_value_t del_result = js_mkundef();

  if (vtype(k) == kTypeSymbol) {
    del_result = js_delete_sym_prop(nenv->js, (ant_value_t)object, k);
  } else {
    ant_value_t kstr = coerce_to_str(nenv->js, k);
    if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
    size_t len = 0;
    const char *s = js_getstr(nenv->js, kstr, &len);
    del_result = js_delete_prop(nenv->js, (ant_value_t)object, s, len);
  }

  if (is_err(del_result) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, del_result);
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
  if (!nenv || !nenv->js || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  js_set(nenv->js, (ant_value_t)object, utf8name, (ant_value_t)value);
  return ant_napi_return_pending_if_any(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(
  napi_env env,
  napi_value object,
  const char *utf8name,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  ant_value_t out = js_getprop_fallback(nenv->js, (ant_value_t)object, utf8name);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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
  if (!nenv || !nenv->js || !result || !utf8name || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  *result = lkp_proto(nenv->js, (ant_value_t)object, utf8name, strlen(utf8name)).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value value
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  int n = snprintf(idx, sizeof(idx), "%u", index);
  if (n < 0) return napi_set_last(env, napi_generic_failure, "index conversion failed");
  ant_value_t key = js_mkstr(nenv->js, idx, (size_t)n);
  ant_value_t r = js_setprop(
    nenv->js, (ant_value_t)object,
    key, (ant_value_t)value
  );
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t out = js_get(nenv->js, (ant_value_t)object, idx);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  *result = lkp_proto(nenv->js, (ant_value_t)object, idx, strlen(idx)).obj;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_element(
  napi_env env,
  napi_value object,
  uint32_t index,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }
  char idx[32];
  snprintf(idx, sizeof(idx), "%u", index);
  ant_value_t del = js_delete_prop(nenv->js, (ant_value_t)object, idx, strlen(idx));
  if (is_err(del) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, del);
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
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)object) || (property_count > 0 && !properties)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor *p = &properties[i];
    ant_value_t key = js_mkundef();
    const char *key_str = NULL;
    size_t key_len = 0;

    if (p->utf8name) {
      key_str = p->utf8name;
      key_len = strlen(p->utf8name);
      key = js_mkstr(nenv->js, key_str, key_len);
    } else key = (ant_value_t)p->name;

    bool is_symbol = (vtype(key) == kTypeSymbol);
    if (!is_symbol && !key_str) {
      if (vtype(key) != kTypeString) continue;
      key_str = js_getstr(nenv->js, key, &key_len);
    }

    ant_value_t value = js_mkundef();
    if (p->method) {
      napi_value fn = 0;
      napi_status st = napi_create_function_common(
        env,  p->utf8name,
        NAPI_AUTO_LENGTH,
        p->method, p->data, &fn
      );
      if (st != napi_ok) return st;
      value = (ant_value_t)fn;
    } else if (p->getter || p->setter) {
      napi_value getter_fn = 0;
      napi_value setter_fn = 0;

      if (p->getter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->getter, p->data, &getter_fn);
        if (st != napi_ok) return st;
      }

      if (p->setter) {
        napi_status st = napi_create_function_common(env, p->utf8name, NAPI_AUTO_LENGTH, p->setter, p->data, &setter_fn);
        if (st != napi_ok) return st;
      }

      int flags = napi_desc_flags(p->attributes);
      ant_value_t desc_obj = js_as_obj((ant_value_t)object);

      if (is_symbol) {
        if (p->getter) js_set_sym_getter_desc(nenv->js, desc_obj, key, (ant_value_t)getter_fn, flags);
        if (p->setter) js_set_sym_setter_desc(nenv->js, desc_obj, key, (ant_value_t)setter_fn, flags);
      } else js_set_accessor_desc(
        nenv->js, desc_obj,
        key_str, key_len,
        p->getter ? (ant_value_t)getter_fn : js_mkundef(),
        p->setter ? (ant_value_t)setter_fn : js_mkundef(),
        flags
      );

      if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
      continue;
    } else value = (ant_value_t)p->value;

    if (is_symbol) js_set_sym(nenv->js, (ant_value_t)object, key, value);
    else {
      js_set(nenv->js, (ant_value_t)object, key_str, value);
      js_set_descriptor(nenv->js, js_as_obj((ant_value_t)object), key_str, key_len, napi_desc_flags(p->attributes));
    }

    if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
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

  ant_value_t ctor = (ant_value_t)*result;
  js_mark_constructor(ctor, true);

  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_err(proto) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, proto);
  if (!is_object_type(proto)) {
    proto = js_mkobj(nenv->js);
    js_set(nenv->js, ctor, "prototype", proto);
    if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  }

  for (size_t i = 0; i < property_count; i++) {
    napi_property_descriptor tmp = properties[i];
    bool is_static = (tmp.attributes & napi_static) != 0;
    tmp.attributes = (napi_property_attributes)(tmp.attributes & ~napi_static);
    st = napi_define_properties(env, (napi_value)(is_static ? ctor : proto), 1, &tmp);
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
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t k = (ant_value_t)key;
  if (vtype(k) == kTypeSymbol) {
    *result = lkp_sym((ant_value_t)object, (ant_offset_t)vdata(k)).obj;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t kstr = coerce_to_str(nenv->js, k);
  if (is_err(kstr) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, kstr);
  size_t len = 0;
  const char *s = js_getstr(nenv->js, kstr, &len);
  *result = s && lkp(nenv->js, (ant_value_t)object, s, len).obj;
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
  if (!nenv || !nenv->js || !is_callable((ant_value_t)func)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    (ant_value_t)func,
    (ant_value_t)recv,
    (ant_value_t *)argv,
    (int)argc,
    NULL,
    false
  );

  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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
  if (!nenv || !nenv->js || !result || !is_callable((ant_value_t)constructor)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_value_t ctor = (ant_value_t)constructor;
  ant_value_t obj = js_mkobj(nenv->js);
  ant_value_t proto = js_get(nenv->js, ctor, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t saved = nenv->js->new_target;
  nenv->js->new_target = ctor;
  ant_value_t out = sv_vm_call(
    nenv->js->vm,
    nenv->js,
    ctor,
    obj,
    (ant_value_t *)argv,
    (int)argc,
    NULL,
    true
  );
  nenv->js->new_target = saved;

  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
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
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry) {
    entry = (napi_wrap_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");
    entry->id = g_napi_wrap_next_id++;
    HASH_ADD(hh, g_napi_wraps, id, sizeof(entry->id), entry);
    napi_slot_set_u64(nenv->js, (ant_value_t)js_object, SLOT_NAPI_WRAP_ID, entry->id);
  }

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
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry) {
    entry = (napi_wrap_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) return napi_set_last(env, napi_generic_failure, "out of memory");
    entry->id = g_napi_wrap_next_id++;
    HASH_ADD(hh, g_napi_wraps, id, sizeof(entry->id), entry);
    napi_slot_set_u64(nenv->js, (ant_value_t)js_object, SLOT_NAPI_WRAP_ID, entry->id);
  }

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
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)js_object)) {
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
  if (!nenv || !nenv->js || !is_object_type((ant_value_t)js_object)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  napi_wrap_entry_t *entry = napi_find_wrap(nenv->js, js_object);
  if (!entry || !entry->has_wrap) return napi_set_last(env, napi_invalid_arg, "object not wrapped");

  if (result) *result = entry->native_object;
  entry->native_object = NULL;
  entry->finalize_cb = NULL;
  entry->finalize_hint = NULL;
  entry->has_wrap = false;

  if (!entry->attached_finalize_cb) {
    HASH_DEL(g_napi_wraps, entry);
    free(entry);
    js_set_slot((ant_value_t)js_object, SLOT_NAPI_WRAP_ID, js_mkundef());
  }
  return napi_set_last(env, napi_ok, NULL);
}
