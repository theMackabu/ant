#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "esm/loader.h"
#include "esm/commonjs.h"
#include "esm/library.h"
#include "gc/roots.h"
#include "modules/symbol.h"

typedef struct { ant_t *js; ant_value_t arr; } builtin_iter_ctx_t;

static void push_builtin_name(const char *name, void *ud) {
  builtin_iter_ctx_t *ctx = (builtin_iter_ctx_t *)ud;
  js_arr_push(ctx->js, ctx->arr, js_mkstr(ctx->js, name, strlen(name)));
}

static ant_value_t resolve_strip_file_url(ant_t *js, ant_value_t resolved) {
  if (is_err(resolved) || vtype(resolved) != kTypeString) return resolved;

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

static ant_value_t builtin_createRequire(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "createRequire() requires a filename string");
  return esm_create_require_from_path(js, js_getstr(js, args[0], NULL));
}

// Module._resolveFilename(request, parent)
static ant_value_t builtin_resolveFilename(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr(js, "Module._resolveFilename() requires a string request");

  const char *base_path = js_module_eval_active_filename(js);
  if (nargs >= 2 && vtype(args[1]) == kTypeObject) {
    ant_value_t parent_filename = js_get(js, args[1], "filename");
    if (vtype(parent_filename) == kTypeString) {
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
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_false;

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
  if (vtype(hook) == kTypeUndefined || vtype(js->modules.hooks) != kTypeArray) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t remaining = js_mkarr(js);
  GC_ROOT_PIN(js, remaining);

  ant_offset_t len = js_arr_len(js, js->modules.hooks);
  bool removed = false;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, js->modules.hooks, i);
    if (!removed && entry == hook) {
      removed = true;
      continue;
    }
    js_arr_push(js, remaining, entry);
  }

  js->modules.hooks = remaining;
  js_set_slot(self, SLOT_DATA, js_mkundef());

  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static bool hook_member_invalid(ant_value_t fn) {
  return vtype(fn) != kTypeUndefined && vtype(fn) != kTypeFunction && vtype(fn) != kTypeBuiltin;
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

  if (vtype(js->modules.hooks) != kTypeArray) js->modules.hooks = js_mkarr(js);
  js_arr_push(js, js->modules.hooks, args[0]);

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

static ant_value_t builtin_module_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs && vtype(args[0]) != kTypeUndefined && vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Module id must be a string");
  if (!is_object_type(js->this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Module requires an object receiver");
  const char *id = nargs && vtype(args[0]) == kTypeString ? js_getstr(js, args[0], NULL) : "";
  return esm_init_cjs_module(js, js->this_val, id, nargs > 1 ? args[1] : js_mkundef());
}

ant_value_t module_library(ant_t *js) {
  if (is_object_type(js->modules.cjs.constructor)) return js->modules.cjs.constructor;
  ant_value_t cache = esm_require_cache(js);
  ant_value_t proto = js_mkobj(js);
  
  js_set_proto_init(proto, js->sym.object_proto);
  ant_value_t lib = js_make_ctor(js, builtin_module_constructor, proto, "Module", 6);
  js->modules.cjs.constructor = lib;
  
  js_set(js, lib, "_cache", cache);
  js->modules.cjs.cache = js_mkundef();
  js_set(js, lib, "Module", lib);
  js_set(js, proto, "require", js_mkfun(esm_cjs_require_module));

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
