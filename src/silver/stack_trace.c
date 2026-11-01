#include "silver/stack_trace.h"
#include "silver/engine.h"
#include "cfunc.h"
#include "errors.h"
#include "error_render.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "ptr.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

js_error_site_t js_error_site_from_source(
  const char *src, ant_offset_t src_len, const char *filename,
  ant_offset_t off, ant_offset_t span_len
) {
  return (js_error_site_t){
    .src = src,
    .src_len = src_len,
    .filename = filename,
    .off = off < 0 ? 0 : off,
    .span_len = span_len < 0 ? 0 : span_len,
    .valid = src != NULL && src_len >= 0,
  };
}

js_error_site_t js_error_site_from_bc(sv_func_t *func, int bc_offset) {
  js_error_site_t site = {0};
  
  const sv_srcpos_t *pos = bc_offset >= 0 ? sv_srcpos_at(func, bc_offset) : NULL;
  if (!pos || !func->debug->source || func->debug->source_len <= 0) return site;

  const char *src = func->debug->source;
  ant_offset_t src_len = (ant_offset_t)func->debug->source_len;
  ant_offset_t off = (ant_offset_t)pos->src_off;
  ant_offset_t span_len = pos->src_end > pos->src_off ? (ant_offset_t)(pos->src_end - pos->src_off) : 0;
  if (span_len == 0 && off < src_len) span_len = 1;

  site = js_error_site_from_source(src, src_len, func->debug->filename, off, span_len);
  site.func = func;
  site.line = pos->line;
  site.col = pos->col;
  
  return site;
}

js_error_site_t js_error_site_from_frame(sv_func_t *func, int bc_offset) {
  return js_error_site_from_bc(func, bc_offset < 0 ? 0 : bc_offset);
}

js_error_site_t js_error_site_from_vm_top(ant_t *js) {
  sv_stack_iter_t it;
  sv_stack_entry_t top;
  
  sv_stack_iter_init(js ? js->vm : NULL, &it, 1, true);
  bool found = sv_stack_iter_next(&it, &top);
  sv_stack_iter_finish(&it);
  
  return found 
    ? js_error_site_from_frame(top.func, top.bc_off) 
    : (js_error_site_t){0};
}

void sv_stack_iter_init(sv_vm_t *vm, sv_stack_iter_t *it, int limit, bool resolve) {
  it->vm = vm;
  it->seg = vm ? vm->js->vm_segs : NULL;
  it->acts = it->inline_acts;
  it->n_acts = 0;
  it->ai = 0;
  it->i = vm ? vm->fp : -1;
  it->skip = -1;
  it->n_pending = 0;
  it->resolve = resolve;
  it->truncated = false;
  
  if (!vm || !vm->js->jit_active_depth) return;
  it->n_acts = sv_jit_collect_activations(vm, &it->acts, SV_STACK_INLINE_ACTIVATIONS, limit);
  
  int funcs = 0;
  for (int i = 0; i < it->n_acts; i++) funcs += it->acts[i].func != NULL;
  it->truncated = limit > 0 && funcs >= limit;
}

void sv_stack_iter_finish(sv_stack_iter_t *it) {
  if (it->acts != it->inline_acts) free(it->acts);
  it->acts = it->inline_acts;
}

static int sv_stack_frame_bc_off(sv_stack_iter_t *it, int idx, const sv_frame_t *frame) {
  if (!frame->func || !frame->ip || !frame->func->code) return -1;
  int off = (int)(frame->ip - frame->func->code);
  if (idx >= it->vm->fp || off == 0) return off;
  while (it->seg && it->seg->entry_vm_fp > idx + 1) it->seg = it->seg->prev;
  return it->seg && it->seg->entry_vm_fp == idx + 1 ? off : off - 1;
}

static bool sv_func_may_construct(const sv_func_t *func) {
  return func && !func->is_arrow && !func->is_eval;
}

static bool sv_frame_is_construct(sv_vm_t *vm, const sv_func_t *func, int vm_fp) {
  if (!sv_func_may_construct(func)) return false;
  if (vm_fp >= 0) return vtype(vm->frames[vm_fp].new_target) != kTypeUndefined;
  return func->is_class_ctor;
}

