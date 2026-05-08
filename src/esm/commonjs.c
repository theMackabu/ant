#include "esm/commonjs.h"
#include "esm/loader.h"

#include "internal.h"
#include "reactor.h"
#include "errors.h"

#include "silver/compiler.h"
#include "silver/engine.h"

#include <libgen.h>
#include <string.h>
#include <stdlib.h>

static ant_value_t cjs_module_exports_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t state = js_get_slot(fn, SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();
  return js_get(js, state, "value");
}

static ant_value_t cjs_module_exports_setter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t state = js_get_slot(fn, SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  js_set(js, state, "value", value);

  ant_value_t ns = js_get(js, state, "namespace");
  if (is_object_type(ns)) {
    js_set_slot_wb(js, ns, SLOT_DEFAULT, value);
    setprop_cstr(js, ns, "default", 7, value);
  }

  return js_mkundef();
}

static ant_value_t esm_cjs_require(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "require() expects a string specifier");

  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t data = js_get_slot(fn, SLOT_DATA);
  const char *base_path = js_module_eval_active_filename(js);

  if (vtype(data) == T_STR) {
    ant_offset_t path_len = 0;
    ant_offset_t path_off = vstr(js, data, &path_len);
    base_path = (const char *)(uintptr_t)(path_off);
  }

  ant_value_t ns = js_esm_import_sync_from_require(js, args[0], base_path);
  if (is_err(ns)) return ns;

  if (vtype(ns) == T_OBJ) {
    ant_value_t default_export = js_get_slot(ns, SLOT_DEFAULT);
    if (vtype(default_export) != T_UNDEF) return default_export;
  }
  
  return ns;
}

static ant_value_t esm_cjs_require_resolve(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "require.resolve() expects a string specifier");
  }

  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t data = js_get_slot(fn, SLOT_DATA);
  const char *base_path = js_module_eval_active_filename(js);

  if (vtype(data) == T_STR) {
    ant_offset_t data_len = 0;
    ant_offset_t data_off = vstr(js, data, &data_len);
    base_path = (const char *)(uintptr_t)(data_off);
  }

  ant_value_t resolved = js_esm_resolve_specifier_require(js, args[0], base_path);
  if (is_err(resolved)) return resolved;
  if (vtype(resolved) != T_STR) return resolved;

  ant_offset_t len = 0;
  ant_offset_t off = vstr(js, resolved, &len);
  
  const char *s = (const char *)(uintptr_t)(off);
  static const char *prefix = "file://";

  if ((size_t)len >= strlen(prefix) && strncmp(s, prefix, strlen(prefix)) == 0) {
    const char *path_part = s + strlen(prefix);
    size_t path_len = (size_t)len - strlen(prefix);
    return js_mkstr(js, path_part, path_len);
  }

  return resolved;
}

static bool copy_own_prop(
  ant_t *js, ant_value_t dst, ant_value_t src,
  const char *key, size_t key_len, ant_value_t *err
) {
  ant_value_t value = js_mkundef();

  if (js_try_get_own_data_prop(js, src, key, key_len, &value)) {
    ant_value_t res = setprop_cstr(js, dst, key, key_len, value);
    if (is_err(res)) { *err = res; return false; }
    return true;
  }

  prop_meta_t meta;
  if (lookup_string_prop_meta(js, src, key, key_len, &meta) && (meta.has_getter || meta.has_setter)) {
    ant_value_t ns = js_as_obj(dst);
    int flags = (meta.enumerable   ? JS_DESC_E : 0) | (meta.configurable ? JS_DESC_C : 0);
    if (meta.has_getter) js_set_getter_desc(js, ns, key, key_len, meta.getter, flags);
    if (meta.has_setter) js_set_setter_desc(js, ns, key, key_len, meta.setter, flags);
    return true;
  }

  value = js_get(js, src, key);
  if (is_err(value)) { *err = value; return false; }
  if (vtype(value) == T_UNDEF) return true;

  ant_value_t res = setprop_cstr(js, dst, key, key_len, value);
  if (is_err(res)) { *err = res; return false; }
  
  return true;
}

