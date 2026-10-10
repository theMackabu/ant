#include "builder.h"

#include "internal.h"
#include "silver/engine.h"
#include "errors.h"
#include "gc/roots.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_valid_identifier(const char *str, ant_offset_t slen) {
  if (slen == 0) return false;
  char c = str[0];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$')) return false;
  for (ant_offset_t i = 1; i < slen; i++) {
    c = str[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
  }
  return true;
}

static size_t strkey_interned(ant_t *js, const char *key, size_t klen, char *buf, size_t len) {
  if (is_valid_identifier(key, (ant_offset_t)klen)) {
    return cpy(buf, len, key, klen);
  }
  ant_value_t key_str = js_mkstr(js, key, klen);
  return strstring(js, key_str, buf, len);
}

static bool is_small_object(ant_t *js, ant_value_t obj, int *prop_count) {
  int count = 0;
  bool has_nested = false;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  uintptr_t obj_off = (uintptr_t)vdata(as_obj);
  if (ptr && ptr->shape) {
    uint32_t shape_count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < shape_count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop) continue;

      if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
        count++;
        continue;
      }

      if ((ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;

      ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();
      uint8_t t = vtype(val);
      if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) has_nested = true;
      count++;
    }
  }
  
  if (ptr && ptr->flags.is_exotic) {
    descriptor_entry_t *desc, *tmp;
    HASH_ITER(hh, js->desc_registry, desc, tmp) {
      if (desc->obj_off != obj_off) continue;
      if (!desc->enumerable) continue;
      if (!desc->has_getter && !desc->has_setter) continue;
      count++;
    }
  }
  
  if (prop_count) *prop_count = count;
  return count <= 4 && !has_nested;
}

void js_inspect_builder_init_fixed(js_inspect_builder_t *builder, ant_t *js, char *buf, size_t len, size_t initial_n) {
  builder->js = js;
  builder->buf = buf;
  builder->len = len;
  builder->n = initial_n;
  builder->growable = false;
  builder->inline_mode = false;
  builder->bare_mode = false;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;
  builder->error_stack = NULL;
  builder->error_stack_len = 0;
}

bool js_inspect_builder_init_dynamic(js_inspect_builder_t *builder, ant_t *js, size_t initial_cap) {
  size_t cap = initial_cap ? initial_cap : 128;
  char *buf = malloc(cap);
  if (!buf) return false;

  buf[0] = '\0';
  builder->js = js;
  builder->buf = buf;
  builder->len = cap;
  builder->n = 0;
  builder->growable = true;
  builder->inline_mode = false;
  builder->bare_mode = false;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;
  builder->error_stack = NULL;
  builder->error_stack_len = 0;
  
  return true;
}

void js_inspect_builder_dispose(js_inspect_builder_t *builder) {
  if (builder->growable) free(builder->buf);
  builder->buf = NULL;
  builder->len = 0;
  builder->n = 0;
}

ant_value_t js_inspect_builder_result(js_inspect_builder_t *builder) {
  ant_value_t out = js_mkstr(builder->js, builder->buf ? builder->buf : "", builder->n);
  js_inspect_builder_dispose(builder);
  return out;
}

static inline char *js_inspect_builder_write_ptr(js_inspect_builder_t *builder, size_t *avail) {
  return fixed_buf_write_ptr(builder->buf, builder->len, builder->n, avail);
}

static bool js_inspect_builder_reserve(js_inspect_builder_t *builder, size_t extra) {
  if (!builder->growable) return true;

  size_t needed = builder->n + extra + 1;
  if (needed <= builder->len) return true;

  size_t new_cap = builder->len ? builder->len : 128;
  while (new_cap < needed) new_cap *= 2;

  char *new_buf = realloc(builder->buf, new_cap);
  if (!new_buf) return false;

  builder->buf = new_buf;
  builder->len = new_cap;
  
  return true;
}

static bool js_inspect_append(js_inspect_builder_t *builder, const char *src, size_t srclen) {
  if (builder->growable) {
    if (!js_inspect_builder_reserve(builder, srclen)) return false;
    memcpy(builder->buf + builder->n, src, srclen);
    builder->n += srclen;
    builder->buf[builder->n] = '\0';
    return true;
  }

  size_t avail = 0;
  char *dst = js_inspect_builder_write_ptr(builder, &avail);
  builder->n += cpy(dst, avail, src, srclen);
  
  return true;
}

