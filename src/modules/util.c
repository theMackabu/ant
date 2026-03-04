#include <compat.h> // IWYU pragma: keep

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "internal.h"
#include "silver/engine.h"

#include "modules/json.h"
#include "modules/symbol.h"
#include "modules/util.h"


typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} util_sb_t;

typedef struct {
  const char *name;
  const char *open;
  const char *close;
} util_style_entry_t;

// migrate to crprintf
static const util_style_entry_t util_styles[] = {
  {"bold", "\x1b[1m", "\x1b[22m"},
  {"dim", "\x1b[2m", "\x1b[22m"},
  {"italic", "\x1b[3m", "\x1b[23m"},
  {"underline", "\x1b[4m", "\x1b[24m"},
  {"inverse", "\x1b[7m", "\x1b[27m"},
  {"hidden", "\x1b[8m", "\x1b[28m"},
  {"strikethrough", "\x1b[9m", "\x1b[29m"},
  {"black", "\x1b[30m", "\x1b[39m"},
  {"red", "\x1b[31m", "\x1b[39m"},
  {"green", "\x1b[32m", "\x1b[39m"},
  {"yellow", "\x1b[33m", "\x1b[39m"},
  {"blue", "\x1b[34m", "\x1b[39m"},
  {"magenta", "\x1b[35m", "\x1b[39m"},
  {"cyan", "\x1b[36m", "\x1b[39m"},
  {"white", "\x1b[37m", "\x1b[39m"},
  {"gray", "\x1b[90m", "\x1b[39m"},
  {"grey", "\x1b[90m", "\x1b[39m"},
  {"bgBlack", "\x1b[40m", "\x1b[49m"},
  {"bgRed", "\x1b[41m", "\x1b[49m"},
  {"bgGreen", "\x1b[42m", "\x1b[49m"},
  {"bgYellow", "\x1b[43m", "\x1b[49m"},
  {"bgBlue", "\x1b[44m", "\x1b[49m"},
  {"bgMagenta", "\x1b[45m", "\x1b[49m"},
  {"bgCyan", "\x1b[46m", "\x1b[49m"},
  {"bgWhite", "\x1b[47m", "\x1b[49m"},
};

static inline bool util_is_callable(ant_value_t v) {
  uint8_t t = vtype(v);
  return t == T_FUNC || t == T_CFUNC;
}

static bool util_sb_reserve(util_sb_t *sb, size_t extra) {
  size_t need = sb->len + extra + 1;
  if (need <= sb->cap) return true;

  size_t next = sb->cap ? sb->cap : 128;
  while (next < need) next *= 2;

  char *buf = (char *)realloc(sb->buf, next);
  if (!buf) return false;

  sb->buf = buf;
  sb->cap = next;
  return true;
}

static bool util_sb_append_n(util_sb_t *sb, const char *s, size_t n) {
  if (n == 0) return true;
  if (!util_sb_reserve(sb, n)) return false;
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = '\0';
  return true;
}

static bool util_sb_append_c(util_sb_t *sb, char c) {
  if (!util_sb_reserve(sb, 1)) return false;
  sb->buf[sb->len++] = c;
  sb->buf[sb->len] = '\0';
  return true;
}

static bool util_sb_append_jsval(ant_t *js, util_sb_t *sb, ant_value_t v) {
  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, v, cbuf, sizeof(cbuf));
  size_t len = strlen(cstr.ptr);
  bool ok = util_sb_append_n(sb, cstr.ptr, len);
  if (cstr.needs_free) free((void *)cstr.ptr);
  return ok;
}

static bool util_sb_append_json(ant_t *js, util_sb_t *sb, ant_value_t v) {
  ant_value_t stringify_args[1] = {v};
  ant_value_t json = js_json_stringify(js, stringify_args, 1);
  if (vtype(json) != T_STR) {
    return util_sb_append_n(sb, "[Circular]", 10);
  }
  size_t len = 0;
  const char *s = js_getstr(js, json, &len);
  return s ? util_sb_append_n(sb, s, len) : util_sb_append_n(sb, "[Circular]", 10);
}

