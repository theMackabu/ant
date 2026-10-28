#ifndef SILVER_FEEDBACK_H
#define SILVER_FEEDBACK_H

#include "debug.h"
#include "silver/engine.h"
#include "jit/entry_stub.h"
#include "modules/buffer.h"

typedef enum sv_tfb_class: uint8_t {
  SV_TFB_NUM   = 1u << 0,
  SV_TFB_STR   = 1u << 1,
  SV_TFB_BOOL  = 1u << 2,
  SV_TFB_OTHER = 1u << 3,
} sv_tfb_class_t;

static constexpr uint8_t SV_TFB_SPEC_COUNT_SHIFT = 4;
static constexpr uint8_t SV_TFB_SPEC_MIN_SAMPLES = 7;

static constexpr uint8_t SV_TFB_SPEC_COUNT_MASK = 7u << SV_TFB_SPEC_COUNT_SHIFT;
static constexpr uint8_t SV_TFB_SPEC_MISMATCH = 1u << 7;
static constexpr uint8_t SV_TFB_DOUBLE = 1u << 4;

static constexpr int SV_CALL_FB_MAX_SLOTS = 32;
static constexpr uint8_t SV_JIT_BAILOUT_LIMIT = 5;
static constexpr uint8_t SV_CALL_FB_MISS_DISABLE = 4;

static constexpr uint32_t SV_JIT_THRESHOLD = 100;
static constexpr uint32_t SV_JIT_RECOMPILE_DELAY = 50;

static constexpr uint32_t SV_TFB_ALLOC_THRESHOLD = 2;
static constexpr uint32_t SV_JIT_WARM_CALLS = SV_JIT_THRESHOLD / 2;

static constexpr uint8_t SV_TFB_CLASS_MASK = SV_TFB_NUM | SV_TFB_STR | SV_TFB_BOOL | SV_TFB_OTHER;
static constexpr ant_value_t SV_JIT_RETRY_INTERP = NANBOX_PREFIX | ((ant_value_t)kTypeError << NANBOX_TYPE_SHIFT) | 1;

static_assert(
  SV_TFB_SPEC_MIN_SAMPLES <= (SV_TFB_SPEC_COUNT_MASK >> SV_TFB_SPEC_COUNT_SHIFT),
  "specialization sample threshold must fit in the feedback counter"
);

static_assert(
  (SV_TFB_CLASS_MASK & (SV_TFB_SPEC_COUNT_MASK | SV_TFB_SPEC_MISMATCH)) == 0,
  "feedback value classes and specialization state must not overlap"
);

static_assert(
  SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY >= SV_JIT_WARM_CALLS,
  "a bailout must leave the function warm"
);

typedef enum {
  SV_JIT_FROM_INTERP,
  SV_JIT_FROM_C,
} sv_jit_entry_t;

static inline bool sv_is_jit_bailout(ant_value_t v) {
  return v == SV_JIT_BAILOUT;
}

typedef struct {
  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t new_target;
  ant_value_t *args;
  int argc;
  ant_value_t *alloc;
} sv_call_ctx_t;

static inline bool sv_jit_interp_innermost(ant_t *js) {
  return js->vm_segs && js->vm_segs->jit_depth == js->jit_active_depth;
}

static inline __attribute__((always_inline)) ant_value_t sv_jit_invoke_same_realm(
  ant_t *js, sv_jit_entry_t from, sv_jit_func_t fn, sv_vm_t *vm,
  const sv_call_ctx_t *ctx, sv_closure_t *closure
) {
#if ANT_JIT_ENTER_STUB
  bool clean = from == SV_JIT_FROM_INTERP || sv_jit_interp_innermost(js);
  js->jit_active_depth++;
  ant_value_t result = clean
    ? ant_jit_enter_clean(fn, vm, ctx->this_val, ctx->new_target, ctx->super_val, ctx->args, ctx->argc, closure)
    : fn(vm, ctx->this_val, ctx->new_target, ctx->super_val, ctx->args, ctx->argc, closure);
#else
  (void)from;
  js->jit_active_depth++;
  ant_value_t result = fn(vm, ctx->this_val, ctx->new_target, ctx->super_val, ctx->args, ctx->argc, closure);
#endif
  js->jit_active_depth--;
  return result;
}

ant_value_t sv_jit_invoke_in_realm(
  ant_t *js, sv_jit_entry_t from, sv_jit_func_t fn, sv_vm_t *vm,
  const sv_call_ctx_t *ctx, sv_closure_t *closure
);

