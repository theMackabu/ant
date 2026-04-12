// TODO: cleanup module, make cleaner

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "utf8.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "utils.h"
#include "escape.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/regex.h"
#include "modules/symbol.h"
#include "gc/objects.h"

#include <pcre2.h>

typedef struct {
  ant_object_t *obj;
  pcre2_code *code;
  pcre2_match_data *match_data;
  bool jit_ready;
} regex_cache_entry_t;

static regex_cache_entry_t *regex_cache = NULL;
static ant_value_t regexp_matchall_iter_proto_val = 0;

static size_t regex_cache_count = 0;
static size_t regex_cache_cap = 0;

static void update_regexp_statics(ant_t *js, const char *str_ptr, PCRE2_SIZE *ovector, uint32_t ovcount) {
  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  if (is_err(regexp_ctor) || vtype(regexp_ctor) == T_UNDEF) return;

  ant_value_t empty = js_mkstr(js, "", 0);
  for (int i = 1; i <= 9; i++) {
    char key[3] = {'$', (char)('0' + i), '\0'};
    ant_value_t val = empty;
    if ((uint32_t)i < ovcount && ovector[2*i] != PCRE2_UNSET)
      val = js_mkstr(js, str_ptr + ovector[2*i], ovector[2*i+1] - ovector[2*i]);
    js_set(js, regexp_ctor, key, val);
  }

  ant_value_t match0 = empty;
  if (ovcount > 0 && ovector[0] != PCRE2_UNSET)
    match0 = js_mkstr(js, str_ptr + ovector[0], ovector[1] - ovector[0]);
  js_set(js, regexp_ctor, "lastMatch", match0);
  js_set(js, regexp_ctor, "$&", match0);
}

static inline bool is_pcre2_passthrough_escape(char c) {
switch (c) {
  case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
  case 'b': case 'B': case 'n': case 'r': case 't': case 'f':
  case '1': case '2': case '3': case '4': case '5':
  case '6': case '7': case '8': case '9':
  case '.': case '*': case '+': case '?':
  case '(': case ')': case '[': case ']':
  case '{': case '}': case '|': case '^':
  case '$': case '\\': case '/': case '-': return true;
  default: return false;
}}

static inline bool is_class_shorthand(char c) {
  return c == 'w' || c == 'W' || c == 'd' || c == 'D' || c == 's' || c == 'S';
}

static size_t v_close_bracket(const char *src, size_t src_len, size_t open) {
  int depth = 0;
  for (size_t i = open; i < src_len; i++) {
    if (src[i] == '\\' && i + 1 < src_len) { i++; continue; }
    if (src[i] == '[') depth++;
    else if (src[i] == ']') { if (--depth == 0) return i; }
  }
  return src_len;
}

static size_t v_translate_part(const char *p, size_t len, char *out, size_t out_size) {
  if (len && p[0] == '[') return js_to_pcre2_pattern(p, len, out, out_size, false);
  char tmp[1024];
  if (len >= sizeof(tmp) - 2) return 0;
  tmp[0] = '['; memcpy(tmp + 1, p, len); tmp[len + 1] = ']';
  return js_to_pcre2_pattern(tmp, len + 2, out, out_size, false);
}

