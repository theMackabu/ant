#ifndef SILVER_FEEDBACK_H
#define SILVER_FEEDBACK_H

#include "silver/engine.h"
#include "debug.h"

// TODO: constexpr / enum
#define SV_TFB_NUM   (1 << 0)
#define SV_TFB_STR   (1 << 1)
#define SV_TFB_BOOL  (1 << 2)
#define SV_TFB_OTHER (1 << 3)

#define SV_TFB_SPEC_COUNT_SHIFT 4
#define SV_TFB_SPEC_MIN_SAMPLES 7u

#define SV_TFB_SPEC_COUNT_MASK  (7u << SV_TFB_SPEC_COUNT_SHIFT)
#define SV_TFB_SPEC_MISMATCH    (1u << 7)

#define SV_TFB_CLASS_MASK (SV_TFB_NUM | SV_TFB_STR | SV_TFB_BOOL | SV_TFB_OTHER)

static_assert(
  SV_TFB_SPEC_MIN_SAMPLES <= (SV_TFB_SPEC_COUNT_MASK >> SV_TFB_SPEC_COUNT_SHIFT),
  "specialization sample threshold must fit in the feedback counter"
);

static_assert(
  (SV_TFB_CLASS_MASK & (SV_TFB_SPEC_COUNT_MASK | SV_TFB_SPEC_MISMATCH)) == 0,
  "feedback value classes and specialization state must not overlap"
);

#define SV_TFB_INOBJ_SLACK_ALLOCATIONS 32
#define SV_TFB_INOBJ_P90_NUMERATOR     9
#define SV_TFB_INOBJ_P90_DENOMINATOR   10

#define SV_JIT_THRESHOLD       100
#define SV_JIT_RECOMPILE_DELAY 50
#define SV_TFB_ALLOC_THRESHOLD 2

#define SV_CALL_FB_MAX_SLOTS    32
#define SV_JIT_BAILOUT_LIMIT    5
#define SV_CALL_FB_MISS_DISABLE 4

#define SV_JIT_RETRY_INTERP mkval(kTypeError, 1)


static inline bool sv_is_jit_bailout(ant_value_t v) {
  return v == SV_JIT_BAILOUT;
}

static inline void sv_jit_enter(ant_t *js) {
  if (js) js->jit_active_depth++;
}

