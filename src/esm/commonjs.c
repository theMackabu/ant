#include "esm/commonjs.h"
#include "cfunc.h"
#include "esm/loader.h"
#include "loader_internal.h"
#include "modules/module.h"
#include "gc/roots.h"
#include "esm/library.h"
#include "gc/strings.h"
#include "shapes.h"

#include "internal.h"
#include "reactor.h"
#include "errors.h"

#include "silver/compiler.h"
#include "silver/engine.h"

#include <string.h>
#include <stdlib.h>

static bool esm_require_cache_own(ant_t *js, ant_value_t ctor, ant_value_t *out) {
  ant_value_t obj = vtype(ctor) == kTypeFunction ? js_func_obj(ctor) : ctor;
  ant_prop_loc_t loc = lkp_interned(obj, js->intern.module_cache);
  if (!loc.obj) return false;
  
  const ant_shape_prop_t *prop = ant_shape_prop_at(loc.obj->shape, loc.slot);
  if (!prop || prop->has_getter || prop->has_setter) return false;
  *out = js_prop_load(loc);
  
  return true;
}

ant_value_t esm_require_cache(ant_t *js) {
  ant_value_t cache;
  
  if (is_object_type(js->modules.cjs.constructor)) 
    return esm_require_cache_own(js, js->modules.cjs.constructor, &cache) 
    ? cache : js_get(js, js->modules.cjs.constructor, "_cache");
  
  if (!is_object_type(js->modules.cjs.cache)) {
    js->modules.cjs.cache = Ant_Object_CreateBare(js);
    js_set_proto_init(js->modules.cjs.cache, js_mknull());
  }
  
  return js->modules.cjs.cache;
}

static bool esm_require_cache_get_plain(ant_t *js, ant_value_t cache, const char *key, size_t len, ant_value_t *out) {
  if (vtype(cache) != kTypeObject) return false;
  
  ant_object_t *ptr = js_obj_ptr(cache);
  if (!ptr || !ptr->shape || ptr->flags.is_exotic) return false;

  const char *interned = intern_find(js, key, len);
  int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!prop || prop->has_getter || prop->has_setter) return false;
    *out = js_prop_load((ant_prop_loc_t){ ptr, (uint32_t)slot });
    return true;
  }

  if (vtype(js_get_proto(js, cache)) != kTypeNull) return false;
  *out = js_mkundef();
  
  return true;
}

ant_value_t esm_require_cache_lookup(ant_t *js, const char *key) {
  ant_value_t cache = esm_require_cache(js);
  if (is_err(cache)) return cache;
  
  if (vtype(cache) == kTypeNull || vtype(cache) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read properties of require cache");
  
  size_t len = strlen(key);
  ant_value_t hit;
  
  return esm_require_cache_get_plain(js, cache, key, len, &hit) ? hit : js_getprop_fallback_len(js, cache, key, len);
}

ant_value_t esm_require_cache_store(ant_t *js, const char *key, ant_value_t module) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, module);
  
  ant_value_t cache = esm_require_cache(js);
  GC_ROOT_PIN(js, cache);
  
  ant_value_t result = cache;
  if (is_err(cache)) goto done;
  
  if (!is_object_type(cache)) {
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot write to require cache");
    goto done;
  }

  ant_value_t object = js_as_obj(cache);
  if (!is_proxy(object)) {
    bool writable = js_obj_ptr(object)->flags.extensible;
    ant_value_t current = object;
    
    while (is_object_type(current)) {
      if (is_proxy(current)) { writable = true; break; }
      prop_meta_t meta;
      
      if (lookup_string_prop_meta(js, current, key, strlen(key), &meta)) {
        if (meta.has_getter || meta.has_setter) writable = meta.has_setter;
        else writable = meta.writable && (current == object || writable);
        break;
      }
      current = js_get_proto(js, current);
    }
    
    if (!writable) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot write require cache entry '%s'", key);
      goto done;
    }
  }
  
  result = js_setprop(js, cache, js_mkstr(js, key, strlen(key)), module);
  if (Ant_Exception_Pending(js)) result = Ant_Exception_Current(js);
done:
  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t esm_require_cached_exports(ant_t *js, ant_value_t cached) {
  if (is_err(cached)) return cached;
  if (vtype(cached) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read properties of null cache entry");
  return js_getprop_fallback_len(js, cached, "exports", 7);
}

ant_value_t esm_init_cjs_module(ant_t *js, ant_value_t obj, const char *id, ant_value_t parent) {
  char *directory = esm_path_dirname(id);
  if (!directory) return js_mkerr(js, "Cannot allocate module directory");
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, parent);
  
  js_set(js, obj, "id", js_mkstr(js, id, strlen(id)));
  js_set(js, obj, "path", js_mkstr(js, directory, strlen(directory)));
  free(directory);
  
  js_set(js, obj, "filename", js_mknull());
  ant_value_t exports = Ant_Object_CreateBare(js);
  
  js_set_proto_init(exports, js->sym.object_proto);
  js_set(js, obj, "exports", exports);
  js_set(js, obj, "loaded", js_false);
  js_set(js, obj, "children", js_mkarr(js));
  js_set(js, obj, "parent", parent);
  
  esm_cjs_update_children(js, parent, obj, false);
  GC_ROOT_RESTORE(js, mark);
  
  return obj;
}