static int v_set_op(const char *src, size_t start, size_t end, size_t *op_pos) {
  int depth = 0;
  for (size_t i = start; i < end; ) {
    if (src[i] == '\\' && i + 1 < end) {
      char n = src[i + 1];
      if ((n == 'p' || n == 'P') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      if ((n == 'u' || n == 'x') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      i += 2; continue;
    }
    if (src[i] == '[') { depth++; i++; continue; }
    if (src[i] == ']') { if (depth > 0) { depth--; i++; continue; } break; }
    if (!depth && i + 1 < end) {
      if (src[i] == '&' && src[i+1] == '&') { *op_pos = i; return 1; }
      if (src[i] == '-' && src[i+1] == '-') { *op_pos = i; return 2; }
    }
    i++;
  }
  return 0;
}

size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size, bool v_flag) {
  size_t di = 0;
  int charclass_depth = 0;

#define OUT(ch) do { if (di < dst_size - 1) dst[di++] = (ch); } while(0)

  for (size_t si = 0; si < src_len && di < dst_size - 1; si++) {
    if (src[si] == '[') {
      if (v_flag && charclass_depth == 0) {
        size_t close = v_close_bracket(src, src_len, si);
        size_t op_pos;
        int op_type = v_set_op(src, si + 1, close, &op_pos);
        if (op_type && close < src_len) {
          char ao[1024], bo[1024];
          size_t aol = v_translate_part(&src[si + 1], op_pos - si - 1, ao, sizeof(ao));
          size_t bol = v_translate_part(&src[op_pos + 2], close - op_pos - 2, bo, sizeof(bo));
          const char *la = op_type == 1 ? ao : bo, *ra = op_type == 1 ? bo : ao;
          size_t ll = op_type == 1 ? aol : bol, rl = op_type == 1 ? bol : aol;
          OUT('('); OUT('?'); OUT(op_type == 1 ? '=' : '!');
          for (size_t k = 0; k < ll; k++) OUT(la[k]);
          OUT(')');
          for (size_t k = 0; k < rl; k++) OUT(ra[k]);
          si = close;
          continue;
        }
      }
      charclass_depth++;
      OUT('[');
      continue;
    }
    if (src[si] == ']' && charclass_depth > 0) {
      charclass_depth--;
      OUT(']');
      continue;
    }

    if (charclass_depth > 0 && src[si] == '-' && si > 0 && src[si - 1] != '[' &&
        si + 1 < src_len && src[si + 1] != ']') {
      bool prev_is_shorthand = (si >= 2 && src[si - 2] == '\\' && is_class_shorthand(src[si - 1]));
      bool next_is_shorthand = (si + 2 < src_len && src[si + 1] == '\\' && is_class_shorthand(src[si + 2]));
      if (prev_is_shorthand || next_is_shorthand) {
        OUT('\\'); OUT('-');
        continue;
      }
      OUT('-');
      continue;
    }

    if (src[si] != '\\' || si + 1 >= src_len) {
      OUT(src[si]);
      continue;
    }

    char next = src[si + 1];

    if (next == 'v') {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('b'); OUT('}');
      si++;
      continue;
    }

    if (next == 'u' && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}' && is_xdigit(src[brace_end])) brace_end++;
      if (brace_end < src_len && src[brace_end] == '}' && brace_end > brace_start) {
        OUT('\\'); OUT('x'); OUT('{');
        for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
        OUT('}');
        si = brace_end;
        continue;
      }
    }

    if (next == 'u' && si + 5 < src_len &&
        is_xdigit(src[si+2]) && is_xdigit(src[si+3]) &&
        is_xdigit(src[si+4]) && is_xdigit(src[si+5])) {
      OUT('\\'); OUT('x'); OUT('{');
      OUT(src[si+2]); OUT(src[si+3]); OUT(src[si+4]); OUT(src[si+5]);
      OUT('}');
      si += 5;
      continue;
    }

    if (next == 'u') {
      si++;
      OUT('u');
      continue;
    }

    if (next == 'x' && si + 3 < src_len &&
        is_xdigit(src[si+2]) && is_xdigit(src[si+3])) {
      OUT('\\'); OUT('x'); OUT(src[si+2]); OUT(src[si+3]);
      si += 3;
      continue;
    }

    if (next == 'x') {
      si++;
      OUT('x');
      continue;
    }

    if (next == '0' && (si + 2 >= src_len || src[si+2] < '0' || src[si+2] > '9')) {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('}');
      si++;
      continue;
    }

    if (next >= '0' && next <= '7') {
      unsigned int octal = next - '0';
      size_t advance = 1;
      if (si + 2 < src_len && src[si+2] >= '0' && src[si+2] <= '7') {
        octal = octal * 8 + (src[si+2] - '0');
        advance = 2;
        if (si + 3 < src_len && src[si+3] >= '0' && src[si+3] <= '7' && octal * 8 + (src[si+3] - '0') <= 255) {
          octal = octal * 8 + (src[si+3] - '0');
          advance = 3;
        }
      }
      
      if (advance > 1 || next == '0') {
        char hex[8];
        int hlen = snprintf(hex, sizeof(hex), "\\x{%02x}", octal);
        for (int k = 0; k < hlen && di < dst_size - 1; k++) OUT(hex[k]);
        si += advance;
        continue;
      }
    }

    if (next == 'c' && si + 2 < src_len &&
        ((src[si+2] >= 'A' && src[si+2] <= 'Z') || (src[si+2] >= 'a' && src[si+2] <= 'z'))) {
      OUT('\\'); OUT('c'); OUT(src[si+2]);
      si += 2;
      continue;
    }

    if (next == 'c') {
      OUT('\\'); OUT('\\'); OUT('c');
      si++;
      continue;
    }

    if ((next == 'p' || next == 'P') && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}') brace_end++;
      if (brace_end < src_len && src[brace_end] == '}') {
        const char *prop = &src[brace_start];
        size_t prop_len = brace_end - brace_start;
        static const struct { const char *name; const char *code; } gc_map[] = {
          {"Letter","L"},{"Cased_Letter","LC"},{"Uppercase_Letter","Lu"},
          {"Lowercase_Letter","Ll"},{"Titlecase_Letter","Lt"},
          {"Modifier_Letter","Lm"},{"Other_Letter","Lo"},
          {"Mark","M"},{"Nonspacing_Mark","Mn"},{"Spacing_Mark","Mc"},
          {"Enclosing_Mark","Me"},
          {"Number","N"},{"Decimal_Number","Nd"},{"Letter_Number","Nl"},
          {"Other_Number","No"},
          {"Punctuation","P"},{"Connector_Punctuation","Pc"},
          {"Dash_Punctuation","Pd"},{"Open_Punctuation","Ps"},
          {"Close_Punctuation","Pe"},{"Initial_Punctuation","Pi"},
          {"Final_Punctuation","Pf"},{"Other_Punctuation","Po"},
          {"Symbol","S"},{"Math_Symbol","Sm"},{"Currency_Symbol","Sc"},
          {"Modifier_Symbol","Sk"},{"Other_Symbol","So"},
          {"Separator","Z"},{"Space_Separator","Zs"},
          {"Line_Separator","Zl"},{"Paragraph_Separator","Zp"},
          {"Other","C"},{"Control","Cc"},{"Format","Cf"},
          {"Surrogate","Cs"},{"Private_Use","Co"},{"Unassigned","Cn"},
        };
        static const struct { const char *script; const char *range; } u17_scripts[] = {
          {"Sidetic",       "\\x{10940}-\\x{1095F}"},
          {"Garay",         "\\x{10D40}-\\x{10D8F}"},
          {"Gurung_Khema",  "\\x{16100}-\\x{1613F}"},
          {"Kirat_Rai",     "\\x{16D40}-\\x{16D7F}"},
          {"Ol_Onal",       "\\x{1E5D0}-\\x{1E5FF}"},
          {"Sunuwar",       "\\x{11BC0}-\\x{11BFF}"},
          {"Tulu_Tigalari", "\\x{11380}-\\x{113FF}"},
        };
        bool has_eq = (memchr(prop, '=', prop_len) != NULL);
        bool has_colon = (memchr(prop, ':', prop_len) != NULL);
        if (!has_eq && !has_colon && next == 'p' && charclass_depth == 0) {
          static const struct { const char *name; const char *exp; } sprops[] = {
            {"Emoji_Keycap_Sequence",
             "(?:\\x{23}\\x{fe0f}\\x{20e3}|\\x{2a}\\x{fe0f}\\x{20e3}|[\\x{30}-\\x{39}]\\x{fe0f}\\x{20e3})"},
            {"RGI_Emoji",
             "(?:[\\x{1f1e6}-\\x{1f1ff}]{2}|(?:\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]?\\x{200d})+\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]?|\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]|\\p{Emoji}\\x{fe0f}?)"},
          };
          for (size_t m = 0; m < sizeof(sprops)/sizeof(sprops[0]); m++) {
            if (strlen(sprops[m].name) == prop_len && memcmp(sprops[m].name, prop, prop_len) == 0) {
              for (const char *r = sprops[m].exp; *r && di < dst_size - 1; r++) OUT(*r);
              si = brace_end;
              goto next_char;
            }
          }
        }
        if (has_eq || has_colon) {
          char sep = has_eq ? '=' : ':';
          const char *val = memchr(prop, sep, prop_len);
          if (val) {
            val++;
            size_t val_len = prop_len - (size_t)(val - prop);
            for (size_t m = 0; m < sizeof(u17_scripts)/sizeof(u17_scripts[0]); m++) {
              if (strlen(u17_scripts[m].script) == val_len &&
                  memcmp(u17_scripts[m].script, val, val_len) == 0) {
                const char *r = u17_scripts[m].range;
                OUT('[');
                if (next == 'P') OUT('^');
                for (; *r; r++) OUT(*r);
                OUT(']');
                si = brace_end;
                goto next_char;
              }
            }
          }
        }
        if (!has_eq && !has_colon) {
          static const struct { const char *name; const char *range; } rangeprops[] = {
            {"ASCII", "\\x{0}-\\x{7f}"},
            {"Any",   "\\x{0}-\\x{10ffff}"},
          };
          for (size_t m = 0; m < sizeof(rangeprops)/sizeof(rangeprops[0]); m++) {
            if (strlen(rangeprops[m].name) == prop_len && memcmp(rangeprops[m].name, prop, prop_len) == 0) {
              if (charclass_depth > 0) {
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
              } else {
                OUT('['); if (next == 'P') OUT('^');
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
                OUT(']');
              }
              si = brace_end;
              goto next_char;
            }
          }
        }
        const char *replacement = NULL;
        if (!has_eq && !has_colon) {
          for (size_t m = 0; m < sizeof(gc_map)/sizeof(gc_map[0]); m++) {
            if (strlen(gc_map[m].name) == prop_len &&
                memcmp(gc_map[m].name, prop, prop_len) == 0) {
              replacement = gc_map[m].code;
              break;
            }
          }
        }
        static const struct { const char *prop; const char *extra; } u17_props[] = {
          {"Emoji", "\\x{1FACD}-\\x{1FACE}\\x{1FAE9}\\x{1FAF9}"},
        };
        const char *extra_range = NULL;
        if (!has_eq && !has_colon && !replacement) {
          for (size_t m = 0; m < sizeof(u17_props)/sizeof(u17_props[0]); m++) {
            if (strlen(u17_props[m].prop) == prop_len &&
                memcmp(u17_props[m].prop, prop, prop_len) == 0) {
              extra_range = u17_props[m].extra;
              break;
            }
          }
        }
        if (extra_range && charclass_depth == 0) {
          const char *pfx = (next == 'p') ? "(?:\\p{" : "(?:\\P{";
          for (const char *r = pfx; *r; r++) OUT(*r);
          for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          OUT('}'); OUT('|'); OUT('[');
          if (next == 'P') OUT('^');
          for (const char *r = extra_range; *r; r++) OUT(*r);
          OUT(']'); OUT(')');
        } else {
          OUT('\\'); OUT(next); OUT('{');
          if (replacement) {
            for (const char *r = replacement; *r; r++) OUT(*r);
          } else {
            for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          }
          OUT('}');
        }
        si = brace_end;
        continue;
      }
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    if (is_pcre2_passthrough_escape(next)) {
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    si++;
    OUT(next);
    next_char:;
  }

#undef OUT
  dst[di] = '\0';
  return di;
}

#define REGEXP_SET_PROP(js, obj, key, klen, val, is_new) \
  ((is_new) ? js_mkprop_fast(js, obj, key, klen, val) \
            : js_setprop(js, obj, js_mkstr(js, key, klen), val))

static void regexp_init_flags(ant_t *js, ant_value_t obj, const char *fstr, ant_offset_t flen, bool is_new) {
  bool d = false, g = false, i = false, m = false;
  bool s = false, u = false, v = false, y = false;
  
  for (ant_offset_t k = 0; k < flen; k++) {
    if (fstr[k] == 'd') d = true;
    if (fstr[k] == 'g') g = true;
    if (fstr[k] == 'i') i = true;
    if (fstr[k] == 'm') m = true;
    if (fstr[k] == 's') s = true;
    if (fstr[k] == 'u') u = true;
    if (fstr[k] == 'v') v = true;
    if (fstr[k] == 'y') y = true;
  }

  char sorted[10]; int si = 0;
  if (d) sorted[si++] = 'd';
  if (g) sorted[si++] = 'g';
  if (i) sorted[si++] = 'i';
  if (m) sorted[si++] = 'm';
  if (s) sorted[si++] = 's';
  if (u) sorted[si++] = 'u';
  if (v) sorted[si++] = 'v';
  if (y) sorted[si++] = 'y';

  REGEXP_SET_PROP(js, obj, "flags", 5, js_mkstr(js, sorted, si), is_new);
  REGEXP_SET_PROP(js, obj, "hasIndices", 10, mkval(T_BOOL, d ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "global", 6, mkval(T_BOOL, g ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "ignoreCase", 10, mkval(T_BOOL, i ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "multiline", 9, mkval(T_BOOL, m ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "dotAll", 6, mkval(T_BOOL, s ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "unicode", 7, mkval(T_BOOL, u ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "unicodeSets", 11, mkval(T_BOOL, v ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "sticky", 6, mkval(T_BOOL, y ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "lastIndex", 9, tov(0), is_new);
}

ant_value_t is_regexp_like(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_false;

  ant_value_t match_sym = get_match_sym();
  if (vtype(match_sym) == T_SYMBOL) {
    ant_value_t match_val = js_get_sym(js, value, match_sym);
    if (is_err(match_val)) return match_val;
    if (vtype(match_val) != T_UNDEF) return js_bool(js_truthy(js, match_val));
  }

  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  if (is_err(regexp_ctor)) return regexp_ctor;

  ant_value_t regexp_proto = js_get(js, regexp_ctor, "prototype");
  if (is_err(regexp_proto)) return regexp_proto;
  if (!is_object_type(regexp_proto)) return js_false;

  return js_bool(proto_chain_contains(js, value, regexp_proto));
}

static ant_value_t should_regexp_passthrough(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) != T_UNDEF) return js_false;
  if (nargs <= 0) return js_false;

  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) return js_false;
  if (!is_object_type(args[0])) return js_false;

  ant_value_t is_re = is_regexp_like(js, args[0]);
  if (is_err(is_re)) return is_re;
  if (!js_truthy(js, is_re)) return js_false;

  ant_value_t ctor = js_getprop_fallback(js, args[0], "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  if (is_err(regexp_ctor)) return regexp_ctor;

  return js_bool(same_ctor_identity(js, ctor, regexp_ctor));
}

ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name) {
  ant_value_t is_re = is_regexp_like(js, value);
  if (is_err(is_re)) return is_re;
  if (js_truthy(js, is_re)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument to %s must not be a RegExp", method_name);
  }
  return js_mkundef();
}

static ant_value_t regexp_species_construct(ant_t *js, ant_value_t rx, ant_value_t ctor, ant_value_t *ctor_args, int nargs) {
  ant_value_t seed = js_mkobj(js);
  if (is_err(seed)) return seed;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t saved = js->new_target;
  js->new_target = ctor;
  ant_value_t result = sv_vm_call(js->vm, js, ctor, seed, ctor_args, nargs, NULL, true);
  js->new_target = saved;

  if (is_err(result)) return result;
  if (!is_object_type(result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species constructor returned non-object");

  return result;
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str);
static ant_value_t builtin_regexp_exec(ant_t *js, ant_value_t *args, int nargs);

static regex_cache_entry_t *regex_cache_lookup(ant_object_t *obj) {
  for (size_t i = 0; i < regex_cache_count; i++) {
    if (regex_cache[i].obj == obj) return &regex_cache[i];
  }
  return NULL;
}

static regex_cache_entry_t *regex_cache_insert(ant_object_t *obj, pcre2_code *code, pcre2_match_data *match_data, bool jit_ready) {
  if (regex_cache_count >= regex_cache_cap) {
    size_t new_cap = regex_cache_cap ? regex_cache_cap * 2 : 64;
    regex_cache_entry_t *new_cache = realloc(regex_cache, new_cap * sizeof(regex_cache_entry_t));
    if (!new_cache) return NULL;
    regex_cache = new_cache;
    regex_cache_cap = new_cap;
  }
  regex_cache_entry_t *entry = &regex_cache[regex_cache_count++];
  entry->obj = obj;
  entry->code = code;
  entry->match_data = match_data;
  entry->jit_ready = jit_ready;
  return entry;
}

typedef struct {
  pcre2_code *code;
  pcre2_match_data *match_data;
  bool jit_ready;
} compiled_regex_t;

static bool regex_get_or_compile(ant_t *js, ant_value_t regexp_obj, compiled_regex_t *out) {
  ant_object_t *obj_ptr = js_obj_ptr(regexp_obj);

  regex_cache_entry_t *cached = regex_cache_lookup(obj_ptr);
  if (cached) {
    out->code = cached->code;
    out->match_data = cached->match_data;
    out->jit_ready = cached->jit_ready;
    return true;
  }

  ant_offset_t source_off = lkp(js, regexp_obj, "source", 6);
  if (source_off == 0) return false;
  ant_value_t source_val = js_propref_load(js, source_off);
  if (vtype(source_val) != T_STR) return false;

  ant_offset_t plen, poff = vstr(js, source_val, &plen);
  const char *pattern_ptr = (char *)(uintptr_t)(poff);

  bool ignore_case = false, multiline = false, dotall = false, v_flag = false;
  ant_offset_t flags_off = lkp(js, regexp_obj, "flags", 5);
  if (flags_off != 0) {
    ant_value_t flags_val = js_propref_load(js, flags_off);
    if (vtype(flags_val) == T_STR) {
    ant_offset_t flen, foff = vstr(js, flags_val, &flen);
    const char *flags_str = (char *)(uintptr_t)(foff);
    for (ant_offset_t i = 0; i < flen; i++) {
      if (flags_str[i] == 'i') ignore_case = true;
      if (flags_str[i] == 'm') multiline = true;
      if (flags_str[i] == 's') dotall = true;
      if (flags_str[i] == 'v') v_flag = true;
    }}
  }

  char pcre2_pattern[4096];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, plen, pcre2_pattern, sizeof(pcre2_pattern), v_flag);

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;
  if (dotall) options |= PCRE2_DOTALL;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return false;

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  bool jit_ready = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE) == 0;
  regex_cache_insert(obj_ptr, re, match_data, jit_ready);

  out->code = re;
  out->match_data = match_data;
  out->jit_ready = jit_ready;
  return true;
}

static ant_value_t builtin_RegExp(ant_t *js, ant_value_t *args, int nargs) {
  bool pattern_is_regexp = false;
  if (nargs > 0) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    pattern_is_regexp = js_truthy(js, is_re);
  }

  if (vtype(js->new_target) == T_UNDEF && nargs > 0 && pattern_is_regexp) {
    if (nargs < 2 || vtype(args[1]) == T_UNDEF) {
      ant_value_t ctor = js_getprop_fallback(js, args[0], "constructor");
      if (is_err(ctor)) return ctor;
      ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
      if (is_err(regexp_ctor)) return regexp_ctor;
      if (same_ctor_identity(js, ctor, regexp_ctor)) return args[0];
    }
  }

  ant_value_t regexp_obj = js->this_val;
  bool use_this = (vtype(js->new_target) != T_UNDEF && vtype(regexp_obj) == T_OBJ);

  if (!use_this) {
    regexp_obj = mkobj(js, 0);
    if (is_err(regexp_obj)) return regexp_obj;
  }

  ant_value_t regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, regexp_proto);

  if (is_object_type(instance_proto)) js_set_proto_init(regexp_obj, instance_proto);
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    js_set_slot(regexp_obj, SLOT_CTOR, js->new_target);
  }

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (pattern_is_regexp) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
        flags = js_tostring_val(js, args[1]);
      } else {
        ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
        if (is_err(fl)) return fl;
        flags = js_tostring_val(js, fl);
      }
      if (is_err(flags)) return flags;
    } else if (vtype(args[0]) == T_STR) {
      pattern = args[0];
      if (nargs > 1 && vtype(args[1]) == T_STR) flags = args[1];
    } else if (vtype(args[0]) != T_UNDEF) {
      ant_value_t s = js_tostring_val(js, args[0]);
      if (is_err(s)) return s;
      pattern = s;
      if (nargs > 1 && vtype(args[1]) == T_STR) flags = args[1];
    }
  }

  js_mkprop_fast(js, regexp_obj, "source", 6, pattern);
  ant_offset_t flags_len, flags_off = vstr(js, flags, &flags_len);
  regexp_init_flags(js, regexp_obj, (const char *)(uintptr_t)(flags_off), flags_len, true);

  return regexp_obj;
}