static inline __attribute__((always_inline)) ant_value_t sv_jit_invoke(
  ant_t *js, sv_jit_entry_t from, sv_jit_func_t fn, sv_vm_t *vm,
  const sv_call_ctx_t *ctx, sv_closure_t *closure
) {
  if (__builtin_expect(closure && closure->func->realm != js->realm, 0))
    return sv_jit_invoke_in_realm(js, from, fn, vm, ctx, closure);
  return sv_jit_invoke_same_realm(js, from, fn, vm, ctx, closure);
}

static inline void sv_jit_on_bailout_at(sv_func_t *fn, const char *reason, int bc_off) {
  if (!fn) return;

  if (fn->jit_bailout_tfb_ver != fn->tfb_version) {
    fn->jit_bailout_tfb_ver = fn->tfb_version;
    fn->jit_bailout_count = 0;
  }

  if (fn->jit_bailout_count < UINT8_MAX)
    fn->jit_bailout_count++;

  fn->jit_code = NULL;
  fn->jit_code_cold = false;
  fn->jit_code_retier = false;
  fn->back_edge_count = 0;

  if (sv_jit_warn_unlikely) {
    const char *op_name = "entry";
    if (bc_off >= 0 && bc_off < fn->code_len) {
      uint8_t op = fn->code[bc_off];
      if (op < OP__COUNT && sv_op_names[op]) op_name = sv_op_names[op];
    }

    uint32_t line = 0, col = 0;
    (void)sv_lookup_srcpos(fn, bc_off, &line, &col);

    fprintf(stderr,
      "jit: bailout %u/%u tfb=%u func=%s op=%s bc=%d at %s:%u:%u reason=%s\n",
      (unsigned)fn->jit_bailout_count, (unsigned)SV_JIT_BAILOUT_LIMIT,
      fn->tfb_version, fn->debug->name ? fn->debug->name : "<anonymous>",
      op_name, bc_off, fn->debug->filename ? fn->debug->filename : "<unknown>",
      line, col, reason ? reason : "unknown"
    );
  }

  if (fn->jit_bailout_count >= SV_JIT_BAILOUT_LIMIT) {
    fn->jit_compile_failed = true;
    fn->call_count = 0;
    if (sv_jit_warn_unlikely) fprintf(
      stderr, "jit: disabling %s after %u bailouts at tfb=%u\n",
      fn->debug->name ? fn->debug->name : "<anonymous>",
      (unsigned)fn->jit_bailout_count, fn->tfb_version
    );
    return;
  }

  fn->call_count = SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY;
}

static inline void sv_jit_on_bailout(sv_func_t *fn) { 
  sv_jit_on_bailout_at(fn, "direct", -1);
}

static inline sv_tfb_class_t sv_tfb_classify(ant_value_t v) {
  if (vtype(v) == kTypeNumber) return SV_TFB_NUM;
  if (vtype(v) == kTypeString) return SV_TFB_STR;
  if (vtype(v) == kTypeBool) return SV_TFB_BOOL;
  return SV_TFB_OTHER;
}

// TODO: cleanup
static constexpr uint8_t SV_TFB_LENGTH_ARRAY = SV_TFB_OTHER;
static constexpr uint8_t SV_TFB_LENGTH_STRING = SV_TFB_STR;
static constexpr uint8_t SV_TFB_LENGTH_OTHER = SV_TFB_NUM;

static inline uint8_t sv_tfb_length_class(ant_value_t object) {
  switch (vtype(object)) {
    case kTypeArray: return SV_TFB_LENGTH_ARRAY;
    case kTypeString: return SV_TFB_LENGTH_STRING;
    default: return SV_TFB_LENGTH_OTHER;
  }
}

static inline bool sv_func_has_sidecar(const sv_func_t *func) {
  return func && (((uintptr_t)func->type_feedback & ant_sidecar) != 0);
}

static inline sv_func_sidecar_t *sv_func_sidecar(const sv_func_t *func) {
  if (!func) return NULL;
  uintptr_t raw = (uintptr_t)func->type_feedback;
  if ((raw & ant_sidecar) == 0) return NULL;
  return (sv_func_sidecar_t *)(raw & ~ant_sidecar);
}

static inline struct sv_jit_await_map *sv_func_jit_await_map(const sv_func_t *func) {
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  return sidecar ? sidecar->jit_await_map : NULL;
}