static bool __attribute__((format(printf, 2, 0)))
js_inspect_vappendf(js_inspect_builder_t *builder, const char *fmt, va_list args) {
  if (builder->growable) {
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) return false;
    if (!js_inspect_builder_reserve(builder, (size_t)needed)) return false;
    vsnprintf(builder->buf + builder->n, builder->len - builder->n, fmt, args);
    builder->n += (size_t)needed;
    return true;
  }

  size_t avail = 0;
  char *dst = js_inspect_builder_write_ptr(builder, &avail);
  int needed = vsnprintf(dst, avail, fmt, args);
  
  if (needed < 0) return false;
  builder->n += (size_t)needed;
  
  return true;
}

static bool js_inspect_append_indent(js_inspect_builder_t *builder, int indent) {
  for (int i = 0; i < indent; i++) if (!js_inspect_append(builder, "  ", 2)) return false;
  return true;
}

static bool js_inspect_append_tostr(js_inspect_builder_t *builder, ant_value_t value) {
  if (!builder->growable) {
    size_t avail = 0;
    char *dst = js_inspect_builder_write_ptr(builder, &avail);
    builder->n += tostr(builder->js, value, dst, avail);
    return true;
  }

  size_t cap = 128;
  char *tmp = malloc(cap);
  if (!tmp) return false;

  for (;;) {
    size_t written = tostr(builder->js, value, tmp, cap);
    if (written < cap) {
      bool ok = js_inspect_append(builder, tmp, written);
      free(tmp);
      return ok;
    }
    
    size_t new_cap = written + 1;
    char *new_tmp = realloc(tmp, new_cap);
    if (!new_tmp) {
      free(tmp);
      return false;
    }
    tmp = new_tmp;
    cap = new_cap;
  }
}

static bool js_inspect_append_key_interned(js_inspect_builder_t *builder, const char *key, size_t klen) {
  if (!builder->growable) {
    size_t avail = 0;
    char *dst = js_inspect_builder_write_ptr(builder, &avail);
    builder->n += strkey_interned(builder->js, key, klen, dst, avail);
    return true;
  }

  size_t cap = klen + 16;
  char *tmp = malloc(cap);
  if (!tmp) return false;

  for (;;) {
    size_t written = strkey_interned(builder->js, key, klen, tmp, cap);
    if (written < cap) {
      bool ok = js_inspect_append(builder, tmp, written);
      free(tmp);
      return ok;
    }
    
    size_t new_cap = written + 1;
    char *new_tmp = realloc(tmp, new_cap);
    if (!new_tmp) {
      free(tmp);
      return false;
    }
    tmp = new_tmp;
    cap = new_cap;
  }
}

static bool __attribute__((format(printf, 3, 0)))
js_inspect_vheader_for(js_inspect_builder_t *builder, ant_value_t obj, const char *fmt, va_list args) {
  bool ok = js_inspect_vappendf(builder, fmt, args);
  if (!ok) return false;

  if (is_object_type(obj)) {
  int prop_count = 0;
  bool inline_mode = is_small_object(builder->js, obj, &prop_count);
  
  if (prop_count == 0) {
    if (!js_inspect_append(builder, " {}", 3)) return false;
    builder->inline_mode = false;
    builder->first = true;
    builder->closed = true;
    builder->did_indent = false;
    return true;
  }
  
  if (inline_mode) {
    if (!js_inspect_append(builder, " { ", 3)) return false;
    builder->inline_mode = true;
    builder->first = true;
    builder->closed = false;
    builder->did_indent = false;
    return true;
  }}

  if (!js_inspect_append(builder, " {\n", 3)) return false;

  builder->inline_mode = false;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;

  return true;
}

bool js_inspect_header_for(js_inspect_builder_t *builder, ant_value_t obj, const char *fmt, ...) {
  va_list args; va_start(args, fmt);
  bool ok = js_inspect_vheader_for(builder, obj, fmt, args);
  va_end(args);
  return ok;
}

bool js_inspect_header(js_inspect_builder_t *builder, const char *fmt, ...) {
  va_list args; va_start(args, fmt);
  bool ok = js_inspect_vheader_for(builder, js_mkundef(), fmt, args);
  va_end(args);
  return ok;
}

bool js_inspect_tagged_header(js_inspect_builder_t *builder, const char *tag, size_t tag_len) {
  if (!js_inspect_append(builder, "Object [", 8)) return false;
  if (!js_inspect_append(builder, tag, tag_len)) return false;
  if (!js_inspect_append(builder, "] {\n", 4)) return false;

  builder->inline_mode = false;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;
  
  return true;
}

