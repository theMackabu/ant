#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NAPI_DLOPEN(name, flags) ((void*)LoadLibraryA(name))
#define NAPI_DLSYM(handle, name) ((void*)GetProcAddress((HMODULE)(handle), (name)))
#define NAPI_DLERROR() "LoadLibrary failed"
#define NAPI_RTLD_LAZY 0
#define NAPI_RTLD_NOW 0
#define NAPI_RTLD_LOCAL 0
#define NAPI_RTLD_GLOBAL 0
#else
#include <dlfcn.h>
#define NAPI_DLOPEN(name, flags) dlopen((name), (flags))
#define NAPI_DLSYM(handle, name) dlsym((handle), (name))
#define NAPI_DLERROR() dlerror()
#define NAPI_RTLD_LAZY RTLD_LAZY
#define NAPI_RTLD_NOW RTLD_NOW
#define NAPI_RTLD_LOCAL RTLD_LOCAL
#define NAPI_RTLD_GLOBAL RTLD_GLOBAL
#endif

#define NAPI_DEFAULT_DLOPEN_FLAGS NAPI_RTLD_LAZY

#include "napi_internal.h"
#include "silver/call.h"
#include "esm/commonjs.h"
#include "gc/roots.h"
#include "sync.h"

static napi_native_lib_t *g_napi_native_libs = NULL;
static ant_mutex_t g_napi_native_libs_lock = ANT_MUTEX_INIT;
static _Thread_local napi_module *g_pending_napi_module = NULL;

static napi_native_lib_t *napi_native_lib_find(void *handle) {
  for (napi_native_lib_t *lib = g_napi_native_libs; lib; lib = lib->next)
    if (lib->handle == handle) return lib;
  return NULL;
}

static void napi_link_exports(void) {
  ant_napi_link_async();
  ant_napi_link_objects();
  ant_napi_link_references();
  ant_napi_link_values();
}

static ant_value_t napi_dlopen_common(ant_t *js, ant_value_t module_obj, const char *filename, int flags) {
  napi_link_exports();

  napi_env env = ant_napi_get_env(js);
  if (!env) return js_mkerr(js, "napi env allocation failed");

  if (!is_object_type(module_obj)) return js_mkerr(js, "process.dlopen module must be an object");
  if (!filename || !filename[0]) return js_mkerr(js, "process.dlopen filename must be a non-empty string");

  g_pending_napi_module = NULL;
  void *handle = NAPI_DLOPEN(filename, flags);
  if (!handle) {
    const char *msg = NAPI_DLERROR();
    return js_mkerr(js, "Failed to load native module '%s': %s", filename, msg ? msg : "unknown");
  }

  ant_mutex_lock(&g_napi_native_libs_lock);
  napi_native_lib_t *lib = napi_native_lib_find(handle);
  if (!lib && (lib = calloc(1, sizeof(*lib)))) {
    lib->handle = handle;
    lib->module = g_pending_napi_module;
    lib->next = g_napi_native_libs;
    g_napi_native_libs = lib;
  }
  
  napi_module *module = lib ? lib->module : g_pending_napi_module;
  ant_mutex_unlock(&g_napi_native_libs_lock);

  napi_register_module_v1_fn reg_fn = (napi_register_module_v1_fn)NAPI_DLSYM(handle, "napi_register_module_v1");
  ant_value_t exports = js_get(js, module_obj, "exports");

  if (!is_object_type(exports)) {
    exports = Ant_Object_CreateBare(js);
    js_set(js, module_obj, "exports", exports);
  }

  ant_value_t ret = Ant_Value_Undefined;
  if (reg_fn) ret = (ant_value_t)reg_fn(env, (napi_value)exports);
  else if (module && module->nm_register_func) {
    ret = (ant_value_t)module->nm_register_func(env, (napi_value)exports);
  } else return js_mkerr(js, "No N-API registration entrypoint found in '%s'", filename);

  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (is_err(nenv->exception) || Ant_Exception_Pending(js)) {
    ant_value_t ex = is_err(nenv->exception)
      ? nenv->exception
      : Ant_Exception_Current(js);
    nenv->exception = Ant_Value_Undefined;
    return js_throw(js, ex);
  }

  if (is_object_type(ret)) exports = ret;
  js_set(js, module_obj, "exports", exports);
  js_set(js, module_obj, "loaded", Ant_Value_True);

  return exports;
}