static inline uint8_t *sv_func_type_feedback(const sv_func_t *func) {
  if (!func) return NULL;
  uintptr_t raw = (uintptr_t)func->type_feedback;
  if ((raw & ant_sidecar) == 0) return func->type_feedback;
  return ((sv_func_sidecar_t *)(raw & ~ant_sidecar))->type_feedback;
}

static inline sv_func_sidecar_t *sv_func_ensure_sidecar(sv_func_t *func) {
  if (!func) return NULL;

  uintptr_t raw = (uintptr_t)func->type_feedback;
  if ((raw & ant_sidecar) != 0)
    return (sv_func_sidecar_t *)(raw & ~ant_sidecar);

  sv_func_sidecar_t *sidecar = (sv_func_sidecar_t *)calloc(1, sizeof(*sidecar));
  if (!sidecar) return NULL;

  sidecar->type_feedback = func->type_feedback;
  func->type_feedback = (uint8_t *)((uintptr_t)sidecar | ant_sidecar);

  return sidecar;
}

static inline bool sv_tfb_is_double(ant_value_t v) {
  if (vtype(v) != kTypeNumber) return false;
  double d = tod(v);
  if (!(d >= (double)INT32_MIN && d <= (double)INT32_MAX)) return true;
  return (double)(int32_t)d != d || (d == 0 && signbit(d));
}

static inline uint8_t *sv_tfb_site(sv_func_t *func, uint8_t *ip) {
  uint8_t *type_feedback = sv_func_type_feedback(func);
  return type_feedback ? &type_feedback[(int)(ip - func->code)] : NULL;
}

static inline void sv_tfb_site_update(sv_func_t *func, uint8_t *site, uint8_t old, uint8_t neu) {
  if (neu != old) { *site = neu; func->tfb_version++; }
}

static inline void sv_tfb_record2(sv_func_t *func, uint8_t *ip, ant_value_t l, ant_value_t r) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site) return;
  
  uint8_t old = *site;
  uint8_t neu = old | sv_tfb_classify(l) | sv_tfb_classify(r);
  
  if (!(old & SV_TFB_DOUBLE) && (sv_tfb_is_double(l) || sv_tfb_is_double(r))) neu |= SV_TFB_DOUBLE;
  sv_tfb_site_update(func, site, old, neu);
}

static inline void sv_tfb_record_iteration(sv_func_t *func, uint8_t *ip, ant_value_t tag) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site || vtype(tag) != kTypeNumber) return;
  uint8_t old = *site;
  sv_tfb_site_update(func, site, old, old | (uint8_t)(1u << ((uint32_t)tod(tag) & 7)));
}

static inline void sv_tfb_record1(sv_func_t *func, uint8_t *ip, ant_value_t v) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site) return;
  
  uint8_t old = *site;
  uint8_t neu = old | sv_tfb_classify(v);
  
  if (!(old & SV_TFB_DOUBLE) && sv_tfb_is_double(v)) neu |= SV_TFB_DOUBLE;
  sv_tfb_site_update(func, site, old, neu);
}

static inline void sv_tfb_record_length(sv_func_t *func, uint8_t *ip, ant_value_t object) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (site) sv_tfb_site_update(func, site, *site, *site | sv_tfb_length_class(object));
}

static inline uint8_t sv_tfb_add_specialization_sample(
  uint8_t old, uint8_t neu, bool matches
) {
  if (!matches) neu |= SV_TFB_SPEC_MISMATCH;
  else {
    uint8_t count = (uint8_t)((old & SV_TFB_SPEC_COUNT_MASK) >> SV_TFB_SPEC_COUNT_SHIFT);
    if (count < SV_TFB_SPEC_MIN_SAMPLES) count++;
    neu = (uint8_t)((neu & ~SV_TFB_SPEC_COUNT_MASK) | (count << SV_TFB_SPEC_COUNT_SHIFT));
  }

  return neu;
}

static inline uint8_t *sv_tfb_specialization_site(
  sv_func_t *func, uint8_t *ip, uint8_t *old
) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site || (*site & SV_TFB_SPEC_MISMATCH)) return NULL;
  *old = *site;
  return site;
}

static inline void sv_tfb_record_specialization_at(
  sv_func_t *func, uint8_t *site, uint8_t old, bool matches
) {
  sv_tfb_site_update(func, site, old, sv_tfb_add_specialization_sample(old, old, matches));
}

