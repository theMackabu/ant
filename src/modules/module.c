#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "esm/loader.h"
#include "esm/library.h"
#include "gc/roots.h"
#include "modules/symbol.h"

typedef struct { ant_t *js; ant_value_t arr; } builtin_iter_ctx_t;

static void push_builtin_name(const char *name, void *ud) {
  builtin_iter_ctx_t *ctx = (builtin_iter_ctx_t *)ud;
  js_arr_push(ctx->js, ctx->arr, js_mkstr(ctx->js, name, strlen(name)));
}

static ant_value_t builtin_createRequire_call(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "require() expects a string specifier");

  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t data = js_get_slot(fn, SLOT_DATA);
  const char *base_path = js_module_eval_active_filename(js);

  if (vtype(data) == T_STR) {
    ant_offset_t plen = 0;
    ant_offset_t poff = vstr(js, data, &plen);
    base_path = (const char *)(uintptr_t)(poff);
  }

  ant_value_t ns = js_esm_import_sync_from_require(js, args[0], base_path);
  if (is_err(ns)) return ns;

  if (vtype(ns) == T_OBJ) {
    ant_value_t default_export = js_get_slot(ns, SLOT_DEFAULT);
    if (vtype(default_export) != T_UNDEF) return default_export;
  }
  
  return ns;
}

static ant_value_t resolve_strip_file_url(ant_t *js, ant_value_t resolved) {
  if (is_err(resolved) || vtype(resolved) != T_STR) return resolved;

  ant_offset_t len = 0;
  ant_offset_t off = vstr(js, resolved, &len);
  
  const char *s = (const char *)(uintptr_t)(off);
  static const char *prefix = "file://";

  if ((size_t)len >= strlen(prefix) && strncmp(s, prefix, strlen(prefix)) == 0) {
    const char *path_part = s + strlen(prefix);
    size_t plen = (size_t)len - strlen(prefix);
    return js_mkstr(js, path_part, plen);
  }

  return resolved;
}

// require.resolve(specifier, options?)
static ant_value_t builtin_createRequire_resolve(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "require.resolve() expects a string specifier");

  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t data = js_get_slot(fn, SLOT_DATA);
  const char *base_path = js_module_eval_active_filename(js);

  if (vtype(data) == T_STR) {
    ant_offset_t dlen = 0;
    ant_offset_t doff = vstr(js, data, &dlen);
    base_path = (const char *)(uintptr_t)(doff);
  }

  ant_value_t paths_val = (nargs >= 2 && is_object_type(args[1]))
    ? js_get(js, args[1], "paths") : js_mkundef();

  if (vtype(paths_val) != T_ARR) {
    ant_value_t resolved = js_esm_resolve_specifier_require(js, args[0], base_path);
    return resolve_strip_file_url(js, resolved);
  }

  ant_offset_t path_count = js_arr_len(js, paths_val);
  for (ant_offset_t i = 0; i < path_count; i++) {
    ant_value_t p = js_arr_get(js, paths_val, i);
    if (vtype(p) != T_STR) continue;
    
    char *dir = js_getstr(js, p, NULL);
    if (!dir) continue;
    
    ant_value_t resolved = js_esm_resolve_specifier_require(js, args[0], dir);
    if (!is_err(resolved) && vtype(resolved) == T_STR)
      return resolve_strip_file_url(js, resolved);
  }

  return js_mkerr(js, "Cannot resolve module");
}

// createRequire(filename)
static ant_value_t builtin_createRequire(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createRequire() requires a filename argument");

  ant_value_t filename_val = args[0];
  if (vtype(filename_val) != T_STR)
    return js_mkerr(js, "createRequire() filename must be a string");

  size_t fname_len;
  char *fname = js_getstr(js, filename_val, &fname_len);
  if (!fname) return js_mkerr(js, "createRequire() invalid filename");

  const char *path = fname;
  size_t path_len = fname_len;
  
  static const char *file_prefix = "file://";
  size_t prefix_len = strlen(file_prefix);

  if (path_len >= prefix_len && strncmp(path, file_prefix, prefix_len) == 0) {
    path += prefix_len;
    path_len -= prefix_len;
  }

  ant_value_t path_val = js_mkstr(js, path, path_len);
  ant_value_t require_fn = js_heavy_mkfun(js, builtin_createRequire_call, path_val);
  ant_value_t resolve_fn = js_heavy_mkfun(js, builtin_createRequire_resolve, path_val);
  js_set(js, require_fn, "resolve", resolve_fn);

  return require_fn;
}

