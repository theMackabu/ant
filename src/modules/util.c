// TODO: split module into smaller files

#include <compat.h> // IWYU pragma: keep

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "internal.h"
#include "esm/library.h"
#include "silver/engine.h"

#include "modules/buffer.h"
#include "modules/date.h"
#include "modules/json.h"
#include "modules/symbol.h"
#include "modules/util.h"
#include "modules/abort.h"
#include "modules/collections.h"

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

typedef enum {
  UTIL_PARSE_ARG_TYPE_BOOLEAN = 0,
  UTIL_PARSE_ARG_TYPE_STRING,
} util_parse_arg_type_t;

typedef struct {
  char *name;
  char short_name;
  util_parse_arg_type_t type;
  bool multiple;
  ant_value_t default_value;
} util_parse_arg_option_t;

// TODO: migrate to crprintf
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
  ant_value_t json = json_stringify_value(js, v);
  if (vtype(json) != T_STR) {
    return util_sb_append_n(sb, "[Circular]", 10);
  }
  size_t len = 0;
  const char *s = js_getstr(js, json, &len);
  return s ? util_sb_append_n(sb, s, len) : util_sb_append_n(sb, "[Circular]", 10);
}

static bool util_set_named_value(
  ant_t *js,
  ant_value_t values,
  const char *name,
  ant_value_t value,
  bool multiple
) {
  if (!multiple) {
    js_set(js, values, name, value);
    return !js->thrown_exists;
  }

  ant_value_t existing = js_get(js, values, name);
  if (is_err(existing)) return false;

  if (vtype(existing) == T_UNDEF) {
    ant_value_t arr = js_mkarr(js);
    js_arr_push(js, arr, value);
    js_set(js, values, name, arr);
    return !js->thrown_exists;
  }

  if (vtype(existing) == T_ARR) {
    js_arr_push(js, existing, value);
    return !js->thrown_exists;
  }

  ant_value_t arr = js_mkarr(js);
  js_arr_push(js, arr, existing);
  js_arr_push(js, arr, value);
  js_set(js, values, name, arr);
  return !js->thrown_exists;
}

static util_parse_arg_option_t *util_find_option_by_name(
  util_parse_arg_option_t *options,
  size_t option_count,
  const char *name
) {
  for (size_t i = 0; i < option_count; i++) {
    if (strcmp(options[i].name, name) == 0) return &options[i];
  }
  return NULL;
}

static util_parse_arg_option_t *util_find_option_by_short(
  util_parse_arg_option_t *options,
  size_t option_count,
  char short_name
) {
  for (size_t i = 0; i < option_count; i++) {
    if (options[i].short_name == short_name) return &options[i];
  }
  return NULL;
}

static void util_free_parse_options(util_parse_arg_option_t *options, size_t option_count) {
  if (!options) return;
  for (size_t i = 0; i < option_count; i++) free(options[i].name);
  free(options);
}

