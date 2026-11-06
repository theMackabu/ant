#include <compat.h> // IWYU pragma: keep
#include <stdio.h>
#include <string.h>

#include "cfunc.h"
#include "cfunc.h"
#include "props.h"
#include "silver/engine.h"

void js_set_func_source(ant_t *js, ant_value_t fn, const char *code, size_t len, js_func_source_t source) {
  ant_value_t text;
  
  if (source == JS_FUNC_SOURCE_UNIT) {
    text = js_mkstr(js, code, len);
    if (is_err(text)) return;
  } else {
    if (source == JS_FUNC_SOURCE_COPY || !ant_cage_contains(code)) code = code_arena_alloc(js, code, len);
    if (!code || !ant_cage_contains(code)) return;
    text = mkref(kTypeSourceCode, code);
  }
  
  js_set_slot_wb(js, fn, SLOT_CODE, text);
  js_set_slot(fn, SLOT_CODE_LEN, Ant_Value_FromNumber((double)len));
}

ant_value_t setup_func_prototype(ant_t *js, ant_value_t func) {
  return setup_func_prototype_property(js, func, true);
}

ant_value_t js_make_data_cfunc(ant_t *js, ant_value_t data, ant_cfunc_t fn) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, data);

  ant_value_t obj = mkobj(js, 0);
  if (is_err(obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return obj;
  }

  GC_ROOT_PIN(js, obj);
  obj_extra_reserve(js_obj_ptr(obj), 3);
  js_set_slot(obj, SLOT_DATA, data);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(fn));

  ant_value_t func = js_obj_to_func(js, obj);
  GC_ROOT_RESTORE(js, root_mark);
  
  return func;
}

static inline ant_value_t js_cfunc_value(const ant_cfunc_meta_t *stored) {
  return stored ? mkref(kTypeBuiltin, stored) : mkval(kTypeError, 0);
}

ant_value_t js_mkfun_meta(const ant_cfunc_meta_t *meta) {
  if (!meta) return mkval(kTypeBuiltin, 0);
  return js_cfunc_value(ant_cfunc_meta_intern(meta));
}

ant_value_t js_mkfun_site(ant_cfunc_site_t *site) {
  if (__builtin_expect(ant_cfunc_intern_realm != NULL, 0)) return js_cfunc_value(ant_cfunc_meta_intern(&site->meta));
  const ant_cfunc_meta_t *stored = __atomic_load_n(&site->stored, __ATOMIC_ACQUIRE);
  if (stored) return mkref(kTypeBuiltin, stored);
  
  stored = ant_cfunc_meta_intern(&site->meta);
  if (stored) __atomic_store_n(&site->stored, stored, __ATOMIC_RELEASE);
  return js_cfunc_value(stored);
}

ant_value_t js_mkfun_dyn(ant_cfunc_t fn) {
  return js_cfunc_value(ant_cfunc_meta_for_entrypoint(fn));
}

ant_value_t js_cfunc_noop(ant_params_t) {
  return Ant_Value_Undefined;
}

ant_value_t js_cfunc_name_value(ant_t *js, ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  if (!meta || !meta->name) return Ant_Value_Undefined;
  return js_mkstr(js, meta->name, strlen(meta->name));
}

ant_value_t js_cfunc_length_value(ant_value_t cfunc) {
  return Ant_Value_FromNumber((double)js_cfunc_length(cfunc));
}

static void js_note_function_display_name(ant_t *js, ant_value_t fn, ant_value_t name_val) {
  sv_func_t *func = vtype(fn) == kTypeFunction ? js_func_closure(fn)->func : NULL;
  if (!func || (func->debug->name && func->debug->name[0]) || func->debug->display_name_varies) return;

  size_t len = 0;
  const char *name = js_getstr(js, name_val, &len);
  if (!name || len == 0) return;
  
  const char *interned = intern_string(js, name, len);
  if (!func->debug->display_name) func->debug->display_name = interned;
  else if (func->debug->display_name != interned) func->debug->display_name_varies = true;
}

static ant_value_t js_define_function_name_value(ant_t *js, ant_value_t fn, ant_value_t name_val) {
  ant_value_t fn_obj = js_as_obj(fn);
  ant_object_t *ptr = js_obj_ptr(fn_obj);
  
  if (!ptr) return Ant_Value_Undefined;
  if (vtype(name_val) != kTypeString) return js_mkerr(js, "function name must be a string");
  js_note_function_display_name(js, fn, name_val);

  const char *name_key = js->intern.name ? js->intern.name : intern_string(js, "name", 4);
  if (!name_key) return js_mkerr(js, "oom");

  return mkprop_interned_exact(
    js, fn_obj, name_key, name_val, 
    ANT_PROP_ATTR_CONFIGURABLE
  );
}

