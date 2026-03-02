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
#include "modules/symbol.h"

typedef struct { ant_t *js; jsval_t arr; } builtin_iter_ctx_t;

static void push_builtin_name(const char *name, void *ud) {
  builtin_iter_ctx_t *ctx = (builtin_iter_ctx_t *)ud;
  js_arr_push(ctx->js, ctx->arr, js_mkstr(ctx->js, name, strlen(name)));
}

static jsval_t builtin_createRequire_call(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "require() expects a string specifier");

  jsval_t fn = js_getcurrentfunc(js);
  jsval_t data = js_get_slot(js, fn, SLOT_DATA);
  const char *prev_filename = js->filename;

  if (vtype(data) == T_STR) {
    jsoff_t plen = 0;
    jsoff_t poff = vstr(js, data, &plen);
    js_set_filename(js, (const char *)&js->mem[poff]);
  }

  jsval_t ns = js_esm_import_sync(js, args[0]);
  js_set_filename(js, prev_filename);
  if (is_err(ns)) return ns;

  if (vtype(ns) == T_OBJ) {
    jsval_t default_export = js_get_slot(js, ns, SLOT_DEFAULT);
    if (vtype(default_export) != T_UNDEF) return default_export;
  }
  return ns;
}

// require.resolve(specifier)
static jsval_t builtin_createRequire_resolve(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "require.resolve() expects a string specifier");

  jsval_t fn = js_getcurrentfunc(js);
  jsval_t data = js_get_slot(js, fn, SLOT_DATA);
  const char *base_path = js->filename ? js->filename : ".";

  if (vtype(data) == T_STR) {
    jsoff_t dlen = 0;
    jsoff_t doff = vstr(js, data, &dlen);
    base_path = (const char *)&js->mem[doff];
  }

  jsval_t resolved = js_esm_resolve_specifier(js, args[0], base_path);
  if (is_err(resolved)) return resolved;
  if (vtype(resolved) != T_STR) return resolved;

  jsoff_t len = 0;
  jsoff_t off = vstr(js, resolved, &len);
  
  const char *s = (const char *)&js->mem[off];
  static const char *prefix = "file://";

  if ((size_t)len >= strlen(prefix) && strncmp(s, prefix, strlen(prefix)) == 0) {
    const char *path_part = s + strlen(prefix);
    size_t plen = (size_t)len - strlen(prefix);
    return js_mkstr(js, path_part, plen);
  }

  return resolved;
}

// createRequire(filename)
static jsval_t builtin_createRequire(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createRequire() requires a filename argument");

  jsval_t filename_val = args[0];
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

  jsval_t path_val = js_mkstr(js, path, path_len);
  jsval_t require_fn = js_heavy_mkfun(js, builtin_createRequire_call, path_val);
  jsval_t resolve_fn = js_heavy_mkfun(js, builtin_createRequire_resolve, path_val);
  js_set(js, require_fn, "resolve", resolve_fn);

  return require_fn;
}

// Module._resolveFilename(request, parent)
static jsval_t builtin_resolveFilename(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) 
    return js_mkerr(js, "Module._resolveFilename() requires a string request");

  const char *base_path = js->filename ? js->filename : ".";
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    jsval_t parent_filename = js_get(js, args[1], "filename");
    if (vtype(parent_filename) == T_STR) {
      jsoff_t plen = 0;
      jsoff_t poff = vstr(js, parent_filename, &plen);
      base_path = (const char *)&js->mem[poff];
    }
  }

  jsval_t resolved = js_esm_resolve_specifier(js, args[0], base_path);
  if (is_err(resolved)) return resolved;
  if (vtype(resolved) != T_STR) return resolved;

  jsoff_t len = 0;
  jsoff_t off = vstr(js, resolved, &len);
  
  const char *s = (const char *)&js->mem[off];
  static const char *prefix = "file://";

  if ((size_t)len >= strlen(prefix) && strncmp(s, prefix, strlen(prefix)) == 0) {
    const char *path_part = s + strlen(prefix);
    size_t plen = (size_t)len - strlen(prefix);
    return js_mkstr(js, path_part, plen);
  }

  return resolved;
}

jsval_t module_library(ant_t *js) {
  jsval_t lib = js_mkobj(js);
  js_set(js, lib, "createRequire", js_mkfun(builtin_createRequire));

  jsval_t modules_arr = js_mkarr(js);
  builtin_iter_ctx_t ctx = { js, modules_arr };
  ant_library_foreach(push_builtin_name, &ctx);
  
  js_set(js, lib, "builtinModules", modules_arr);
  js_set(js, lib, "_resolveFilename", js_mkfun(builtin_resolveFilename));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "Module", 6));

  return lib;
}