static inline bool sv_tfb_specialization_ready(uint8_t feedback) {
  uint8_t count = (uint8_t)((feedback & SV_TFB_SPEC_COUNT_MASK) >> SV_TFB_SPEC_COUNT_SHIFT);
  return count >= SV_TFB_SPEC_MIN_SAMPLES && (feedback & SV_TFB_SPEC_MISMATCH) == 0;
}

static inline bool sv_tfb_is_word32_number(ant_value_t value) {
  if (vtype(value) != kTypeNumber) return false;
  double number = tod(value);
  return fabs(number) < 0x1p63;
}

static inline bool sv_tfb_still_sampling(uint8_t old) {
  return (old & SV_TFB_SPEC_MISMATCH) == 0;
}

static inline void sv_tfb_record2_spec(
  sv_func_t *func, uint8_t *ip, ant_value_t l, ant_value_t r, bool word32
) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site) return;

  uint8_t old = *site;
  uint8_t neu = old | sv_tfb_classify(l) | sv_tfb_classify(r);
  
  if (sv_tfb_still_sampling(old)) neu = sv_tfb_add_specialization_sample(old, neu, word32
    ? sv_tfb_is_word32_number(l) && sv_tfb_is_word32_number(r)
    : vtype(l) == kTypeNumber && vtype(r) == kTypeNumber);

  sv_tfb_site_update(func, site, old, neu);
}

static inline void sv_tfb_record1_word32_spec(
  sv_func_t *func, uint8_t *ip, ant_value_t value
) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site) return;

  uint8_t old = *site;
  uint8_t neu = old | sv_tfb_classify(value);
  if (sv_tfb_still_sampling(old)) neu = sv_tfb_add_specialization_sample(old, neu, sv_tfb_is_word32_number(value));

  sv_tfb_site_update(func, site, old, neu);
}

static inline bool sv_tfb_dense_element(
  ant_value_t object, ant_value_t key, ant_value_t *slot
) {
  if (vtype(object) != kTypeArray || vtype(key) != kTypeNumber) return false;

  double number = tod(key);
  if (!isfinite(number) || number < 0 || number >= (double)UINT32_MAX || trunc(number) != number) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(object));
  if (!ptr || ptr->flags.is_exotic || !ptr->flags.fast_array || !ptr->u.array.data) return false;

  uint32_t index = (uint32_t)number;
  if (index >= ptr->u.array.len || index >= ptr->u.array.cap) return false;

  ant_value_t *candidate = &ptr->u.array.data[index];
  if (vtype(*candidate) == kTypeSentinel) return false;
  if (slot) *slot = *candidate;

  return true;
}

static inline bool sv_tfb_dense_numeric_element(ant_value_t object, ant_value_t key, ant_value_t *slot) {
  ant_value_t element;
  if (!sv_tfb_dense_element(object, key, &element) || vtype(element) != kTypeNumber) return false;
  if (slot) *slot = element;
  return true;
}

// TODO: cleanup
static constexpr uint8_t SV_TFB_ELEM_TYPED_ARRAY = SV_TFB_NUM;
static constexpr uint8_t SV_TFB_ELEMENT_DENSE = SV_TFB_BOOL;
static constexpr uint8_t SV_TFB_ELEMENT_OTHER = SV_TFB_OTHER;
static constexpr uint8_t SV_TFB_ELEM_TYPE_MIXED = UINT8_MAX;

static inline bool sv_tfb_elem_saw_typed_array(uint8_t feedback) {
  return (feedback & SV_TFB_ELEM_TYPED_ARRAY) != 0;
}

static inline int sv_tfb_elem_type(const sv_func_t *func, int bc_off) {
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  uint8_t seen = sidecar && sidecar->elem_types && bc_off >= 0 && bc_off < func->code_len
    ? sidecar->elem_types[bc_off] : 0;
  return seen && seen != SV_TFB_ELEM_TYPE_MIXED ? seen - 1 : -1;
}

static inline void sv_tfb_record_elem_type(sv_func_t *func, uint8_t *site, TypedArrayType type) {
  size_t bc_off = (size_t)(site - sv_func_type_feedback(func));
  sv_func_sidecar_t *sidecar = sv_func_ensure_sidecar(func);
  
  if (!sidecar) return;
  if (!sidecar->elem_types && !(sidecar->elem_types = calloc((size_t)func->code_len, 1))) return;
  
  uint8_t *slot = &sidecar->elem_types[bc_off];
  uint8_t seen = (uint8_t)(type + 1);
  uint8_t next = *slot == 0 || *slot == seen ? seen : SV_TFB_ELEM_TYPE_MIXED;
  
  if (next != *slot) { *slot = next; func->tfb_version++; }
}

