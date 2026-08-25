#include <compat.h> // IWYU pragma: keep

#include "esm/loader.h"
#include "esm/builtin_bundle.h"
#include "esm/remote.h"
#include "loader_cache.h"
#include "loader_internal.h"

#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/buffer.h"
#include "silver/engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool esm_has_url_scheme(const char *s) {
  if (!s || !isalpha((unsigned char)s[0])) return false;
  size_t i = 1;
  while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
  return i > 1 && s[i] == ':';
}

bool esm_hooks_present(ant_t *js) {
  return vtype(js->esm.hooks) == kTypeArray && js_arr_len(js, js->esm.hooks) > 0;
}

typedef struct {
  ant_value_t hooks;
  ant_value_t ctx;
  const char *base;
  bool is_require;
} esm_chain_t;

static ant_value_t esm_run_resolve_chain(ant_t *js, const esm_chain_t *chain, int level, ant_value_t spec);
static ant_value_t esm_run_load_chain(ant_t *js, const esm_chain_t *chain, int level, ant_value_t url);

static ant_value_t esm_hook_make_next(ant_t *js, ant_value_t next_fn, const esm_chain_t *chain, int level) {
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t data = js_mkobj(js);
  GC_ROOT_PIN(js, data);
  js_set(js, data, "hooks", chain->hooks);
  js_set(js, data, "level", js_mknum((double)level));
  js_set(js, data, "ctx", chain->ctx);
  js_set(js, data, "base", chain->base ? js_mkstr(js, chain->base, strlen(chain->base)) : js_mkundef());
  js_set(js, data, "require", js_bool(chain->is_require));

  ant_value_t obj = js_mkobj(js);
  GC_ROOT_PIN(js, obj);
  js_set_slot(obj, SLOT_CFUNC, next_fn);
  js_set_slot(obj, SLOT_DATA, data);

  ant_value_t fn = js_obj_to_func(js, obj);
  GC_ROOT_RESTORE(js, root_mark);
  return fn;
}

static bool esm_hook_next_was_called(ant_t *js, ant_value_t next) {
  ant_value_t data = js_get_slot(next, SLOT_DATA);
  return js_truthy(js, js_get(js, data, "called"));
}

static ant_value_t builtin_esm_next_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t data = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  js_set(js, data, "called", js_true);

  int level = (int)js_getnum(js_get(js, data, "level"));
  ant_value_t base_val = js_get(js, data, "base");

  ant_value_t spec = nargs > 0 ? args[0] : js_mkundef();

  esm_chain_t chain = {
    .hooks = js_get(js, data, "hooks"),
    .ctx = nargs > 1 && is_object_type(args[1]) ? args[1] : js_get(js, data, "ctx"),
    .base = vtype(base_val) == kTypeString ? js_getstr(js, base_val, NULL) : NULL,
    .is_require = js_truthy(js, js_get(js, data, "require")),
  };

  char *base_owned = NULL;
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t purl = js_get(js, args[1], "parentURL");
    if (vtype(purl) == kTypeString) {
      const char *purl_str = js_getstr(js, purl, NULL);
      base_owned = esm_file_url_to_path(js, purl_str);
      if (!base_owned) base_owned = strdup(purl_str);
      if (base_owned) chain.base = base_owned;
    }
  }

  ant_value_t result = esm_run_resolve_chain(js, &chain, level, spec);
  free(base_owned);
  return result;
}

static ant_value_t builtin_esm_next_load(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t data = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  js_set(js, data, "called", js_true);

  int level = (int)js_getnum(js_get(js, data, "level"));
  ant_value_t url = nargs > 0 ? args[0] : js_mkundef();

  esm_chain_t chain = {
    .hooks = js_get(js, data, "hooks"),
    .ctx = nargs > 1 && is_object_type(args[1]) ? args[1] : js_get(js, data, "ctx"),
    .base = NULL,
    .is_require = false,
  };

  return esm_run_load_chain(js, &chain, level, url);
}

