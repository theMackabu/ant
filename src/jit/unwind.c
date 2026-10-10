#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "jit_internal.h"
#include "silver/engine.h"

#include <pthread.h>
#include <unwind.h>

#if defined(_WIN32) || !(defined(__x86_64__) || defined(__aarch64__))
#define JIT_UNWIND_SUPPORTED 0
#else
#define JIT_UNWIND_SUPPORTED 1
#endif

#if JIT_UNWIND_SUPPORTED && !defined(__APPLE__)
#define JIT_UNWIND_REGISTER 1
extern void __register_frame(const void *);
extern void __deregister_frame(const void *);
extern void __unw_add_dynamic_fde(uintptr_t) __attribute__((weak));
extern void __unw_remove_dynamic_fde(uintptr_t) __attribute__((weak));
#else
#define JIT_UNWIND_REGISTER 0
#endif

typedef struct {
  uint32_t ret_off;
  int32_t bc_off;
  uint32_t inline_at;
  uint32_t inline_depth;
} jit_call_site_t;

static constexpr uint32_t JIT_SITE_CHECKPOINT = 32;
static constexpr int JIT_RESUME_CALLS_MAX = 3;

typedef struct {
  jit_call_site_t site;
  uint32_t next;
} jit_site_checkpoint_t;


typedef struct jit_code_range {
  uintptr_t start, end;
  sv_func_t *func;
  uint8_t *sites;
  uint32_t n_sites;
  uint32_t resume_ret[JIT_RESUME_CALLS_MAX];
  uint8_t n_resume;
#if JIT_UNWIND_REGISTER
  uint32_t fp_cfa_off;
  uint8_t *eh_frame;
#endif
} jit_code_range_t;

struct jit_code_registry {
  jit_code_range_t *ranges;
  size_t len, cap;
  uintptr_t lo, hi;
#if JIT_UNWIND_REGISTER
  bool registering;
#endif
};

_Thread_local jit_code_info_ctx_t jit_code_info_ctx;

static bool jit_inline_stack_eq(const jit_inline_stack_t *a, const jit_inline_stack_t *b) {
  return a->depth == b->depth && !memcmp(a->frames, b->frames, (size_t)a->depth * sizeof(a->frames[0]));
}

static uint32_t jit_call_site_tag(jit_call_tagging_t *t) {
  jit_call_site_info_t info = { .bc_off = *t->bc_off, .resume = t->resume, .inl = t->inl };
  if (info.inl.depth > SV_JIT_INLINE_FRAMES_MAX) info.inl.depth = SV_JIT_INLINE_FRAMES_MAX;

  if (t->n_sites) {
    jit_call_site_info_t *last = &t->sites[t->n_sites - 1];
    if (last->bc_off == info.bc_off && last->resume == info.resume && jit_inline_stack_eq(&last->inl, &info.inl)) return t->n_sites;
  }

  if (t->n_sites == t->cap_sites) {
    uint32_t cap = t->cap_sites ? t->cap_sites * 2 : 64;
    jit_call_site_info_t *grown = realloc(t->sites, cap * sizeof(*grown));
    if (!grown) return 0;
    t->sites = grown;
    t->cap_sites = cap;
  }

  t->sites[t->n_sites++] = info;
  return t->n_sites;
}

MIR_insn_t jit_tag_new_call(MIR_context_t ctx, MIR_insn_t insn) {
  jit_call_tagging_t *t = jit_call_tagging;
  if (insn && t && t->ctx == ctx) insn->ops[0].tag = jit_call_site_tag(t);
  return insn;
}

static void jit_put_uleb(uint8_t **p, uint64_t v) {
  do {
    uint8_t byte = v & 0x7f;
    v >>= 7;
    *(*p)++ = v ? byte | 0x80 : byte;
  } while (v);
}

static size_t jit_uleb_size(uint32_t v) {
  size_t n = 1;
  while (v >>= 7) n++;
  return n;
}