static ant_value_t util_format_impl(ant_t *js, ant_value_t *args, int nargs, int fmt_index) {
  util_sb_t sb = {0};
  if (fmt_index >= nargs) {
    ant_value_t out = js_mkstr(js, "", 0);
    free(sb.buf);
    return out;
  }

  if (vtype(args[fmt_index]) != T_STR) {
    for (int i = fmt_index; i < nargs; i++) {
      if (i > fmt_index) util_sb_append_c(&sb, ' ');
      util_sb_append_jsval(js, &sb, args[i]);
    }
    ant_value_t out = js_mkstr(js, sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    return out;
  }

  size_t fmt_len = 0;
  const char *fmt = js_getstr(js, args[fmt_index], &fmt_len);
  int argi = fmt_index + 1;

  for (size_t i = 0; i < fmt_len; i++) {
    if (fmt[i] != '%' || i + 1 >= fmt_len) {
      util_sb_append_c(&sb, fmt[i]);
      continue;
    }

    char spec = fmt[i + 1];
    if (spec == '%') {
      util_sb_append_c(&sb, '%');
      i++;
      continue;
    }

    bool known = (
      spec == 's' || spec == 'd' || spec == 'i' || spec == 'f' ||
      spec == 'j' || spec == 'o' || spec == 'O' || spec == 'c'
    );

    if (!known) {
      util_sb_append_c(&sb, '%');
      util_sb_append_c(&sb, spec);
      i++;
      continue;
    }

    ant_value_t v = (argi < nargs) ? args[argi++] : js_mkundef();

    if (spec == 's') {
      util_sb_append_jsval(js, &sb, v);
    } else if (spec == 'd' || spec == 'i') {
      double d = js_to_number(js, v);
      char nb[64];
      if (isnan(d)) snprintf(nb, sizeof(nb), "NaN");
      else if (!isfinite(d)) snprintf(nb, sizeof(nb), d < 0 ? "-Infinity" : "Infinity");
      else snprintf(nb, sizeof(nb), "%lld", (long long)d);
      util_sb_append_n(&sb, nb, strlen(nb));
    } else if (spec == 'f') {
      double d = js_to_number(js, v);
      char nb[64];
      if (isnan(d)) snprintf(nb, sizeof(nb), "NaN");
      else if (!isfinite(d)) snprintf(nb, sizeof(nb), d < 0 ? "-Infinity" : "Infinity");
      else snprintf(nb, sizeof(nb), "%g", d);
      util_sb_append_n(&sb, nb, strlen(nb));
    } else if (spec == 'j') {
      util_sb_append_json(js, &sb, v);
    } else if (spec == 'o' || spec == 'O') {
      util_sb_append_jsval(js, &sb, v);
    } else if (spec == 'c') // style placeholder: consume arg, emit nothing

    i++;
  }

  for (; argi < nargs; argi++) {
    util_sb_append_c(&sb, ' ');
    util_sb_append_jsval(js, &sb, args[argi]);
  }

  ant_value_t out = js_mkstr(js, sb.buf ? sb.buf : "", sb.len);
  free(sb.buf);
  return out;
}

static ant_value_t util_format(ant_t *js, ant_value_t *args, int nargs) {
  return util_format_impl(js, args, nargs, 0);
}

static ant_value_t util_format_with_options(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs <= 1) return js_mkstr(js, "", 0);
  return util_format_impl(js, args, nargs, 1);
}

static ant_value_t util_inspect(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, args[0], cbuf, sizeof(cbuf));
  ant_value_t out = js_mkstr(js, cstr.ptr, strlen(cstr.ptr));
  if (cstr.needs_free) free((void *)cstr.ptr);
  return out;
}

static ant_value_t util_strip_vt_control_characters(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkstr(js, "", 0);

  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, args[0], cbuf, sizeof(cbuf));
  const char *src = cstr.ptr;
  size_t len = strlen(src);

  util_sb_t sb = {0};
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)src[i];
    if (ch != 0x1b) {
      util_sb_append_c(&sb, (char)ch);
      continue;
    }

    if (i + 1 < len && src[i + 1] == '[') {
      i += 2;
      while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c >= '@' && c <= '~') break;
        i++;
      }
      continue;
    }

    if (i + 1 < len && src[i + 1] == ']') {
      i += 2;
      while (i < len) {
        if ((unsigned char)src[i] == 0x07) break;
        if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '\\') {
          i++;
          break;
        }
        i++;
      }
      continue;
    }
  }

  ant_value_t out = js_mkstr(js, sb.buf ? sb.buf : "", sb.len);
  if (cstr.needs_free) free((void *)cstr.ptr);
  free(sb.buf);
  return out;
}

static const util_style_entry_t *util_find_style(const char *name) {
  for (size_t i = 0; i < sizeof(util_styles) / sizeof(util_styles[0]); i++) {
    if (strcmp(name, util_styles[i].name) == 0) return &util_styles[i];
  }
  return NULL;
}