static bool sv_inlined_is_construct(const sv_jit_frame_t *callee, const sv_jit_frame_t *caller) {
  const sv_func_t *f = caller->func;
  if (!sv_func_may_construct(callee->func)) return false;
  if (callee->func->is_class_ctor) return true;
  if (!f || !f->code || caller->bc_off < 0 || caller->bc_off >= f->code_len) return false;
  
  switch (f->code[caller->bc_off]) {
    case OP_NEW: case OP_NEW_APPLY:
    case OP_CALL_SUPER: case OP_SUPER_APPLY: case OP_SUPER_FORWARD: return true;
    default: return false;
  }
}

static bool sv_jit_frame_is_construct(const sv_jit_frame_t *at, int i, int m, bool outer) {
  return i == m - 1 ? outer : sv_inlined_is_construct(&at[i], &at[i + 1]);
}

static void sv_stack_resolve(sv_stack_iter_t *it, const sv_jit_activation_t *a, ant_value_t callee, int host_fp) {
  sv_jit_frame_t frames[SV_JIT_INLINE_FRAMES_MAX + 1];
  
  int n = sv_jit_frames_at(it->vm->js, a->pc, frames, SV_JIT_INLINE_FRAMES_MAX + 1);
  if (n <= 0) frames[n++] = (sv_jit_frame_t){ a->func, -1 };
  
  bool outer = sv_frame_is_construct(it->vm, frames[n - 1].func, host_fp);
  for (int k = 0; k < n; k++) it->pending[n - 1 - k] = (sv_stack_entry_t){
    .func = frames[k].func,
    .callee = k == n - 1 ? callee : Ant_Value_Undefined,
    .bc_off = frames[k].bc_off,
    .frame_index = -1,
    .construct = sv_jit_frame_is_construct(frames, k, n, outer),
  };
  
  it->n_pending = n;
}

bool sv_stack_iter_next(sv_stack_iter_t *it, sv_stack_entry_t *out) {
  sv_vm_t *vm = it->vm;
  if (!vm) return false;
  
  if (it->n_pending > 0) {
    *out = it->pending[--it->n_pending];
    return true;
  }

  for (;;) {
    if (it->ai < it->n_acts && it->acts[it->ai].vm_fp >= it->i) {
      sv_jit_activation_t *a = &it->acts[it->ai++];
      bool host = a->osr_host && a->vm_fp >= 0 && a->vm_fp <= vm->fp;
      
      if (host) it->skip = a->vm_fp;
      if (!a->func) continue;
      
      int host_fp = host ? a->vm_fp : -1;
      ant_value_t callee = host ? vm->frames[a->vm_fp].callee : Ant_Value_Undefined;
      if (it->resolve) {
        sv_stack_resolve(it, a, callee, host_fp);
        *out = it->pending[--it->n_pending];
      } else *out = (sv_stack_entry_t){
        .func = a->func, .callee = callee, .pc = a->pc, .bc_off = -1, .frame_index = -1,
        .construct = sv_frame_is_construct(vm, a->func, host_fp),
      };
      
      return true;
    }

    if (it->i < 0) return false;
    int idx = it->i--;
    if (idx == it->skip) continue;
    
    sv_frame_t *frame = &vm->frames[idx];
    *out = (sv_stack_entry_t){
      .func = frame->func, .callee = frame->callee,
      .bc_off = sv_stack_frame_bc_off(it, idx, frame), .frame_index = idx,
      .construct = sv_frame_is_construct(vm, frame->func, idx),
    };
    
    return true;
  }
}

void js_get_call_location(ant_t *js, const char **out_filename, int *out_line, int *out_col) {
  if (!js) return;
  js_error_site_t site = js_error_site_from_vm_top(js);
  
  if (out_filename) *out_filename = (site.valid && site.filename) ? site.filename : js->filename;
  if (out_line) *out_line = 1;
  if (out_col)  *out_col  = 1;
  
  if (site.valid && site.line > 0) {
    if (out_line) *out_line = (int)site.line;
    if (out_col)  *out_col  = (int)site.col;
  } else if (site.valid && site.src) error_line_col(
    site.src, site.src_len, site.off, out_line, out_col
  );
}