static ant_value_t builtin_regexp_exec(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp = js->this_val;
  if (vtype(regexp) != T_OBJ) return js_mkerr(js, "exec called on non-regexp");
  if (nargs < 1) return js_mknull();

  ant_value_t str_arg = args[0];
  if (vtype(str_arg) != T_STR) return js_mknull();

  ant_offset_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);

  bool global_flag = false, sticky_flag = false;
  ant_offset_t flags_off = lkp(js, regexp, "flags", 5);
  if (flags_off != 0) {
    ant_value_t flags_val = js_propref_load(js, flags_off);
    if (vtype(flags_val) == T_STR) {
      ant_offset_t flen, foff = vstr(js, flags_val, &flen);
      const char *flags_str = (char *)(uintptr_t)(foff);
      for (ant_offset_t i = 0; i < flen; i++) {
        if (flags_str[i] == 'g') global_flag = true;
        if (flags_str[i] == 'y') sticky_flag = true;
      }
    }
  }

  PCRE2_SIZE start_offset = 0;
  if (global_flag || sticky_flag) {
    ant_offset_t lastindex_off = lkp(js, regexp, "lastIndex", 9);
    if (lastindex_off != 0) {
      ant_value_t li_val = js_propref_load(js, lastindex_off);
      if (vtype(li_val) == T_NUM) {
        double li = tod(li_val);
        if (li >= 0 && li <= (double)str_len) start_offset = (PCRE2_SIZE)li;
        else {
          js_setprop(js, regexp, js_mkstr(js, "lastIndex", 9), tov(0));
          return js_mknull();
        }
      }
    }
  }

  compiled_regex_t compiled;
  if (!regex_get_or_compile(js, regexp, &compiled)) return js_mknull();

  uint32_t match_options = 0;
  if (sticky_flag) match_options |= PCRE2_ANCHORED;

  int rc;
  if (compiled.jit_ready && !sticky_flag) {
    rc = pcre2_jit_match(compiled.code, (PCRE2_SPTR)str_ptr, str_len, start_offset, match_options, compiled.match_data, NULL);
  } else rc = pcre2_match(compiled.code, (PCRE2_SPTR)str_ptr, str_len, start_offset, match_options, compiled.match_data, NULL);

  if (rc < 0) {
    if (global_flag || sticky_flag) js_setprop(js, regexp, js_mkstr(js, "lastIndex", 9), tov(0));
    return js_mknull();
  }

  PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(compiled.match_data);
  uint32_t ovcount = pcre2_get_ovector_count(compiled.match_data);

  ant_value_t result_arr = js_mkarr(js);
  for (uint32_t i = 0; i < ovcount && i < 32; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, js_mkundef());
    } else {
      ant_value_t match_str = js_mkstr(js, str_ptr + start, end - start);
      js_arr_push(js, result_arr, match_str);
    }
  }

  js_setprop(js, result_arr, js_mkstr(js, "index", 5), tov((double)ovector[0]));
  js_setprop(js, result_arr, js_mkstr(js, "input", 5), str_arg);

  uint32_t namecount = 0;
  pcre2_pattern_info(compiled.code, PCRE2_INFO_NAMECOUNT, &namecount);
  if (namecount > 0) {
    uint32_t nameentrysize = 0;
    PCRE2_SPTR nametable = NULL;
    pcre2_pattern_info(compiled.code, PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize);
    pcre2_pattern_info(compiled.code, PCRE2_INFO_NAMETABLE, (void *)&nametable);

    ant_value_t groups = js_mkobj(js);
    js_set_proto_init(groups, js_mknull());

    PCRE2_SPTR tabptr = nametable;
    for (uint32_t i = 0; i < namecount; i++) {
      int n = (tabptr[0] << 8) | tabptr[1];
      const char *name = (const char *)(tabptr + 2);
      ant_value_t val = ((uint32_t)n < ovcount) ? js_arr_get(js, result_arr, n) : js_mkundef();
      js_setprop(js, groups, js_mkstr(js, name, strlen(name)), val);
      tabptr += nameentrysize;
    }
    js_setprop(js, result_arr, js_mkstr(js, "groups", 6), groups);
  } else js_setprop(js, result_arr, js_mkstr(js, "groups", 6), js_mkundef());

  update_regexp_statics(js, str_ptr, ovector, ovcount);

  if (global_flag || sticky_flag) {
    js_setprop(js, regexp, js_mkstr(js, "lastIndex", 9), tov((double)ovector[1]));
  }

  return result_arr;
}