// TODO: modularize
bool js_inspect_plain_header(js_inspect_builder_t *builder, ant_value_t obj) {
  ant_t *js = builder->js;
  int prop_count = 0;
  bool inline_mode = is_small_object(js, obj, &prop_count);

  ant_value_t proto_val = js_get_proto(js, obj);
  bool is_null_proto = (vtype(proto_val) == kTypeNull);
  bool proto_is_null_proto = false;
  const char *class_name = NULL;
  ant_offset_t class_name_len = 0;

  do {
    if (is_null_proto) break;
    uint8_t pt = vtype(proto_val);
    if (pt != kTypeObject && pt != kTypeFunction) break;
    
    ant_value_t proto_proto = js_get_proto(js, proto_val);
    ant_value_t object_proto = js->sym.object_proto;
    proto_is_null_proto = (vtype(proto_proto) == kTypeNull) && (vdata(proto_val) != vdata(object_proto));
    
    class_name = get_class_name(js, obj, &class_name_len, "Object");
  } while (0);

  if (prop_count == 0) {
    if (is_null_proto) {
      if (!js_inspect_append(builder, "[Object: null prototype] {}", 27)) return false;
    } else if (class_name && class_name_len > 0) {
      if (!js_inspect_append(builder, class_name, class_name_len)) return false;
      if (proto_is_null_proto) {
        if (!js_inspect_append(builder, " <[Object: null prototype] {}> {}", 33)) return false;
      } else if (!js_inspect_append(builder, " {}", 3)) return false;
    } else if (proto_is_null_proto) {
      if (!js_inspect_append(builder, "<[Object: null prototype] {}> {}", 32)) return false;
    } else if (!js_inspect_append(builder, "{}", 2)) return false;

    builder->closed = true;
    return true;
  }

  if (is_null_proto) {
    if (!js_inspect_append(builder, "[Object: null prototype] ", 25)) return false;
  } else if (class_name && class_name_len > 0) {
    if (!js_inspect_append(builder, class_name, class_name_len)) return false;
    if (proto_is_null_proto) {
      if (!js_inspect_append(builder, " <[Object: null prototype] {}> ", 31)) return false;
    } else if (!js_inspect_append(builder, " ", 1)) return false;
  } else if (proto_is_null_proto) {
    if (!js_inspect_append(builder, "<[Object: null prototype] {}> ", 30)) return false;
  }

  if (!js_inspect_append(builder, inline_mode ? "{ " : "{\n", 2)) return false;
  builder->inline_mode = inline_mode;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;
  
  return true;
}

static bool js_inspect_property_prefix(js_inspect_builder_t *builder) {
  if (builder->bare_mode) return js_inspect_append(builder, "\n ", 2);
  if (!builder->first && !js_inspect_append(builder, builder->inline_mode ? ", " : ",\n", 2)) return false;
  builder->first = false;
  return builder->inline_mode || js_inspect_append_indent(builder, builder->js->stringify.indent);
}

static void js_inspect_begin_body(js_inspect_builder_t *builder) {
  if (!builder->bare_mode && !builder->inline_mode && !builder->did_indent) {
    builder->js->stringify.indent++;
    builder->did_indent = true;
  }
}

bool js_inspect_field(js_inspect_builder_t *builder, const char *key, ant_value_t value) {
  if (builder->closed) return true;
  js_inspect_begin_body(builder);
  
  if (!js_inspect_property_prefix(builder)) return false;
  if (!js_inspect_append(builder, key, strlen(key))) return false;
  if (!js_inspect_append(builder, ": ", 2)) return false;
  
  return js_inspect_append_tostr(builder, value);
}