static int error_stack_trace_limit(ant_t *js) {
  ant_value_t ctor = js->builtins.error_ctor, limit;
  
  if (vtype(ctor) != kTypeFunction) return 10;
  if (!js_try_get_own_data_prop_interned(js, ctor, js->intern.stack_trace_limit, &limit) || vtype(limit) != kTypeNumber) return -1;
  
  double d = Ant_Value_AsNumberUnchecked(limit);
  if (!(d > 0)) return 0;
  
  return d >= (double)INT_MAX ? INT_MAX : (int)d;
}

static const char *error_frame_name_of(ant_t *js, ant_value_t callee, sv_func_t *func) {
  if (func && func->debug->name && func->debug->name[0]) return func->debug->name;
  
  ant_value_t name;
  if (vtype(callee) == kTypeFunction && js_try_get_own_data_prop(js, callee, "name", 4, &name) && vtype(name) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, name, &len);
    if (text && len > 0) return text;
  }
  
  if (func && func->debug->display_name && !func->debug->display_name_varies) 
    return func->debug->display_name;
  
  return "<anonymous>";
}

typedef struct {
  const char *name;
  const char *file;
  int line;
  int col;
  bool construct;
} error_frame_view_t;

typedef struct {
  sv_func_t *func;
  ant_value_t callee;
  uintptr_t pc;
  int bc_off;
  bool construct;
} error_frame_t;

static error_frame_view_t error_frame_view(ant_t *js, const error_frame_t *f, const char *fallback_file) {
  error_frame_view_t view = {
    .name = error_frame_name_of(js, f->callee, f->func),
    .file = f->func && f->func->debug->filename ? f->func->debug->filename : fallback_file,
    .line = f->func && f->func->debug->source_line > 0 ? f->func->debug->source_line : 1,
    .col = 1,
    .construct = f->construct,
  };

  uint32_t l, c;
  if (f->bc_off >= 0 && sv_lookup_srcpos(f->func, f->bc_off, &l, &c)) {
    view.line = (int)l; 
    view.col = (int)c;
  }
  
  return view;
}

static error_frame_t error_frame_of(const sv_stack_entry_t *e) {
  return (error_frame_t){
    .func = e->func, .callee = e->callee, .pc = e->pc,
    .bc_off = e->bc_off, .construct = e->construct
  };
}

static bool error_entry_is_wrapper(ant_t *js, const sv_stack_entry_t *e, int newer) {
  if (e->frame_index != 0 || newer == 0) return false;
  error_frame_t f = error_frame_of(e);
  error_frame_view_t view = error_frame_view(js, &f, "");
  return view.line == 1 && view.col == 1 && strcmp(view.name, "<anonymous>") == 0;
}

static constexpr int ERROR_FRAMES_INLINE = 32;

static bool error_frames_reserve(error_frame_t **frames, error_frame_t *local, int *cap, int need) {
  if (need <= *cap) return true;
  int grown_cap = *cap * 2 > need ? *cap * 2 : need;
  
  error_frame_t *grown = *frames == local
    ? malloc((size_t)grown_cap * sizeof(*grown))
    : realloc(*frames, (size_t)grown_cap * sizeof(*grown));
    
  if (!grown) return false;
  if (*frames == local) memcpy(grown, local, (size_t)*cap * sizeof(*grown));
  
  *frames = grown;
  *cap = grown_cap;
  
  return true;
}

static void error_append_frames(
  ant_t *js, const error_frame_t *frames, int count, const char *fallback_file,
  errbuf_t *eb, size_t *n, error_frame_style_t style
) {
  for (int i = 0; i < count; i++) {
    error_frame_view_t view = error_frame_view(js, &frames[i], fallback_file);
    if (!error_render_frame(eb, n, view.name, view.construct, view.file, view.line, view.col, style)) return;
  }
}