static ant_value_t util_style_text(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkstr(js, "", 0);

  char text_buf[512];
  js_cstr_t text_cstr = js_to_cstr(js, args[1], text_buf, sizeof(text_buf));

  const util_style_entry_t *picked[16];
  int picked_n = 0;

  if (vtype(args[0]) == T_STR) {
    const char *name = js_getstr(js, args[0], NULL);
    const util_style_entry_t *e = name ? util_find_style(name) : NULL;
    if (e) picked[picked_n++] = e;
  } else if (vtype(args[0]) == T_ARR) {
    ant_offset_t len = js_arr_len(js, args[0]);
    for (ant_offset_t i = 0; i < len && picked_n < (int)(sizeof(picked) / sizeof(picked[0])); i++) {
      ant_value_t item = js_arr_get(js, args[0], i);
      if (vtype(item) != T_STR) continue;
      const char *name = js_getstr(js, item, NULL);
      const util_style_entry_t *e = name ? util_find_style(name) : NULL;
      if (e) picked[picked_n++] = e;
    }
  }

  if (picked_n == 0) {
    ant_value_t out = js_mkstr(js, text_cstr.ptr, strlen(text_cstr.ptr));
    if (text_cstr.needs_free) free((void *)text_cstr.ptr);
    return out;
  }

  util_sb_t sb = {0};
  for (int i = 0; i < picked_n; i++) {
    util_sb_append_n(&sb, picked[i]->open, strlen(picked[i]->open));
  }
  util_sb_append_n(&sb, text_cstr.ptr, strlen(text_cstr.ptr));
  for (int i = picked_n - 1; i >= 0; i--) {
    util_sb_append_n(&sb, picked[i]->close, strlen(picked[i]->close));
  }

  ant_value_t out = js_mkstr(js, sb.buf ? sb.buf : "", sb.len);
  if (text_cstr.needs_free) free((void *)text_cstr.ptr);
  free(sb.buf);
  return out;
}

static ant_value_t util_promisify_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t ctx = js_get_slot(js, fn, SLOT_DATA);
  if (!is_object_type(ctx)) return js_mkundef();

  ant_value_t settled = js_get_slot(js, ctx, SLOT_SETTLED);
  if (vtype(settled) == T_BOOL && settled == js_true) return js_mkundef();
  js_set_slot(js, ctx, SLOT_SETTLED, js_true);

  ant_value_t promise = js_get_slot(js, ctx, SLOT_DATA);
  if (!is_object_type(promise)) return js_mkundef();

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    js_reject_promise(js, promise, args[0]);
    return js_mkundef();
  }

  if (nargs <= 1) {
    js_resolve_promise(js, promise, js_mkundef());
    return js_mkundef();
  }

  if (nargs == 2) {
    js_resolve_promise(js, promise, args[1]);
    return js_mkundef();
  }

  ant_value_t arr = js_mkarr(js);
  for (int i = 1; i < nargs; i++) js_arr_push(js, arr, args[i]);
  js_resolve_promise(js, promise, arr);
  return js_mkundef();
}

static ant_value_t util_promisified_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t original = js_get_slot(js, fn, SLOT_DATA);
  if (!util_is_callable(original)) return js_mkerr(js, "promisified target is not callable");

  ant_value_t promise = js_mkpromise(js);
  ant_value_t ctx = js_mkobj(js);
  js_set_slot(js, ctx, SLOT_DATA, promise);
  js_set_slot(js, ctx, SLOT_SETTLED, js_false);
  ant_value_t cb = js_heavy_mkfun(js, util_promisify_callback, ctx);

  ant_value_t *call_args = (ant_value_t *)malloc((size_t)(nargs + 1) * sizeof(ant_value_t));
  if (!call_args) {
    js_reject_promise(js, promise, js_mkerr(js, "Out of memory"));
    return promise;
  }
  for (int i = 0; i < nargs; i++) call_args[i] = args[i];
  call_args[nargs] = cb;

  ant_value_t this_arg = js_getthis(js);
  ant_value_t call_result = sv_vm_call(
    js->vm, js, original, this_arg, call_args, nargs + 1, NULL, false
  );
  free(call_args);

  ant_value_t settled = js_get_slot(js, ctx, SLOT_SETTLED);
  bool is_settled = (vtype(settled) == T_BOOL && settled == js_true);
  if (!is_settled && (is_err(call_result) || js->thrown_exists)) {
    ant_value_t ex = js->thrown_exists ? js->thrown_value : call_result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    js_set_slot(js, ctx, SLOT_SETTLED, js_true);
    js_reject_promise(js, promise, ex);
  }

  return promise;
}

static ant_value_t util_promisify(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !util_is_callable(args[0])) {
    return js_mkerr(js, "promisify(fn) requires a function");
  }
  return js_heavy_mkfun(js, util_promisified_call, args[0]);
}

ant_value_t util_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  js_set(js, lib, "format", js_mkfun(util_format));
  js_set(js, lib, "formatWithOptions", js_mkfun(util_format_with_options));
  js_set(js, lib, "inspect", js_mkfun(util_inspect));
  js_set(js, lib, "promisify", js_mkfun(util_promisify));
  js_set(js, lib, "stripVTControlCharacters", js_mkfun(util_strip_vt_control_characters));
  js_set(js, lib, "styleText", js_mkfun(util_style_text));

  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "util", 4));
  return lib;
}