static uint32_t jit_get_uleb(const uint8_t **p) {
  uint32_t v = 0;
  for (int shift = 0;; shift += 7) {
    uint8_t byte = *(*p)++;
    v |= (uint32_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return v;
  }
}

static uint32_t jit_zigzag(int32_t v) {
  return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static int32_t jit_unzigzag(uint32_t v) {
  return (int32_t)((v >> 1) ^ -(v & 1));
}

#if JIT_UNWIND_REGISTER

enum {
  DW_CFA_nop = 0x00,
  DW_CFA_def_cfa = 0x0c,
  DW_CFA_offset = 0x80,
};

#if defined(__x86_64__)
enum { DW_REG_FP = 6, DW_REG_SP = 7, DW_REG_RA = 16, DW_CODE_ALIGN = 1 };
#else
enum { DW_REG_FP = 29, DW_REG_SP = 31, DW_REG_RA = 30, DW_CODE_ALIGN = 4 };
#endif

typedef struct {
  uint8_t buf[96];
  size_t n;
} cfi_buf_t;

static void cfi_u8(cfi_buf_t *b, uint8_t v) { b->buf[b->n++] = v; }

static void cfi_u32(cfi_buf_t *b, uint32_t v) {
  memcpy(b->buf + b->n, &v, 4);
  b->n += 4;
}

static void cfi_u64(cfi_buf_t *b, uint64_t v) {
  memcpy(b->buf + b->n, &v, 8);
  b->n += 8;
}

static void cfi_uleb(cfi_buf_t *b, uint64_t v) {
  uint8_t *p = b->buf + b->n;
  jit_put_uleb(&p, v);
  b->n = (size_t)(p - b->buf);
}

static void cfi_sleb(cfi_buf_t *b, int64_t v) {
  for (;;) {
    uint8_t byte = v & 0x7f;
    v >>= 7;
    bool done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
    cfi_u8(b, done ? byte : byte | 0x80);
    if (done) return;
  }
}

static void cfi_close(cfi_buf_t *b, size_t length_at) {
  while ((b->n - length_at) % 8) cfi_u8(b, DW_CFA_nop);
  uint32_t length = (uint32_t)(b->n - length_at - 4);
  memcpy(b->buf + length_at, &length, 4);
}

static uint8_t *jit_build_eh_frame(uintptr_t start, size_t len, uint32_t fp_cfa_off) {
  cfi_buf_t b = {0};

  size_t cie_at = b.n;
  cfi_u32(&b, 0);
  cfi_u32(&b, 0);
  cfi_u8(&b, 1);
  cfi_u8(&b, 'z');
  cfi_u8(&b, 'R');
  cfi_u8(&b, 0);
  cfi_uleb(&b, DW_CODE_ALIGN);
  cfi_sleb(&b, -8);
  cfi_uleb(&b, DW_REG_RA);
  cfi_uleb(&b, 1);
  cfi_u8(&b, 0x00);
  cfi_u8(&b, DW_CFA_def_cfa);
  cfi_uleb(&b, DW_REG_SP);
  cfi_uleb(&b, DW_CODE_ALIGN == 1 ? 8 : 0);
  cfi_close(&b, cie_at);

  size_t fde_at = b.n;
  cfi_u32(&b, 0);
  cfi_u32(&b, (uint32_t)(b.n - cie_at));
  cfi_u64(&b, (uint64_t)start);
  cfi_u64(&b, (uint64_t)len);
  cfi_uleb(&b, 0);
  cfi_u8(&b, DW_CFA_def_cfa);
  cfi_uleb(&b, DW_REG_FP);
  cfi_uleb(&b, fp_cfa_off);
  cfi_u8(&b, DW_CFA_offset | DW_REG_RA);
  cfi_uleb(&b, (fp_cfa_off - 8) / 8);
  cfi_u8(&b, DW_CFA_offset | DW_REG_FP);
  cfi_uleb(&b, fp_cfa_off / 8);
  cfi_close(&b, fde_at);
  cfi_u32(&b, 0);

  uint8_t *out = malloc(b.n);
  if (out) memcpy(out, b.buf, b.n);
  return out;
}

static uintptr_t jit_eh_frame_fde(const uint8_t *eh_frame) {
  uint32_t cie_len;
  memcpy(&cie_len, eh_frame, 4);
  return (uintptr_t)(eh_frame + 4 + cie_len);
}

static void jit_register_unwind(jit_code_range_t *r) {
  r->eh_frame = jit_build_eh_frame(r->start, r->end - r->start, r->fp_cfa_off);
  if (!r->eh_frame) return;
  if (__unw_add_dynamic_fde) __unw_add_dynamic_fde(jit_eh_frame_fde(r->eh_frame));
  else __register_frame(r->eh_frame);
}

static void jit_register_all(struct jit_code_registry *reg) {
  if (reg->registering) return;
  reg->registering = true;
  for (size_t i = 0; i < reg->len; i++)
    if (reg->ranges[i].fp_cfa_off) jit_register_unwind(&reg->ranges[i]);
}

static void jit_deregister_unwind(jit_code_range_t *r) {
  if (!r->eh_frame) return;
  if (__unw_remove_dynamic_fde) __unw_remove_dynamic_fde(jit_eh_frame_fde(r->eh_frame));
  else __deregister_frame(r->eh_frame);
  free(r->eh_frame);
  r->eh_frame = NULL;
}

#endif

static jit_code_range_t *jit_code_range_find(struct jit_code_registry *reg, uintptr_t pc) {
  if (!reg || !reg->len) return NULL;
  size_t lo = 0, hi = reg->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    jit_code_range_t *r = &reg->ranges[mid];
    if (pc < r->start) hi = mid;
    else if (pc >= r->end) lo = mid + 1;
    else return r;
  }
  return NULL;
}

static uint32_t jit_site_checkpoints(uint32_t n_sites) {
  return (n_sites + JIT_SITE_CHECKPOINT - 1) / JIT_SITE_CHECKPOINT;
}

static size_t jit_site_inline_offset(uint32_t n_sites) {
  size_t at = jit_site_checkpoints(n_sites) * sizeof(jit_site_checkpoint_t);
  size_t align = _Alignof(sv_jit_frame_t);
  return (at + align - 1) / align * align;
}

static const sv_jit_frame_t *jit_site_inline_frames(const jit_code_range_t *r) {
  return (const sv_jit_frame_t *)(r->sites + jit_site_inline_offset(r->n_sites));
}

static bool jit_code_range_site(const jit_code_range_t *r, uintptr_t ret, jit_call_site_t *out) {
  uint32_t n_ckpt = jit_site_checkpoints(r->n_sites);
  const jit_site_checkpoint_t *ckpt = (const jit_site_checkpoint_t *)r->sites;
  
  uint32_t off = (uint32_t)(ret - r->start);
  if (!r->n_sites || ckpt[0].site.ret_off > off) return false;

  uint32_t lo = 0, hi = n_ckpt;
  while (hi - lo > 1) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (ckpt[mid].site.ret_off <= off) lo = mid;
    else hi = mid;
  }

  jit_call_site_t site = ckpt[lo].site;
  const uint8_t *p = r->sites + ckpt[lo].next;
  
  uint32_t left = r->n_sites - lo * JIT_SITE_CHECKPOINT - 1;
  if (left >= JIT_SITE_CHECKPOINT) left = JIT_SITE_CHECKPOINT - 1;

  while (left-- > 0) {
    jit_call_site_t next = site;
    next.ret_off += jit_get_uleb(&p);
    next.bc_off += jit_unzigzag(jit_get_uleb(&p));
    next.inline_at += site.inline_depth;
    next.inline_depth = jit_get_uleb(&p);
    if (next.ret_off > off) break;
    site = next;
  }

  *out = site;
  return true;
}

