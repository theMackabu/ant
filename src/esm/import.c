#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "cfunc.h"
#include "convert.h"
#include "ptr.h"
#include "esm/remote.h"
#include "esm/import.h"
#include "esm/builtin_bundle.h"
#include "modules/proxy.h"
#include "modules/promise.h"
#include "modules/microtasks.h"
#include "silver/engine.h"

static constexpr uint32_t MODULE_CTX_NATIVE_TAG = Ant_NativeTag("MCTX");

const char *js_module_ctx_filename(ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return NULL;
  return js_get_native(module_ctx, MODULE_CTX_NATIVE_TAG);
}

static void module_ctx_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, MODULE_CTX_NATIVE_TAG));
  js_clear_native(value, MODULE_CTX_NATIVE_TAG);
}

static ant_value_t js_get_import_func(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_prop_loc_t import_off = lkp(js, glob, "import", 6);
  if (!import_off.obj) return Ant_Value_Undefined;
  return js_prop_load(import_off);
}

static ant_value_t js_module_ctx_from_func(ant_value_t func) {
  if (vtype(func) != kTypeFunction) return Ant_Value_Undefined;
  ant_value_t module_ctx = js_get_slot(js_func_obj(func), SLOT_MODULE_CTX);
  return is_object_type(module_ctx) ? module_ctx : Ant_Value_Undefined;
}

static ant_value_t js_get_execution_module_ctx(ant_t *js) {
  sv_vm_t *vm = js->vm;
  if (vm && vm->fp >= 0) {
    ant_value_t module_ctx = js_module_ctx_from_func(vm->frames[vm->fp].callee);
    if (is_object_type(module_ctx)) return module_ctx;
  }

  return js_module_eval_active_ctx(js);
}

static ant_value_t js_get_import_owner_module_ctx(ant_t *js, ant_value_t call_func) {
  ant_value_t module_ctx = js_module_ctx_from_func(call_func);
  if (is_object_type(module_ctx)) return module_ctx;
  return js_get_execution_module_ctx(js);
}

static const char *js_import_base_path(ant_t *js, ant_value_t module_ctx) {
  const char *base_path = js_module_ctx_filename(module_ctx);
  if (!base_path) base_path = js_module_ctx_filename(js_get_execution_module_ctx(js));
  return base_path ? base_path : js->filename;
}

