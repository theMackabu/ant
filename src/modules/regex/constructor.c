#include "ant/values.h"
#include "regex_internal.h"

static bool regexp_init_last_index(ant_t *js, ant_value_t value) {
  ant_object_t *obj = js_obj_ptr(value);
  
  if (!js->regex_state || !obj || obj->prop_count != 0) return false;
  regexp_instance_shape_t *cache = &js->regex_state->instance_shape;
  
  if (!cache->shape) {
    if (ant_shape_count(obj->shape) != 0) return false;
    ant_shape_t *shape = ant_shape_clone(obj->shape);
    const char *key = intern_string(js, "lastIndex", 9);
    uint32_t slot;
    
    if (!shape) return false;
    
    if (!key || !ant_shape_add_interned(js, shape, key, ANT_PROP_ATTR_WRITABLE, &slot)) {
      ant_shape_release(shape);
      return false;
    }
    
    ant_shape_retain(obj->shape);
    *cache = (regexp_instance_shape_t){.base = obj->shape, .shape = shape};
  }
  
  if (obj->shape != cache->base || !js_obj_ensure_prop_capacity(js, obj, 1)) return false;
  ant_shape_transition_existing(&obj->shape, cache->shape);
  ant_object_invalidate_guarded_absence(js, obj);
  ant_object_prop_set_unchecked(obj, REGEXP_LAST_INDEX_SLOT, Ant_Value_FromNumber(0));
  
  return true;
}

void regexp_init_flags(ant_t *js, ant_value_t obj, ant_value_t flags, bool is_new) {
  ant_offset_t flen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  uint8_t mask = regexp_parse_flags_mask(fstr, flen);

  char sorted[REGEXP_FLAG_COUNT]; int si = 0;
#define REGEXP_FLAG(id, name, ch) if (mask & REGEXP_FLAG_##id) sorted[si++] = ch;
#include "modules/regex_flags.h"

  ant_value_t flags_value = flen == (ant_offset_t)si && memcmp(fstr, sorted, si) == 0
    ? flags : js_mkstr(js, sorted, si);
  if (!is_new) setprop_cstr(js, obj, "lastIndex", 9, Ant_Value_FromNumber(0));
  else if (!regexp_init_last_index(js, obj)) mkprop_exact_attrs(js, obj, Ant_String_FromLiteral(js, "lastIndex"), Ant_Value_FromNumber(0), ANT_PROP_ATTR_WRITABLE);
  if (is_new) {
    unsigned count = ant_object_extra_count(js_obj_ptr(obj));
    if (count <= UINT8_MAX - 3) (void)js_reserve_slots(obj, (uint8_t)(count + 3));
  }
  js_set_slot(obj, SLOT_REGEXP_FLAGS_MASK, Ant_Value_FromNumber((double)mask));
  js_set_slot_wb(js, obj, SLOT_REGEXP_FLAGS_STRING, flags_value);
  js_set_slot(obj, SLOT_REGEXP_NAMED_GROUPS, Ant_Value_Undefined);
}

ant_value_t is_regexp_like(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return Ant_Value_False;

  ant_value_t match_sym = js->sym.match_sym;
  if (vtype(match_sym) == kTypeSymbol) {
    ant_value_t match_val = js_get_sym(js, value, match_sym);
    if (is_err(match_val)) return match_val;
    if (vtype(match_val) != kTypeUndefined) return Ant_Value_FromBool(js_truthy(js, match_val));
  }

  return Ant_Value_FromBool(proto_chain_contains(js, value, js->builtins.regexp_proto));
}

ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name) {
  ant_value_t is_re = is_regexp_like(js, value);
  if (is_err(is_re)) return is_re;
  if (js_truthy(js, is_re))
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument to String.prototype.%s must not be a regular expression", method_name);
  return Ant_Value_Undefined;
}