static int jit_mir_site_cmp(const void *a, const void *b) {
  uint32_t x = ((const MIR_call_site_t *)a)->ret_off, y = ((const MIR_call_site_t *)b)->ret_off;
  return x < y ? -1 : x > y;
}

static uint8_t *jit_encode_sites(
  const MIR_call_site_t *sites, size_t n, const jit_call_tagging_t *t, uint32_t *out_n
) {
  MIR_call_site_t *sorted = malloc(n * sizeof(*sorted));
  if (!sorted) return NULL;

  uint32_t count = 0, n_inline = 0;
  for (size_t i = 0; i < n; i++) {
    if (sites[i].tag - 1 >= t->n_sites) continue;
    sorted[count++] = sites[i];
    n_inline += (uint32_t)t->sites[sites[i].tag - 1].inl.depth;
  }
  
  qsort(sorted, count, sizeof(*sorted), jit_mir_site_cmp);

  size_t head = jit_site_inline_offset(count) + n_inline * sizeof(sv_jit_frame_t);
  size_t stream = 0;
  
  for (uint32_t i = 1; i < count; i++) {
    if (i % JIT_SITE_CHECKPOINT == 0) continue;
    const jit_call_site_info_t *info = &t->sites[sorted[i].tag - 1];
    stream += jit_uleb_size(sorted[i].ret_off - sorted[i - 1].ret_off);
    stream += jit_uleb_size(jit_zigzag(info->bc_off - t->sites[sorted[i - 1].tag - 1].bc_off));
    stream += jit_uleb_size((uint32_t)info->inl.depth);
  }
  
  uint8_t *block = count ? malloc(head + stream) : NULL;
  if (!block) {
    free(sorted);
    return NULL;
  }

  jit_site_checkpoint_t *ckpt = (jit_site_checkpoint_t *)block;
  sv_jit_frame_t *frames = (sv_jit_frame_t *)(block + jit_site_inline_offset(count));
  
  uint8_t *p = block + head;
  uint32_t inline_at = 0;
  int32_t prev_bc_off = 0;

  for (uint32_t i = 0; i < count; i++) {
    const jit_call_site_info_t *info = &t->sites[sorted[i].tag - 1];
    jit_call_site_t site = { sorted[i].ret_off, info->bc_off, inline_at, (uint32_t)info->inl.depth };
    memcpy(frames + inline_at, info->inl.frames, (size_t)site.inline_depth * sizeof(*frames));

    if (i % JIT_SITE_CHECKPOINT == 0) {
      ckpt[i / JIT_SITE_CHECKPOINT] = (jit_site_checkpoint_t){ site, (uint32_t)(p - block) };
    } else {
      jit_put_uleb(&p, site.ret_off - sorted[i - 1].ret_off);
      jit_put_uleb(&p, jit_zigzag(site.bc_off - prev_bc_off));
      jit_put_uleb(&p, site.inline_depth);
    }
    
    prev_bc_off = site.bc_off;
    inline_at += site.inline_depth;
  }

  free(sorted);
  *out_n = count;
  
  return block;
}