static ant_value_t builtin_regexp_toString(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString called on non-object");

  ant_value_t source_val = js_getprop_fallback(js, regexp, "source");
  if (is_err(source_val)) return source_val;
  ant_value_t source_str = js_tostring_val(js, source_val);
  if (is_err(source_str)) return source_str;

  ant_value_t flags_val = js_getprop_fallback(js, regexp, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_offset_t src_len, src_off = vstr(js, source_str, &src_len);
  ant_offset_t fl_len, fl_off = vstr(js, flags_str, &fl_len);

  size_t total = 1 + src_len + 1 + fl_len;
  char *buf = ant_calloc(total + 1);
  if (!buf) return js_mkerr(js, "oom");
  size_t n = 0;
  buf[n++] = '/';
  memcpy(buf + n, (const void *)(uintptr_t)src_off, src_len); n += src_len;
  buf[n++] = '/';
  memcpy(buf + n, (const void *)(uintptr_t)fl_off, fl_len); n += fl_len;

  ant_value_t result = js_mkstr(js, buf, n);
  free(buf);
  return result;
}

static ant_value_t builtin_regexp_compile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "compile called on non-object");

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);

  if (nargs > 0 && vtype(args[0]) != T_UNDEF) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    if (js_truthy(js, is_re)) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
      if (is_err(fl)) return fl;
      flags = js_tostring_val(js, fl);
      if (is_err(flags)) return flags;
    } else {
      pattern = js_tostring_val(js, args[0]);
      if (is_err(pattern)) return pattern;
    }
  }
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    flags = js_tostring_val(js, args[1]);
    if (is_err(flags)) return flags;
  }

  js_setprop(js, rx, js_mkstr(js, "source", 6), pattern);
  ant_offset_t flen, foff = vstr(js, flags, &flen);
  regexp_init_flags(js, rx, (const char *)(uintptr_t)(foff), flen, false);

  ant_object_t *rx_ptr = js_obj_ptr(rx);
  for (size_t i = 0; i < regex_cache_count; i++) {
    if (regex_cache[i].obj == rx_ptr) {
      pcre2_match_data_free(regex_cache[i].match_data);
      pcre2_code_free(regex_cache[i].code);
      regex_cache[i] = regex_cache[--regex_cache_count];
      break;
    }
  }

  return rx;
}

static inline bool is_syntax_char(char c) {
  return 
    c == '^' || c == '$' || c == '\\' || c == '.' || c == '*' ||
    c == '+' || c == '?' || c == '(' || c == ')' || c == '[' ||
    c == ']' || c == '{' || c == '}' || c == '|' || c == '/';
}

static inline bool is_other_punctuator(char c) {
  return
    c == ',' || c == '-' || c == ':' || c == ';' || c == '<' ||
    c == '=' || c == '>' || c == '@' || c == '!' || c == '"' ||
    c == '#' || c == '%' || c == '&' || c == '\'' || c == '`' || c == '~';
}