static ant_value_t esm_populate_cjs_namespace(ant_t *js, ant_value_t ns, ant_value_t exports_val) {
  ant_value_t set_default = setprop_cstr(js, ns, "default", 7, exports_val);
  if (is_err(set_default)) return set_default;
  
  js_set_slot_wb(js, ns, SLOT_DEFAULT, exports_val);
  if (!is_object_type(exports_val)) return js_mkundef();

  ant_iter_t iter = js_prop_iter_begin(js, exports_val);
  const char *key = NULL;
  size_t key_len = 0;
  
  while (js_prop_iter_next(&iter, &key, &key_len, NULL)) {
  if (key_len == 7 && memcmp(key, "default", 7) == 0) continue;
  
  ant_value_t err = js_mkundef();
  if (!copy_own_prop(js, ns, exports_val, key, key_len, &err)) {
    if (is_err(err)) { js_prop_iter_end(&iter); return err; }
  }}

  js_prop_iter_end(&iter);
  return js_mkundef();
}

static ant_value_t esm_eval_commonjs_function(
  ant_t *js,
  const char *code,
  size_t code_len,
  ant_value_t require_fn,
  ant_value_t module_obj,
  ant_value_t exports_obj,
  ant_value_t filename_val,
  ant_value_t dirname_val
) {
  static const sv_param_t cjs_params[] = {
    SV_PARAM("require"),
    SV_PARAM("module"),
    SV_PARAM("exports"),
    SV_PARAM("__filename"),
    SV_PARAM("__dirname"),
  };

  sv_func_t *compiled = sv_compile_function_with_params(
    js, cjs_params,
    (int)(sizeof(cjs_params) / sizeof(cjs_params[0])),
    code, code_len, false
  );

  if (!compiled) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
  }

  js_clear_error_site(js);
  ant_value_t args[] = {require_fn, module_obj, exports_obj, filename_val, dirname_val};
  return sv_execute_entry(js->vm, compiled, exports_obj, args, 5);
}

ant_value_t esm_load_commonjs_module(
  ant_t *js,
  const char *module_path, const char *code,
  size_t code_len, ant_value_t ns
) {
  char *path_copy = strdup(module_path);
  if (!path_copy) return js_mkerr(js, "OOM loading CommonJS module");

  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t module_obj = js_mkobj(js);
  ant_value_t exports_obj = js_mkobj(js);
  ant_value_t exports_state = js_mkobj(js);
  
  if (is_object_type(object_proto)) {
    js_set_proto_init(module_obj, object_proto);
    js_set_proto_init(exports_obj, object_proto);
    js_set_proto_init(exports_state, object_proto);
  }
  
  js_set(js, exports_state, "value", exports_obj);
  js_set(js, exports_state, "namespace", ns);
  
  js_set_getter_desc(
    js, js_as_obj(module_obj), "exports", 7,
    js_heavy_mkfun(js, cjs_module_exports_getter, exports_state),
    JS_DESC_E | JS_DESC_C
  );
  
  js_set_setter_desc(
    js, js_as_obj(module_obj), "exports", 7,
    js_heavy_mkfun(js, cjs_module_exports_setter, exports_state),
    JS_DESC_E | JS_DESC_C
  );
  
  js_set_slot_wb(js, ns, SLOT_DEFAULT, exports_obj);
  setprop_cstr(js, ns, "default", 7, exports_obj);
  js_set(js, module_obj, "loaded", js_false);
  js_set(js, module_obj, "id", js_mkstr(js, module_path, strlen(module_path)));
  js_set(js, module_obj, "filename", js_mkstr(js, module_path, strlen(module_path)));

  ant_value_t require_fn = js_heavy_mkfun(
    js, esm_cjs_require,
    js_mkstr(js, module_path, strlen(module_path))
  );
  
  ant_value_t require_resolve_fn = js_heavy_mkfun(
    js, esm_cjs_require_resolve,
    js_mkstr(js, module_path, strlen(module_path))
  );
  
  js_set(js, require_fn, "resolve", require_resolve_fn);

  char *dir = dirname(path_copy);
  ant_value_t dirname_val = js_mkstr(js, dir, strlen(dir));
  ant_value_t filename_val = js_mkstr(js, module_path, strlen(module_path));

  const char *prev_filename = js->filename;
  js_set_filename(js, module_path);

  ant_value_t result = esm_eval_commonjs_function(
    js, code, code_len,
    require_fn, module_obj, exports_obj,
    filename_val, dirname_val
  );
  
  if (vtype(result) == T_PROMISE) js_run_event_loop(js);
  js_set(js, module_obj, "loaded", js_true);
  ant_value_t exports_val = js_get(js, module_obj, "exports");
  
  if (!is_err(result) && !js->thrown_exists) {
    ant_value_t ns_res = esm_populate_cjs_namespace(js, ns, exports_val);
    if (is_err(ns_res)) result = ns_res;
  }

  js_set_filename(js, prev_filename);
  free(path_copy);

  if (is_err(result)) return result;
  return exports_val;
}
