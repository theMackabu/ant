#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "value.h"
#include "object.h"
#include "errors.h"
#include "isolate.h"

#include "gc/strings.h"
#include "silver/ast.h"
#include "descriptors.h"
#include "esm/loader.h"

#include <assert.h>
#include <string.h>

// TODO: CLEANUP
static constexpr uint32_t ANT_RUNTIME_WEB = 1u << 0;
static constexpr uint32_t PROTO_WALK_F_OBJECT_ONLY = 1u << 0;
static constexpr uint32_t PROTO_WALK_F_LOOKUP = 1u << 1;
static constexpr int JS_ERR_NO_STACK  = 1 << 8;

typedef enum {
  ANT_STRING_INTRINSIC_INDEX_OF = 0,
  ANT_STRING_INTRINSIC_SUBSTRING,
} ant_string_intrinsic_kind_t;

static inline bool ant_value_stack_push_with_spill(
  ant_value_t **stack, size_t *sp, size_t *cap,
  ant_value_t *local, ant_value_t value
) {
  if (*sp == *cap) {
    size_t next_cap = *cap * 2u;

    ant_value_t *next = *stack == local
      ? (ant_value_t *)malloc(next_cap * sizeof(*next))
      : (ant_value_t *)realloc(*stack, next_cap * sizeof(*next));

    if (!next) return false;
    if (*stack == local) memcpy(next, local, *sp * sizeof(*next));

    *stack = next;
    *cap = next_cap;
  }

  (*stack)[(*sp)++] = value;
  return true;
}

#define T_FLAG_FIND(t) (1u << (t))
#define T_MASK_HOLD ()
#define T_MASK_EXPAND(...) T_MASK_E1(T_MASK_E1(T_MASK_E1(T_MASK_E1(__VA_ARGS__))))
#define T_MASK_E1(...)     T_MASK_E2(T_MASK_E2(T_MASK_E2(T_MASK_E2(__VA_ARGS__))))
#define T_MASK_E2(...)     __VA_ARGS__
#define T_MASK_STEP(a, ...) \
  T_FLAG_FIND(a) __VA_OPT__(| T_MASK_AGAIN T_MASK_HOLD (__VA_ARGS__))
#define T_MASK_AGAIN() T_MASK_STEP
#define T_MASK(...) (T_MASK_EXPAND(T_MASK_STEP(__VA_ARGS__)))

static_assert(T_MASK(kTypeObject) == T_FLAG_FIND(kTypeObject));

#define is_non_numeric(v)    ((1u << vtype(v)) & T_NON_NUMERIC_MASK)
#define is_object_type(v)    ((1u << vtype(v)) & T_OBJECT_MASK)
#define is_special_object(v) ((1u << vtype(v)) & T_SPECIAL_OBJECT_MASK)

static constexpr uint32_t T_SPECIAL_OBJECT_MASK =
  T_MASK(kTypeObject, kTypeArray);

static constexpr uint32_t T_BOXABLE_PRIMITIVE_MASK =
  T_MASK(kTypeString, kTypeNumber, kTypeBool, kTypeBigInt, kTypeSymbol);

static constexpr uint32_t T_NEEDS_PROTO_FALLBACK =
  T_MASK(kTypeFunction, kTypeArray, kTypePromise, kTypeGenerator);

static constexpr uint32_t T_OBJECT_MASK =
  T_MASK(kTypeObject, kTypeArray, kTypeFunction, kTypePromise, kTypeGenerator);

static constexpr uint32_t T_NON_NUMERIC_MASK =
  T_MASK(kTypeString, kTypeArray, kTypeFunction, kTypeBuiltin, kTypeObject, kTypeGenerator);

static_assert(
  T_NON_NUMERIC_MASK == (
    T_FLAG_FIND(kTypeString)   |
    T_FLAG_FIND(kTypeArray)    |
    T_FLAG_FIND(kTypeFunction) |
    T_FLAG_FIND(kTypeBuiltin)  |
    T_FLAG_FIND(kTypeObject)   |
    T_FLAG_FIND(kTypeGenerator)),
  "T_MASK variadic expansion"
);