static ant_value_t builtin_regexp_escape(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.escape requires a string argument");

  ant_offset_t slen, soff = vstr(js, args[0], &slen);
  const char *src = (const char *)(uintptr_t)(soff);

  size_t buf_cap = slen * 6 + 1;
  char *buf = ant_calloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t di = 0;
  bool first = true;

  for (size_t si = 0; si < slen; ) {
    unsigned char c = (unsigned char)src[si];

    if (c >= 0x80) {
      utf8proc_int32_t cp;
      int bytes = (int)utf8_next(
        (const utf8proc_uint8_t *)&src[si],
        (utf8proc_ssize_t)(slen - si), &cp
      );
      for (int b = 0; b < bytes && si < slen; b++)
        buf[di++] = src[si++];
      first = false;
      continue;
    }

    if (first && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      di += snprintf(buf + di, buf_cap - di, "\\x%02x", c);
      si++; first = false;
      continue;
    }

    if (is_syntax_char(c)) {
      buf[di++] = '\\'; buf[di++] = c;
      si++; first = false;
      continue;
    }

    if (is_other_punctuator(c) || c == ' ' || c == '\t' || c == '\n' ||
        c == '\r' || c == '\v' || c == '\f') {
      di += snprintf(buf + di, buf_cap - di, "\\x%02x", c);
      si++; first = false;
      continue;
    }

    buf[di++] = c;
    si++; first = false;
  }

  ant_value_t result = js_mkstr(js, buf, di);
  free(buf);
  return result;
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str) {
  ant_value_t exec_fn = js_get(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;

  if (vtype(exec_fn) == T_FUNC || vtype(exec_fn) == T_CFUNC) {
    ant_value_t call_args[1] = { str };
    ant_value_t result = sv_vm_call(js->vm, js, exec_fn, rx, call_args, 1, NULL, false);
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != T_NULL)
      return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp exec returned non-object");
    return result;
  }

  ant_value_t call_args[1] = { str };
  ant_value_t saved = js->this_val;
  js->this_val = rx;
  ant_value_t result = builtin_regexp_exec(js, call_args, 1);
  js->this_val = saved;

  return result;
}

static ant_value_t builtin_regexp_test(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "test called on non-object");
  ant_value_t str_arg = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;
  ant_value_t result = regexp_exec_abstract(js, regexp, str_arg);
  if (is_err(result)) return result;
  return mkval(T_BOOL, vtype(result) != T_NULL ? 1 : 0);
}

static ant_value_t builtin_regexp_flags_getter(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.flags called on non-object");

  char buf[16];
  int n = 0;

  static const struct { const char *name; size_t len; char flag; } flag_props[] = {
    {"hasIndices", 10, 'd'}, {"global", 6, 'g'}, {"ignoreCase", 10, 'i'},
    {"multiline", 9, 'm'}, {"dotAll", 6, 's'}, {"unicode", 7, 'u'},
    {"unicodeSets", 11, 'v'}, {"sticky", 6, 'y'},
  };

  for (int i = 0; i < 8; i++) {
    ant_value_t v = js_getprop_fallback(js, rx, flag_props[i].name);
    if (is_err(v)) return v;
    if (js_truthy(js, v)) buf[n++] = flag_props[i].flag;
  }

  return js_mkstr(js, buf, n);
}

static ant_value_t builtin_regexp_symbol_match(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@match] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (is_err(global_val)) return global_val;

  if (!js_truthy(js, global_val))
    return regexp_exec_abstract(js, rx, str);

  ant_value_t unicode_val = js_getprop_fallback(js, rx, "unicode");
  if (is_err(unicode_val)) return unicode_val;

  bool full_unicode = js_truthy(js, unicode_val);
  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));

  ant_value_t A = js_mkarr(js);
  if (is_err(A)) return A;
  ant_offset_t n = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == T_NULL) return n == 0 ? js_mknull() : mkval(T_ARR, vdata(A));

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    js_arr_push(js, A, match_str);
    n++;

    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      if (is_err(li_val)) return li_val;
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      ant_offset_t str_len, str_off = vstr(js, str, &str_len);
      double advance = 1;
      if (full_unicode && li < (double)str_len) {
        advance = (double)utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, (ant_offset_t)li);
      } js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + advance));
    }
  }
}


static ant_value_t regexp_matchall_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js->this_val;
  ant_value_t rx = js_get_slot(iter, SLOT_MATCHALL_RX);
  ant_value_t str = js_get_slot(iter, SLOT_MATCHALL_STR);
  ant_value_t done_val = js_get_slot(iter, SLOT_MATCHALL_DONE);
  
  if (js_truthy(js, done_val))
    return js_iter_result(js, false, js_mkundef());

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  if (vtype(result) == T_NULL) {
    js_set_slot(iter, SLOT_MATCHALL_DONE, js_true);
    return js_iter_result(js, false, js_mkundef());
  }

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (js_truthy(js, global_val)) {
    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + 1));
    }
  } else js_set_slot(iter, SLOT_MATCHALL_DONE, js_true);

  return js_iter_result(js, true, result);
}

static ant_value_t builtin_regexp_symbol_matchAll(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@matchAll] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  ant_value_t flags_val = js_getprop_fallback(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_value_t source_val = js_getprop_fallback(js, rx, "source");
  if (is_err(source_val)) return source_val;

  ant_value_t ctor_args[2] = { source_val, flags_str };
  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  ant_value_t new_rx = sv_vm_call(js->vm, js, regexp_ctor, js_mkundef(), ctor_args, 2, NULL, true);
  if (is_err(new_rx)) return new_rx;

  ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
  js_setprop(js, new_rx, js_mkstr(js, "lastIndex", 9), li_val);

  ant_value_t iter = js_mkobj(js);
  js_set_slot(iter, SLOT_MATCHALL_RX, new_rx);
  js_set_slot(iter, SLOT_MATCHALL_STR, str);
  js_set_slot(iter, SLOT_MATCHALL_DONE, js_false);

  js_set_proto_init(iter, regexp_matchall_iter_proto_val);

  return iter;
}

static ant_value_t builtin_string_matchAll(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "matchAll requires at least 1 argument");

  if (is_object_type(args[0])) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (js_truthy(js, is_re)) {
      ant_value_t flags_val = js_getprop_fallback(js, args[0], "flags");
      if (is_err(flags_val)) return flags_val;
      
      ant_value_t flags_str = js_tostring_val(js, flags_val);
      ant_offset_t flen, foff = vstr(js, flags_str, &flen);
      
      const char *fp = (const char *)(uintptr_t)(foff);
      bool has_g = false;
      for (ant_offset_t i = 0; i < flen; i++) if (fp[i] == 'g') has_g = true;
      if (!has_g) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.matchAll called with a non-global RegExp");
    }

    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_matchAll_sym(), args[0], call_args, 1, &called
    );
    
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern_str = js_tostring_val(js, args[0]);
  if (is_err(pattern_str)) return pattern_str;

  ant_value_t ctor_args[2] = { pattern_str, js_mkstr(js, "g", 1) };
  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  ant_value_t rx = sv_vm_call(js->vm, js, regexp_ctor, js_mkundef(), ctor_args, 2, NULL, true);
  if (is_err(rx)) return rx;

  ant_value_t ma_args[1] = { str };
  js->this_val = rx;
  
  return builtin_regexp_symbol_matchAll(js, ma_args, 1);
}

static ant_value_t builtin_regexp_symbol_replace(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@replace] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  ant_value_t replace_value = nargs > 1 ? args[1] : js_mkundef();
  bool func_replace = (vtype(replace_value) == T_FUNC || vtype(replace_value) == T_CFUNC);
  ant_value_t replace_str = js_mkundef();
  if (!func_replace) {
    replace_str = js_tostring_val(js, replace_value);
    if (is_err(replace_str)) return replace_str;
  }

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (is_err(global_val)) return global_val;
  bool global = js_truthy(js, global_val);

  bool full_unicode = false;
  if (global) {
    ant_value_t unicode_val = js_getprop_fallback(js, rx, "unicode");
    if (is_err(unicode_val)) return unicode_val;
    full_unicode = js_truthy(js, unicode_val);
    js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));
  }

  ant_value_t results = js_mkarr(js);
  if (is_err(results)) return results;
  ant_offset_t nresults = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == T_NULL) break;
    js_arr_push(js, results, result);
    nresults++;
    if (!global) break;

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen; vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      if (is_err(li_val)) return li_val;
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      ant_offset_t sl, so = vstr(js, str, &sl);
      double advance = 1;
      if (full_unicode && li < (double)sl) {
        advance = (double)utf8_char_len_at((const char *)(uintptr_t)(so), sl, (ant_offset_t)li);
      }
      js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + advance));
    }
  }

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  size_t buf_cap = str_len + 256;
  char *buf = ant_calloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t buf_len = 0;
  ant_offset_t next_src_pos = 0;