static inline void sv_tfb_record_typed_array_access(
  sv_func_t *func, uint8_t *site, uint8_t old, buffer_ta_access_t access
) {
  uint8_t neu = access != BUFFER_TA_ACCESS_NONE ? (uint8_t)(old | SV_TFB_ELEM_TYPED_ARRAY) : old;
  sv_tfb_site_update(func, site, old, sv_tfb_add_specialization_sample(old, neu, access == BUFFER_TA_ACCESS_INLINE));
}

static inline __attribute__((noinline)) void sv_tfb_record_typed_array_get(
  sv_func_t *func, uint8_t *site, uint8_t old, ant_value_t object, ant_value_t key
) {
  TypedArrayType type;
  buffer_ta_access_t access = buffer_typedarray_inline_get(object, key, &type);
  
  if (access == BUFFER_TA_ACCESS_NONE) {
    uint8_t neu = (uint8_t)(old | SV_TFB_ELEMENT_OTHER);
    if (sv_tfb_still_sampling(old)) neu = sv_tfb_add_specialization_sample(old, neu, false);
    sv_tfb_site_update(func, site, old, neu);
    return;
  }
  
  if (!sv_tfb_still_sampling(old)) return;
  sv_tfb_record_elem_type(func, site, type);
  sv_tfb_record_typed_array_access(func, site, old, access);
}

static inline __attribute__((noinline)) void sv_tfb_record_typed_array_put(
  sv_func_t *func, uint8_t *site, uint8_t old, ant_value_t object, ant_value_t key, ant_value_t value
) {
  TypedArrayType type;
  buffer_ta_access_t access = buffer_typedarray_inline_put(object, key, value, &type);
  if (access != BUFFER_TA_ACCESS_NONE) sv_tfb_record_elem_type(func, site, type);
  sv_tfb_record_typed_array_access(func, site, old, access);
}

static inline void sv_tfb_record_get_elem(sv_func_t *func, uint8_t *ip, ant_value_t object, ant_value_t key) {
  uint8_t *site = sv_tfb_site(func, ip);
  if (!site) return;
  
  uint8_t old = *site;
  if (vtype(object) != kTypeArray) {
    if (!(old & SV_TFB_ELEMENT_OTHER) || sv_tfb_still_sampling(old))
      sv_tfb_record_typed_array_get(func, site, old, object, key);
    return;
  }
  
  uint8_t seen = SV_TFB_ELEMENT_DENSE | SV_TFB_ELEMENT_OTHER;
  if ((old & seen) == seen && !sv_tfb_still_sampling(old)) return;
  
  ant_value_t element = 0;
  bool dense = sv_tfb_dense_element(object, key, &element);
  uint8_t neu = old | (dense ? SV_TFB_ELEMENT_DENSE : SV_TFB_ELEMENT_OTHER);
  
  if (sv_tfb_still_sampling(old)) neu = sv_tfb_add_specialization_sample(old, neu, dense && vtype(element) == kTypeNumber);
  sv_tfb_site_update(func, site, old, neu);
}

static inline bool sv_tfb_dense_numeric_put(
  ant_value_t object, ant_value_t key, ant_value_t value,
  bool *tagged_old
) {
  if (vtype(value) != kTypeNumber || vtype(object) != kTypeArray ||
      vtype(key) != kTypeNumber)
    return false;

  double number = tod(key);
  if (!isfinite(number) || number < 0 ||
      number >= (double)UINT32_MAX || trunc(number) != number)
    return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(object));
  if (!ptr || ptr->flags.is_exotic || ptr->flags.frozen ||
      !ptr->flags.fast_array || !ptr->u.array.data)
    return false;

  uint32_t index = (uint32_t)number;
  if (index >= ptr->u.array.len || index >= ptr->u.array.cap ||
      vtype(ptr->u.array.data[index]) == kTypeSentinel)
    return false;

  if (tagged_old)
    *tagged_old = vtype(ptr->u.array.data[index]) != kTypeNumber;
  return true;
}