static bool js_inspect_contains(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
  if (needle_len == 0) return true;
  for (size_t i = 0; i + needle_len <= hay_len; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return true;
  return false;
}

static bool js_inspect_error_key_in_stack(js_inspect_builder_t *builder, const char *key, size_t klen, ant_value_t val) {
  if (!builder->error_stack || vtype(val) != kTypeString) return false;
  if (!streq(key, klen, "name", 4) && !streq(key, klen, "message", 7) && !streq(key, klen, "stack", 5)) return false;
  size_t len = 0;
  const char *text = js_getstr(builder->js, val, &len);
  return text && js_inspect_contains(builder->error_stack, builder->error_stack_len, text, len);
}

bool js_inspect_object_body(js_inspect_builder_t *builder, ant_value_t obj) {
  if (builder->closed) return true;
  js_inspect_begin_body(builder);

  ant_t *js = builder->js;
  ant_value_t tag_sym = js->sym.toStringTag_sym;
  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  
  uintptr_t obj_off = (uintptr_t)vdata(as_obj);
  uint32_t shape_count = (ptr && ptr->shape) ? ant_shape_count(ptr->shape) : 0;

  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;
    if ((ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;
    ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();
    
    if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
      ant_offset_t sym_off = prop->key.sym_off;
      if (vtype(tag_sym) == kTypeSymbol && sym_off == (ant_offset_t)vdata(tag_sym)) continue;
      
      if (ptr && ptr->flags.is_exotic) {
        prop_meta_t meta;
        if (lookup_symbol_prop_meta(js, as_obj, sym_off, &meta) && !meta.enumerable) continue;
      }
      
      ant_value_t sym = mkval(kTypeSymbol, sym_off);
      
      if (!js_inspect_property_prefix(builder)) return false;
      if (!js_inspect_append(builder, "[", 1)) return false;
      if (!js_inspect_append_tostr(builder, sym)) return false;
      if (!js_inspect_append(builder, "]: ", 3)) return false;
      if (!js_inspect_append_tostr(builder, val)) return false;
      continue;
    }

    const char *key = prop->key.interned;
    ant_offset_t klen = (ant_offset_t)intern_length(key);
    if (ptr && ptr->flags.is_exotic) {
      prop_meta_t meta;
      if (lookup_string_prop_meta(js, as_obj, key, (size_t)klen, &meta) && !meta.enumerable) continue;
    }

    if (prop->has_getter || prop->has_setter) {
      if (!js_inspect_property_prefix(builder)) return false;
      if (!js_inspect_append_key_interned(builder, key, (size_t)klen)) return false;
      if (!js_inspect_append(builder, ": ", 2)) return false;
      if (prop->has_getter && prop->has_setter) {
        if (!js_inspect_append(builder, "[Getter/Setter]", 15)) return false;
      } else if (prop->has_getter) {
        if (!js_inspect_append(builder, "[Getter]", 8)) return false;
      } else if (!js_inspect_append(builder, "[Setter]", 8)) return false;
      continue;
    }

    if (js_inspect_error_key_in_stack(builder, key, (size_t)klen, val)) continue;
    if (!js_inspect_property_prefix(builder)) return false;

    bool is_special_global = false;
    if (vtype(val) == kTypeUndefined && streq(key, klen, "undefined", 9)) {
      is_special_global = true;
    } else if (vtype(val) == kTypeNumber) {
      double d = tod(val);
      if (isinf(d) && d > 0 && streq(key, klen, "Infinity", 8)) {
        is_special_global = true;
      } else if (isnan(d) && streq(key, klen, "NaN", 3)) {
        is_special_global = true;
      }
    }

    if (is_special_global) {
      if (!js_inspect_append_tostr(builder, val)) return false;
    } else {
      if (!js_inspect_append_key_interned(builder, key, (size_t)klen)) return false;
      if (!js_inspect_append(builder, ": ", 2)) return false;
      if (!js_inspect_append_tostr(builder, val)) return false;
    }
  }

  if (ptr && ptr->flags.is_exotic) {
    descriptor_entry_t *desc, *tmp;
    HASH_ITER(hh, js->desc_registry, desc, tmp) {
      if (desc->obj_off != obj_off) continue;
      if (!desc->enumerable) continue;
      if (!desc->has_getter && !desc->has_setter) continue;

      if (!js_inspect_property_prefix(builder)) return false;
      if (!js_inspect_append(builder, desc->prop_name, desc->prop_len)) return false;
      if (!js_inspect_append(builder, ": ", 2)) return false;

      if (desc->has_getter && desc->has_setter) {
        if (!js_inspect_append(builder, "[Getter/Setter]", 15)) return false;
      } else if (desc->has_getter) {
        if (!js_inspect_append(builder, "[Getter]", 8)) return false;
      } else if (!js_inspect_append(builder, "[Setter]", 8)) return false;
    }
  }

  return true;
}

bool js_inspect_close(js_inspect_builder_t *builder) {
  if (builder->closed) return true;

  if (builder->did_indent) {
    builder->js->stringify.indent--;
    builder->did_indent = false;
  }

  if (builder->inline_mode) {
    if (!js_inspect_append(builder, " }", 2)) return false;
  } else {
    if (!builder->first && !js_inspect_append(builder, "\n", 1)) return false;
    if (!js_inspect_append_indent(builder, builder->js->stringify.indent)) return false;
    if (!js_inspect_append(builder, "}", 1)) return false;
  }

  builder->closed = true;
  return true;
}

static ant_value_t js_inspect_error_to_string(ant_t *js, ant_value_t obj) {
  ant_value_t text = js_error_header_text(js, obj);
  if (!is_err(text)) return text;
  js_take_thrown(js, text);
  return js_mkstr(js, "Error", 5);
}

static bool js_inspect_append_error_stack(js_inspect_builder_t *builder, ant_value_t obj, const char *stack, size_t stack_len) {
  ant_t *js = builder->js;
  size_t skip = 0;

  ant_offset_t class_len = 0;
  const char *class_name = get_class_name(js, obj, &class_len, NULL);
  ant_value_t name_val = js_getprop_fallback_len(js, obj, "name", 4);
  if (is_err(name_val)) name_val = js_take_thrown(js, name_val);

  size_t name_len = 0;
  const char *name = vtype(name_val) == kTypeString ? js_getstr(js, name_val, &name_len) : NULL;

  bool improve = 
    class_name && name && name_len >= 5 &&
    memcmp(name + name_len - 5, "Error", 5) == 0 &&
    stack_len >= name_len && memcmp(stack, name, name_len) == 0 &&
    (stack_len == name_len || stack[name_len] == ':' || stack[name_len] == '\n') &&
    !((size_t)class_len == name_len && memcmp(class_name, name, name_len) == 0);

  bool bracket = !js_inspect_contains(stack, stack_len, "\n    at", 7);
  if (bracket && !js_inspect_append(builder, "[", 1)) return false;

  if (improve) {
    if (!js_inspect_append(builder, class_name, (size_t)class_len)) return false;
    if (!js_inspect_contains(class_name, (size_t)class_len, name, name_len)) {
      if (!js_inspect_append(builder, " [", 2)) return false;
      if (!js_inspect_append(builder, name, name_len)) return false;
      if (!js_inspect_append(builder, "]", 1)) return false;
    }
    skip = name_len;
  }

  int indent = js->stringify.indent;
  for (const char *p = stack + skip, *end = stack + stack_len; p < end;) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t run = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
    if (!js_inspect_append(builder, p, run)) return false;
    if (nl && !js_inspect_append_indent(builder, indent)) return false;
    p += run;
  }

  return !bracket || js_inspect_append(builder, "]", 1);
}