enum: ant_value_t {
  T_EMPTY             = ANT_SENTINEL_TAG | 0xDEADULL,
  SV_JIT_BAILOUT      = ANT_SENTINEL_TAG | 0xBA110ULL,
  SV_AITER_ARRAY_TAG  = ANT_SENTINEL_TAG | 0xFA1ULL,
  SV_AITER_AWAIT_MARK = ANT_SENTINEL_TAG | 0xFA2ULL,
  SV_AITER_STEP_MARK  = ANT_SENTINEL_TAG | 0xFA3ULL,
  SV_AITER_CLOSE_SKIP = ANT_SENTINEL_TAG | 0xFA4ULL,
};

static inline void ant_prototype_write_epoch_bump(ant_t *js) {
  if (++js->prototype_write_epoch == 0) {
    ant_ic_epoch_bump();
    js->prototype_write_epoch = 1;
  }
}

static inline void ant_property_mutation_invalidate(
  ant_t *js, ant_object_t *holder, const char *key
) {
  if (!js || !holder || !key) return;

  if (key == js->intern.promise) {
    if (holder == js_obj_ptr(js->global)) 
      js->promise_resolve_lookup_protector_invalid = true;
    return;
  }

  if (key == js->intern.resolve) {
    if (
      is_object_type(js->sym.promise_ctor) &&
      holder == js_obj_ptr(js->sym.promise_ctor)
    ) js->promise_resolve_lookup_protector_invalid = true;
    return;
  }

  if (key == js->intern.prototype) {
    ant_prototype_write_epoch_bump(js);
    return;
  }

  bool invalidates_constructor = key == js->intern.constructor;
  if (!invalidates_constructor && key != js->intern.then) return;

  ant_object_t *promise_proto = is_object_type(js->sym.promise_proto)
    ? js_obj_ptr(js->sym.promise_proto) : NULL;

  if (!holder->promise_state && holder != promise_proto) return;
  if (invalidates_constructor) {
    js->promise_constructor_protector_invalid = true;
    js->promise_species_protector_invalid = true;
  } else js->promise_then_protector_invalid = true;
}

typedef struct {
  bool has_getter;
  bool has_setter;
  bool writable;
  bool enumerable;
  bool configurable;
  ant_value_t getter;
  ant_value_t setter;
} prop_meta_t;

typedef enum {
  PROP_META_STRING = 0,
  PROP_META_SYMBOL = 1,
} prop_meta_key_t;

static inline bool is_err(ant_value_t v) {
  return vtype(v) == kTypeError;
}

static inline bool is_null(ant_value_t v) {
  return vtype(v) == kTypeNull;
}

static inline bool is_undefined(ant_value_t v) {
  return vtype(v) == kTypeUndefined;
}

static inline bool is_empty_slot(ant_value_t v) {
  return v == T_EMPTY;
}

static inline void js_cstk_refresh_floor(ant_t *js) {
  uintptr_t base = (uintptr_t)js->cstk.base;
  js->cstk.floor = (js->cstk.base != NULL && js->cstk.limit != 0 && base > js->cstk.limit)
    ? (void *)(base - js->cstk.limit) : NULL;
}

static inline bool is_callable(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == kTypeFunction || t == kTypeBuiltin) return true;
  if (t != kTypeObject) return false;
  ant_object_t *obj = js_obj_ptr(v);
  return obj && obj->flags.is_callable;
}

static inline const ant_cfunc_meta_t *js_as_cfunc_meta(ant_value_t fn_val) {
  return (const ant_cfunc_meta_t *)ant_cage_decode(vdata(fn_val));
}

static inline ant_cfunc_t js_as_cfunc(ant_value_t fn_val) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta ? meta->fn : NULL;
}

static inline uint32_t js_cfunc_length(ant_value_t fn_val) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta ? meta->length : 0;
}

static inline bool js_cfunc_same_entrypoint(ant_value_t fn_val, ant_cfunc_t fn) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta && meta->fn == fn;
}

size_t uint_to_str(char *buf, size_t bufsize, uint64_t val);
ant_value_t extract_array_args(ant_t *js, ant_value_t arr, ant_value_t **out_args, int *out_count);
ant_value_t js_proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);