typedef struct {
  sv_func_t *func;
  bool active;
} error_above_t;

static constexpr int ERROR_ABOVE_SLACK = 2;

static error_above_t error_above_of(ant_value_t call_func, ant_value_t fn) {
  bool direct = true;
  
  while (fn != call_func) {
    if (vtype(fn) == kTypeBuiltin) return (error_above_t){ NULL, direct };
    if (vtype(fn) != kTypeFunction) break;

    sv_closure_t *c = js_func_closure(fn);
    if (c->func_obj && vtype(js_get_slot(c->func_obj, SLOT_TARGET_FUNC)) != kTypeUndefined) break;
    if (!(c->call_flags & SV_CALL_IS_DEFAULT_CTOR)) {
      if (c->func || direct) return (error_above_t){ c->func, true };
      break;
    }
    fn = c->super_val;
    direct = false;
  }
  
  return (error_above_t){ NULL, false };
}

static int error_collect_frames(
  ant_t *js, int limit, error_above_t above, bool resolve,
  error_frame_t *local, error_frame_t **out
) {
  *out = local;
  if (!js || !js->vm || limit == 0 || (above.active && !above.func)) return 0;
  
  int cap = ERROR_FRAMES_INLINE;
  int walk = !above.active ? limit
    : limit >= 0 && limit < INT_MAX - ERROR_ABOVE_SLACK 
    ? limit + ERROR_ABOVE_SLACK : -1;
  
  for (;;) {
    sv_stack_iter_t it;
    sv_stack_entry_t e;
    
    sv_func_t *seek = above.func;
    int n = 0;
    
    sv_stack_iter_init(js->vm, &it, walk, resolve || seek);
    while ((limit < 0 || n < limit) && sv_stack_iter_next(&it, &e)) {
      if (seek) {
        if (e.func == seek) { seek = NULL; it.resolve = resolve; }
        continue;
      }
      
      if (error_entry_is_wrapper(js, &e, n)) continue;
      if (!error_frames_reserve(out, local, &cap, n + 1)) break;
      (*out)[n++] = error_frame_of(&e);
    }
    
    bool cut_short = above.active && walk >= 0 && it.truncated && it.ai == it.n_acts;
    sv_stack_iter_finish(&it);
    
    if (!cut_short) return n;
    walk = walk <= INT_MAX / 4 ? walk * 4 : -1;
  }
}

static size_t error_append_live_frames(ant_t *js, int limit, const char *file, errbuf_t *eb, size_t n, error_frame_style_t style) {
  error_frame_t local[ERROR_FRAMES_INLINE], *frames;
  int count = error_collect_frames(js, limit, (error_above_t){0}, true, local, &frames);
  error_append_frames(js, frames, count, file, eb, &n, style);
  if (frames != local) free(frames);
  return n;
}

ant_value_t js_capture_raw_stack(ant_t *js) {
  errbuf_t eb;
  if (!errbuf_init(&eb, 4096)) return Ant_Value_Undefined;
  const char *file = js->filename ? js->filename : "<eval>";
  size_t n = error_append_live_frames(js, error_stack_trace_limit(js), file, &eb, 0, ERROR_FRAME_RAW);
  return errbuf_take_string(js, &eb, n);
}

void js_print_stack_trace_vm(ant_t *js, FILE *stream) {
  errbuf_t eb;
  if (!stream || !errbuf_init(&eb, 4096)) return;
  
  const char *file = js->filename ? js->filename : "<unknown>";
  size_t n = error_append_live_frames(js, -1, file, &eb, 0, ERROR_FRAME_PRETTY);
  
  if (n > 0) {
    fwrite(eb.buf + 1, 1, n - 1, stream);
    fputc('\n', stream);
  }
  
  free(eb.buf);
}

typedef enum {
  ERROR_SITE_NEWEST_FRAME,
  ERROR_SITE_SOURCE,
  ERROR_SITE_RENDERED,
} error_site_kind_t;