static ant_value_t js_mkstr_concat2(
  ant_t *js,
  const char *left, size_t left_len,
  const char *right, size_t right_len
) {
  if (!left) left_len = 0;
  if (!right) right_len = 0;

  if (SIZE_MAX - left_len < right_len) return js_mkerr(js, "oom");
  size_t total = left_len + right_len;
  ant_value_t out = js_mkstr(js, NULL, total);
  if (is_err(out)) return out;

  ant_flat_string_t *flat = ant_str_flat_ptr(out);
  if (!flat) return js_mkerr(js, "oom");
  if (left_len) memcpy(flat->bytes, left, left_len);
  if (right_len) memcpy(flat->bytes + left_len, right, right_len);
  
  flat->bytes[total] = '\0';
  str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, total));
  
  return out;
}

static ant_value_t js_mkstr_symbol_function_name(
  ant_t *js,
  const char *prefix, size_t prefix_len,
  const char *desc
) {
  if (!prefix) prefix_len = 0;
  if (!desc) return js_mkstr_concat2(js, prefix, prefix_len, "", 0);

  size_t desc_len = strlen(desc);
  if (SIZE_MAX - prefix_len < desc_len ||
      SIZE_MAX - prefix_len - desc_len < 2) {
    return js_mkerr(js, "oom");
  }

  size_t total = prefix_len + desc_len + 2;
  ant_value_t out = js_mkstr(js, NULL, total);
  if (is_err(out)) return out;

  ant_flat_string_t *flat = ant_str_flat_ptr(out);
  if (!flat) return js_mkerr(js, "oom");
  if (prefix_len) memcpy(flat->bytes, prefix, prefix_len);
  
  flat->bytes[prefix_len] = '[';
  memcpy(flat->bytes + prefix_len + 1, desc, desc_len);
  
  flat->bytes[total - 1] = ']';
  flat->bytes[total] = '\0';
  str_flat_init_meta(flat, str_detect_ascii_bytes(flat->bytes, total));
  
  return out;
}

ant_value_t js_set_function_name(ant_t *js, ant_value_t fn, const char *name, size_t name_len) {
  if (!name) {
    name = "";
    name_len = 0;
  }

  ant_value_t name_val = js_mkstr(js, name, name_len);
  if (is_err(name_val)) return name_val;
  return js_define_function_name_value(js, fn, name_val);
}

ant_value_t js_set_function_name_prefixed(
  ant_t *js, ant_value_t fn,
  const char *prefix, size_t prefix_len,
  const char *name, size_t name_len
) {
  if (!prefix || prefix_len == 0)
    return js_set_function_name(js, fn, name, name_len);

  if (!name) {
    name = "";
    name_len = 0;
  }

  ant_value_t prefixed = js_mkstr_concat2(js, prefix, prefix_len, name, name_len);
  if (is_err(prefixed)) return prefixed;
  return js_define_function_name_value(js, fn, prefixed);
}

ant_value_t js_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
) {
  if (vtype(key) == kTypeSymbol) {
    const char *desc = js_sym_desc(key);
    ant_value_t symbol_name = js_mkstr_symbol_function_name(js, prefix, prefix_len, desc);
    if (is_err(symbol_name)) return symbol_name;
    return js_define_function_name_value(js, fn, symbol_name);
  }

  ant_value_t key_str = coerce_to_str(js, key);
  if (is_err(key_str)) return key_str;
  if (vtype(key_str) != kTypeString)
    return js_set_function_name_prefixed(js, fn, prefix, prefix_len, "", 0);

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key_str, &key_len);
  
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  return js_set_function_name_prefixed(js, fn, prefix, prefix_len, key_ptr, (size_t)key_len);
}

static bool js_function_name_is_inferable(ant_t *js, ant_value_t fn) {
  if (vtype(fn) != kTypeFunction) return false;
  
  ant_value_t fn_obj = js_func_obj(fn);
  ant_value_t existing = lkp_interned_val(js, fn_obj, js->intern.name);
  
  if (vtype(existing) == kTypeUndefined) return true;
  if (vtype(existing) != kTypeString) return false;

  ant_offset_t len = 0;
  vstr(js, existing, &len);
  
  return len == 0;
}

ant_value_t js_maybe_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
) {
  if (!js_function_name_is_inferable(js, fn)) return Ant_Value_Undefined;
  return js_set_function_name_from_key(js, fn, key, prefix, prefix_len);
}