static ant_value_t util_parse_args(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t config = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(config)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "parseArgs(config) requires an options object");
  }

  ant_value_t args_list = js_get(js, config, "args");
  if (is_err(args_list)) return args_list;
  if (vtype(args_list) == T_UNDEF) {
    ant_value_t process_obj = js_get(js, js_glob(js), "process");
    if (is_err(process_obj)) return process_obj;
    args_list = js_get(js, process_obj, "argv");
    if (is_err(args_list)) return args_list;
  }

  ant_value_t options_obj = js_get(js, config, "options");
  if (is_err(options_obj)) return options_obj;
  if (!is_object_type(options_obj)) options_obj = js_mkobj(js);

  bool strict = js_truthy(js, js_get(js, config, "strict"));
  if (vtype(js_get(js, config, "strict")) == T_UNDEF) strict = true;
  bool allow_positionals = js_truthy(js, js_get(js, config, "allowPositionals"));

  size_t option_count = 0;
  {
    ant_iter_t iter = js_prop_iter_begin(js, options_obj);
    const char *key = NULL;
    size_t key_len = 0;
    while (js_prop_iter_next(&iter, &key, &key_len, NULL)) option_count++;
    js_prop_iter_end(&iter);
  }

  util_parse_arg_option_t *options = NULL;
  if (option_count > 0) {
    options = calloc(option_count, sizeof(*options));
    if (!options) return js_mkerr(js, "Out of memory");
    
    ant_iter_t iter = js_prop_iter_begin(js, options_obj);
    const char *key = NULL;
    size_t key_len = 0;
    size_t idx = 0;
    
    while (idx < option_count && js_prop_iter_next(&iter, &key, &key_len, NULL)) {
      ant_value_t spec = js_get(js, options_obj, key);
      ant_value_t type_val = is_object_type(spec) ? js_get(js, spec, "type") : js_mkundef();
      ant_value_t short_val = is_object_type(spec) ? js_get(js, spec, "short") : js_mkundef();
      ant_value_t multiple_val = is_object_type(spec) ? js_get(js, spec, "multiple") : js_mkundef();
      ant_value_t default_val = is_object_type(spec) ? js_get(js, spec, "default") : js_mkundef();
      
      options[idx].name = strndup(key, key_len);
      if (!options[idx].name) {
        js_prop_iter_end(&iter);
        util_free_parse_options(options, option_count);
        return js_mkerr(js, "Out of memory");
      }
      
      options[idx].type = UTIL_PARSE_ARG_TYPE_BOOLEAN;
      if (vtype(type_val) == T_STR) {
        size_t type_len = 0;
        const char *type_str = js_getstr(js, type_val, &type_len);
        if (type_str && type_len == 6 && memcmp(type_str, "string", 6) == 0) {
          options[idx].type = UTIL_PARSE_ARG_TYPE_STRING;
        }
      }
      
      if (vtype(short_val) == T_STR) {
        size_t short_len = 0;
        const char *short_str = js_getstr(js, short_val, &short_len);
        if (short_str && short_len > 0) options[idx].short_name = short_str[0];
      }
      
      options[idx].multiple = js_truthy(js, multiple_val);
      options[idx].default_value = default_val;
      idx++;
    }
    
    js_prop_iter_end(&iter);
  }

  ant_value_t values = js_mkobj(js);
  ant_value_t positionals = js_mkarr(js);
  ant_value_t out = js_mkobj(js);

  for (size_t i = 0; i < option_count; i++) {
  if (vtype(options[i].default_value) != T_UNDEF) {
  if (!util_set_named_value(js, values, options[i].name, options[i].default_value, options[i].multiple)) {
    util_free_parse_options(options, option_count);
    return js->thrown_exists ? js->thrown_value : js_mkerr(js, "parseArgs failed to set default");
  }}}

  ant_offset_t arg_len = js_arr_len(js, args_list);
  bool stop_parsing = false;

  for (ant_offset_t i = 0; i < arg_len; i++) {
    ant_value_t arg_val = js_arr_get(js, args_list, i);
    if (vtype(arg_val) != T_STR) continue;
    
    size_t arg_slen = 0;
    const char *arg = js_getstr(js, arg_val, &arg_slen);
    if (!arg) continue;
    
    if (stop_parsing) {
      js_arr_push(js, positionals, arg_val);
      continue;
    }
    
    if (arg_slen == 2 && memcmp(arg, "--", 2) == 0) {
      stop_parsing = true;
      continue;
    }
    
    if (arg_slen > 2 && arg[0] == '-' && arg[1] == '-') {
      const char *name = arg + 2;
      size_t name_len = arg_slen - 2;
      const char *inline_value = NULL;
      size_t inline_len = 0;
      const char *eq = memchr(name, '=', name_len);
      
      if (eq) {
        name_len = (size_t)(eq - name);
        inline_value = eq + 1;
        inline_len = (size_t)(arg + arg_slen - inline_value);
      }
      
      char *name_buf = strndup(name, name_len);
      if (!name_buf) {
        util_free_parse_options(options, option_count);
        return js_mkerr(js, "Out of memory");
      }
      
      util_parse_arg_option_t *opt = util_find_option_by_name(options, option_count, name_buf);
      if (!opt) {
        if (strict) {
          free(name_buf);
          util_free_parse_options(options, option_count);
          return js_mkerr_typed(js, JS_ERR_TYPE, "Unknown option '--%.*s'", (int)name_len, name);
        }
        
        ant_value_t unknown = inline_value
          ? js_mkstr(js, inline_value, inline_len)
          : js_true;
          
        js_set(js, values, name_buf, unknown);
        free(name_buf);
        
        continue;
      }

      ant_value_t parsed_value = js_true;
      if (opt->type == UTIL_PARSE_ARG_TYPE_STRING) {
        if (inline_value) parsed_value = js_mkstr(js, inline_value, inline_len);
        else if (i + 1 < arg_len) parsed_value = js_arr_get(js, args_list, ++i);
        else {
          free(name_buf);
          util_free_parse_options(options, option_count);
          return js_mkerr_typed(js, JS_ERR_TYPE, "Option '--%s' requires a value", opt->name);
        }
      } else if (inline_value) parsed_value = js_mkstr(js, inline_value, inline_len);
      
      if (!util_set_named_value(js, values, opt->name, parsed_value, opt->multiple)) {
        free(name_buf);
        util_free_parse_options(options, option_count);
        return js->thrown_exists ? js->thrown_value : js_mkerr(js, "parseArgs failed");
      }
      
      free(name_buf);
      continue;
    }

    if (arg_slen > 1 && arg[0] == '-') {
      for (size_t j = 1; j < arg_slen; j++) {
        util_parse_arg_option_t *opt = util_find_option_by_short(options, option_count, arg[j]);
        if (!opt) {
          if (strict) {
            util_free_parse_options(options, option_count);
            return js_mkerr_typed(js, JS_ERR_TYPE, "Unknown option '-%c'", arg[j]);
          }
          char key[2] = {arg[j], '\0'};
          js_set(js, values, key, js_true);
          continue;
        }

        ant_value_t parsed_value = js_true;
        if (opt->type == UTIL_PARSE_ARG_TYPE_STRING) {
          if (j + 1 < arg_slen) {
            parsed_value = js_mkstr(js, arg + j + 1, arg_slen - (j + 1));
            j = arg_slen;
          } else if (i + 1 < arg_len) {
            parsed_value = js_arr_get(js, args_list, ++i);
            j = arg_slen;
          } else {
            util_free_parse_options(options, option_count);
            return js_mkerr_typed(js, JS_ERR_TYPE, "Option '-%c' requires a value", arg[j]);
          }
        }
        
        if (!util_set_named_value(js, values, opt->name, parsed_value, opt->multiple)) {
          util_free_parse_options(options, option_count);
          return js->thrown_exists ? js->thrown_value : js_mkerr(js, "parseArgs failed");
        }
        
        if (opt->type == UTIL_PARSE_ARG_TYPE_STRING) break;
      }
      continue;
    }
    
    if (!allow_positionals) {
      util_free_parse_options(options, option_count);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Unexpected positional argument '%s'", arg);
    }
    js_arr_push(js, positionals, arg_val);
  }
  
  util_free_parse_options(options, option_count);
  js_set(js, out, "values", values);
  js_set(js, out, "positionals", positionals);
  
  return out;
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
    } else if (spec == 'c') {
      // style placeholder: consume arg, emit nothing.
    }

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