static ant_value_t esm_run_hook_level(
  ant_t *js,
  ant_value_t fn,
  ant_value_t next,
  ant_value_t arg0,
  ant_value_t ctx,
  const char *hook_name
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, next);

  ant_value_t call_args[3] = { arg0, ctx, next };
  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 3, NULL, false);
  GC_ROOT_PIN(js, result);

  if (!is_err(result)) {
    if (!is_object_type(result))
      result = js_mkerr_typed(js, JS_ERR_TYPE, "%s hook must return an object", hook_name);
    else if (!esm_hook_next_was_called(js, next) && !js_truthy(js, js_get(js, result, "shortCircuit")))
      result = js_mkerr_typed(js, JS_ERR_TYPE, "%s hook must call next() or set shortCircuit: true", hook_name);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static int esm_extract_condition_override(ant_t *js, ant_value_t ctx, bool is_require, char ***out) {
  *out = NULL;
  if (!is_object_type(ctx)) return 0;

  ant_value_t arr = js_get(js, ctx, "conditions");
  if (vtype(arr) != kTypeArray) return 0;

  ant_offset_t len = js_arr_len(js, arr);
  if (len == 0 || len > 32) return 0;

  char **list = calloc((size_t)len, sizeof(char *));
  if (!list) return 0;

  int count = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, arr, i);
    if (vtype(item) != kTypeString) continue;
    list[count] = strdup(js_getstr(js, item, NULL));
    if (list[count]) count++;
  }

  const char *lead = is_require ? "require" : "import";
  bool is_default_set = count == 3
    && strcmp(list[0], lead) == 0
    && strcmp(list[1], "node") == 0
    && strcmp(list[2], "default") == 0;

  if (count == 0 || is_default_set) {
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
    return 0;
  }

  *out = list;
  return count;
}

static void esm_free_condition_override(char **list, int count) {
  if (!list) return;
  for (int i = 0; i < count; i++) free(list[i]);
  free(list);
}