bool js_cfunc_has_prototype(ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  return meta && (meta->flags & CFUNC_HAS_PROTOTYPE) != 0;
}

static bool js_cfunc_is_constructor(ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  return meta && (meta->flags & CFUNC_IS_CONSTRUCTOR) != 0;
}

ant_value_t setup_func_prototype_property(ant_t *js, ant_value_t func, bool mark_constructor) {
  ant_value_t func_obj = (vtype(func) == kTypeFunction)
    ? js_func_obj(func) 
    : js_as_obj(func);
  
  ant_value_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;
  
  ant_value_t object_proto = js->sym.object_proto;
  if (vtype(object_proto) == kTypeObject) js_set_proto_init(proto_obj, object_proto);
  
  ant_value_t res = defhidden(js, proto_obj, "constructor", 11, func);
  if (is_err(res)) return res;
  
  ant_prop_loc_t existing = lkp(js, func_obj, "prototype", 9);
  if (existing.obj) {
    if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");
    js_prop_store(js, existing, proto_obj);
    js_set_descriptor(js, func_obj, "prototype", 9, JS_DESC_W);
  } else {
    res = mkprop_interned_exact(js, func_obj, js->intern.prototype, proto_obj, ANT_PROP_ATTR_WRITABLE);
    if (is_err(res)) return res;
  }
  
  if (mark_constructor) js_mark_constructor(func_obj, true);
  return Ant_Value_Undefined;
}

static ant_value_t cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len, uint32_t length) {
  const ant_cfunc_meta_t *base = js_as_cfunc_meta(cfunc);
  if (!base) return cfunc;

  if (
    base->name && base->length == length &&
    strlen(base->name) == name_len &&
    memcmp(base->name, name, name_len) == 0
  ) return cfunc;

  const char *interned = intern_string(js, name, name_len);
  if (!interned) return js_mkerr(js, "oom");

  ant_value_t cached;
  ant_ptr_pair_map_t *named_map = js_cfunc_named_map(js, base);
  
  if (
    ant_ptr_pair_get(named_map, (uintptr_t)base, (uintptr_t)interned, &cached) &&
    js_as_cfunc_meta(cached)->length == length
  ) return cached;

  ant_cfunc_meta_t named_meta = {
    .fn = base->fn,
    .name = interned,
    .length = length,
    .flags = base->flags,
  };

  const ant_cfunc_meta_t *stored = ant_cfunc_meta_derive(base, &named_meta);
  if (!stored) return js_mkerr(js, "oom");

  ant_value_t named = mkref(kTypeBuiltin, stored);
  if (!ant_ptr_pair_put(named_map, (uintptr_t)base, (uintptr_t)interned, named)) return js_mkerr(js, "oom");
  
  return named;
}

ant_value_t js_cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len) {
  if (vtype(cfunc) != kTypeBuiltin || !name) return cfunc;
  const ant_cfunc_meta_t *base = js_as_cfunc_meta(cfunc);
  return base ? cfunc_expose_named(js, cfunc, name, name_len, base->length) : cfunc;
}

ant_value_t js_cfunc_expose_property(ant_t *js, ant_value_t fn, js_cfunc_role_t role, const char *key, size_t key_len, bool symbol) {
  static const char *const prefixes[] = {
    [JS_CFUNC_VALUE] = "", [JS_CFUNC_GETTER] = "get ", [JS_CFUNC_SETTER] = "set ",
  };

  const ant_cfunc_meta_t *meta = vtype(fn) == kTypeBuiltin ? js_as_cfunc_meta(fn) : NULL;
  if (!meta || meta->name) return fn;

  char name[256];
  int len = !key ? snprintf(name, sizeof(name), "%s", prefixes[role])
    : snprintf(name, sizeof(name), symbol ? "%s[%.*s]" : "%s%.*s", prefixes[role], (int)key_len, key);
  if (len < 0) return fn;

  size_t name_len = (size_t)len < sizeof(name) ? (size_t)len : sizeof(name) - 1;
  return cfunc_expose_named(js, fn, name, name_len, role == JS_CFUNC_SETTER ? 1 : meta->length);
}

ant_value_t js_expose_cfunc_for_key(ant_t *js, ant_value_t value, const char *key, size_t key_len) {
  if (vtype(value) != kTypeBuiltin) return value;
  return js_cfunc_expose_named(js, value, key, key_len);
}