static bool js_inspect_hidden_own(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t *out) {
  prop_meta_t meta;
  if (!lookup_string_prop_meta(js, js_as_obj(obj), key, klen, &meta) || meta.enumerable) return false;
  return js_try_get_own_data_prop(js, obj, key, klen, out);
}

bool js_inspect_error(js_inspect_builder_t *builder, ant_value_t obj) {
  ant_t *js = builder->js;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t stack = js_getprop_fallback_len(js, obj, "stack", 5);
  if (is_err(stack)) stack = js_take_thrown(js, stack);
  if (vtype(stack) != kTypeString) stack = js_inspect_error_to_string(js, obj);
  GC_ROOT_PIN(js, stack);

  size_t stack_len = 0;
  const char *stack_text = js_getstr(js, stack, &stack_len);
  bool ok = stack_text && js_inspect_append_error_stack(builder, obj, stack_text, stack_len);

  size_t mark = builder->n;
  if (ok) ok = js_inspect_append(builder, " {\n", 3);
  builder->inline_mode = false;
  builder->first = true;
  builder->closed = false;
  builder->did_indent = false;
  builder->error_stack = stack_text;
  builder->error_stack_len = stack_len;

  if (ok) ok = js_inspect_object_body(builder, obj);

  ant_value_t hidden;
  if (ok && js_inspect_hidden_own(js, obj, "cause", 5, &hidden)) ok = js_inspect_field(builder, "[cause]", hidden);
  if (ok && js_inspect_hidden_own(js, obj, "errors", 6, &hidden)) ok = js_inspect_field(builder, "[errors]", hidden);

  builder->error_stack = NULL;
  builder->error_stack_len = 0;

  if (ok && builder->first) {
    if (builder->did_indent) js->stringify.indent--;
    builder->did_indent = false;
    builder->closed = true;
    builder->n = mark;
    if (mark < builder->len) builder->buf[mark] = '\0';
  } else if (ok) ok = js_inspect_close(builder);

  GC_ROOT_RESTORE(js, root_mark);
  return ok;
}