ant_value_t esm_create_cjs_module(ant_t *js, const char *filename, ant_value_t parent) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, parent);
  
  ant_value_t ctor = module_library(js);
  ant_value_t obj = Ant_Object_CreateBare(js);
  
  GC_ROOT_PIN(js, obj);
  js_set_proto_wb(js, obj, js_get(js, ctor, "prototype"));
  ant_value_t result = esm_init_cjs_module(js, obj, filename, parent);
  
  if (!is_err(result)) {
    ant_value_t directory = js_get(js, obj, "path");
    result = esm_node_module_paths(js, js_getstr(js, directory, NULL));
    if (!is_err(result)) {
      js_set(js, obj, "filename", js_get(js, obj, "id"));
      js_set(js, obj, "paths", result);
      result = obj;
    }
  }
  
  if (is_err(result)) esm_cjs_update_children(js, parent, obj, true);
  GC_ROOT_RESTORE(js, mark);
  
  return result;
}

ant_value_t esm_require_namespace(ant_t *js, ant_value_t exports) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, exports);
  ant_value_t ns = Ant_Object_CreateBare(js);
  
  GC_ROOT_PIN(js, ns);
  js_set_slot_wb(js, ns, SLOT_DEFAULT, exports);
  
  js_set(js, ns, "default", exports);
  GC_ROOT_RESTORE(js, mark);
  
  return ns;
}

ant_value_t esm_require_unwrap(ant_t *js, ant_value_t ns) {
  if (is_err(ns)) return ns;
  ant_value_t value;
  if (is_object_type(ns) && js_try_get_own_data_prop(js, ns, "default", 7, &value)) return value;
  return ns;
}

void esm_cjs_update_children(ant_t *js, ant_value_t parent, ant_value_t child, bool remove) {
  if (!is_object_type(parent)) return;
  ant_value_t children = js_get(js, parent, "children");
  
  if (vtype(children) != kTypeArray) return;
  ant_offset_t len = js_arr_len(js, children);
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (js_arr_get(js, children, i) != child) continue;
    if (remove) {
      for (ant_offset_t j = i + 1; j < len; j++)
        js_setprop(js, children, js_mknum((double)(j - 1)), js_arr_get(js, children, j));
      js_setprop(js, children, js->length_str, js_mknum((double)(len - 1)));
    }
    return;
  }
  
  if (!remove) js_arr_push(js, children, child);
}

static ant_value_t esm_cjs_require_parent(ant_t *js) {
  ant_value_t parent = js_func_data(js->current_func);
  return is_object_type(parent) ? parent : js->this_val;
}

static bool esm_cjs_builtin_unshadowed(ant_t *js, const char *name) {
  return vtype(esm_require_cache_lookup(js, name)) == kTypeUndefined;
}

typedef enum {
  CJS_BUILTIN_UNKNOWN,
  CJS_BUILTIN_NONE,
  CJS_BUILTIN_HIT,
} esm_cjs_builtin_t;

static esm_cjs_builtin_t esm_cjs_require_builtin(ant_t *js, ant_value_t specifier, ant_value_t *out) {
  if (!esm_require_builtin_cacheable(js)) return CJS_BUILTIN_UNKNOWN;

  uint32_t slot = (uint32_t)((specifier >> 3) ^ (specifier >> 11)) & (CJS_BUILTIN_CACHE_SIZE - 1);
  __typeof__(js->modules.cjs.builtins[0]) *entry = &js->modules.cjs.builtins[slot];

  size_t len = 0;
  if (entry->specifier == specifier) {
    if (entry->bare && !esm_cjs_builtin_unshadowed(js, js_getstr(js, specifier, NULL))) return CJS_BUILTIN_UNKNOWN;
    *out = entry->exports;
    return CJS_BUILTIN_HIT;
  }

  const char *name = js_getstr(js, specifier, &len);
  if (!name) return CJS_BUILTIN_UNKNOWN;
  if (name[0] == '.' || name[0] == '/') return CJS_BUILTIN_NONE;

  esm_builtin_t builtin = esm_find_builtin(name, len);
  bool bare = builtin.kind == ANT_BUILTIN_NAME_BARE;
  if (builtin.kind == ANT_BUILTIN_NAME_NONE) return CJS_BUILTIN_NONE;
  if (bare && !esm_cjs_builtin_unshadowed(js, name)) return CJS_BUILTIN_UNKNOWN;

  GC_ROOT_SAVE(mark, js);
  ant_value_t previous = js->modules.cjs.parent;
  
  GC_ROOT_PIN(js, previous);
  js->modules.cjs.parent = esm_cjs_require_parent(js);
  
  ant_value_t ns = esm_require_load_builtin(js, name, builtin);
  js->modules.cjs.parent = previous;
  GC_ROOT_RESTORE(js, mark);

  *out = esm_require_unwrap(js, ns);
  if (!is_err(*out)) {
    entry->specifier = specifier;
    entry->exports = *out;
    entry->bare = bare;
  }

  return CJS_BUILTIN_HIT;
}