static ant_value_t js_import_check_attribute_values(ant_t *js, ant_value_t attrs) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, attrs);
  
  ant_value_t keys = is_proxy(attrs)
    ? proxy_enum(js, attrs, OBJ_ENUM_KEYS)
    : js_own_property_keys(js, attrs, false, true);
  
  GC_ROOT_PIN(js, keys);
  if (is_err(keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_value_t key = Ant_Value_Undefined;
  GC_ROOT_PIN(js, key);

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    key = js_arr_get(js, keys, i);
    if (vtype(key) != kTypeString) continue;

    ant_value_t val = js_get(js, attrs, js_getstr(js, key, NULL));
    if (!is_err(val) && vtype(val) != kTypeString)
      val = js_mkerr_typed(js, JS_ERR_TYPE, "Import attribute values must be strings");
    if (is_err(val)) {
      GC_ROOT_RESTORE(js, root_mark);
      return val;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_Undefined;
}

static ant_value_t js_import_attributes(ant_t *js, ant_value_t options) {
  if (vtype(options) == kTypeUndefined) return Ant_Value_Undefined;
  if (!is_object_type(options))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The second argument to import() must be an object");

  ant_value_t attrs = js_get(js, options, "with");
  if (is_err(attrs) || vtype(attrs) == kTypeUndefined) return attrs;
  if (!is_object_type(attrs)) return js_mkerr_typed(js, JS_ERR_TYPE, "The 'with' option must be an object");

  ant_value_t checked = js_import_check_attribute_values(js, attrs);
  return is_err(checked) ? checked : attrs;
}

static ant_value_t builtin_import_tla_resolve(ant_params_t) {
  return js_get_slot(call_func, SLOT_DATA);
}

enum {
  IMPORT_JOB_PROMISE,
  IMPORT_JOB_SPECIFIER,
  IMPORT_JOB_ATTRS,
  IMPORT_JOB_BASE,
  IMPORT_JOB_FIELD_COUNT,
};

static const char *js_import_job_base_path(ant_t *js, ant_value_t base) {
  return vtype(base) == kTypeString ? js_getstr(js, base, NULL) : js_module_ctx_filename(base);
}

static ant_value_t builtin_import_job(ant_params_t) {
  if (nargs < IMPORT_JOB_FIELD_COUNT) return Ant_Value_Undefined;
  
  ant_value_t promise = args[IMPORT_JOB_PROMISE];
  ant_value_t base = args[IMPORT_JOB_BASE];
  
  ant_value_t evaluation = Ant_Value_Undefined;
  ant_value_t ns = js_esm_import_dynamic_ex(js, args[IMPORT_JOB_SPECIFIER], js_import_job_base_path(js, base), args[IMPORT_JOB_ATTRS], &evaluation);

  if (is_err(ns)) js_reject_promise(js, promise, js_take_thrown(js, ns));
  else if (vtype(evaluation) != kTypePromise) js_resolve_promise(js, promise, ns);
  else js_resolve_promise(js, promise, js_promise_then(js, evaluation, js_make_data_cfunc(js, ns, builtin_import_tla_resolve), Ant_Value_Undefined));
  
  return Ant_Value_Undefined;
}

static ant_value_t js_import_job_base(ant_t *js, ant_value_t call_func) {
  ant_value_t module_ctx = js_get_import_owner_module_ctx(js, call_func);
  if (js_module_ctx_filename(module_ctx)) return module_ctx;
  
  module_ctx = js_get_execution_module_ctx(js);
  if (js_module_ctx_filename(module_ctx)) return module_ctx;
  
  return js->filename ? js_mkstr(js, js->filename, strlen(js->filename)) : Ant_Value_Undefined;
}

static bool js_import_is_source_phase(ant_t *js, ant_value_t marker) {
  size_t len = 0;
  const char *str = js_getstr(js, marker, &len);
  return str && len == SV_IMPORT_SOURCE_PHASE_LEN && memcmp(str, SV_IMPORT_SOURCE_PHASE, len) == 0;
}

static ant_value_t js_import_source_phase(ant_t *js, ant_value_t specifier, ant_value_t call_func) {
  ant_value_t base = js_import_job_base(js, call_func);
  ant_value_t resolved = js_esm_resolve_specifier(js, specifier, js_import_job_base_path(js, base));
  if (is_err(resolved)) return resolved;
  
  static const char code[] = "ERR_SOURCE_PHASE_NOT_DEFINED";
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, code, sizeof(code) - 1));
  size_t len = 0;
  const char *url = js_getstr(js, resolved, &len);
  
  return js_mkerr_props(
    js, JS_ERR_SYNTAX, props,
    "Source phase import object is not defined for module '%.*s'", (int)len, url ? url : ""
  );
}

ant_value_t js_builtin_import(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "import() requires a string specifier");
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t specifier = vtype(args[0]) == kTypeString ? args[0] : coerce_to_str_hint(js, args[0], 1);
  GC_ROOT_PIN(js, specifier);
  
  bool source_phase = nargs >= 2 && vtype(args[1]) == kTypeString && js_import_is_source_phase(js, args[1]);
  ant_value_t attrs = is_err(specifier) ? specifier
    : source_phase ? js_import_source_phase(js, specifier, call_func)
    : js_import_attributes(js, nargs >= 2 ? args[1] : Ant_Value_Undefined);
  GC_ROOT_PIN(js, attrs);
  
  if (is_err(attrs)) {
    ant_value_t rejected = js_promise_reject_with_ctor(js, js->sym.promise_ctor, js_take_thrown(js, attrs));
    GC_ROOT_RESTORE(js, root_mark);
    return rejected;
  }

  ant_value_t base = js_import_job_base(js, call_func);
  GC_ROOT_PIN(js, base);
  
  ant_value_t loaded = js_esm_import_loaded(js, specifier, js_import_job_base_path(js, base), attrs);
  if (is_object_type(loaded)) {
    ant_value_t resolved = js_promise_assimilate_awaitable(js, loaded);
    GC_ROOT_RESTORE(js, root_mark);
    return resolved;
  }

  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_PIN(js, promise);

  ant_value_t fields[IMPORT_JOB_FIELD_COUNT] = {
    [IMPORT_JOB_PROMISE] = promise,
    [IMPORT_JOB_SPECIFIER] = specifier,
    [IMPORT_JOB_ATTRS] = attrs,
    [IMPORT_JOB_BASE] = base,
  };
  
  queue_microtask_with_args(js, Ant_Function_Create(builtin_import_job), fields, IMPORT_JOB_FIELD_COUNT);
  
  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

static ant_value_t js_get_module_ctx_import_meta(ant_value_t module_ctx) {
  return js_get_slot(module_ctx, SLOT_IMPORT_META);
}