void jit_code_info(
  void *data, MIR_item_t func_item, void *code, size_t code_len,
  uint32_t fp_cfa_off, const MIR_call_site_t *sites, size_t n_sites
) {
  sv_jit_ctx_t *jc = data;
  if (!jc || !code || !code_len) return;

  struct jit_code_registry *reg = jc->code_registry;
  if (!reg) {
    reg = jc->code_registry = calloc(1, sizeof(*reg));
    if (!reg) return;
  }

  const jit_code_info_ctx_t *info = &jit_code_info_ctx;
  bool body = info->item && func_item == info->item;
  if (!body && !JIT_UNWIND_REGISTER) return;

  if (reg->len == reg->cap) {
    size_t cap = reg->cap ? reg->cap + reg->cap / 2 : 64;
    jit_code_range_t *grown = realloc(reg->ranges, cap * sizeof(*grown));
    if (!grown) return;
    reg->ranges = grown;
    reg->cap = cap;
  }

  jit_code_range_t r = {
    .start = (uintptr_t)code,
    .end = (uintptr_t)code + code_len,
    .func = body ? info->func : NULL,
  };

  if (body && n_sites) r.sites = jit_encode_sites(sites, n_sites, info->tagging, &r.n_sites);
  for (size_t i = 0; body && i < n_sites && r.n_resume < JIT_RESUME_CALLS_MAX; i++) {
    uint32_t idx = sites[i].tag - 1;
    if (idx < info->tagging->n_sites && info->tagging->sites[idx].resume) r.resume_ret[r.n_resume++] = sites[i].ret_off;
  }

#if JIT_UNWIND_REGISTER
  if (fp_cfa_off >= 16 && fp_cfa_off % 8 == 0) r.fp_cfa_off = fp_cfa_off;
  if (reg->registering && r.fp_cfa_off) jit_register_unwind(&r);
#else
  (void)fp_cfa_off;
#endif

  size_t at = reg->len;
  while (at > 0 && reg->ranges[at - 1].start > r.start) at--;
  memmove(&reg->ranges[at + 1], &reg->ranges[at], (reg->len - at) * sizeof(*reg->ranges));
  reg->ranges[at] = r;
  reg->len++;
  if (!reg->lo || r.start < reg->lo) reg->lo = r.start;
  if (r.end > reg->hi) reg->hi = r.end;
}