static inline void sv_jit_leave(ant_t *js) {
  if (js && js->jit_active_depth > 0) js->jit_active_depth--;
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

static inline uint8_t sv_tfb_classify(ant_value_t v) {
  if (vtype(v) == kTypeNumber) return SV_TFB_NUM;
  if (vtype(v) == kTypeString) return SV_TFB_STR;
  if (vtype(v) == kTypeBool) return SV_TFB_BOOL;
  return SV_TFB_OTHER;
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

static inline void sv_tfb_record2(sv_func_t *func, uint8_t *ip, ant_value_t l, ant_value_t r) {
  uint8_t *type_feedback = sv_func_type_feedback(func);

  if (type_feedback) {
  int off = (int)(ip - func->code);

  uint8_t old = type_feedback[off];
  uint8_t neu = old | sv_tfb_classify(l) | sv_tfb_classify(r);

  if (neu != old) {
    type_feedback[off] = neu;
    func->tfb_version++;
  }}
}

static inline void sv_tfb_record1(sv_func_t *func, uint8_t *ip, ant_value_t v) {
  uint8_t *type_feedback = sv_func_type_feedback(func);
  if (type_feedback) {
  int off = (int)(ip - func->code);

  uint8_t old = type_feedback[off];
  uint8_t neu = old | sv_tfb_classify(v);

  if (neu != old) {
    type_feedback[off] = neu;
    func->tfb_version++;
  }}
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
  uint8_t *type_feedback = sv_func_type_feedback(func);
  if (!type_feedback) return NULL;

  uint8_t *site = &type_feedback[(int)(ip - func->code)];
  *old = *site;

  if (*old & SV_TFB_SPEC_MISMATCH) return NULL;
  return site;
}

static inline void sv_tfb_record_specialization_at(
  sv_func_t *func, uint8_t *site, uint8_t old, bool matches
) {
  uint8_t neu = sv_tfb_add_specialization_sample(old, old, matches);
  if (neu != old) { *site = neu; func->tfb_version++; }
}

static inline bool sv_tfb_specialization_ready(uint8_t feedback) {
  uint8_t count = (uint8_t)((feedback & SV_TFB_SPEC_COUNT_MASK) >> SV_TFB_SPEC_COUNT_SHIFT);
  return count >= SV_TFB_SPEC_MIN_SAMPLES && (feedback & SV_TFB_SPEC_MISMATCH) == 0;
}

static inline bool sv_tfb_is_word32_number(ant_value_t value) {
  if (vtype(value) != kTypeNumber) return false;
  double number = tod(value);
  return isfinite(number) && number >= (double)INT32_MIN && number <= (double)UINT32_MAX && trunc(number) == number;
}

static inline void sv_tfb_record2_spec(
  sv_func_t *func, uint8_t *ip, ant_value_t l, ant_value_t r,
  bool word32
) {
  uint8_t old;
  uint8_t *site = sv_tfb_specialization_site(func, ip, &old);
  if (!site) return;

  bool matches = word32
    ? sv_tfb_is_word32_number(l) && sv_tfb_is_word32_number(r)
    : vtype(l) == kTypeNumber && vtype(r) == kTypeNumber;

  uint8_t neu = old | sv_tfb_classify(l) | sv_tfb_classify(r);
  neu = sv_tfb_add_specialization_sample(old, neu, matches);
  if (neu != old) { *site = neu; func->tfb_version++; }
}

static inline void sv_tfb_record1_word32_spec(
  sv_func_t *func, uint8_t *ip, ant_value_t value
) {
  uint8_t old;
  uint8_t *site = sv_tfb_specialization_site(func, ip, &old);
  if (!site) return;

  uint8_t neu = old | sv_tfb_classify(value);
  neu = sv_tfb_add_specialization_sample(old, neu, sv_tfb_is_word32_number(value));
  if (neu != old) { *site = neu; func->tfb_version++; }
}

static inline bool sv_tfb_dense_numeric_element(
  ant_value_t object, ant_value_t key, ant_value_t *slot
) {
  if (vtype(object) != kTypeArray || vtype(key) != kTypeNumber) return false;

  double number = tod(key);
  if (!isfinite(number) || number < 0 ||
      number >= (double)UINT32_MAX || trunc(number) != number)
    return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(object));
  if (!ptr || ptr->flags.is_exotic || !ptr->flags.fast_array ||
      !ptr->u.array.data)
    return false;

  uint32_t index = (uint32_t)number;
  if (index >= ptr->u.array.len || index >= ptr->u.array.cap) return false;

  ant_value_t *candidate = &ptr->u.array.data[index];
  if (vtype(*candidate) != kTypeNumber) return false;
  if (slot) *slot = *candidate;

  return true;
}

static inline bool sv_tfb_dense_numeric_get(
  ant_value_t object, ant_value_t key
) {
  return sv_tfb_dense_numeric_element(object, key, NULL);
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

  bool tagged_old = false;
  bool matches = sv_tfb_dense_numeric_put(object, key, value, &tagged_old);

  uint8_t neu = tagged_old ? (uint8_t)(old | SV_TFB_OTHER) : old;
  neu = sv_tfb_add_specialization_sample(old, neu, matches);
  if (neu != old) { *site = neu; func->tfb_version++; }
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

static inline uint8_t sv_tfb_clamp_inobj_limit(uint32_t limit) {
  return (limit > ANT_INOBJ_MAX_SLOTS) ? (uint8_t)ANT_INOBJ_MAX_SLOTS : (uint8_t)limit;
}

static inline sv_ctor_prop_fb_t *sv_tfb_ctor_prop_fb(sv_func_t *func, bool create) {
  if (!func) return NULL;
  sv_func_sidecar_t *sidecar = create ? sv_func_ensure_sidecar(func) : sv_func_sidecar(func);
  return sidecar ? &sidecar->ctor_prop_fb : NULL;
}

static inline uint64_t sv_tfb_ctor_prop_samples(const sv_func_t *func) {
  sv_ctor_prop_fb_t *fb = func ? sv_tfb_ctor_prop_fb((sv_func_t *)func, false) : NULL;
  return fb ? fb->samples : 0;
}

static inline uint64_t sv_tfb_ctor_prop_bin(const sv_func_t *func, uint32_t bin) {
  sv_ctor_prop_fb_t *fb = func ? sv_tfb_ctor_prop_fb((sv_func_t *)func, false) : NULL;
  if (!fb || bin >= SV_TFB_CTOR_PROP_BINS) return 0;
  return fb->hist[bin];
}

static inline uint8_t sv_tfb_infer_inobj_limit(const sv_func_t *func, uint64_t samples) {
  if (!func || samples == 0) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  sv_ctor_prop_fb_t *fb = sv_tfb_ctor_prop_fb((sv_func_t *)func, false);
  if (!fb) return (uint8_t)ANT_INOBJ_MAX_SLOTS;

  uint64_t target = (
    (samples * SV_TFB_INOBJ_P90_NUMERATOR)
    + (SV_TFB_INOBJ_P90_DENOMINATOR - 1)
  ) / SV_TFB_INOBJ_P90_DENOMINATOR;
  if (target == 0) target = 1;

  uint64_t seen = 0;
  for (uint32_t i = 0; i < SV_TFB_CTOR_PROP_BINS; i++) {
    seen += fb->hist[i];
    if (seen < target) continue;
    if (i >= SV_TFB_CTOR_PROP_OVERFLOW_FROM) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
    return sv_tfb_clamp_inobj_limit(i);
  }

  return (uint8_t)ANT_INOBJ_MAX_SLOTS;
}

static inline void sv_tfb_record_ctor_prop_count(ant_value_t ctor_func, ant_value_t instance) {
  if (vtype(ctor_func) != kTypeFunction) return;
  if (!is_object_type(instance)) return;

  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return;

  ant_object_t *obj = js_obj_ptr(js_as_obj(instance));
  if (!obj) return;

  sv_func_t *func = closure->func;
  sv_ctor_prop_fb_t *fb = sv_tfb_ctor_prop_fb(func, true);
  if (!fb) return;

  uint32_t count = obj->prop_count;
  uint32_t bin = (count < SV_TFB_CTOR_PROP_OVERFLOW_FROM)
    ? count
    : SV_TFB_CTOR_PROP_OVERFLOW_FROM;

  fb->hist[bin]++;
  uint64_t samples = ++fb->samples;

  if (!fb->inobj_frozen && samples >= SV_TFB_INOBJ_SLACK_ALLOCATIONS) {
    fb->inobj_limit = sv_tfb_infer_inobj_limit(func, samples);
    fb->inobj_frozen = 1;
  }
}

static inline uint8_t sv_tfb_ctor_inobj_limit(ant_value_t ctor_func) {
  if (vtype(ctor_func) != kTypeFunction) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return (uint8_t)ANT_INOBJ_MAX_SLOTS;

  sv_func_t *func = closure->func;
  sv_ctor_prop_fb_t *fb = sv_tfb_ctor_prop_fb(func, false);

  if (!fb || !fb->inobj_frozen) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  return sv_tfb_clamp_inobj_limit(fb->inobj_limit);
}

static inline bool sv_tfb_ctor_inobj_limit_frozen(ant_value_t ctor_func) {
  if (vtype(ctor_func) != kTypeFunction) return false;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return false;
  sv_ctor_prop_fb_t *fb = sv_tfb_ctor_prop_fb(closure->func, false);
  return fb && fb->inobj_frozen != 0;
}

static inline uint32_t sv_tfb_ctor_inobj_slack_remaining(ant_value_t ctor_func) {
  if (vtype(ctor_func) != kTypeFunction) return SV_TFB_INOBJ_SLACK_ALLOCATIONS;
  sv_closure_t *closure = js_func_closure(ctor_func);

  if (!closure || !closure->func) return SV_TFB_INOBJ_SLACK_ALLOCATIONS;
  sv_func_t *func = closure->func;
  sv_ctor_prop_fb_t *fb = sv_tfb_ctor_prop_fb(func, false);

  if (!fb) return SV_TFB_INOBJ_SLACK_ALLOCATIONS;
  if (fb->inobj_frozen || fb->samples >= SV_TFB_INOBJ_SLACK_ALLOCATIONS) return 0;

  return (uint32_t)(SV_TFB_INOBJ_SLACK_ALLOCATIONS - fb->samples);
}


#endif