static bool util_has_proto_in_chain(ant_t *js, ant_value_t value, ant_value_t proto) {
  if (!is_special_object(proto)) return false;

  ant_value_t current = value;
  while (is_special_object(current)) {
    current = js_get_proto(js, current);
    if (current == proto) return true;
  }

  return false;
}

static bool util_is_boxed_primitive(ant_value_t value, uint8_t *type_out) {
  ant_value_t primitive;

  if (!is_object_type(value)) return false;
  primitive = js_get_slot(value, SLOT_PRIMITIVE);
  if (vtype(primitive) == T_UNDEF) return false;

  if (type_out) *type_out = vtype(primitive);
  return true;
}

static bool util_has_to_string_tag(ant_t *js, ant_value_t value, const char *tag, size_t tag_len) {
  ant_value_t to_string_tag;
  size_t actual_len = 0;
  const char *actual;

  if (!is_object_type(value)) return false;

  to_string_tag = js_get_sym(js, value, get_toStringTag_sym());
  if (vtype(to_string_tag) != T_STR) return false;

  actual = js_getstr(js, to_string_tag, &actual_len);
  return actual != NULL && actual_len == tag_len && memcmp(actual, tag, tag_len) == 0;
}

static bool util_is_arguments_object_value(ant_t *js, ant_value_t value) {
  ant_value_t callee = js_mkundef();

  if (vtype(value) != T_ARR) return false;
  if (js_get_slot(value, SLOT_STRICT_ARGS) == js_true) return true;
  if (!util_has_to_string_tag(js, value, "Arguments", 9)) return false;

  return js_try_get_own_data_prop(js, value, "callee", 6, &callee);
}

static ant_value_t util_types_is_any_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  ArrayBufferData *buffer = (nargs > 0) ? buffer_get_arraybuffer_data(args[0]) : NULL;
  return js_bool(buffer != NULL);
}

static ant_value_t util_types_is_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  ArrayBufferData *buffer = (nargs > 0) ? buffer_get_arraybuffer_data(args[0]) : NULL;
  return js_bool(buffer != NULL && !buffer->is_shared);
}

static ant_value_t util_types_is_shared_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  ArrayBufferData *buffer = (nargs > 0) ? buffer_get_arraybuffer_data(args[0]) : NULL;
  return js_bool(buffer != NULL && buffer->is_shared);
}

static ant_value_t util_types_is_array_buffer_view(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(buffer_is_dataview(args[0]) || buffer_get_typedarray_data(args[0]) != NULL);
}

static ant_value_t util_types_is_data_view(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(buffer_is_dataview(args[0]));
}

static ant_value_t util_types_is_typed_array(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(buffer_get_typedarray_data(args[0]) != NULL);
}