void jit_code_registry_destroy(sv_jit_ctx_t *jc) {
  struct jit_code_registry *reg = jc ? jc->code_registry : NULL;
  if (!reg) return;
  for (size_t i = 0; i < reg->len; i++) {
#if JIT_UNWIND_REGISTER
    jit_deregister_unwind(&reg->ranges[i]);
#endif
    free(reg->ranges[i].sites);
  }
  free(reg->ranges);
  free(reg);
  jc->code_registry = NULL;
}

static jit_code_range_t *jit_code_range_at(struct jit_code_registry *reg, uintptr_t pc) {
  if (!reg || pc < reg->lo || pc >= reg->hi) return NULL;
  jit_code_range_t *r = jit_code_range_find(reg, pc);
  return r && r->func ? r : NULL;
}

int sv_jit_frames_at(ant_t *js, uintptr_t pc, sv_jit_frame_t *out, int cap) {
  sv_jit_ctx_t *jc = js ? js->jit_ctx : NULL;
  jit_code_range_t *r = jit_code_range_at(jc ? jc->code_registry : NULL, pc);
  if (!r || cap < 1) return 0;

  jit_call_site_t site;
  if (!r->sites || !jit_code_range_site(r, pc, &site)) {
    out[0] = (sv_jit_frame_t){ r->func, -1 };
    return 1;
  }

  const sv_jit_frame_t *frames = jit_site_inline_frames(r) + site.inline_at;
  int n = 0;
  
  for (int d = (int)site.inline_depth - 1; d >= 0 && n < cap - 1; d--) out[n++] = frames[d];
  out[n++] = (sv_jit_frame_t){ r->func, site.bc_off };
  
  return n;
}

#if JIT_UNWIND_SUPPORTED

typedef struct {
  struct jit_code_registry *reg;
  sv_jit_activation_t **out;
  sv_jit_activation_t *inline_out;
  int cap, n;
  int funcs, limit;
  gc_vm_seg_t *seg;
  sv_jit_osr_mark_t *osr;
  uintptr_t stack_top;
  int vm_fp;
  int seg_start;
  uint32_t runs, jit_entries;
  bool in_run;
} jit_walk_t;

static bool jit_walk_push(jit_walk_t *w, sv_jit_activation_t a) {
  if (w->n == w->cap) {
    int cap = w->cap * 2;
    bool owned = *w->out != w->inline_out;
    
    sv_jit_activation_t *grown = owned ? realloc(*w->out, (size_t)cap * sizeof(*grown)) : malloc((size_t)cap * sizeof(*grown));
    if (!grown) return false;
    if (!owned) memcpy(grown, *w->out, (size_t)w->n * sizeof(*grown));
    
    *w->out = grown;
    w->cap = cap;
  }
  
  (*w->out)[w->n++] = a;
  if (a.func) w->funcs++;
  
  return true;
}