typedef struct {
  error_site_kind_t site_kind;
  bool no_site_frame;
  sv_func_t *site_func;
  ant_offset_t site_pos, site_span;
  int line, col;
  ant_value_t rendered_site;
  ant_value_t file;
  int limit;
  int n_frames;
  error_frame_t frames[];
} error_record_t;

static constexpr uint32_t ERROR_RECORD_NATIVE_TAG = Ant_NativeTag("ERRR");

static error_record_t *error_record_of(ant_value_t value) {
  return vtype(value) == kTypeObject ? js_get_native(value, ERROR_RECORD_NATIVE_TAG) : NULL;
}

bool js_is_error_record(ant_value_t value) {
  return error_record_of(value) != NULL;
}

void gc_mark_error_record(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  error_record_t *rec = error_record_of(value);
  if (!rec) return;
  
  if (rec->site_func) mark(js, mkref(kTypeFunctionInfo, rec->site_func));
  mark(js, rec->rendered_site);
  mark(js, rec->file);
  
  for (int i = 0; i < rec->n_frames; i++) {
    if (rec->frames[i].func) mark(js, mkref(kTypeFunctionInfo, rec->frames[i].func));
    mark(js, rec->frames[i].callee);
  }
}

static void error_record_finalize(ant_t *js, ant_object_t *obj) {
  if (obj->native_tag == ERROR_RECORD_NATIVE_TAG) free(obj->native_ptr);
}