static ant_value_t js_cfunc_promote_in_realm(ant_t *js, ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);

  ant_value_t fn_obj = mkobj(js, 0);
  js_set_slot(fn_obj, SLOT_CFUNC, cfunc);

  ant_value_t proto = js_primitive_prototype(js, kTypeBuiltin);
  if (is_object_type(proto)) {
    ant_object_t *obj_ptr = js_obj_ptr(js_as_obj(fn_obj));
    if (obj_ptr) ant_object_set_proto(js, obj_ptr, proto);
  }

  ant_value_t length_result = js_define_function_length(js, fn_obj, (int)js_cfunc_length(cfunc));
  if (is_err(length_result)) return length_result;

  if (meta && meta->name) {
    ant_value_t name_result = js_set_function_name(js, fn_obj, meta->name, strlen(meta->name));
    if (is_err(name_result)) return name_result;
  }

  ant_value_t promoted = js_obj_to_func(js, fn_obj);

  bool is_constructor = js_cfunc_is_constructor(cfunc);
  if (js_cfunc_has_prototype(cfunc)) {
    ant_value_t proto_result = setup_func_prototype_property(
      js, promoted, is_constructor
    );
    if (is_err(proto_result)) return proto_result;
  } else if (is_constructor) js_mark_constructor(promoted, true);

  if (!ant_ptr_pair_put(js_cfunc_promoted_map(js, meta), vdata(cfunc), 0, promoted))
    return js_mkerr(js, "oom");
  
  ant_realm_t *owner = ant_cfunc_meta_realm(meta);
  if (owner) owner->gc_dirty = true;

  return promoted;
}

ant_value_t js_cfunc_promote(ant_t *js, ant_value_t cfunc) {
  ant_value_t cached;
  if (ant_ptr_pair_get(js_cfunc_promoted_map(js, js_as_cfunc_meta(cfunc)), vdata(cfunc), 0, &cached)) return cached;
  
  ant_realm_t *prev = ant_realm_switch(js, ant_realm_of_builtin(js, cfunc));
  ant_value_t promoted = js_cfunc_promote_in_realm(js, cfunc);
  ant_realm_restore(js, prev);
  
  return promoted;
}

static ant_value_t js_heavy_function(ant_t *js, ant_value_t cfunc, ant_value_t data) {
  ant_value_t fn_obj = mkobj(js, 0);
  
  js_set_slot(fn_obj, SLOT_CFUNC, cfunc);
  js_set_slot(fn_obj, SLOT_DATA, data);
  
  return js_obj_to_func(js, fn_obj);
}

void js_define_methods(ant_t *js, ant_value_t obj, const ant_cfunc_meta_t *methods, size_t count, uint8_t attrs) {
  for (const ant_cfunc_meta_t *m = methods; m < methods + count; m++)
    mkprop_interned(js, obj, intern_string(js, m->name, strlen(m->name)), js_mkfun_meta(m), attrs);
}

ant_value_t js_make_ctor_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t proto, uint8_t proto_attrs) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  if (!meta || !meta->name) return js_mkerr(js, "constructor requires a named cfunc");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_slot(obj, SLOT_CFUNC, cfunc);
  js_set_function_name(js, obj, meta->name, strlen(meta->name));
  mkprop_exact_attrs(js, obj, js_mkstr(js, "prototype", 9), proto, proto_attrs);

  ant_value_t fn_val = js_obj_to_func(js, obj);
  defhidden(js, proto, "constructor", 11, fn_val);
  return fn_val;
}

ant_value_t js_heavy_mkfun(ant_t *js, ant_value_t (*fn)(ant_params_t), ant_value_t data) {
  return js_heavy_function(js, js_mkfun_dyn(fn), data);
}

ant_value_t js_heavy_mkfun_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t data) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  ant_value_t fn = js_heavy_function(js, cfunc, data);
  if (is_err(fn) || !meta) return fn;

  ant_value_t length_result = js_define_function_length(js, fn, (int)meta->length);
  if (is_err(length_result)) return length_result;
  if (!meta->name) return fn;

  ant_value_t name_result = js_set_function_name(js, fn, meta->name, strlen(meta->name));
  return is_err(name_result) ? name_result : fn;
}

ant_value_t js_heavy_mkfun_native(ant_t *js, ant_value_t (*fn)(ant_params_t), void *ptr, uint32_t tag) {
  ant_value_t cfunc = js_mkfun_dyn(fn);
  ant_value_t fn_obj = mkobj(js, 0);
  
  js_set_slot(fn_obj, SLOT_CFUNC, cfunc);
  js_set_native(fn_obj, ptr, tag);
  
  return js_obj_to_func(js, fn_obj);
}