static ant_value_t esm_run_resolve_chain(ant_t *js, const esm_chain_t *chain, int level, ant_value_t spec) {
  if (vtype(spec) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "resolve hook requires a string specifier");

  for (; level >= 0; level--) {
    ant_value_t hook = js_arr_get(js, chain->hooks, (ant_offset_t)level);
    if (!is_object_type(hook)) continue;

    ant_value_t fn = js_get(js, hook, "resolve");
    if (is_err(fn)) return fn;
    if (vtype(fn) != kTypeFunction && vtype(fn) != kTypeBuiltin) continue;

    ant_value_t next = esm_hook_make_next(js, js_mkfun(builtin_esm_next_resolve), chain, level - 1);
    return esm_run_hook_level(js, fn, next, spec, chain->ctx, "resolve");
  }

  const char *base_path = chain->base;
  bool is_require = chain->is_require;
  ant_value_t ctx = chain->ctx;

  ant_offset_t slen = 0;
  ant_offset_t soff = vstr(js, spec, &slen);

  char *spec_copy = strndup((const char *)(uintptr_t)soff, (size_t)slen);
  if (!spec_copy) return js_mkerr(js, "oom");

  char *url_suffix = NULL;
  char *file_url_path = esm_file_url_to_path(js, spec_copy);
  if (file_url_path) {
    const char *suffix = spec_copy + strcspn(spec_copy, "?#");
    if (*suffix) url_suffix = strdup(suffix);
    free(spec_copy);
    spec_copy = file_url_path;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t out = js_mkobj(js);
  GC_ROOT_PIN(js, out);

  bool scheme_passthrough = esm_has_builtin_scheme(spec_copy)
    || esm_is_data_url(spec_copy)
    || esm_is_url(spec_copy);

  if (!scheme_passthrough && !esm_lookup_builtin_alias(spec_copy, strlen(spec_copy))) {
    if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);

    char **cond_override = NULL;
    int cond_count = esm_extract_condition_override(js, ctx, is_require, &cond_override);
    ant_esm_state_t *st = cond_override ? esm_state(js) : NULL;
    if (st) {
      st->active_conditions = cond_override;
      st->active_condition_count = cond_count;
    }

    char *resolved = esm_resolve(js, spec_copy, base_path, is_require ? esm_resolve_path_require : esm_resolve_path);

    if (st) {
      st->active_conditions = NULL;
      st->active_condition_count = 0;
    }
    esm_free_condition_override(cond_override, cond_count);

    if (resolved) {
      char *url = esm_path_to_file_url(resolved);
      free(resolved);
      free(spec_copy);
      if (!url) {
        free(url_suffix);
        GC_ROOT_RESTORE(js, root_mark);
        return js_mkerr(js, "oom");
      }

      if (url_suffix) {
        size_t url_len = strlen(url);
        size_t suffix_len = strlen(url_suffix);
        char *full = realloc(url, url_len + suffix_len + 1);
        if (!full) {
          free(url);
          free(url_suffix);
          GC_ROOT_RESTORE(js, root_mark);
          return js_mkerr(js, "oom");
        }
        memcpy(full + url_len, url_suffix, suffix_len + 1);
        url = full;
      }
      js_set(js, out, "url", js_mkstr(js, url, strlen(url)));

      free(url);
      free(url_suffix);
      GC_ROOT_RESTORE(js, root_mark);
      return out;
    }
  }

  js_set(js, out, "url", spec);
  free(spec_copy);
  free(url_suffix);
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t esm_run_load_chain(ant_t *js, const esm_chain_t *chain, int level, ant_value_t url) {
  if (vtype(url) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "load hook requires a string url");

  for (; level >= 0; level--) {
    ant_value_t hook = js_arr_get(js, chain->hooks, (ant_offset_t)level);
    if (!is_object_type(hook)) continue;

    ant_value_t fn = js_get(js, hook, "load");
    if (is_err(fn)) return fn;
    if (vtype(fn) != kTypeFunction && vtype(fn) != kTypeBuiltin) continue;

    ant_value_t next = esm_hook_make_next(js, js_mkfun(builtin_esm_next_load), chain, level - 1);
    return esm_run_hook_level(js, fn, next, url, chain->ctx, "load");
  }

  ant_offset_t ulen = 0;
  ant_offset_t uoff = vstr(js, url, &ulen);

  char *url_copy = strndup((const char *)(uintptr_t)uoff, (size_t)ulen);
  if (!url_copy) return js_mkerr(js, "oom");

  char *path = esm_file_url_to_path(js, url_copy);
  free(url_copy);

  if (!path) return js_mkobj(js);

  esm_file_data_t file = {0};
  ant_value_t err = esm_read_file(js, path, "module source", &file);
  if (is_err(err)) {
    free(path);
    return err;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t out = js_mkobj(js);
  GC_ROOT_PIN(js, out);

  js_set(js, out, "source", js_mkstr(js, file.data, file.size));

  if (esm_is_json(path)) js_set(js, out, "format", js_mkstr(js, "json", 4));
  else js_set(js, out, "format", esm_decide_module_format(js, path) == MODULE_EVAL_FORMAT_CJS
    ? js_mkstr(js, "commonjs", 8)
    : js_mkstr(js, "module", 6));

  free(path);
  free(file.data);
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

ant_value_t esm_import_via_hooks(
  ant_t *js,
  const char *specifier,
  size_t spec_len,
  const char *base_path,
  ant_value_t attrs,
  bool is_require,
  bool *handled
) {
  *handled = false;
  GC_ROOT_SAVE(root_mark, js);

  if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);

  ant_value_t ctx = js_mkobj(js);
  GC_ROOT_PIN(js, ctx);

  if (esm_has_url_scheme(base_path)) {
    js_set(js, ctx, "parentURL", js_mkstr(js, base_path, strlen(base_path)));
  } else {
    char *abs = esm_make_absolute_path(base_path);
    char *purl = esm_path_to_file_url(abs ? abs : base_path);
    if (purl) {
      js_set(js, ctx, "parentURL", js_mkstr(js, purl, strlen(purl)));
      free(purl);
    }
    free(abs);
  }

  ant_value_t conditions = js_mkarr(js);
  GC_ROOT_PIN(js, conditions);
  if (is_require) js_arr_push(js, conditions, js_mkstr(js, "require", 7));
  else js_arr_push(js, conditions, js_mkstr(js, "import", 6));
  js_arr_push(js, conditions, js_mkstr(js, "node", 4));
  js_arr_push(js, conditions, js_mkstr(js, "default", 7));
  js_set(js, ctx, "conditions", conditions);
  js_set(js, ctx, "importAttributes", is_object_type(attrs) ? attrs : js_mkobj(js));

  ant_value_t hooks = js->esm.hooks;
  GC_ROOT_PIN(js, hooks);
  int top = (int)js_arr_len(js, hooks) - 1;

  ant_value_t spec_val = js_mkstr(js, specifier, spec_len);
  GC_ROOT_PIN(js, spec_val);

  esm_chain_t chain = {
    .hooks = hooks,
    .ctx = ctx,
    .base = base_path,
    .is_require = is_require,
  };

  ant_value_t resolved = esm_run_resolve_chain(js, &chain, top, spec_val);
  GC_ROOT_PIN(js, resolved);

  if (is_err(resolved)) {
    *handled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return resolved;
  }

  ant_value_t url_val = js_get(js, resolved, "url");
  GC_ROOT_PIN(js, url_val);

  if (is_err(url_val)) {
    *handled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return url_val;
  }
  if (vtype(url_val) != kTypeString) {
    *handled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "resolve hook result must include a string 'url'");
  }

  ant_value_t res_fmt = js_get(js, resolved, "format");
  if (is_err(res_fmt)) {
    *handled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return res_fmt;
  }
  if (vtype(res_fmt) != kTypeUndefined) js_set(js, ctx, "format", res_fmt);

  ant_value_t res_attrs = js_get(js, resolved, "importAttributes");
  if (is_err(res_attrs)) {
    *handled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return res_attrs;
  }
  if (is_object_type(res_attrs)) js_set(js, ctx, "importAttributes", res_attrs);

  const char *url = js_getstr(js, url_val, NULL);
  char *fs_path = esm_file_url_to_path(js, url);
  const char *cache_key = fs_path
    ? ((strchr(url, '?') || strchr(url, '#')) ? url : fs_path)
    : url;

  esm_module_t *cached = esm_find_module(js, cache_key);
  if (cached && (cached->is_loaded || cached->is_loading)) {
    *handled = true;
    ant_value_t cached_ns = cached->namespace_obj;
    free(fs_path);
    GC_ROOT_RESTORE(js, root_mark);
    return cached_ns;
  }

  ant_value_t loaded = esm_run_load_chain(js, &chain, top, url_val);
  GC_ROOT_PIN(js, loaded);

  if (is_err(loaded)) {
    *handled = true;
    free(fs_path);
    GC_ROOT_RESTORE(js, root_mark);
    return loaded;
  }

  ant_module_format_t format = MODULE_EVAL_FORMAT_UNKNOWN;
  esm_module_kind_t kind_hint = ESM_MODULE_KIND_NONE;
  ant_value_t fmt_val = js_get(js, loaded, "format");
  if (is_err(fmt_val)) {
    *handled = true;
    free(fs_path);
    GC_ROOT_RESTORE(js, root_mark);
    return fmt_val;
  }
  if (vtype(fmt_val) == kTypeString) {
    const char *fmt_str = js_getstr(js, fmt_val, NULL);
    if (strcmp(fmt_str, "commonjs") == 0) format = MODULE_EVAL_FORMAT_CJS;
    else if (strcmp(fmt_str, "module") == 0) format = MODULE_EVAL_FORMAT_ESM;
    else if (strcmp(fmt_str, "json") == 0) kind_hint = ESM_MODULE_KIND_JSON;
  }

  const uint8_t *source = NULL;
  size_t source_len = 0;
  ant_value_t src_val = js_get(js, loaded, "source");
  if (is_err(src_val)) {
    *handled = true;
    free(fs_path);
    GC_ROOT_RESTORE(js, root_mark);
    return src_val;
  }
  if (vtype(src_val) == kTypeString) {
    ant_offset_t sl = 0;
    ant_offset_t so = vstr(js, src_val, &sl);
    source = (const uint8_t *)(uintptr_t)so;
    source_len = (size_t)sl;
  } else if (vtype(src_val) != kTypeUndefined && vtype(src_val) != kTypeNull) {
    buffer_source_get_bytes(js, src_val, &source, &source_len);
  }

  if (!source && format == MODULE_EVAL_FORMAT_ESM) {
    *handled = true;
    free(fs_path);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "load hook returned format 'module' without a source");
  }

  if (!fs_path) {
    if (!source) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkundef();
    }
    *handled = true;
    ant_value_t ns = esm_get_or_load_ex(js, specifier, url, url, format, source, source_len, true, kind_hint);
    GC_ROOT_RESTORE(js, root_mark);
    return ns;
  }

  if (kind_hint == ESM_MODULE_KIND_JSON || esm_is_json(fs_path)) format = MODULE_EVAL_FORMAT_UNKNOWN;
  else if (format == MODULE_EVAL_FORMAT_UNKNOWN) format = esm_decide_module_format(js, fs_path);

  *handled = true;

  ant_value_t ns = esm_get_or_load_ex(js, specifier, fs_path, cache_key, format, source, source_len, true, kind_hint);
  free(fs_path);
  GC_ROOT_RESTORE(js, root_mark);

  return ns;
}