ant_value_t js_error_record_capture(ant_t *js, ant_value_t call_func, const js_error_site_t *at, ant_value_t above_fn) {
  int limit = error_stack_trace_limit(js);
  if (limit < 0) return Ant_Value_Undefined;

  error_frame_t local[ERROR_FRAMES_INLINE], *frames;
  error_above_t above = error_above_of(call_func, above_fn);
  int n = error_collect_frames(js, limit, above, false, local, &frames);
  bool need_file = false;
  
  for (int i = 0; i < n; i++) {
    error_frame_t *f = &frames[i];
    if (f->func && f->func->debug->name && f->func->debug->name[0]) f->callee = Ant_Value_Undefined;
    if (!f->func || !f->func->debug->filename) need_file = true;
  }

  error_record_t *rec = calloc(1, sizeof(*rec) + (size_t)n * sizeof(rec->frames[0]));
  if (rec) memcpy(rec->frames, frames, (size_t)n * sizeof(rec->frames[0]));
  if (frames != local) free(frames);
  if (!rec) return Ant_Value_Undefined;
  
  rec->limit = limit;
  rec->no_site_frame = above.active;
  rec->n_frames = n;

  js_error_site_t es = at && at->valid ? *at : (js_error_site_t){0};
  const char *file = es.valid ? error_site_file(js, &es) : js->filename ? js->filename : "<eval>";
  js_error_render_site_t site;
  error_render_site_init(&es, &site);

  if (!es.valid) {
    rec->site_kind = ERROR_SITE_NEWEST_FRAME;
    need_file |= rec->n_frames == 0;
  } else if (es.func && site.src == es.func->debug->source && site.src_len <= ERROR_CONTEXT_MAX_SOURCE_BYTES) {
    rec->site_kind = ERROR_SITE_SOURCE;
    rec->site_func = es.func;
    rec->site_pos = site.src_pos;
    rec->site_span = site.src_span_len;
    need_file |= file != es.func->debug->filename;
  } else {
    rec->site_kind = ERROR_SITE_RENDERED;
    need_file = true;
  }
  
  rec->line = site.line;
  rec->col = site.col;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t rendered = Ant_Value_Undefined, file_str = Ant_Value_Undefined;
  GC_ROOT_PIN(js, rendered);
  GC_ROOT_PIN(js, file_str);
  
  errbuf_t eb;
  if (rec->site_kind == ERROR_SITE_RENDERED && errbuf_init(&eb, 1024)) {
    size_t len = 0;
    error_render_site_finish(&site);
    error_render_site_prefix(&eb, &len, file, &site);
    rendered = errbuf_take_string(js, &eb, len);
  }
  
  if (need_file) file_str = js_mkstr(js, file, strlen(file));
  rec->rendered_site = rendered;
  rec->file = file_str;

  ant_value_t record = Ant_Object_CreateBare(js);
  if (vtype(record) == kTypeObject) {
    js_set_native(record, rec, ERROR_RECORD_NATIVE_TAG);
    js_set_finalizer(record, error_record_finalize);
  } else {
    free(rec);
    record = Ant_Value_Undefined;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return record;
}

typedef struct {
  const error_record_t *rec;
  error_frame_t local[ERROR_FRAMES_INLINE];
  error_frame_t *frames;
  int n;
  const char *file;
  js_error_render_site_t site;
} error_render_t;

static void error_render_begin(ant_t *js, const error_record_t *rec, error_render_t *r) {
  r->rec = rec;
  r->frames = r->local;
  r->n = 0;
  int cap = ERROR_FRAMES_INLINE;

  for (int k = 0; k < rec->n_frames && r->n < rec->limit; k++) {
    const error_frame_t *f = &rec->frames[k];
    sv_jit_frame_t at[SV_JIT_INLINE_FRAMES_MAX + 1];
    int m = f->pc ? sv_jit_frames_at(js, f->pc, at, SV_JIT_INLINE_FRAMES_MAX + 1) : 0;
    if (m <= 0) at[m++] = (sv_jit_frame_t){ f->func, f->bc_off };
    
    for (int i = 0; i < m && r->n < rec->limit; i++) {
      if (!error_frames_reserve(&r->frames, r->local, &cap, r->n + 1)) return;
      r->frames[r->n++] = (error_frame_t){
        .func = at[i].func,
        .callee = i == m - 1 ? f->callee : Ant_Value_Undefined,
        .bc_off = at[i].bc_off,
        .construct = sv_jit_frame_is_construct(at, i, m, f->construct),
      };
    }
  }

  sv_func_t *file_func = rec->site_func ? rec->site_func : r->n > 0 ? r->frames[0].func : NULL;
  r->file = vtype(rec->file) == kTypeString ? js_getstr(js, rec->file, NULL)
    : file_func && file_func->debug->filename ? file_func->debug->filename : "<eval>";

  js_error_site_t es = {0};
  if (rec->site_kind == ERROR_SITE_SOURCE) {
    sv_func_t *func = rec->site_func;
    es = js_error_site_from_source(func->debug->source, (ant_offset_t)func->debug->source_len, func->debug->filename, rec->site_pos, rec->site_span);
    es.func = func;
    es.line = (uint32_t)rec->line;
    es.col = (uint32_t)rec->col;
  } else if (rec->site_kind == ERROR_SITE_NEWEST_FRAME && r->n > 0)
    es = js_error_site_from_frame(r->frames[0].func, r->frames[0].bc_off);
  
  error_render_site_init(&es, &r->site);
  if (rec->site_kind == ERROR_SITE_RENDERED) {
    r->site.line = rec->line;
    r->site.col = rec->col;
  }
}

static void error_render_end(error_render_t *r) {
  if (r->frames != r->local) free(r->frames);
}

static void error_render_prefix(ant_t *js, error_render_t *r, errbuf_t *eb, size_t *n) {
  if (r->rec->site_kind != ERROR_SITE_RENDERED) {
    error_render_site_finish(&r->site);
    error_render_site_prefix(eb, n, r->file, &r->site);
    return;
  }
  
  size_t len = 0;
  const char *text = vtype(r->rec->rendered_site) == kTypeString ? js_getstr(js, r->rec->rendered_site, &len) : NULL;
  if (text) *n = errbuf_append(eb, *n, text, len);
}

static void error_render_frames(ant_t *js, const error_render_t *r, errbuf_t *eb, size_t *n, error_frame_style_t style) {
  error_append_frames(js, r->frames, r->n, r->file, eb, n, style);
  if (r->n == 0 && r->rec->limit > 0 && !r->rec->no_site_frame) error_render_frame(eb, n, NULL, false, r->file, r->site.line, r->site.col, style);
}

char *js_error_render_pretty(ant_t *js, ant_value_t err, ant_value_t stack, size_t *out_len, size_t *header_at) {
  if (!is_object_type(err) || vtype(stack) != kTypeString) return NULL;
  
  error_record_t *rec = error_record_of(js_get_slot(err, SLOT_ERROR_STACK));
  errbuf_t eb;
  
  if (!rec || js_get_slot(err, SLOT_ERROR_STACK_TEXT) != stack) return NULL;
  if (!errbuf_init(&eb, 4096)) return NULL;

  error_render_t r;
  error_render_begin(js, rec, &r);
  
  size_t n = 0;
  error_render_prefix(js, &r, &eb, &n);
  *header_at = n;
  
  error_render_frames(js, &r, &eb, &n, ERROR_FRAME_PRETTY);
  error_render_end(&r);

  *out_len = n;
  return eb.buf;
}

ant_value_t js_error_record_throw_text(ant_t *js, ant_value_t value, ant_value_t record) {
  error_record_t *rec = error_record_of(record);
  errbuf_t eb;
  
  if (!rec || !errbuf_init(&eb, 4096)) return Ant_Value_Undefined;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  GC_ROOT_PIN(js, record);

  error_render_t r;
  error_render_begin(js, rec, &r);
  
  size_t n = 0;
  error_render_prefix(js, &r, &eb, &n);
  n = error_render_value(&eb, js, n, value);
  
  error_render_frames(js, &r, &eb, &n, ERROR_FRAME_PRETTY);
  error_render_end(&r);

  ant_value_t text = errbuf_take_string(js, &eb, n);
  GC_ROOT_RESTORE(js, root_mark);
  
  return text;
}

ant_value_t js_error_record_stack_text(ant_t *js, ant_value_t header, ant_value_t record) {
  error_record_t *rec = error_record_of(record);
  errbuf_t eb;
  
  if (!rec || !errbuf_init(&eb, 1024)) return Ant_Value_Undefined;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, header);
  GC_ROOT_PIN(js, record);

  size_t len = 0;
  const char *text = js_getstr(js, header, &len);
  size_t n = text ? errbuf_append(&eb, 0, text, len) : 0;

  error_render_t r;
  error_render_begin(js, rec, &r);
  error_render_frames(js, &r, &eb, &n, ERROR_FRAME_PLAIN);
  error_render_end(&r);

  ant_value_t stack = errbuf_take_string(js, &eb, n);
  GC_ROOT_RESTORE(js, root_mark);
  
  return stack;
}