static void js_set_import_module_ctx(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return;
  ant_value_t import_fn = js_get_import_func(js);
  if (vtype(import_fn) != kTypeFunction) return;
  js_set_slot_wb(js, js_func_obj(import_fn), SLOT_MODULE_CTX, module_ctx);
}

static ant_value_t js_get_current_import_meta(ant_t *js) {
  ant_value_t import_meta = js_get_module_ctx_import_meta(js_get_execution_module_ctx(js));
  if (vtype(import_meta) == kTypeObject) return import_meta;
  return js->modules.import_meta;
}

bool js_try_import_meta(ant_t *js, ant_value_t func_obj, ant_value_t *out) {
  ant_value_t cfunc = js_get_slot(func_obj, SLOT_CFUNC);
  if (vtype(cfunc) != kTypeBuiltin) return false;
  if (!js_cfunc_same_entrypoint(cfunc, js_builtin_import)) return false;

  ant_value_t meta = js_get_module_ctx_import_meta(js_get_slot(func_obj, SLOT_MODULE_CTX));
  if (vtype(meta) == kTypeUndefined) meta = js_get_current_import_meta(js);
  if (vtype(meta) == kTypeUndefined) return false;

  *out = meta;
  return true;
}

static ant_value_t builtin_import_meta_resolve(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "import.meta.resolve() requires a string specifier");

  ant_value_t module_ctx = Ant_Value_Undefined;
  if (is_object_type(call_this)) module_ctx = js_get_slot(call_this, SLOT_MODULE_CTX);

  if (!is_object_type(module_ctx))
    module_ctx = js_get_import_owner_module_ctx(js, call_func);

  return js_esm_resolve_specifier(js, args[0], js_import_base_path(js, module_ctx));
}

static inline void js_set_import_meta_special_dirname(
  ant_t *js, ant_value_t import_meta, const char *filename, bool is_builtin
) {
  char *filename_copy = strdup(filename);
  if (!filename_copy) return;

  char *last_slash = strrchr(filename_copy, '/');
  char *scheme_end = strstr(filename_copy, "://");

  if ((is_builtin && last_slash) || (!is_builtin && last_slash && scheme_end && last_slash > scheme_end + 2)) {
    *last_slash = '\0';
    ant_value_t dirname_val = js_mkstr(js, filename_copy, strlen(filename_copy));
    if (!is_err(dirname_val)) setprop_cstr(js, import_meta, "dirname", 7, dirname_val);
  }

  free(filename_copy);
}

static inline void js_set_import_meta_path_dirname(
  ant_t *js, ant_value_t import_meta, const char *filename
) {
  char *filename_copy = strdup(filename);
  if (!filename_copy) return;

  char *dir = dirname(filename_copy);
  if (dir) {
    ant_value_t dirname_val = js_mkstr(js, dir, strlen(dir));
    if (!is_err(dirname_val)) setprop_cstr(js, import_meta, "dirname", 7, dirname_val);
  }

  free(filename_copy);
}

static ant_value_t js_create_import_meta_for_context(
  ant_t *js, ant_value_t module_ctx, const char *filename, bool is_main
) {
  ant_value_t import_meta = mkobj(js, 0);
  if (is_err(import_meta)) return import_meta;
  js_set_slot_wb(js, import_meta, SLOT_MODULE_CTX, module_ctx);

  bool is_url = esm_is_url(filename) || esm_is_data_url(filename);
  bool is_builtin = esm_has_builtin_scheme(filename);

  ant_value_t url_val = (is_url || is_builtin)
    ? js_mkstr(js, filename, strlen(filename))
    : js_esm_make_file_url(js, filename);
  if (!is_err(url_val)) setprop_cstr(js, import_meta, "url", 3, url_val);

  ant_value_t filename_val = js_mkstr(js, filename, strlen(filename));
  if (!is_err(filename_val)) {
    setprop_cstr(js, import_meta, "filename", 8, filename_val);
    setprop_cstr(js, import_meta, "path", 4, filename_val);
  }

  if (is_url || is_builtin) js_set_import_meta_special_dirname(js, import_meta, filename, is_builtin);
  else js_set_import_meta_path_dirname(js, import_meta, filename);

  ant_value_t dirname_val = js_get(js, import_meta, "dirname");
  if (vtype(dirname_val) == kTypeString)
    setprop_cstr(js, import_meta, "dir", 3, dirname_val);

  char *filename_copy = strdup(filename);
  if (filename_copy) {
    char *file = (is_url || is_builtin) ? strrchr(filename_copy, '/') : NULL;
    file = (is_url || is_builtin) ? (file ? file + 1 : filename_copy) : basename(filename_copy);
    if (file) {
      ant_value_t file_val = js_mkstr(js, file, strlen(file));
      if (!is_err(file_val)) setprop_cstr(js, import_meta, "file", 4, file_val);
    }
    free(filename_copy);
  }

  setprop_cstr(js, import_meta, "env", 3, js->builtins.process_env);
  setprop_cstr(js, import_meta, "main", 4, is_main ? Ant_Value_True : Ant_Value_False);

  ant_value_t resolve_fn = js_heavy_mkfun(js, builtin_import_meta_resolve, Ant_Value_Undefined);
  if (vtype(resolve_fn) == kTypeFunction)
    js_set_slot_wb(js, js_func_obj(resolve_fn), SLOT_MODULE_CTX, module_ctx);
  setprop_cstr(js, import_meta, "resolve", 7, resolve_fn);

  return import_meta;
}