ant_value_t regexp_species_ctor(ant_t *js, ant_value_t rx, const char *method) {
  ant_value_t ctor = js_get(js, rx, "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t C = js->builtins.regexp_ctor_value;
  if (vtype(ctor) != kTypeUndefined) {
    if (!is_object_type(ctor))
      return js_mkerr_typed(js, JS_ERR_TYPE, "%s: constructor is not an object", method);
    ant_value_t species = get_ctor_species_value(js, ctor);
    if (is_err(species)) return species;
    if (vtype(species) != kTypeUndefined && vtype(species) != kTypeNull) C = species;
  }

  if (vtype(C) != kTypeFunction && vtype(C) != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species is not a constructor");
  return C;
}

ant_value_t regexp_species_construct(ant_t *js, ant_value_t rx, ant_value_t ctor, ant_value_t *ctor_args, int nargs) {
  if (vtype(ctor) == kTypeObject && is_proxy(ctor))
    return js_proxy_construct(js, ctor, ctor_args, nargs, ctor);

  ant_value_t new_target = ctor;
  ant_value_t proto = sv_prepare_construct_meta(js, ctor, ctor, &new_target);
  if (is_err(proto)) return proto;

  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_this = seed;
  ant_value_t result = sv_vm_call(js->vm, js, ctor, seed, ctor_args, nargs, &ctor_this, new_target);

  if (is_err(result)) return result;
  if (!is_object_type(result)) result = is_object_type(ctor_this) ? ctor_this : seed;
  return result;
}

bool regexp_has_internal_slots(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return false;
  ant_object_t *obj = js_obj_ptr(value);
  return obj && obj->flags.regexp_brand;
}

static ant_value_t regexp_check_flags(ant_t *js, ant_value_t flags) {
  ant_offset_t flen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  if (regexp_flags_valid(fstr, flen)) return Ant_Value_Undefined;
  return js_mkerr_typed(
    js, JS_ERR_SYNTAX, "Invalid flags supplied to RegExp constructor '%.*s'", (int)flen, fstr
  );
}

ant_value_t regexp_syntax_error_value(
  ant_t *js, const char *pattern, size_t pattern_len, uint8_t flags_mask, const char *message
) {
  char sorted[REGEXP_FLAG_COUNT]; int si = 0;
#define REGEXP_FLAG(id, name, ch) if (flags_mask & REGEXP_FLAG_##id) sorted[si++] = ch;
#include "modules/regex_flags.h"
  return js_mkerr_typed(
    js, JS_ERR_SYNTAX, "Invalid regular expression: /%.*s/%.*s: %s",
    (int)pattern_len, pattern, si, sorted, message
  );
}

static ant_value_t regexp_check_pattern(ant_t *js, ant_value_t pattern, ant_value_t flags) {
  ant_offset_t flen, plen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  uint8_t mask = regexp_parse_flags_mask(fstr, flen);
  ant_regex_state_t *state = js->regex_state;
  uint64_t epoch = gc_strings_sweep_epoch();
  regexp_validated_entry_t *validated = NULL;
  if (state && vtype(pattern) == kTypeString) {
    const regexp_site_entry_t *site = regexp_site_for(state, pattern);
    if (site->pattern == pattern && site->epoch == epoch && site->entry->flags_mask == mask) return Ant_Value_Undefined;
    validated = &state->validated[(vdata(pattern) >> 4) % REGEXP_SITE_CACHE_SIZE];
    if (validated->pattern == pattern && validated->epoch == epoch && validated->flags_mask == mask) return Ant_Value_Undefined;
  }

  const char *src = (const char *)(uintptr_t)vstr(js, pattern, &plen);
  const char *error = regexp_pattern_syntax_error(src, (size_t)plen, mask);
  if (error) return regexp_syntax_error_value(js, src, (size_t)plen, mask, error);
  if (validated) *validated = (regexp_validated_entry_t){ .pattern = pattern, .epoch = epoch, .flags_mask = mask };
  return Ant_Value_Undefined;
}

static ant_value_t regexp_escape_source(ant_t *js, ant_value_t pattern) {
  size_t len = 0;
  const char *src = js_getstr(js, pattern, &len);
  if (!src) return pattern;
  if (len == 0) return Ant_String_FromLiteral(js, "(?:)");

  bool needs_escape = false;
  for (size_t i = 0; i < len && !needs_escape; i++) {
    unsigned char c = (unsigned char)src[i];
    needs_escape = c == '/' || c == '\n' || c == '\r' || c == 0xe2;
  }
  if (!needs_escape) return pattern;

  char *out = malloc(len * 2);
  if (!out) return Ant_Error_CreateOutOfMemory(js);
  size_t n = 0;
  bool in_class = false;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' && i + 1 < len) {
      out[n++] = src[i++];
      out[n++] = src[i];
    } else if (c == '/' && !in_class) {
      out[n++] = '\\';
      out[n++] = '/';
    } else if (c == '\n' || c == '\r') {
      out[n++] = '\\';
      out[n++] = c == '\n' ? 'n' : 'r';
    } else if (c == 0xe2 && i + 2 < len && (unsigned char)src[i + 1] == 0x80 &&
               ((unsigned char)src[i + 2] == 0xa8 || (unsigned char)src[i + 2] == 0xa9)) {
      memcpy(out + n, (unsigned char)src[i + 2] == 0xa8 ? "\\u2028" : "\\u2029", 6);
      n += 6;
      i += 2;
    } else {
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      out[n++] = (char)c;
    }
  }

  ant_value_t escaped = js_mkstr(js, out, n);
  free(out);
  return escaped;
}