#define SB_APPEND(data, dlen) do { \
  if (buf_len + (dlen) >= buf_cap) { \
    buf_cap = (buf_len + (dlen) + 1) * 2; \
    char *nb = ant_realloc(buf, buf_cap); \
    if (!nb) { free(buf); return js_mkerr(js, "oom"); } \
    buf = nb; \
  } \
  memcpy(buf + buf_len, data, dlen); buf_len += (dlen); \
} while(0)

  for (ant_offset_t i = 0; i < nresults; i++) {
    ant_value_t result = js_arr_get(js, results, i);
    ant_value_t matched = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(matched)) { free(buf); return matched; }
    ant_offset_t matched_len; vstr(js, matched, &matched_len);

    ant_value_t pos_val = js_getprop_fallback(js, result, "index");
    ant_offset_t position = 0;
    if (!is_err(pos_val) && vtype(pos_val) == T_NUM) {
      double d = tod(pos_val);
      position = d < 0 ? 0 : (ant_offset_t)d;
    }
    if (position > str_len) position = str_len;

    ant_value_t replacement;
    if (func_replace) {
      ant_offset_t ncaptures = js_arr_len(js, result);
      ant_value_t call_args[32];
      int ca = 0;
      for (ant_offset_t c = 0; c < ncaptures && ca < 30; c++)
        call_args[ca++] = js_arr_get(js, result, c);
      call_args[ca++] = tov((double)position);
      call_args[ca++] = str;
      replacement = sv_vm_call(js->vm, js, replace_value, js_mkundef(), call_args, ca, NULL, false);
    } else {
      replacement = replace_str;
    }
    if (is_err(replacement)) { free(buf); return replacement; }
    ant_value_t rep_str = js_tostring_val(js, replacement);
    if (is_err(rep_str)) { free(buf); return rep_str; }

    if (position >= next_src_pos) {
      str_off = vstr(js, str, &str_len);
      if (position > next_src_pos)
        SB_APPEND((const char *)(uintptr_t)(str_off + next_src_pos), position - next_src_pos);
      ant_offset_t rep_len, rep_off = vstr(js, rep_str, &rep_len);
      if (func_replace) {
        SB_APPEND((const char *)(uintptr_t)(rep_off), rep_len);
      } else {
        ant_offset_t ncap = js_arr_len(js, result);
        int num_caps = ncap > 1 ? (int)(ncap - 1) : 0;
        repl_capture_t caps_buf[16], *caps = num_caps <= 16 ? caps_buf : ant_calloc(sizeof(repl_capture_t) * (size_t)num_caps);
        if (num_caps > 16 && !caps) {
          free(buf);
          return js_mkerr(js, "oom");
        }
        for (int ci = 0; ci < num_caps; ci++) {
          ant_value_t cap = js_arr_get(js, result, (ant_offset_t)(ci + 1));
          if (vtype(cap) == T_STR) { ant_offset_t cl, co = vstr(js, cap, &cl); caps[ci] = (repl_capture_t){ (const char *)(uintptr_t)(co), cl }; }
          else caps[ci] = (repl_capture_t){ NULL, 0 };
        }
        ant_offset_t mlen, moff = vstr(js, matched, &mlen);
        str_off = vstr(js, str, &str_len);
        bool ok = repl_template(
          (const char *)(uintptr_t)(rep_off), rep_len,
          (const char *)(uintptr_t)(moff), mlen,
          (const char *)(uintptr_t)(str_off), str_len, position,
          caps, num_caps, &buf, &buf_len, &buf_cap
        );
        if (caps != caps_buf) free(caps);
        if (!ok) {
          free(buf);
          return js_mkerr(js, "oom");
        }
      }
      next_src_pos = position + matched_len;
    }
  }

  str_off = vstr(js, str, &str_len);
  if (next_src_pos < str_len)
    SB_APPEND((const char *)(uintptr_t)(str_off + next_src_pos), str_len - next_src_pos);

#undef SB_APPEND

  ant_value_t ret = js_mkstr(js, buf, buf_len);
  free(buf);
  return ret;
}

static ant_value_t builtin_regexp_symbol_search(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@search] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  ant_value_t prev_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(prev_li)) return prev_li;
  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  ant_value_t cur_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(cur_li)) return cur_li;
  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), prev_li);

  if (vtype(result) == T_NULL) return tov(-1);

  ant_value_t idx = js_getprop_fallback(js, result, "index");
  if (is_err(idx)) return idx;
  return vtype(idx) == T_NUM ? idx : tov(-1);
}

static ant_value_t builtin_regexp_symbol_split(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js_getthis(js);
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(str)) return str;

  ant_value_t ctor = js_get(js, rx, "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t C;
  if (vtype(ctor) == T_UNDEF) {
    C = js_get(js, js_glob(js), "RegExp");
  } else if (!is_object_type(ctor)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split]: constructor is not an object");
  } else {
    ant_value_t species = get_ctor_species_value(js, ctor);
    if (is_err(species)) return species;
    if (vtype(species) == T_UNDEF || vtype(species) == T_NULL)
      C = js_get(js, js_glob(js), "RegExp");
    else C = species;
  }

  if (is_err(C)) return C;
  if (vtype(C) != T_FUNC && vtype(C) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species is not a constructor");

  ant_value_t flags_val = js_get(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_offset_t flen, foff = vstr(js, flags_str, &flen);
  const char *fptr = (const char *)(uintptr_t)(foff);
  bool unicode_matching = false, has_sticky = false;
  for (ant_offset_t i = 0; i < flen; i++) {
    if (fptr[i] == 'u' || fptr[i] == 'v') unicode_matching = true;
    if (fptr[i] == 'y') has_sticky = true;
  }

  ant_value_t new_flags;
  if (has_sticky) new_flags = flags_str; else {
    char fbuf[16];
    if (flen > 14) flen = 14;
    foff = vstr(js, flags_str, &flen);
    fptr = (const char *)(uintptr_t)(foff);
    memcpy(fbuf, fptr, flen);
    fbuf[flen] = 'y';
    new_flags = js_mkstr(js, fbuf, flen + 1);
  }

  ant_value_t ctor_args[2] = { rx, new_flags };
  ant_value_t splitter = regexp_species_construct(js, rx, C, ctor_args, 2);
  if (is_err(splitter)) return splitter;

  ant_value_t A = js_mkarr(js);
  if (is_err(A)) return A;
  ant_offset_t lengthA = 0;

  uint32_t lim = UINT32_MAX;
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    double d = tod(args[1]);
    if (d >= 0 && d <= UINT32_MAX) lim = (uint32_t)d;
  } if (lim == 0) return mkval(T_ARR, vdata(A));

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t size = str_len;

  if (size == 0) {
    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;
    if (vtype(z) == T_NULL) js_arr_push(js, A, str);
    return mkval(T_ARR, vdata(A));
  }

  ant_offset_t p = 0, q = p;
  ant_value_t lastIndex_key = js_mkstr(js, "lastIndex", 9);

  while (q < size) {
    js_setprop(js, splitter, lastIndex_key, tov((double)q));

    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;

    if (vtype(z) == T_NULL) {
      if (unicode_matching) {
        str_off = vstr(js, str, &str_len);
        q += utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, q);
      } else q++;
      continue;
    }

    ant_value_t li_val = js_get(js, splitter, "lastIndex");
    if (is_err(li_val)) return li_val;
    double e_raw = vtype(li_val) == T_NUM ? tod(li_val) : 0;
    ant_offset_t e = (ant_offset_t)(e_raw < 0 ? 0 : (e_raw > (double)size ? (double)size : e_raw));

    if (e == p) {
      if (unicode_matching) {
        str_off = vstr(js, str, &str_len);
        q += utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, q);
      } else q++;
      continue;
    }

    str_off = vstr(js, str, NULL);
    ant_value_t T_val = js_mkstr(js, (char *)(uintptr_t)(str_off + p), q - p);
    js_arr_push(js, A, T_val);
    lengthA++;
    if (lengthA == lim) return mkval(T_ARR, vdata(A));

    ant_offset_t num_caps = js_arr_len(js, z);
    for (ant_offset_t i = 1; i < num_caps; i++) {
      ant_value_t cap = js_arr_get(js, z, i);
      js_arr_push(js, A, cap);
      lengthA++;
      if (lengthA == lim) return mkval(T_ARR, vdata(A));
    }

    p = e;
    q = p;
  }

  str_off = vstr(js, str, &str_len);
  ant_value_t trailing = js_mkstr(js, (char *)(uintptr_t)(str_off + p), str_len - p);
  js_arr_push(js, A, trailing);
  return mkval(T_ARR, vdata(A));
}