size_t tostr(ant_t *js, ant_value_t value, char *buf, size_t len);
size_t strstring(ant_t *js, ant_value_t value, char *buf, size_t len);

double js_to_number(ant_t *js, ant_value_t arg);
double js_parse_int_value(ant_t *js, ant_value_t arg);
double js_parse_float_value(ant_t *js, ant_value_t arg);

bool js_obj_ensure_prop_capacity(ant_object_t *obj, uint32_t needed);
bool js_obj_ensure_unique_shape(ant_object_t *obj);

ant_value_t js_template_to_string(ant_t *js, ant_value_t v);
ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode);

ant_value_t mkprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_exact_attrs(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_interned_exact(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_append_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

ant_value_t setprop_cstr(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v);
ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto, ant_value_t new_target);
ant_value_t js_construct_native(ant_t *js, ant_cfunc_t ctor, ant_value_t *args, int nargs);

ant_value_t js_get_module_import_binding(ant_t *js);
ant_value_t js_builtin_import(ant_params_t);
ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main);
ant_value_t js_create_module_context(ant_t *js, const char *filename, bool is_main);
ant_value_t js_create_arguments_object(ant_t *js, sv_vm_t *vm, ant_value_t callee, sv_frame_t *frame, int argc, int mapped_count, bool is_strict);

void js_arguments_detach(ant_t *js, ant_value_t obj);
void js_arguments_sync_slot(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value);
void js_arguments_rebind_frame(ant_t *js, ant_value_t obj, int frame_index);
void js_arguments_bind_direct(ant_t *js, ant_value_t obj, struct sv_frame *frame);
void ant_symbol_property_mutation_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off);

ant_value_t coerce_to_str(ant_t *js, ant_value_t v);
ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v);
ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor);

bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target);
bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b);

ant_value_t lkp_interned_val(ant_t *js, ant_value_t obj, const char *search_intern);
ant_prop_loc_t lkp_interned(ant_value_t obj, const char *search_intern);

ant_prop_loc_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_prop_loc_t lkp_proto(ant_t *js, ant_value_t obj, const char *buf, size_t len);

ant_prop_loc_t lkp_sym(ant_value_t obj, ant_offset_t sym_off);
ant_prop_loc_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off);

ant_value_t mkobj(ant_t *js, ant_offset_t parent);
ant_value_t js_mkobj_with_inobj_limit(ant_t *js, uint8_t inobj_limit);
ant_value_t js_mkarr_dense_literal(ant_t *js, const ant_value_t *elements, uint32_t count);
ant_value_t js_mkobj_from_template(ant_t *js, ant_value_t template);

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj);
ant_value_t js_own_property_keys(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only);
ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym);

ant_value_t js_cfunc_promote(ant_t *js, ant_value_t cfunc);
ant_value_t js_cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len);
ant_value_t js_set_function_name(ant_t *js, ant_value_t fn, const char *name, size_t name_len);
ant_value_t js_setprop_index(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value);

ant_value_t js_set_function_name_prefixed(
  ant_t *js, ant_value_t fn,
  const char *prefix, size_t prefix_len,
  const char *name, size_t name_len
);

ant_value_t js_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
);

ant_value_t js_maybe_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
);

sv_func_t *js_compile_parsed_bytecode(
  ant_t *js, struct sv_ast *program,
  const char *buf, size_t len, int mode
);

bool is_proxy(ant_value_t obj);
bool is_array_value(ant_value_t value);
bool js_is_array_includes_builtin(ant_value_t func);
bool js_is_function_apply_builtin(ant_value_t func);
bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r);
bool same_value_values(ant_t *js, ant_value_t l, ant_value_t r);
bool js_string_intrinsic_builtin_matches(ant_value_t func, ant_string_intrinsic_kind_t kind);
bool js_deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict);
bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj);

bool js_try_char_code_at(
  ant_t *js, ant_value_t func, ant_value_t receiver,
  ant_value_t *args, int argc, ant_value_t *result
);

ant_value_t js_eval_bytecode_eval_in_env_with_strict(
  ant_t *js, const char *buf, size_t len, bool inherit_strict, 
  ant_value_t this_val, ant_value_t eval_env, 
  ant_value_t new_target, bool allows_new_target
);