static inline void sv_tfb_record_dense_numeric_put(
  sv_func_t *func, uint8_t *ip,
  ant_value_t object, ant_value_t key, ant_value_t value
) {
  uint8_t old;
  uint8_t *site = sv_tfb_specialization_site(func, ip, &old);
  if (!site) return;
  
  if (vtype(object) != kTypeArray) {
    sv_tfb_record_typed_array_put(func, site, old, object, key, value);
    return;
  }

  bool tagged_old = false;
  bool matches = sv_tfb_dense_numeric_put(object, key, value, &tagged_old);

  uint8_t neu = tagged_old ? (uint8_t)(old | SV_TFB_OTHER) : old;
  sv_tfb_site_update(func, site, old, sv_tfb_add_specialization_sample(old, neu, matches));
}

static inline bool sv_tfb_put_needs_tagged_old_guard(uint8_t feedback) {
  return (feedback & SV_TFB_OTHER) != 0;
}

static inline void sv_tfb_ensure(sv_func_t *fn) {
  if (!sv_func_type_feedback(fn) && fn->code_len > 0) {
    uint8_t *type_feedback = calloc((size_t)fn->code_len, 1);
    if (sv_func_has_sidecar(fn)) sv_func_sidecar(fn)->type_feedback = type_feedback;
    else fn->type_feedback = type_feedback;
  }
  if (!fn->local_type_feedback && fn->max_locals > 0)
    fn->local_type_feedback = calloc((size_t)fn->max_locals, 1);
}

static inline void sv_tfb_record_call_target(sv_func_t *func, int bc_off, sv_func_t *callee) {
  if (!callee) return;
  
  if (__builtin_expect(callee->unit != NULL, 0)) {
    if (!func->unit && !func->fb_unit_watched && !sv_code_units_watch_feedback(func, callee)) return;
    func->fb_unit_target = true;
  }
  
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  
  for (int i = 0; i < count; i++) {
    if (fb[i].bc_off != (uint16_t)bc_off) continue;
    if (fb[i].disabled) return;
    if (fb[i].target == callee) return;
    if (fb[i].target == NULL) { fb[i].target = callee; return; }
    fb[i].miss_count++;
    if (fb[i].miss_count >= SV_CALL_FB_MISS_DISABLE) {
      fb[i].disabled = 1;
      fb[i].target = NULL;
    } else fb[i].target = callee;
    func->tfb_version++;
    return;
  }
  
  if (count >= SV_CALL_FB_MAX_SLOTS) return;
  if (!fb) {
    fb = calloc(SV_CALL_FB_MAX_SLOTS, sizeof(sv_call_target_fb_t));
    if (!fb) return;
    func->call_target_fb = fb;
  }
  
  fb[count].bc_off = (uint16_t)bc_off;
  fb[count].target = callee;
  fb[count].miss_count = 0;
  fb[count].disabled = 0;
  func->call_target_fb_count = (uint8_t)(count + 1);
}

static constexpr uint8_t SV_TFB_CALLED_BUILTIN = 1u << 0;

static inline void sv_tfb_record_builtin_call(sv_func_t *func, uint8_t *ip) {
  uint8_t *type_feedback = sv_func_type_feedback(func);
  if (!type_feedback) return;
  
  uint8_t *site = &type_feedback[ip - func->code];
  if (*site & SV_TFB_CALLED_BUILTIN) return;
  
  *site |= SV_TFB_CALLED_BUILTIN;
  func->tfb_version++;
}

static inline bool sv_tfb_call_site_may_call_builtin(sv_func_t *func, int bc_off) {
  uint8_t *type_feedback = sv_func_type_feedback(func);
  if (type_feedback && (type_feedback[bc_off] & SV_TFB_CALLED_BUILTIN)) return true;
  
  for (int i = 0; i < func->call_target_fb_count; i++)
    if (func->call_target_fb[i].bc_off == (uint16_t)bc_off) return false;
  
  return true;
}

static inline sv_func_t *sv_tfb_get_call_target(sv_func_t *func, int bc_off) {
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  for (int i = 0; i < count; i++) {
    if (fb[i].bc_off == (uint16_t)bc_off && !fb[i].disabled)
      return fb[i].target;
  }
  return NULL;
}

static inline void sv_tfb_record_local(sv_func_t *func, int idx, ant_value_t v) {
  if (func->local_type_feedback && idx >= 0 && idx < func->max_locals) {
    uint8_t old = func->local_type_feedback[idx];
    uint8_t neu = old | sv_tfb_classify(v);
    if (neu != old) { func->local_type_feedback[idx] = neu; func->tfb_version++; }
  }
}

#endif