ant_value_t do_regex_match_pcre2(ant_t *js, regex_match_args_t args) {
  char pcre2_pattern[4096];
  size_t pcre2_len = js_to_pcre2_pattern(args.pattern_ptr, args.pattern_len, pcre2_pattern, sizeof(pcre2_pattern), false);

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;
  if (args.ignore_case) options |= PCRE2_CASELESS;
  if (args.multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return js_mknull();

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  uint32_t capture_count;
  pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return result_arr;
  }

  PCRE2_SIZE pos = 0;
  int match_count = 0;

  while (pos <= (PCRE2_SIZE)args.str_len) {
    int rc = pcre2_match(re, (PCRE2_SPTR)args.str_ptr, args.str_len, pos, 0, match_data, NULL);
    if (rc < 0) break;

    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    PCRE2_SIZE match_start = ovector[0];
    PCRE2_SIZE match_end = ovector[1];

    if (args.global) {
      ant_value_t match_str = js_mkstr(js, args.str_ptr + match_start, match_end - match_start);
      if (is_err(match_str)) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return match_str;
      }
      js_arr_push(js, result_arr, match_str);
    } else {
      for (uint32_t i = 0; i <= capture_count; i++) {
        PCRE2_SIZE start = ovector[2*i];
        PCRE2_SIZE end = ovector[2*i+1];
        if (start == PCRE2_UNSET) {
          js_arr_push(js, result_arr, js_mkundef());
        } else {
          ant_value_t match_str = js_mkstr(js, args.str_ptr + start, end - start);
          if (is_err(match_str)) {
            pcre2_match_data_free(match_data);
            pcre2_code_free(re);
            return match_str;
          }
          js_arr_push(js, result_arr, match_str);
        }
      }
      js_setprop(js, result_arr, js_mkstr(js, "index", 5), tov((double)match_start));
    }
    match_count++;

    if (!args.global) break;
    if (match_start == match_end) {
      pos = match_end + 1;
    } else { pos = match_end; }
  }

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  if (match_count == 0) return js_mknull();
  return result_arr;
}

static bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
  if (n == 0) return true;
  if (*len + n >= *cap) {
    size_t new_cap = (*len + n + 1) * 2;
    char *nb = (char *)ant_realloc(*buf, new_cap);
    if (!nb) return false;
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  return true;
}

static inline ant_value_t emit_str_replacement(
  ant_t *js, ant_value_t replacement, bool is_func,
  const char *repl_ptr, ant_offset_t repl_len,
  const char *str_ptr, ant_value_t str,
  ant_offset_t pos, ant_offset_t match_len,
  char **buf, size_t *buf_len, size_t *buf_cap
) {
  if (is_func) {
    ant_value_t cb_args[3] = { js_mkstr(js, str_ptr + pos, match_len), tov((double)pos), str };
    ant_value_t r = sv_vm_call(js->vm, js, replacement, js_mkundef(), cb_args, 3, NULL, false);
    
    if (vtype(r) == T_ERR) return r;
    ant_value_t r_str = js_tostring_val(js, r);
    
    if (is_err(r_str)) return r_str;
    ant_offset_t rlen, roff = vstr(js, r_str, &rlen);
    
    if (!str_buf_append(buf, buf_len, buf_cap, (const char *)(uintptr_t)roff, rlen)) return js_mkerr(js, "oom");
  } else if (!str_buf_append(buf, buf_len, buf_cap, repl_ptr, repl_len)) return js_mkerr(js, "oom");
  
  return js_mkundef();
}

static ant_value_t string_replace_impl(ant_t *js, ant_value_t *args, int nargs, bool replace_all) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  
  if (is_err(str)) return str;
  if (nargs < 1) return str;

  if (is_object_type(args[0])) {
    if (replace_all) {
      ant_value_t global_val = js_getprop_fallback(js, args[0], "global");
      if (!js_truthy(js, global_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
    }
    
    bool called = false;
    ant_value_t replacement_arg = nargs > 1 ? args[1] : js_mkundef();
    ant_value_t call_args[2] = { str, replacement_arg };
    
    ant_value_t result = maybe_call_symbol_method(js, args[0], get_replace_sym(), args[0], call_args, 2, &called);
    if (is_err(result)) return result;
    if (called) return result;
  }
  
  if (nargs < 2) return str;
  ant_value_t search = args[0];
  ant_value_t replacement = args[1];
  if (vtype(search) != T_STR) return str;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  bool is_func = (vtype(replacement) == T_FUNC);
  ant_offset_t repl_len = 0;
  const char *repl_ptr = NULL;
  
  if (!is_func) {
    if (vtype(replacement) != T_STR) return str;
    ant_offset_t repl_off = vstr(js, replacement, &repl_len);
    repl_ptr = (char *)(uintptr_t)(repl_off);
  }

  if (!replace_all) {
    if (search_len > str_len) return str;
    ant_offset_t match_pos = 0;
    bool found = false;
    
    for (ant_offset_t i = 0; i <= str_len - search_len; i++)
      if (memcmp(str_ptr + i, search_ptr, search_len) == 0) { 
        match_pos = i; found = true; break;
      }
      
    if (!found) return str;

    size_t cap = str_len + repl_len + 256, len = 0;
    char *buf = (char *)ant_calloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    if (!str_buf_append(&buf, &len, &cap, str_ptr, match_pos)) { 
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    ant_value_t err = emit_str_replacement(
      js, replacement, is_func, repl_ptr, 
      repl_len, str_ptr, str, match_pos, 
      search_len, &buf, &len, &cap
    );
    
    if (vtype(err) == T_ERR) { 
      free(buf);
      return err;
    }
    
    if (!str_buf_append(
      &buf, &len, &cap, str_ptr + match_pos + search_len, 
      str_len - match_pos - search_len)
    ) { 
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    ant_value_t ret = js_mkstr(js, buf, len);
    free(buf);
    
    return ret;
  } else {
    size_t cap = str_len + repl_len + 256, len = 0;
    char *buf = (char *)ant_calloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    ant_offset_t pos = 0;
    bool replaced = false;
    
    while (pos + (ant_offset_t)search_len <= str_len) {
      if (search_len == 0 || memcmp(str_ptr + pos, search_ptr, search_len) == 0) {
        replaced = true;
        ant_value_t err = emit_str_replacement(js, replacement, is_func, repl_ptr, repl_len, str_ptr, str, pos, search_len, &buf, &len, &cap);
        if (vtype(err) == T_ERR) { free(buf); return err; }
        if (search_len == 0) {
          if (pos < str_len && !str_buf_append(&buf, &len, &cap, str_ptr + pos, 1)) { free(buf); return js_mkerr(js, "oom"); }
          pos++;
        } else pos += search_len;
      } else {
        if (!str_buf_append(&buf, &len, &cap, str_ptr + pos, 1)) { free(buf); return js_mkerr(js, "oom"); }
        pos++;
      }
    }
    
    if (!str_buf_append(
      &buf, &len, &cap, str_ptr + pos, 
      str_len - pos)
    ) {
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    if (!replaced) { 
      free(buf);
      return str;
    }
    
    ant_value_t ret = js_mkstr(js, buf, len);
    free(buf);
    
    return ret;
  }
}

static ant_value_t builtin_string_replace(ant_t *js, ant_value_t *args, int nargs) {
  return string_replace_impl(js, args, nargs, false);
}

static ant_value_t builtin_string_replaceAll(ant_t *js, ant_value_t *args, int nargs) {
  return string_replace_impl(js, args, nargs, true);
}

static ant_value_t builtin_string_search(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return tov(-1);

  if (is_object_type(args[0])) {
    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_search_sym(), args[0], call_args, 1, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern = args[0];
  const char *pattern_ptr = NULL;
  ant_offset_t pattern_len = 0;
  bool ignore_case = false, multiline = false;

  if (vtype(pattern) == T_OBJ) {
    ant_offset_t source_off = lkp(js, pattern, "source", 6);
    if (source_off == 0) return tov(-1);
    ant_value_t source_val = js_propref_load(js, source_off);
    if (vtype(source_val) != T_STR) return tov(-1);

    ant_offset_t poff;
    poff = vstr(js, source_val, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);

    ant_offset_t flags_off = lkp(js, pattern, "flags", 5);
    if (flags_off != 0) {
      ant_value_t flags_val = js_propref_load(js, flags_off);
      if (vtype(flags_val) == T_STR) {
        ant_offset_t flen, foff = vstr(js, flags_val, &flen);
        const char *flags_str = (char *)(uintptr_t)(foff);
        for (ant_offset_t i = 0; i < flen; i++) {
          if (flags_str[i] == 'i') ignore_case = true;
          if (flags_str[i] == 'm') multiline = true;
        }
      }
    }
  } else if (vtype(pattern) == T_STR) {
    ant_offset_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);
  } else {
    return tov(-1);
  }

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);

  char pcre2_pattern[4096];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, pattern_len, pcre2_pattern, sizeof(pcre2_pattern), false);

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return tov(-1);

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, 0, 0, match_data, NULL);

  if (rc < 0) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return tov(-1);
  }

  PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
  double result = (double)ovector[0];

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  return tov(result);
}