ant_value_t napi_process_dlopen_js(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "process.dlopen(module, filename) requires 2 arguments");
  if (!is_object_type(args[0])) return js_mkerr(js, "process.dlopen module must be an object");
  if (vtype(args[1]) != kTypeString) return js_mkerr(js, "process.dlopen filename must be a string");
  if (nargs >= 3 && vtype(args[2]) != kTypeUndefined && vtype(args[2]) != kTypeNumber)
    return js_mkerr(js, "process.dlopen flags must be a number");

  size_t path_len = 0;
  const char *path = js_getstr(js, args[1], &path_len);
  if (!path || path_len == 0) return js_mkerr(js, "process.dlopen filename must be non-empty");

  int flags = NAPI_DEFAULT_DLOPEN_FLAGS;
  if (nargs >= 3 && vtype(args[2]) == kTypeNumber) flags = (int)Ant_Value_AsNumberUnchecked(args[2]);

  ant_value_t loaded = napi_dlopen_common(js, args[0], path, flags);
  if (is_err(loaded)) return loaded;
  return Ant_Value_Undefined;
}

ant_value_t napi_load_native_module(ant_t *js, const char *module_path, ant_value_t ns, ant_value_t module_obj) {
  if (!module_path) return js_mkerr(js, "native module path is null");

  GC_ROOT_SAVE(mark, js);
  bool pending = is_object_type(module_obj);
  if (!pending) module_obj = esm_create_cjs_module(js, module_path, js->modules.cjs.parent);

  GC_ROOT_PIN(js, module_obj);
  ant_value_t result = is_err(module_obj) || pending ? module_obj : esm_require_cache_store(js, module_path, module_obj);
  GC_ROOT_PIN(js, result);
  if (is_err(result)) {
    if (!is_err(module_obj)) esm_cjs_update_children(js, js->modules.cjs.parent, module_obj, true);
    GC_ROOT_RESTORE(js, mark);
    return result;
  }

  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  ant_value_t dlopen_fn = is_object_type(process_obj) ? js_get(js, process_obj, "dlopen") : Ant_Value_Undefined;

  if (is_callable(dlopen_fn)) {
    ant_value_t argv[2] = {module_obj, js_mkstr(js, module_path, strlen(module_path))};
    ant_value_t dl_res = sv_vm_call(js->vm, js, dlopen_fn, process_obj, argv, 2, NULL, Ant_Value_Undefined);
    if (is_err(dl_res) || Ant_Exception_Pending(js)) {
      result = is_err(dl_res) ? dl_res : Ant_Exception_Current(js);
      goto cleanup;
    }
  } else {
    ant_value_t load_res = napi_dlopen_common(js, module_obj, module_path, NAPI_DEFAULT_DLOPEN_FLAGS);
    if (is_err(load_res)) {
      result = load_res;
      goto cleanup;
    }
  }

  js_set(js, module_obj, "loaded", Ant_Value_True);
  ant_value_t exports_val = js_get(js, module_obj, "exports");
  result = exports_val;
  if (!is_object_type(ns)) goto cleanup;

  setprop_cstr(js, ns, "default", 7, exports_val);
  js_set_slot_wb(js, ns, SLOT_DEFAULT, exports_val);

  if (!is_object_type(exports_val)) goto cleanup;
  ant_iter_t iter = js_prop_iter_begin(js, exports_val);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = Ant_Value_Undefined;

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (key_len == 7 && memcmp(key, "default", 7) == 0) continue;
    setprop_cstr(js, ns, key, key_len, value);
  }
  js_prop_iter_end(&iter);

cleanup:
  if (!pending && is_err(result)) {
    js_delete_prop(js, esm_require_cache(js), module_path, strlen(module_path));
    esm_cjs_update_children(js, js->modules.cjs.parent, module_obj, true);
  }
  GC_ROOT_RESTORE(js, mark);
  return result;
}

NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module *mod) {
  g_pending_napi_module = mod;
}