ant_value_t esm_cjs_require_module(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr(js, "require() expects a string specifier");
  
  ant_value_t builtin;
  esm_cjs_builtin_t found = esm_cjs_require_builtin(js, args[0], &builtin);
  if (found == CJS_BUILTIN_HIT) return builtin;
  
  ant_value_t parent = esm_cjs_require_parent(js);
  ant_value_t filename = js_get(js, parent, "filename");
  
  const char *base = vtype(filename) == kTypeString ? js_getstr(js, filename, NULL) : NULL;
  GC_ROOT_SAVE(mark, js);
  
  ant_value_t previous = js->modules.cjs.parent;
  GC_ROOT_PIN(js, previous);
  
  js->modules.cjs.parent = parent;
  size_t spec_len = 0;
  
  const char *spec = js_getstr(js, args[0], &spec_len);
  ant_value_t ns = esm_require_cstr(js, spec, spec_len, base, found == CJS_BUILTIN_NONE);
  
  js->modules.cjs.parent = previous;
  GC_ROOT_RESTORE(js, mark);
  
  return esm_require_unwrap(js, ns);
}

static ant_value_t esm_cjs_require_paths(ant_params_t) {
  if (!nargs || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "require.resolve.paths expects a string");
  
  size_t len;
  const char *spec = js_getstr(js, args[0], &len);
  if (esm_find_builtin(spec, len).kind != ANT_BUILTIN_NAME_NONE) return js_mknull();
  
  ant_value_t module = js_func_data(js->current_func);
  ant_value_t directory = js_get(js, module, "path");
  
  const char *path = js_getstr(js, directory, NULL);
  if (!path) return js_mkerr(js, "Module has no search directory");
  
  bool relative = spec[0] == '.' && (spec[1] == '/' || (spec[1] == '.' && spec[2] == '/'));
#ifdef _WIN32
  relative |= spec[0] == '.' && (spec[1] == '\\' || (spec[1] == '.' && spec[2] == '\\'));
#endif
  if (!relative) return esm_node_module_paths(js, path);
  
  ant_value_t paths = js_mkarr(js);
  js_arr_push(js, paths, directory);
  
  return paths;
}

static ant_value_t esm_cjs_require_resolve(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr(js, "require.resolve() expects a string specifier");
  
  ant_value_t module = js_func_data(js->current_func);
  ant_value_t filename = js_get(js, module, "filename");
  
  const char *base = js_getstr(js, filename, NULL);
  ant_value_t paths = nargs > 1 && is_object_type(args[1]) ? js_get(js, args[1], "paths") : js_mkundef();
  ant_value_t resolved = js_mkundef();
  
  if (vtype(paths) != kTypeArray) resolved = js_esm_resolve_specifier_require(js, args[0], base);
  else {
    ant_offset_t count = js_arr_len(js, paths);
    for (ant_offset_t i = 0; i < count; i++) {
      ant_value_t path = js_arr_get(js, paths, i);
      if (vtype(path) != kTypeString) continue;
      if (is_err(resolved)) Ant_Exception_Clear(js);
      resolved = js_esm_resolve_specifier_require(js, args[0], js_getstr(js, path, NULL));
      if (!is_err(resolved) && vtype(resolved) == kTypeString) break;
    }
  }
  
  if (is_err(resolved)) return resolved;
  if (vtype(resolved) != kTypeString) return js_mkerr(js, "Cannot resolve module");
  
  char *path = esm_file_url_to_path(js, js_getstr(js, resolved, NULL));
  if (!path) return resolved;
  ant_value_t result = js_mkstr(js, path, strlen(path));
  free(path);
  
  return result;
}

ant_value_t esm_create_require(ant_t *js, ant_value_t module) {
  if (is_err(module)) return module;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, module);
  ant_value_t require = js_heavy_mkfun(js, esm_cjs_require_module, module);
  
  GC_ROOT_PIN(js, require);
  ant_value_t resolve = js_heavy_mkfun(js, esm_cjs_require_resolve, module);
  
  GC_ROOT_PIN(js, resolve);
  js_set(js, resolve, "paths", js_heavy_mkfun(js, esm_cjs_require_paths, module));
  js_set(js, require, "resolve", resolve);
  js_set(js, require, "cache", esm_require_cache(js));
  js_set(js, require, "main", js->modules.cjs.main);
  GC_ROOT_RESTORE(js, mark);
  
  return require;
}