ant_value_t builtin_RegExp(ant_params_t) {
  bool pattern_is_regexp = false;
  if (nargs > 0) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    pattern_is_regexp = js_truthy(js, is_re);
  }

  if (vtype(call_new_target) == kTypeUndefined && nargs > 0 && pattern_is_regexp) {
    if (nargs < 2 || vtype(args[1]) == kTypeUndefined) {
      ant_value_t ctor = js_getprop_fallback(js, args[0], "constructor");
      if (is_err(ctor)) return ctor;
      if (same_ctor_identity(js, ctor, js->builtins.regexp_ctor_value)) return args[0];
    }
  }

  ant_value_t regexp_obj = call_this;
  bool use_this = (vtype(call_new_target) != kTypeUndefined && vtype(regexp_obj) == kTypeObject);

  if (!use_this) {
    regexp_obj = mkobj(js, 0);
    if (is_err(regexp_obj)) return regexp_obj;
  }

  ant_value_t regexp_proto = js->builtins.regexp_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, regexp_proto, call_new_target);

  if (is_object_type(instance_proto)) js_set_proto_init(regexp_obj, instance_proto);
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin) {
    js_set_slot(regexp_obj, SLOT_CTOR, call_new_target);
  }

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (regexp_has_internal_slots(js, args[0])) {
      ant_value_t original = js_get_slot(args[0], SLOT_DATA);
      if (vtype(original) == kTypeString) pattern = original;
      if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) flags = js_tostring_val(js, args[1]);
      else {
        ant_value_t original_flags = regexp_flags_value(js, args[0]);
        if (vtype(original_flags) == kTypeString) flags = original_flags;
      }
      if (is_err(flags)) return flags;
    } else if (pattern_is_regexp) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
        flags = js_tostring_val(js, args[1]);
      } else {
        ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
        if (is_err(fl)) return fl;
        flags = js_tostring_val(js, fl);
      }
      if (is_err(flags)) return flags;
    } else {
      if (vtype(args[0]) == kTypeString) pattern = args[0];
      else if (vtype(args[0]) != kTypeUndefined) {
        ant_value_t s = js_tostring_val(js, args[0]);
        if (is_err(s)) return s;
        pattern = s;
      }
      if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
        flags = vtype(args[1]) == kTypeString ? args[1] : js_tostring_val(js, args[1]);
        if (is_err(flags)) return flags;
      }
    }
  }

  ant_value_t flags_error = regexp_check_flags(js, flags);
  if (is_err(flags_error)) return flags_error;

  ant_value_t pattern_error = regexp_check_pattern(js, pattern, flags);
  if (is_err(pattern_error)) return pattern_error;
  
  js_set_slot_wb(js, regexp_obj, SLOT_DATA, pattern);
  regexp_init_flags(js, regexp_obj, flags, true);

  return regexp_obj;
}