ant_value_t js_primitive_prototype(ant_t *js, uint8_t type);
ant_value_t js_normalize_sloppy_this(ant_t *js, ant_value_t value);
ant_value_t js_resolve_bound_target(ant_value_t value);
ant_value_t js_resolve_bound_target_known_bound(ant_value_t value);
ant_value_t js_execute_compiled_bytecode(ant_t *js, sv_func_t *func, coroutine_t **async_coro_out);
ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc);
ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target);
ant_value_t sv_call_native(ant_t *js, ant_value_t func, ant_value_t this_val, ant_value_t *args, int nargs, ant_value_t new_target);
ant_value_t js_array_includes_call(ant_t *js, ant_value_t this_val, ant_value_t *args, int nargs);

const char *typestr(ant_value_type_t t);
ant_value_t unwrap_primitive(ant_t *js, ant_value_t val);
ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r);
ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint);
ant_value_t js_is_array_value_checked(ant_t *js, ant_value_t value, bool *out);

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r);

ant_value_t builtin_function_apply(ant_params_t);
ant_value_t builtin_object_isPrototypeOf(ant_params_t);
ant_value_t builtin_object_freeze(ant_params_t);
ant_value_t builtin_string_charCodeAt(ant_params_t);
ant_value_t builtin_array_includes(ant_params_t);
ant_value_t js_builtin_eval(ant_t *js);

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx);
void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx);

ant_value_t js_string_intrinsic_call(
  ant_t *js, ant_string_intrinsic_kind_t kind,
  ant_value_t this_val, ant_value_t *args, int nargs
);

bool js_try_get_own_data_prop(
  ant_t *js, ant_value_t obj, 
  const char *key, size_t key_len,
  ant_value_t *out
);

bool lookup_prop_meta(
  ant_t *js, ant_value_t cur_obj,
  prop_meta_key_t key_kind,
  const char *key, size_t klen,
  ant_offset_t sym_off, prop_meta_t *out
);

const char *get_class_name(
  ant_t *js, ant_value_t obj,
  ant_offset_t *out_len, const char *skip
);

static inline bool streq(const char *buf, size_t len, const char *s, size_t n) {
  return len == n && !memcmp(buf, s, n);
}

static inline bool is_boxable_primitive_type(uint8_t type) {
  return (T_FLAG_FIND(type) & T_BOXABLE_PRIMITIVE_MASK) != 0;
}

static inline size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  if (dstlen == 0) return srclen;
  size_t len = srclen < dstlen - 1 ? srclen : dstlen - 1;
  memcpy(dst, src, len); dst[len] = '\0';
  return srclen;
}

static inline ant_module_t *js_active_tla_module_ctx(ant_t *js) {
  if (!js) return NULL;
  for (coroutine_t *coro = js->active_async_coro; coro; coro = coro->active_parent)
    if (coro->module_eval_ctx) return coro->module_eval_ctx;
  return NULL;
}

static inline void js_module_ctx_link_namespace(ant_t *js, ant_value_t module_ctx, ant_value_t ns) {
  if (!is_object_type(module_ctx) || !is_object_type(ns)) return;
  js_set_slot_wb(js, ns, SLOT_MODULE_CTX, module_ctx);
  js_set_slot_wb(js, module_ctx, SLOT_DATA, ns);
}

static inline ant_value_t js_module_ctx_namespace(ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return js_mkundef();
  ant_value_t ns = js_get_slot(module_ctx, SLOT_DATA);
  return is_object_type(ns) ? ns : js_mkundef();
}

static inline ant_value_t js_current_func_module_ns(ant_t *js) {
  if (!js || !is_object_type(js->current_func)) return js_mkundef();
  return js_module_ctx_namespace(js_get_slot(js->current_func, SLOT_MODULE_CTX));
}

static inline ant_value_t js_module_eval_active_ns(ant_t *js) {
  ant_module_t *ctx = js->modules.module_stack;
  if (ctx) return ctx->module_ns;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
  if (ctx) return ctx->module_ns;
  return js_current_func_module_ns(js);
}

static inline ant_value_t js_module_eval_active_ctx(ant_t *js) {
  ant_module_t *ctx = js->modules.module_stack;
  if (ctx) return ctx->module_ctx;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
  return ctx ? ctx->module_ctx : js_mkundef();
}