static ant_value_t util_types_is_float16_array(ant_t *js, ant_value_t *args, int nargs) {
  TypedArrayData *typed_array = (nargs > 0) ? buffer_get_typedarray_data(args[0]) : NULL;
  if (!typed_array) return js_false;
  return js_bool(typed_array != NULL && typed_array->type == TYPED_ARRAY_FLOAT16);
}

#define DEFINE_TYPED_ARRAY_CHECK(fn_name, typed_array_kind)                                   \
  static ant_value_t fn_name(ant_t *js, ant_value_t *args, int nargs) {                       \
    TypedArrayData *typed_array = (nargs > 0) ? buffer_get_typedarray_data(args[0]) : NULL;   \
    if (!typed_array) return js_false;                                                        \
    return js_bool(typed_array != NULL && typed_array->type == typed_array_kind);             \
  }

DEFINE_TYPED_ARRAY_CHECK(util_types_is_int8_array, TYPED_ARRAY_INT8)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_uint8_array, TYPED_ARRAY_UINT8)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_int16_array, TYPED_ARRAY_INT16)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_uint16_array, TYPED_ARRAY_UINT16)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_int32_array, TYPED_ARRAY_INT32)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_uint32_array, TYPED_ARRAY_UINT32)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_float32_array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_float64_array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_bigint64_array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_biguint64_array, TYPED_ARRAY_BIGUINT64)
DEFINE_TYPED_ARRAY_CHECK(util_types_is_uint8_clamped_array, TYPED_ARRAY_UINT8_CLAMPED)

static ant_value_t util_types_is_promise(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(vtype(args[0]) == T_PROMISE);
}

static ant_value_t util_types_is_proxy(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  return js_bool(is_proxy(args[0]));
}

static ant_value_t util_types_is_regexp(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp_proto;
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  return js_bool(util_has_proto_in_chain(js, args[0], regexp_proto));
}

static ant_value_t util_types_is_date(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(is_date_instance(args[0]));
}

static ant_value_t util_types_is_map(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return js_false;
  return js_bool(js_obj_ptr(args[0])->type_tag == T_MAP);
}

static ant_value_t util_types_is_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return js_false;
  return js_bool(js_obj_ptr(args[0])->type_tag == T_SET);
}

static ant_value_t util_types_is_weak_map(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return js_false;
  return js_bool(js_obj_ptr(args[0])->type_tag == T_WEAKMAP);
}

static ant_value_t util_types_is_weak_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return js_false;
  return js_bool(js_obj_ptr(args[0])->type_tag == T_WEAKSET);
}

static ant_value_t util_types_is_async_function(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t func_obj;
  if (nargs < 1 || vtype(args[0]) != T_FUNC) return js_false;
  func_obj = js_func_obj(args[0]);
  return js_bool(js_get_slot(func_obj, SLOT_ASYNC) == js_true);
}

static ant_value_t util_types_is_generator_function(ant_t *js, ant_value_t *args, int nargs) {
  sv_closure_t *closure;

  if (nargs < 1 || vtype(args[0]) != T_FUNC) return js_false;

  closure = js_func_closure(args[0]);
  return js_bool(closure != NULL && closure->func != NULL && closure->func->is_generator);
}

static ant_value_t util_types_is_generator_object(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(vtype(args[0]) == T_GENERATOR);
}

static ant_value_t util_types_is_arguments_object(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(util_is_arguments_object_value(js, args[0]));
}

static ant_value_t util_types_is_native_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  return js_bool(js_get_slot(args[0], SLOT_ERROR_BRAND) == js_true);
}

static ant_value_t util_types_is_boxed_primitive(ant_t *js, ant_value_t *args, int nargs) {
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], NULL));
}

static ant_value_t util_types_is_boolean_object(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t type = T_UNDEF;
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], &type) && type == T_BOOL);
}

static ant_value_t util_types_is_number_object(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t type = T_UNDEF;
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], &type) && type == T_NUM);
}

static ant_value_t util_types_is_string_object(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t type = T_UNDEF;
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], &type) && type == T_STR);
}

static ant_value_t util_types_is_symbol_object(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t type = T_UNDEF;
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], &type) && type == T_SYMBOL);
}

static ant_value_t util_types_is_bigint_object(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t type = T_UNDEF;
  return js_bool(nargs > 0 && util_is_boxed_primitive(args[0], &type) && type == T_BIGINT);
}

static ant_value_t util_types_is_map_iterator(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  return js_bool(util_has_proto_in_chain(js, args[0], g_map_iter_proto));
}

static ant_value_t util_types_is_set_iterator(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  return js_bool(util_has_proto_in_chain(js, args[0], g_set_iter_proto));
}