// Module._resolveFilename(request, parent)
static ant_value_t builtin_resolveFilename(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) 
    return js_mkerr(js, "Module._resolveFilename() requires a string request");

  const char *base_path = js_module_eval_active_filename(js);
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t parent_filename = js_get(js, args[1], "filename");
    if (vtype(parent_filename) == T_STR) {
      ant_offset_t plen = 0;
      ant_offset_t poff = vstr(js, parent_filename, &plen);
      base_path = (const char *)(uintptr_t)(poff);
    }
  }

  ant_value_t resolved = js_esm_resolve_specifier(js, args[0], base_path);
  return resolve_strip_file_url(js, resolved);
}

typedef struct { 
  const char *name; 
  bool found; 
} builtin_lookup_ctx_t;

static void match_builtin_name(const char *name, void *ud) {
  builtin_lookup_ctx_t *ctx = (builtin_lookup_ctx_t *)ud;
  if (!ctx->found && strcmp(name, ctx->name) == 0) ctx->found = true;
}

static ant_value_t builtin_module_isBuiltin(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) return js_false;

  size_t name_len = 0;
  const char *name = js_getstr(js, args[0], &name_len);
  if (!name || strlen(name) != name_len) return js_false;
  if (strncmp(name, "node:", 5) == 0) name += 5;

  builtin_lookup_ctx_t ctx = { name, false };
  ant_library_foreach(match_builtin_name, &ctx);
  return js_bool(ctx.found);
}

static ant_value_t builtin_module_deregisterHooks(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getcurrentfunc(js);
  ant_value_t hook = js_get_slot(self, SLOT_DATA);
  if (vtype(hook) == T_UNDEF || vtype(js->esm.hooks) != T_ARR) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t remaining = js_mkarr(js);
  GC_ROOT_PIN(js, remaining);

  ant_offset_t len = js_arr_len(js, js->esm.hooks);
  bool removed = false;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, js->esm.hooks, i);
    if (!removed && entry == hook) {
      removed = true;
      continue;
    }
    js_arr_push(js, remaining, entry);
  }

  js->esm.hooks = remaining;
  js_set_slot(self, SLOT_DATA, js_mkundef());

  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static bool hook_member_invalid(ant_value_t fn) {
  return vtype(fn) != T_UNDEF && vtype(fn) != T_FUNC && vtype(fn) != T_CFUNC;
}

static ant_value_t builtin_module_registerHooks(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "registerHooks requires an options object");

  ant_value_t resolve_fn = js_get(js, args[0], "resolve");
  if (is_err(resolve_fn)) return resolve_fn;
  if (hook_member_invalid(resolve_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The 'resolve' hook must be a function");

  ant_value_t load_fn = js_get(js, args[0], "load");
  if (is_err(load_fn)) return load_fn;
  if (hook_member_invalid(load_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The 'load' hook must be a function");

  if (vtype(js->esm.hooks) != T_ARR) js->esm.hooks = js_mkarr(js);
  js_arr_push(js, js->esm.hooks, args[0]);

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t dereg_obj = js_mkobj(js);
  GC_ROOT_PIN(js, dereg_obj);
  js_set_slot(dereg_obj, SLOT_CFUNC, js_mkfun(builtin_module_deregisterHooks));
  js_set_slot(dereg_obj, SLOT_DATA, args[0]);

  ant_value_t out = js_mkobj(js);
  GC_ROOT_PIN(js, out);
  js_set(js, out, "deregister", js_obj_to_func(js, dereg_obj));

  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

ant_value_t module_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  
  js_set(js, lib, "createRequire", js_mkfun(builtin_createRequire));
  js_set(js, lib, "registerHooks", js_mkfun(builtin_module_registerHooks));
  js_set(js, lib, "isBuiltin", js_mkfun(builtin_module_isBuiltin));

  ant_value_t modules_arr = js_mkarr(js);
  builtin_iter_ctx_t ctx = { js, modules_arr };
  ant_library_foreach(push_builtin_name, &ctx);
  
  js_set(js, lib, "builtinModules", modules_arr);
  js_set(js, lib, "_resolveFilename", js_mkfun(builtin_resolveFilename));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "Module", 6));

  return lib;
}