static inline ant_value_t js_module_eval_active_import_meta(ant_t *js) {
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  return is_object_type(module_ctx) ? js_get(js, module_ctx, "meta") : js_mkundef();
}

static inline const char *js_module_eval_active_filename(ant_t *js) {
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  if (is_object_type(module_ctx)) {
    ant_value_t filename = js_get(js, module_ctx, "filename");
    if (vtype(filename) == kTypeString) return js_getstr(js, filename, NULL);
  }
  return js->filename;
}

static inline ant_module_format_t js_module_eval_active_format(ant_t *js) {
  ant_module_t *ctx = js->modules.module_stack;
  if (ctx) return ctx->format;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
  return ctx ? ctx->format : MODULE_EVAL_FORMAT_UNKNOWN;
}

static inline bool is_length_key(const char *key, size_t len) {
  return len == 6 && !memcmp(key, "length", 6);
}

static inline int js_brand_id(ant_value_t obj) {
  if (!is_object_type(obj)) return BRAND_NONE;
  ant_value_t brand = js_get_slot(obj, SLOT_BRAND);
  return vtype(brand) == kTypeNumber ? (int)js_getnum(brand) : BRAND_NONE;
}

static inline bool js_check_brand(ant_value_t obj, int brand) {
  return js_brand_id(obj) == brand;
}

static inline bool lookup_symbol_prop_meta(ant_t *js, ant_value_t cur_obj, ant_offset_t sym_off, prop_meta_t *out) {
  return lookup_prop_meta(js, cur_obj, PROP_META_SYMBOL, NULL, 0, sym_off, out);
}

static inline bool lookup_string_prop_meta(ant_t *js, ant_value_t cur_obj, const char *key, size_t klen, prop_meta_t *out) {
  return lookup_prop_meta(js, cur_obj, PROP_META_STRING, key, klen, 0, out);
}

static inline ant_value_t defmethod(ant_t *js, ant_value_t obj, const char *name, size_t len, ant_value_t fn) {
  const char *interned = intern_string(name, len);
  if (!interned) return js_mkerr(js, "oom");

  return mkprop_interned(
    js, obj, interned, fn,
    ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
  );
}

static inline ant_value_t defalias(ant_t *js, ant_value_t obj, const char *name, size_t len, ant_value_t fn) {
  const char *interned = intern_string(name, len);
  if (!interned) return js_mkerr(js, "oom");

  return mkprop_interned_exact(
    js, obj, interned, fn,
    ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
  );
}

static inline void js_set_global_builtin(
  ant_t *js,
  const char *name,
  ant_value_t value
) {
  ant_value_t global = js->global;
  size_t name_len = strlen(name);
  js_set(js, global, name, value);
  js_set_descriptor(js, global, name, name_len, JS_DESC_W | JS_DESC_C);
}

static inline void js_set_module_default(ant_t *js, ant_value_t lib, ant_value_t ctor_fn, const char *name) {
  js_set(js, ctor_fn, name, ctor_fn);
  js_set(js, lib, name, ctor_fn);
  js_set(js, lib, "default", ctor_fn);
  js_set(js, ctor_fn, "default", ctor_fn);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, ctor_fn);
}

static inline ant_value_t js_cfunc_lookup_promoted(ant_t *js, ant_value_t cfunc) {
  uintptr_t ptr = vdata(cfunc);
  for (uint8_t i = 0; i < js->cfunc_promote_cache.len; i++) if (
    js->cfunc_promote_cache.cfunc_ptr[i] == ptr
  ) return js->cfunc_promote_cache.promoted[i];
  return cfunc;
}

static inline ant_value_t js_make_ctor(ant_t *js, ant_cfunc_t fn, ant_value_t proto, const char *name, size_t nlen) {
  ant_value_t obj = js_mkobj(js);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(fn));
  js_mkprop_fast(js, obj, "prototype", 9, proto);
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, name, nlen));
  js_set_descriptor(js, obj, "name", 4, 0);

  ant_value_t fn_val = js_obj_to_func(js, obj);
  js_set(js, proto, "constructor", fn_val);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  return fn_val;
}

#endif