ant_value_t builtin_regexp_toString(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString called on non-object");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, call_this);

  ant_value_t source_val = js_getprop_fallback(js, call_this, "source");
  if (is_err(source_val)) {
    GC_ROOT_RESTORE(js, root_mark);
    return source_val;
  }
  
  ant_value_t source_str = js_tostring_val(js, source_val);
  GC_ROOT_PIN(js, source_str);
  if (is_err(source_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return source_str;
  }

  ant_value_t flags_val = js_getprop_fallback(js, call_this, "flags");
  if (is_err(flags_val)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flags_val;
  }
  
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  GC_ROOT_PIN(js, flags_str);
  if (is_err(flags_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flags_str;
  }

  ant_offset_t src_len, src_off = vstr(js, source_str, &src_len);
  ant_offset_t fl_len, fl_off = vstr(js, flags_str, &fl_len);

  if ((size_t)fl_len > SIZE_MAX - 2 ||
      (size_t)src_len > SIZE_MAX - (size_t)fl_len - 2) {
    ant_value_t err = js_mkerr(js, "string too large");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }
  
  size_t total = (size_t)src_len + (size_t)fl_len + 2;
  ant_value_t result = js_mkstr(js, NULL, total);
  GC_ROOT_PIN(js, result);
  if (is_err(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_flat_string_t *flat = (ant_flat_string_t *)vptr(result);
  src_off = vstr(js, source_str, &src_len);
  fl_off = vstr(js, flags_str, &fl_len);
  
  size_t n = 0;
  flat->bytes[n++] = '/';
  memcpy(flat->bytes + n, (const void *)(uintptr_t)src_off, (size_t)src_len);
  
  n += (size_t)src_len;
  flat->bytes[n++] = '/';
  memcpy(flat->bytes + n, (const void *)(uintptr_t)fl_off, (size_t)fl_len);
  
  n += (size_t)fl_len;
  flat->bytes[n] = '\0';
  str_flat_init_meta(flat, str_detect_ascii_bytes(flat->bytes, n));

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

ant_value_t builtin_regexp_compile(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "compile called on non-object");

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);

  bool pattern_has_slots = nargs > 0 && regexp_has_internal_slots(js, args[0]);
  if (pattern_has_slots) {
    if (nargs > 1 && vtype(args[1]) != kTypeUndefined)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot supply flags when constructing one RegExp from another");
    ant_value_t original = js_get_slot(args[0], SLOT_DATA);
    ant_value_t original_flags = regexp_flags_value(js, args[0]);
    if (vtype(original) == kTypeString) pattern = original;
    if (vtype(original_flags) == kTypeString) flags = original_flags;
  } else if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    pattern = js_tostring_val(js, args[0]);
    if (is_err(pattern)) return pattern;
  }
  if (!pattern_has_slots && nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    flags = js_tostring_val(js, args[1]);
    if (is_err(flags)) return flags;
  }

  ant_value_t flags_error = regexp_check_flags(js, flags);
  if (is_err(flags_error)) return flags_error;
  
  ant_value_t pattern_error = regexp_check_pattern(js, pattern, flags);
  if (is_err(pattern_error)) return pattern_error;
  
  js_set_slot_wb(js, call_this, SLOT_DATA, pattern);  
  regexp_init_flags(js, call_this, flags, false);
  regexp_object_compiled_detach(js, call_this);

  return call_this;
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

ant_value_t builtin_regexp_escape(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.escape requires a string argument");

  ant_offset_t slen, soff = vstr(js, args[0], &slen);
  const char *src = (const char *)(uintptr_t)(soff);
  if ((size_t)slen > (SIZE_MAX - 1) / 6) return js_mkerr(js, "string too large");
  
  size_t capacity = (size_t)slen * 6 + 1;
  char static_buf[256];
  
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)calloc(1, capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  static const char hex[] = "0123456789abcdef";
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
      if (bytes <= 0) bytes = 1;
      if ((size_t)bytes > (size_t)slen - si) bytes = (int)((size_t)slen - si);
      memcpy(buf + di, src + si, (size_t)bytes);
      di += (size_t)bytes;
      si += (size_t)bytes;
      first = false;
      continue;
    }

    bool short_escape = c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    bool hex_escape = 
      (first && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) 
      || is_other_punctuator(c) || c == ' ';

    if (short_escape) {
      buf[di++] = '\\';
      switch (c) {
        case '\t': buf[di++] = 't'; break;
        case '\n': buf[di++] = 'n'; break;
        case '\v': buf[di++] = 'v'; break;
        case '\f': buf[di++] = 'f'; break;
        default:   buf[di++] = 'r'; break;
      }
    } else if (hex_escape) {
      buf[di++] = '\\';
      buf[di++] = 'x';
      buf[di++] = hex[c >> 4];
      buf[di++] = hex[c & 0x0F];
    } else if (is_syntax_char(c)) {
      buf[di++] = '\\';
      buf[di++] = (char)c;
    } else buf[di++] = (char)c;

    si++;
    first = false;
  }

  ant_value_t result = js_mkstr(js, buf, di);
  if (needs_free) free(buf);
  return result;
}

static inline bool regexp_is_proto(ant_t *js, ant_value_t value) {
  return is_object_type(value) && vdata(js_as_obj(value)) == vdata(js_as_obj(js->builtins.regexp_proto));
}

static ant_value_t regexp_flag_getter(ant_t *js, ant_value_t rx, uint8_t bit, const char *name) {
  if (regexp_has_internal_slots(js, rx)) return Ant_Value_FromBool((regexp_flags_mask(js, rx, NULL) & bit) != 0);
  if (regexp_is_proto(js, rx)) return Ant_Value_Undefined;
  return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.%s getter called on incompatible receiver", name);
}
#define REGEXP_FLAG(id, name, ch)                                       \
  ant_value_t builtin_regexp_get_##name(ant_params_t) {                 \
    return regexp_flag_getter(js, call_this, REGEXP_FLAG_##id, #name); \
  }
#include "modules/regex_flags.h"

typedef struct {
  const char *name;
  uint8_t len;
  uint8_t bit;
  char ch;
  ant_cfunc_t getter;
} regexp_flag_info_t;

static const regexp_flag_info_t regexp_flag_info[REGEXP_FLAG_COUNT] = {
#define REGEXP_FLAG(id, name, ch) { #name, sizeof(#name) - 1, REGEXP_FLAG_##id, ch, builtin_regexp_get_##name },
#include "modules/regex_flags.h"
};

static bool regexp_flag_getter_is_builtin(ant_t *js, const ant_shape_t *own_shape, const regexp_flag_info_t *flag) {
  const char *key = intern_string(js, flag->name, flag->len);
  if (!key || (own_shape && ant_shape_lookup_interned(own_shape, key) >= 0)) return false;
  
  ant_object_t *proto = js_obj_ptr(js->builtins.regexp_proto);
  if (!proto || !proto->shape || proto->flags.is_exotic) return false;
  int32_t slot = ant_shape_lookup_interned(proto->shape, key);
  
  const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(proto->shape, (uint32_t)slot) : NULL;
  return prop && prop->has_getter && js_cfunc_same_entrypoint(prop->getter, flag->getter);
}

bool regexp_shape_flags_pristine(ant_t *js, const ant_shape_t *own_shape) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return false;
  if (
    state->pristine_flags_epoch == js->ic.epoch &&
    state->pristine_flags_shape == own_shape &&
    state->pristine_flags_realm == js->realm
  ) return true;
  
  for (int i = 0; i < REGEXP_FLAG_COUNT; i++)
    if (!regexp_flag_getter_is_builtin(js, own_shape, &regexp_flag_info[i])) return false;
  
  state->pristine_flags_epoch = js->ic.epoch;
  state->pristine_flags_shape = own_shape;
  state->pristine_flags_realm = js->realm;
  return true;
}