enum { 
  CS_FILE = 0,
  CS_LINE,
  CS_COL,
  CS_NAME,
  CS_CONSTRUCT
};

static ant_value_t callsite_field(ant_t *js, ant_value_t call_this, int field) {
  ant_value_t data = js_get_slot(call_this, SLOT_DATA);
  if (vtype(data) != kTypeArray) return Ant_Value_Undefined;
  return js_arr_get(js, data, field);
}

static ant_value_t callsite_getFileName(ant_params_t)     { return callsite_field(js, call_this, CS_FILE); }
static ant_value_t callsite_getLineNumber(ant_params_t)   { return callsite_field(js, call_this, CS_LINE); }
static ant_value_t callsite_getColumnNumber(ant_params_t) { return callsite_field(js, call_this, CS_COL); }
static ant_value_t callsite_getFunctionName(ant_params_t) { return callsite_field(js, call_this, CS_NAME); }
static ant_value_t callsite_isConstructor(ant_params_t)   { return Ant_Value_FromBool(js_truthy(js, callsite_field(js, call_this, CS_CONSTRUCT))); }

static ant_value_t callsite_null(ant_params_t)      { return Ant_Value_Null; }
static ant_value_t callsite_false(ant_params_t)     { return Ant_Value_False; }
static ant_value_t callsite_undefined(ant_params_t) { return Ant_Value_Undefined; }