static ant_value_t util_types_is_module_namespace_object(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(js_check_brand(args[0], BRAND_MODULE_NAMESPACE));
}

ant_value_t util_types_library(ant_t *js) {
  ant_value_t types = js_mkobj(js);

  js_set(js, types, "isAnyArrayBuffer", js_mkfun(util_types_is_any_array_buffer));
  js_set(js, types, "isArrayBuffer", js_mkfun(util_types_is_array_buffer));
  js_set(js, types, "isArgumentsObject", js_mkfun(util_types_is_arguments_object));
  js_set(js, types, "isArrayBufferView", js_mkfun(util_types_is_array_buffer_view));
  js_set(js, types, "isAsyncFunction", js_mkfun(util_types_is_async_function));
  js_set(js, types, "isBigInt64Array", js_mkfun(util_types_is_bigint64_array));
  js_set(js, types, "isBigIntObject", js_mkfun(util_types_is_bigint_object));
  js_set(js, types, "isBigUint64Array", js_mkfun(util_types_is_biguint64_array));
  js_set(js, types, "isBooleanObject", js_mkfun(util_types_is_boolean_object));
  js_set(js, types, "isBoxedPrimitive", js_mkfun(util_types_is_boxed_primitive));
  js_set(js, types, "isDataView", js_mkfun(util_types_is_data_view));
  js_set(js, types, "isDate", js_mkfun(util_types_is_date));
  js_set(js, types, "isFloat16Array", js_mkfun(util_types_is_float16_array));
  js_set(js, types, "isFloat32Array", js_mkfun(util_types_is_float32_array));
  js_set(js, types, "isFloat64Array", js_mkfun(util_types_is_float64_array));
  js_set(js, types, "isGeneratorFunction", js_mkfun(util_types_is_generator_function));
  js_set(js, types, "isGeneratorObject", js_mkfun(util_types_is_generator_object));
  js_set(js, types, "isInt8Array", js_mkfun(util_types_is_int8_array));
  js_set(js, types, "isInt16Array", js_mkfun(util_types_is_int16_array));
  js_set(js, types, "isInt32Array", js_mkfun(util_types_is_int32_array));
  js_set(js, types, "isMap", js_mkfun(util_types_is_map));
  js_set(js, types, "isMapIterator", js_mkfun(util_types_is_map_iterator));
  js_set(js, types, "isModuleNamespaceObject", js_mkfun(util_types_is_module_namespace_object));
  js_set(js, types, "isNativeError", js_mkfun(util_types_is_native_error));
  js_set(js, types, "isNumberObject", js_mkfun(util_types_is_number_object));
  js_set(js, types, "isPromise", js_mkfun(util_types_is_promise));
  js_set(js, types, "isProxy", js_mkfun(util_types_is_proxy));
  js_set(js, types, "isRegExp", js_mkfun(util_types_is_regexp));
  js_set(js, types, "isSet", js_mkfun(util_types_is_set));
  js_set(js, types, "isSetIterator", js_mkfun(util_types_is_set_iterator));
  js_set(js, types, "isSharedArrayBuffer", js_mkfun(util_types_is_shared_array_buffer));
  js_set(js, types, "isStringObject", js_mkfun(util_types_is_string_object));
  js_set(js, types, "isSymbolObject", js_mkfun(util_types_is_symbol_object));
  js_set(js, types, "isTypedArray", js_mkfun(util_types_is_typed_array));
  js_set(js, types, "isUint8Array", js_mkfun(util_types_is_uint8_array));
  js_set(js, types, "isUint8ClampedArray", js_mkfun(util_types_is_uint8_clamped_array));
  js_set(js, types, "isUint16Array", js_mkfun(util_types_is_uint16_array));
  js_set(js, types, "isUint32Array", js_mkfun(util_types_is_uint32_array));
  js_set(js, types, "isWeakMap", js_mkfun(util_types_is_weak_map));
  js_set(js, types, "isWeakSet", js_mkfun(util_types_is_weak_set));

  return types;
}

static ant_value_t util_get_types_object(ant_t *js) {
  bool loaded = false;
  ant_value_t types = js_esm_load_registered_library(js, "util/types", 10, &loaded);
  return loaded ? types : util_types_library(js);
}

static ant_value_t util_debuglog_call(ant_params_t) {
  return js_mkundef();
}