static void jit_walk_pass_marks(jit_walk_t *w, uintptr_t frame_addr) {
  for (;;) {
    uintptr_t seg = w->seg && (uintptr_t)w->seg < frame_addr ? (uintptr_t)w->seg : UINTPTR_MAX;
    uintptr_t osr = w->osr && (uintptr_t)w->osr < frame_addr ? (uintptr_t)w->osr : UINTPTR_MAX;
    if (seg == UINTPTR_MAX && osr == UINTPTR_MAX) return;
    
    if (osr < seg) {
      if (w->n > w->seg_start) (*w->out)[w->n - 1].osr_host = true;
      w->osr = w->osr->prev;
    } else {
      w->vm_fp = w->seg->entry_vm_fp - 1;
      w->seg = w->seg->prev;
    }
    w->seg_start = w->n;
  }
}

static bool jit_walk_visit(jit_walk_t *w, uintptr_t pc, uintptr_t frame_addr) {
  if (frame_addr > w->stack_top) return false;
  jit_walk_pass_marks(w, frame_addr);

  jit_code_range_t *r = jit_code_range_at(w->reg, pc);
  if (!r) {
    if (w->in_run && w->runs >= w->jit_entries) {
      jit_walk_pass_marks(w, w->seg ? (uintptr_t)w->seg : UINTPTR_MAX);
      return false;
    }
    w->in_run = false;
    return true;
  }
  
  if (!w->in_run) w->runs++;
  w->in_run = true;

  bool resumed = false;
  for (int i = 0; i < r->n_resume; i++) resumed |= pc - r->start == r->resume_ret[i];
  
  sv_jit_activation_t a = { .func = resumed ? NULL : r->func, .pc = pc, .vm_fp = w->vm_fp };
  return jit_walk_push(w, a) && (w->limit < 0 || w->funcs < w->limit);
}

#if defined(__APPLE__)

static uintptr_t jit_stack_top(void) {
  return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
}

__attribute__((noinline))
static void jit_walk_native(jit_walk_t *w) {
  uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
  while (fp && !(fp & 7)) {
    uintptr_t caller_fp = ((const uintptr_t *)fp)[0];
    uintptr_t ret = ((const uintptr_t *)fp)[1];
    if (caller_fp <= fp) return;
    if (!jit_walk_visit(w, ret, caller_fp)) return;
    fp = caller_fp;
  }
}

#else

static uintptr_t jit_stack_top(void) {
  return UINTPTR_MAX;
}

static _Unwind_Reason_Code jit_walk_frame(struct _Unwind_Context *uc, void *arg) {
  jit_walk_t *w = arg;
  uintptr_t pc = (uintptr_t)_Unwind_GetIP(uc);
  return jit_walk_visit(w, pc, (uintptr_t)_Unwind_GetCFA(uc)) ? _URC_NO_REASON : _URC_NORMAL_STOP;
}

static void jit_walk_native(jit_walk_t *w) {
  jit_register_all(w->reg);
  _Unwind_Backtrace(jit_walk_frame, w);
}

#endif

int sv_jit_collect_activations(sv_vm_t *vm, sv_jit_activation_t **acts, int cap, int limit) {
  ant_t *js = vm ? vm->js : NULL;
  sv_jit_ctx_t *jc = js ? js->jit_ctx : NULL;
  if (!jc || !jc->code_registry || !js->jit_active_depth || limit == 0) return 0;

  jit_walk_t w = {
    .reg = jc->code_registry,
    .out = acts, .inline_out = *acts, .cap = cap,
    .limit = limit,
    .seg = js->vm_segs,
    .osr = vm->jit_osr_marks,
    .stack_top = jit_stack_top(),
    .vm_fp = vm->fp,
    .jit_entries = js->jit_active_depth,
  };
  jit_walk_native(&w);
  return w.n;
}

#else

int sv_jit_collect_activations(sv_vm_t *vm, sv_jit_activation_t **acts, int cap, int limit) {
  (void)vm; (void)acts; (void)cap; (void)limit;
  return 0;
}

#endif