static ant_value_t builtin_string_match(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return js_mknull();

  if (is_object_type(args[0])) {
    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_match_sym(), args[0], call_args, 1, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern = args[0];
  const char *pattern_ptr = NULL;
  ant_offset_t pattern_len = 0;
  bool global_flag = false;
  bool ignore_case = false;
  bool multiline = false;

  if (vtype(pattern) == T_OBJ) {
    ant_offset_t source_off = lkp(js, pattern, "source", 6);
    if (source_off == 0) return js_mknull();

    ant_value_t source_val = js_propref_load(js, source_off);
    if (vtype(source_val) != T_STR) return js_mknull();

    ant_offset_t poff;
    poff = vstr(js, source_val, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);

    ant_offset_t flags_off = lkp(js, pattern, "flags", 5);
    if (flags_off != 0) {
    ant_value_t flags_val = js_propref_load(js, flags_off);
    if (vtype(flags_val) == T_STR) {
      ant_offset_t flen, foff = vstr(js, flags_val, &flen);
      const char *flags_str = (char *)(uintptr_t)(foff);
      for (ant_offset_t i = 0; i < flen; i++) {
        if (flags_str[i] == 'g') global_flag = true;
        if (flags_str[i] == 'i') ignore_case = true;
        if (flags_str[i] == 'm') multiline = true;
      }}
    }
  } else if (vtype(pattern) == T_STR) {
    ant_offset_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);
  } else return js_mknull();

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);

  ant_value_t result = do_regex_match_pcre2(js, (regex_match_args_t){
    .pattern_ptr = pattern_ptr, .pattern_len = pattern_len,
    .str_ptr = str_ptr, .str_len = str_len,
    .global = global_flag, .ignore_case = ignore_case, .multiline = multiline,
  });

  if (!global_flag && vtype(result) == T_ARR) {
    js_setprop(js, result, js_mkstr(js, "input", 5), str);
  }

  return result;
}

void init_regex_module(void) {
  ant_t *js = rt->js;
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t regexp_proto = js_mkobj(js);
  js_set_proto_init(regexp_proto, object_proto);
  
  defmethod(js, regexp_proto, "test", 4, js_mkfun(builtin_regexp_test));
  defmethod(js, regexp_proto, "exec", 4, js_mkfun(builtin_regexp_exec));
  defmethod(js, regexp_proto, "toString", 8, js_mkfun(builtin_regexp_toString));

  js_mkprop_fast(js, regexp_proto, "global", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "ignoreCase", 10, js_false);
  js_mkprop_fast(js, regexp_proto, "multiline", 9, js_false);
  js_mkprop_fast(js, regexp_proto, "dotAll", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "unicode", 7, js_false);
  js_mkprop_fast(js, regexp_proto, "sticky", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "hasIndices", 10, js_false);
  js_mkprop_fast(js, regexp_proto, "unicodeSets", 11, js_false);

  js_set_sym(js, regexp_proto, get_split_sym(), js_mkfun(builtin_regexp_symbol_split));
  js_set_sym(js, regexp_proto, get_match_sym(), js_mkfun(builtin_regexp_symbol_match));
  js_set_sym(js, regexp_proto, get_matchAll_sym(), js_mkfun(builtin_regexp_symbol_matchAll));

  regexp_matchall_iter_proto_val = js_mkobj(js);
  js_set_proto_init(regexp_matchall_iter_proto_val, js->sym.iterator_proto);
  defmethod(js, regexp_matchall_iter_proto_val, "next", 4, js_mkfun(regexp_matchall_next));
  js_set_sym(js, regexp_matchall_iter_proto_val, get_iterator_sym(), js_mkfun(sym_this_cb));
  js_set_sym(js, regexp_proto, get_replace_sym(), js_mkfun(builtin_regexp_symbol_replace));
  js_set_sym(js, regexp_proto, get_search_sym(), js_mkfun(builtin_regexp_symbol_search));
  js_set_sym(js, regexp_proto, get_toStringTag_sym(), js_mkstr(js, "RegExp", 6));
  js_set_getter_desc(js, regexp_proto, "flags", 5, js_mkfun(builtin_regexp_flags_getter), JS_DESC_C);
  defmethod(js, regexp_proto, "compile", 7, js_mkfun(builtin_regexp_compile));

  ant_value_t regexp_ctor = js_mkobj(js);
  js_set_slot(regexp_ctor, SLOT_CFUNC, js_mkfun(builtin_RegExp));
  js_mkprop_fast(js, regexp_ctor, "prototype", 9, regexp_proto);
  js_mkprop_fast(js, regexp_ctor, "name", 4, js_mkstr(js, "RegExp", 6));
  js_set_descriptor(js, regexp_ctor, "name", 4, 0);
  js_define_species_getter(js, regexp_ctor);

  ant_value_t regexp_func = js_obj_to_func(regexp_ctor);
  js_setprop(js, regexp_proto, js_mkstr(js, "constructor", 11), regexp_func);
  js_set_descriptor(js, regexp_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, regexp_ctor, "escape", js_mkfun(builtin_regexp_escape));

  ant_value_t empty = js_mkstr(js, "", 0);
  for (int i = 1; i <= 9; i++) {
    char key[3] = {'$', (char)('0' + i), '\0'};
    js_set(js, regexp_ctor, key, empty);
  }
  
  js_set(js, regexp_ctor, "lastMatch", empty);
  js_set(js, regexp_ctor, "$&", empty);
  js_set(js, glob, "RegExp", regexp_func);

  ant_value_t string_ctor = js_get(js, glob, "String");
  ant_value_t string_proto = js_get(js, string_ctor, "prototype");
  
  defmethod(js, string_proto, "search", 6, js_mkfun(builtin_string_search));
  defmethod(js, string_proto, "match", 5, js_mkfun(builtin_string_match));
  defmethod(js, string_proto, "matchAll", 8, js_mkfun(builtin_string_matchAll));
  defmethod(js, string_proto, "replace", 7, js_mkfun(builtin_string_replace));
  defmethod(js, string_proto, "replaceAll", 10, js_mkfun(builtin_string_replaceAll));
}

void gc_sweep_regex_cache(void) {
  size_t write = 0;
  for (size_t i = 0; i < regex_cache_count; i++) {
    if (!gc_obj_is_marked(regex_cache[i].obj)) {
      pcre2_match_data_free(regex_cache[i].match_data);
      pcre2_code_free(regex_cache[i].code);
    } else {
      if (write != i) regex_cache[write] = regex_cache[i];
      write++;
    }
  }
  regex_cache_count = write;
}

void cleanup_regex_module(void) {
  for (size_t i = 0; i < regex_cache_count; i++) {
    pcre2_match_data_free(regex_cache[i].match_data);
    pcre2_code_free(regex_cache[i].code);
  }
  free(regex_cache);
  regex_cache = NULL;
  regex_cache_count = 0;
  regex_cache_cap = 0;
}