static ant_value_t util_debuglog(ant_params_t) {
  ant_value_t logger = js_mkfun(util_debuglog_call);
  js_set(js, logger, "enabled", js_false);

  if (nargs >= 2 && is_callable(args[1])) {
    ant_value_t cb_args[1] = { logger };
    ant_value_t result = sv_vm_call(js->vm, js, args[1], js_mkundef(), cb_args, 1, NULL, false);
    if (is_err(result) || js->thrown_exists) return result;
  }

  return logger;
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

static inline bool util_env_is_inline_ws(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r';
}

static inline bool util_env_is_ident_start(char ch) {
  return 
    (ch >= 'A' && ch <= 'Z') ||
    (ch >= 'a' && ch <= 'z') ||
    ch == '_';
}

static inline bool util_env_is_ident_continue(char ch) {
  return util_env_is_ident_start(ch) || (ch >= '0' && ch <= '9');
}

static void util_env_skip_inline_ws(const char *src, size_t len, size_t *cursor) {
  while (*cursor < len && util_env_is_inline_ws(src[*cursor])) (*cursor)++;
}

static void util_env_skip_line(const char *src, size_t len, size_t *cursor) {
  while (*cursor < len && src[*cursor] != '\n') (*cursor)++;
  if (*cursor < len && src[*cursor] == '\n') (*cursor)++;
}

static bool util_env_consume_export(const char *src, size_t len, size_t *cursor) {
  if (*cursor + 6 > len) return false;
  if (memcmp(src + *cursor, "export", 6) != 0) return false;
  
  if (
    *cursor + 6 < len &&
    !util_env_is_inline_ws(src[*cursor + 6]) &&
    src[*cursor + 6] != '\n'
  ) return false;

  *cursor += 6;
  util_env_skip_inline_ws(src, len, cursor);
  return true;
}

static bool util_env_parse_key(
  const char *src, size_t len, size_t *cursor,
  size_t *key_start, size_t *key_end
) {
  if (*cursor >= len || !util_env_is_ident_start(src[*cursor])) return false;
  *key_start = *cursor; (*cursor)++;
  
  while (*cursor < len && util_env_is_ident_continue(src[*cursor])) (*cursor)++;
  *key_end = *cursor;
  
  return true;
}

static void util_env_set_entry(
  ant_t *js, ant_value_t obj, const char *key,
  size_t key_len, ant_value_t value
) {
  ant_value_t key_str = js_mkstr(js, key, key_len);
  js_setprop(js, obj, key_str, value);
}

static ant_value_t util_env_parse_quoted_value(
  ant_t *js, const char *src,
  size_t len, size_t *cursor
) {
  util_sb_t sb = {0};
  ant_value_t value = js_mkstr(js, "", 0);
  char quote;

  if (*cursor >= len) return value;
  quote = src[(*cursor)++];

  while (*cursor < len) {
    char ch = src[(*cursor)++];
    if (ch == quote) goto done;
    if (ch != '\\' || *cursor >= len) {
      util_sb_append_c(&sb, ch);
      continue;
    }

    char esc = src[(*cursor)++];
    if (quote != '"') {
      util_sb_append_c(&sb, esc);
      continue;
    }

    switch (esc) {
      case 'n': util_sb_append_c(&sb, '\n'); break;
      case 'r': util_sb_append_c(&sb, '\r'); break;
      case 't': util_sb_append_c(&sb, '\t'); break;
      case '\\': util_sb_append_c(&sb, '\\'); break;
      case '"': util_sb_append_c(&sb, '"'); break;
      default: util_sb_append_c(&sb, esc); break;
    }
  }

done:
  value = js_mkstr(js, sb.buf ? sb.buf : "", sb.len);
  free(sb.buf);

  while (*cursor < len && src[*cursor] != '\n') {
    if (src[*cursor] == '#') break;
    (*cursor)++;
  }

  return value;
}

static ant_value_t util_env_parse_unquoted_value(
  ant_t *js, const char *src,
  size_t len, size_t *cursor
) {
  size_t value_start = *cursor;
  size_t value_end = *cursor;
  bool saw_space = false;

  while (*cursor < len && src[*cursor] != '\n' && src[*cursor] != '\r') {
    if (src[*cursor] == '#') {
      if (*cursor == value_start || saw_space) goto done;
    }

    saw_space = util_env_is_inline_ws(src[*cursor]);
    (*cursor)++;
    value_end = *cursor;
  }

done:
  while (value_start < value_end && util_env_is_inline_ws(src[value_start])) {
    value_start++;
  }
  while (value_end > value_start && util_env_is_inline_ws(src[value_end - 1])) {
    value_end--;
  }

  return js_mkstr(js, src + value_start, value_end - value_start);
}

static ant_value_t util_parse_env(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t out = js_mkobj(js);
  if (nargs < 1) return out;

  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, args[0], cbuf, sizeof(cbuf));
  const char *src = cstr.ptr;
  size_t len = strlen(src);
  size_t i = 0;

  if (len >= 3 &&
      (unsigned char)src[0] == 0xEF &&
      (unsigned char)src[1] == 0xBB &&
      (unsigned char)src[2] == 0xBF) {
    i = 3;
  }

  while (i < len) {
    size_t key_start = 0;
    size_t key_end = 0;
    ant_value_t value = js_mkstr(js, "", 0);

    util_env_skip_inline_ws(src, len, &i);
    if (i >= len) break;
    if (src[i] == '\n') {
      i++;
      continue;
    }
    if (src[i] == '#') goto skip_line;

    util_env_consume_export(src, len, &i);
    if (!util_env_parse_key(src, len, &i, &key_start, &key_end)) goto skip_line;

    util_env_skip_inline_ws(src, len, &i);
    if (i >= len || src[i] != '=') goto skip_line;
    i++;
    util_env_skip_inline_ws(src, len, &i);

    if (i < len && (src[i] == '"' || src[i] == '\'' || src[i] == '`')) {
      value = util_env_parse_quoted_value(js, src, len, &i);
    } else {
      value = util_env_parse_unquoted_value(js, src, len, &i);
    }

    util_env_set_entry(js, out, src + key_start, key_end - key_start, value);

skip_line:
    util_env_skip_line(src, len, &i);
  }

  if (cstr.needs_free) free((void *)cstr.ptr);
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
  ant_value_t ctx = js_get_slot(fn, SLOT_DATA);
  if (!is_object_type(ctx)) return js_mkundef();

  ant_value_t settled = js_get_slot(ctx, SLOT_SETTLED);
  if (vtype(settled) == T_BOOL && settled == js_true) return js_mkundef();
  js_set_slot(ctx, SLOT_SETTLED, js_true);

  ant_value_t promise = js_get_slot(ctx, SLOT_DATA);
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
  ant_value_t original = js_get_slot(fn, SLOT_DATA);
  if (!is_callable(original)) return js_mkerr(js, "promisified target is not callable");

  ant_value_t promise = js_mkpromise(js);
  ant_value_t ctx = js_mkobj(js);
  js_set_slot(ctx, SLOT_DATA, promise);
  js_set_slot(ctx, SLOT_SETTLED, js_false);
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

  ant_value_t settled = js_get_slot(ctx, SLOT_SETTLED);
  bool is_settled = (vtype(settled) == T_BOOL && settled == js_true);
  if (!is_settled && (is_err(call_result) || js->thrown_exists)) {
    ant_value_t ex = js->thrown_exists ? js->thrown_value : call_result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    js_set_slot(ctx, SLOT_SETTLED, js_true);
    js_reject_promise(js, promise, ex);
  }

  return promise;
}