ant_value_t js_create_module_context(ant_t *js, const char *filename, bool is_main) {
  if (!filename || !filename[0]) return Ant_Value_Undefined;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t module_ctx = Ant_Object_CreateBare(js);
  if (is_err(module_ctx)) {
    GC_ROOT_RESTORE(js, root_mark);
    return module_ctx;
  }
  GC_ROOT_PIN(js, module_ctx);

  char *filename_copy = strdup(filename);
  if (!filename_copy) {
    GC_ROOT_RESTORE(js, root_mark);
    return Ant_Error_CreateOutOfMemory(js);
  }

  js_set_native(module_ctx, filename_copy, MODULE_CTX_NATIVE_TAG);
  js_set_finalizer(module_ctx, module_ctx_finalize);

  ant_value_t import_meta = js_create_import_meta_for_context(js, module_ctx, filename, is_main);
  if (is_err(import_meta)) {
    GC_ROOT_RESTORE(js, root_mark);
    return import_meta;
  }
  
  GC_ROOT_PIN(js, import_meta);
  js_set_slot_wb(js, module_ctx, SLOT_IMPORT_META, import_meta);
  GC_ROOT_RESTORE(js, root_mark);
  
  return module_ctx;
}

ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main) {
  ant_value_t module_ctx = js_create_module_context(js, filename, is_main);
  if (is_err(module_ctx)) return module_ctx;
  return js_get_module_ctx_import_meta(module_ctx);
}

ant_value_t js_get_module_import_binding(ant_t *js) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t module_ctx = js_get_execution_module_ctx(js);
  ant_value_t import_meta = js_get_module_ctx_import_meta(module_ctx);

  GC_ROOT_PIN(js, module_ctx);
  GC_ROOT_PIN(js, import_meta);

  if (!is_object_type(module_ctx) || vtype(import_meta) != kTypeObject) {
    GC_ROOT_RESTORE(js, root_mark);
    return Ant_Value_Undefined;
  }

  ant_value_t import_obj = Ant_Object_CreateBare(js);
  if (is_err(import_obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return import_obj;
  }

  GC_ROOT_PIN(js, import_obj);
  ant_value_t function_proto = js->sym.function_proto;
  GC_ROOT_PIN(js, function_proto);

  if (is_object_type(function_proto)) js_set_proto_wb(js, import_obj, function_proto);
  js_set_slot(import_obj, SLOT_CFUNC, Ant_Function_Create(js_builtin_import));
  
  js_set_slot_wb(js, import_obj, SLOT_MODULE_CTX, module_ctx);
  setprop_cstr(js, import_obj, "meta", 4, import_meta);

  ant_value_t import_fn = js_obj_to_func(js, import_obj);
  GC_ROOT_RESTORE(js, root_mark);
  
  return import_fn;
}

void js_setup_import_meta(ant_t *js, const char *filename) {
  ant_value_t module_ctx = js_create_module_context(js, filename, true);
  ant_value_t import_meta = js_get_module_ctx_import_meta(module_ctx);
  if (is_err(import_meta) || vtype(import_meta) == kTypeUndefined) return;

  js_set_import_module_ctx(js, module_ctx);
  js->modules.import_meta = import_meta;
}

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  ctx->prev = js->modules.module_stack;
  ctx->prev_import_meta_prop = js->modules.import_meta;
  js->modules.module_stack = ctx;

  ant_value_t import_meta = js_get_module_ctx_import_meta(ctx->module_ctx);
  if (vtype(import_meta) != kTypeUndefined) js->modules.import_meta = import_meta;
}

void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  if (js->modules.module_stack == ctx) {
    js->modules.import_meta = ctx->prev_import_meta_prop;
    js->modules.module_stack = ctx->prev;
  }
}