static bool regexp_flags_pristine(ant_t *js, ant_value_t rx) {
  if (!regexp_has_internal_slots(js, rx)) return false;
  ant_object_t *obj = js_obj_ptr(rx);
  return 
    !obj->flags.is_exotic && 
    js_get_proto(js, rx) == js->builtins.regexp_proto &&
    regexp_shape_flags_pristine(js, obj->shape);
}

ant_value_t regexp_read_flag(ant_t *js, ant_value_t rx, int index) {
  const regexp_flag_info_t *flag = &regexp_flag_info[index];
  if (regexp_flags_pristine(js, rx)) return Ant_Value_FromBool((regexp_flags_mask(js, rx, NULL) & flag->bit) != 0);
  return js_getprop_fallback(js, rx, flag->name);
}

ant_value_t regexp_source_value(ant_t *js, ant_value_t rx) {
  ant_value_t pattern = js_get_slot(rx, SLOT_DATA);
  return vtype(pattern) == kTypeString ? regexp_escape_source(js, pattern) : Ant_String_FromLiteral(js, "(?:)");
}

ant_value_t builtin_regexp_source_getter(ant_params_t) {
  if (regexp_has_internal_slots(js, call_this)) return regexp_source_value(js, call_this);
  if (regexp_is_proto(js, call_this)) return Ant_String_FromLiteral(js, "(?:)");
  return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.source getter called on incompatible receiver");
}

ant_value_t builtin_regexp_flags_getter(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.flags called on non-object");
  if (regexp_flags_pristine(js, call_this)) return regexp_flags_value(js, call_this);

  char buf[REGEXP_FLAG_COUNT]; int n = 0;
  for (int i = 0; i < REGEXP_FLAG_COUNT; i++) {
    ant_value_t v = regexp_read_flag(js, call_this, i);
    if (is_err(v)) return v;
    if (js_truthy(js, v)) buf[n++] = regexp_flag_info[i].ch;
  }

  return js_mkstr(js, buf, n);
}