static ant_value_t util_callbackify_success(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t callback = js_get_slot(state, SLOT_DATA);
  if (!is_callable(callback)) return js_mkundef();

  ant_value_t cb_args[2] = {
    js_mknull(),
    nargs > 0 ? args[0] : js_mkundef()
  };
  
  return sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 2, NULL, false);
}

static ant_value_t util_callbackify_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t callback = js_get_slot(state, SLOT_DATA);
  if (!is_callable(callback)) return js_mkundef();

  ant_value_t err = nargs > 0 ? args[0] : js_mkerr(js, "Promise was rejected");
  ant_value_t cb_args[1] = { err };
  return sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 1, NULL, false);
}

static ant_value_t util_callbackified_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t original = js_get_slot(fn, SLOT_DATA);
  
  if (!is_callable(original)) return js_mkerr(js, "callbackified target is not callable");
  if (nargs < 1 || !is_callable(args[nargs - 1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "callbackified function requires a callback");

  ant_value_t callback = args[nargs - 1];
  int call_nargs = nargs - 1;
  ant_value_t *call_args = NULL;

  if (call_nargs > 0) {
    call_args = (ant_value_t *)malloc((size_t)call_nargs * sizeof(ant_value_t));
    if (!call_args) return js_mkerr(js, "Out of memory");
    for (int i = 0; i < call_nargs; i++) call_args[i] = args[i];
  }

  ant_value_t result = sv_vm_call(js->vm, js, original, js_getthis(js), call_args, call_nargs, NULL, false);
  free(call_args);

  if (is_err(result) || js->thrown_exists) {
    ant_value_t ex = js->thrown_exists ? js->thrown_value : result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    ant_value_t cb_args[1] = { ex };
    sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 1, NULL, false);
    return js_mkundef();
  }

  if (vtype(result) != T_PROMISE) {
    ant_value_t cb_args[2] = { js_mknull(), result };
    sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 2, NULL, false);
    return js_mkundef();
  }

  ant_value_t state = js_mkobj(js);
  js_set_slot(state, SLOT_DATA, callback);
  ant_value_t success = js_heavy_mkfun(js, util_callbackify_success, state);
  ant_value_t error = js_heavy_mkfun(js, util_callbackify_error, state);
  js_promise_then(js, result, success, error);
  
  return js_mkundef();
}