static ant_value_t callsite_toString(ant_params_t) {
  ant_value_t name = callsite_field(js, call_this, CS_NAME);
  ant_value_t file = callsite_field(js, call_this, CS_FILE);
  ant_value_t line = callsite_field(js, call_this, CS_LINE);
  ant_value_t col  = callsite_field(js, call_this, CS_COL);
  bool construct = js_truthy(js, callsite_field(js, call_this, CS_CONSTRUCT));

  const char *n = "", *f = "";
  size_t n_len = 0, f_len = 0;
  
  if (vtype(name) == kTypeString) n = js_getstr(js, name, &n_len);
  if (vtype(file) == kTypeString) f = js_getstr(js, file, &f_len);
  int l = vtype(line) == kTypeNumber ? (int)Ant_Value_AsNumberUnchecked(line) : 0;
  int c = vtype(col)  == kTypeNumber ? (int)Ant_Value_AsNumberUnchecked(col)  : 0;

  errbuf_t eb;
  if (!errbuf_init(&eb, 256)) return Ant_Error_CreateOutOfMemory(js);
  size_t len = errbuf_appendf(&eb, 0, "%s%.*s (%.*s:%d:%d)", construct ? "new " : "", (int)n_len, n, (int)f_len, f, l, c);
  
  return errbuf_take_string(js, &eb, len);
}

static const ant_cfunc_meta_t callsite_methods[] = {
  { callsite_getFileName, "getFileName", 0, 0 },
  { callsite_getLineNumber, "getLineNumber", 0, 0 },
  { callsite_getColumnNumber, "getColumnNumber", 0, 0 },
  { callsite_getFunctionName, "getFunctionName", 0, 0 },
  { callsite_null, "getTypeName", 0, 0 },
  { callsite_getFunctionName, "getMethodName", 0, 0 },
  { callsite_false, "isNative", 0, 0 },
  { callsite_false, "isToplevel", 0, 0 },
  { callsite_false, "isEval", 0, 0 },
  { callsite_isConstructor, "isConstructor", 0, 0 },
  { callsite_undefined, "getEvalOrigin", 0, 0 },
  { callsite_undefined, "getThis", 0, 0 },
  { callsite_toString, "toString", 0, 0 },
};

static ant_value_t callsite_proto(ant_t *js) {
  if (vtype(js->builtins.callsite_proto) == kTypeObject) return js->builtins.callsite_proto;
  
  ant_value_t proto = Ant_Object_CreateBare(js);
  js->builtins.callsite_proto = proto;
  
  for (size_t i = 0; i < sizeof(callsite_methods) / sizeof(callsite_methods[0]); i++)
    js_set(js, proto, callsite_methods[i].name, js_mkfun_meta(&callsite_methods[i]));
  
  return proto;
}

ant_value_t js_build_callsite_array(ant_t *js, ant_value_t call_func, ant_value_t above) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t proto = callsite_proto(js);
  ant_value_t arr = Ant_Array_Create(js);
  GC_ROOT_PIN(js, arr);

  error_frame_t local[ERROR_FRAMES_INLINE], *frames;
  int count = error_collect_frames(js, error_stack_trace_limit(js), error_above_of(call_func, above), true, local, &frames);
  const char *file = js->filename ? js->filename : "<eval>";

  ant_value_t data = Ant_Value_Undefined, site = Ant_Value_Undefined;
  GC_ROOT_PIN(js, data);
  GC_ROOT_PIN(js, site);

  for (int i = 0; i < count; i++) {
    error_frame_view_t view = error_frame_view(js, &frames[i], file);
    data = Ant_Array_Create(js);
    js_arr_push(js, data, js_mkstr(js, view.file, strlen(view.file)));
    js_arr_push(js, data, Ant_Value_FromNumber((double)view.line));
    js_arr_push(js, data, Ant_Value_FromNumber((double)view.col));
    js_arr_push(js, data, js_mkstr(js, view.name, strlen(view.name)));
    js_arr_push(js, data, Ant_Value_FromBool(view.construct));

    site = Ant_Object_CreateBare(js);
    js_set_proto_init(site, proto);
    js_set_slot(site, SLOT_DATA, data);
    js_arr_push(js, arr, site);
  }
  
  if (frames != local) free(frames);
  GC_ROOT_RESTORE(js, root_mark);
  
  return arr;
}