ant_value_t esm_create_require_from_path(ant_t *js, const char *filename) {
  char *path = esm_file_url_to_path(js, filename);
  ant_value_t module = esm_create_cjs_module(js, path ? path : filename, js_mkundef());
  free(path);
  return esm_create_require(js, module);
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
  if (vtype(value) == kTypeUndefined) return true;

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
  
  int param_count = (int)(sizeof(cjs_params) / sizeof(cjs_params[0]));
  sv_func_t *compiled = sv_compile_function_with_params(
    js, cjs_params, param_count, code, code_len, false
  );

  if (!compiled) {
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
  }

  ant_value_t args[] = {require_fn, module_obj, exports_obj, filename_val, dirname_val};
  return sv_execute_entry(js->vm, compiled, exports_obj, args, param_count);
}

ant_value_t esm_load_commonjs_module(
  ant_t *js,
  const char *module_path, const char *code,
  size_t code_len, ant_value_t ns, ant_value_t module_obj
) {
  bool builtin = strncmp(module_path, "node:", 5) == 0 || strncmp(module_path, "ant:", 4) == 0;
  bool pending = is_object_type(module_obj);
  ant_value_t cached = builtin || pending ? js_mkundef() : esm_require_cache_lookup(js, module_path);
  if (vtype(cached) != kTypeUndefined && !pending) {
    ant_value_t exports = esm_require_cached_exports(js, cached);
    ant_value_t result = is_err(exports) ? exports : esm_populate_cjs_namespace(js, ns, exports);
    return is_err(result) ? result : exports;
  }

  GC_ROOT_SAVE(mark, js);
  if (!pending) module_obj = esm_create_cjs_module(js, module_path, builtin ? js_mkundef() : js->modules.cjs.parent);
  
  GC_ROOT_PIN(js, module_obj);
  if (is_err(module_obj)) {
    GC_ROOT_RESTORE(js, mark);
    return module_obj;
  }
  
  ant_value_t exports_obj = js_get(js, module_obj, "exports");
  GC_ROOT_PIN(js, exports_obj);
  js_set_slot_wb(js, ns, SLOT_DEFAULT, exports_obj);
  setprop_cstr(js, ns, "default", 7, exports_obj);

  bool is_main = js->modules.module_stack && !js->modules.module_stack->prev &&
    vtype(js->modules.cjs.parent) == kTypeUndefined;
  if (is_main) {
    js->modules.cjs.main = module_obj;
    js_set(js, js_get(js, js_glob(js), "process"), "mainModule", module_obj);
    js_set(js, module_obj, "id", js_mkstr(js, ".", 1));
  }
  
  ant_value_t require_fn = esm_create_require(js, module_obj);
  GC_ROOT_PIN(js, require_fn);
  
  ant_value_t registration = builtin || pending ? js_mkundef() : esm_require_cache_store(js, module_path, module_obj);
  if (is_err(registration)) {
    esm_cjs_update_children(js, js_get(js, module_obj, "parent"), module_obj, true);
    GC_ROOT_RESTORE(js, mark);
    return registration;
  }
  
  ant_value_t dirname_val = js_get(js, module_obj, "path");
  ant_value_t filename_val = js_get(js, module_obj, "filename");

  const char *prev_filename = js->filename;
  js_set_filename(js, module_path);

  ant_value_t result = esm_eval_commonjs_function(
    js, code, code_len,
    require_fn, module_obj, exports_obj,
    filename_val, dirname_val
  );
  
  if (vtype(result) == kTypePromise) js_run_event_loop(js);
  if (!is_err(result) && !Ant_Exception_Pending(js)) js_set(js, module_obj, "loaded", js_true);

  ant_value_t exports_val = js_get(js, module_obj, "exports");
  
  if (!is_err(result) && !Ant_Exception_Pending(js)) {
    ant_value_t ns_res = esm_populate_cjs_namespace(js, ns, exports_val);
    if (is_err(ns_res)) result = ns_res;
  }

  if (!is_err(result) && Ant_Exception_Pending(js)) result = Ant_Exception_Current(js);

  if (!pending && is_err(result)) {
    if (!builtin) js_delete_prop(js, esm_require_cache(js), module_path, strlen(module_path));
    esm_cjs_update_children(js, js_get(js, module_obj, "parent"), module_obj, true);
  }

  js_set_filename(js, prev_filename);
  GC_ROOT_RESTORE(js, mark);

  if (is_err(result)) return result;
  return exports_val;
}