static ant_value_t util_deprecated_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t ctx = js_get_slot(fn, SLOT_DATA);
  if (!is_object_type(ctx)) return js_mkundef();

  ant_value_t warned = js_get_slot(ctx, SLOT_SETTLED);
  if (!(vtype(warned) == T_BOOL && warned == js_true)) {
    js_set_slot(ctx, SLOT_SETTLED, js_true);
    ant_value_t msg_val = js_get(js, ctx, "msg");
    const char *msg = js_getstr(js, msg_val, NULL);
    if (msg) fprintf(stderr, "DeprecationWarning: %s\n", msg);
  }

  ant_value_t original = js_get_slot(ctx, SLOT_DATA);
  ant_value_t this_arg = js_getthis(js);
  return sv_vm_call(js->vm, js, original, this_arg, args, nargs, NULL, false);
}

static ant_value_t util_deprecate(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0])) {
    return js_mkerr(js, "deprecate(fn, msg) requires a function");
  }

  ant_value_t ctx = js_mkobj(js);
  js_set_slot(ctx, SLOT_DATA, args[0]);
  js_set_slot(ctx, SLOT_SETTLED, js_false);
  if (nargs >= 2) js_set(js, ctx, "msg", args[1]);

  return js_heavy_mkfun(js, util_deprecated_call, ctx);
}

static ant_value_t util_promisify(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0])) {
    return js_mkerr(js, "promisify(fn) requires a function");
  }
  return js_heavy_mkfun(js, util_promisified_call, args[0]);
}

static ant_value_t util_callbackify(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0])) {
    return js_mkerr(js, "callbackify(fn) requires a function");
  }
  return js_heavy_mkfun(js, util_callbackified_call, args[0]);
}

static ant_value_t util_aborted_listener(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (vtype(promise) == T_PROMISE) js_resolve_promise(js, promise, js_mkundef());
  return js_mkundef();
}

static ant_value_t util_aborted(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !abort_signal_is_signal(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "aborted(signal, resource) requires an AbortSignal");

  ant_value_t promise = js_mkpromise(js);
  if (abort_signal_is_aborted(args[0])) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  abort_signal_add_listener(js, args[0], js_heavy_mkfun(js, util_aborted_listener, promise));
  return promise;
}

static ant_value_t util_inherits(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || !is_callable(args[0]) || !is_callable(args[1])) {
    return js_mkerr(js, "inherits(ctor, superCtor) requires constructor functions");
  }

  ant_value_t ctor = args[0];
  ant_value_t super_ctor = args[1];
  ant_value_t ctor_proto = js_get(js, ctor, "prototype");
  ant_value_t super_proto = js_get(js, super_ctor, "prototype");

  if (!is_object_type(ctor_proto) || !is_object_type(super_proto)) {
    return js_mkerr(js, "inherits(ctor, superCtor) requires prototype objects");
  }

  js_set(js, ctor, "super_", super_ctor);
  js_set_proto_init(ctor_proto, super_proto);
  
  return js_mkundef();
}

static ant_value_t util_is_deep_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_bool(true);
  return js_bool(js_deep_equal(js, args[0], args[1], true));
}

ant_value_t util_library(ant_t *js) {  
  ant_value_t lib = js_mkobj(js);
  ant_value_t types = util_get_types_object(js);

  js_set(js, lib, "format", js_mkfun(util_format));
  js_set(js, lib, "formatWithOptions", js_mkfun(util_format_with_options));
  js_set(js, lib, "debuglog", js_mkfun(util_debuglog));
  js_set(js, lib, "inspect", js_mkfun(util_inspect));
  js_set(js, lib, "deprecate", js_mkfun(util_deprecate));
  js_set(js, lib, "inherits", js_mkfun(util_inherits));
  js_set(js, lib, "isDeepStrictEqual", js_mkfun(util_is_deep_strict_equal));
  js_set(js, lib, "parseArgs", js_mkfun(util_parse_args));
  js_set(js, lib, "parseEnv", js_mkfun(util_parse_env));
  js_set(js, lib, "promisify", js_mkfun(util_promisify));
  js_set(js, lib, "callbackify", js_mkfun(util_callbackify));
  js_set(js, lib, "aborted", js_mkfun(util_aborted));
  js_set(js, lib, "stripVTControlCharacters", js_mkfun(util_strip_vt_control_characters));
  js_set(js, lib, "styleText", js_mkfun(util_style_text));
  js_set(js, lib, "types", types);

  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "util", 4));
  return lib;
}
