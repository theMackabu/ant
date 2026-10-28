// TODO: split into multiple module files

#include "tier.h"
#include "build/build.h"
#include "codegen/codegen.h"
#include "ir/ir_value.h"
#include "passes/passes.h"

#include "../jit_internal.h"
#include "gc.h"
#include "internal.h"
#include "proto_watch.h"
#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/generic_step.h"
#include "silver/glue.h"
#include "silver/upvalues.h"
#include "silver/vm.h"
#include "silver/jit.h"
#include "gc/objects.h"
#include "modules/regex.h"

#include "debug.h"
#include <time.h>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

static constexpr uint32_t OPT_MAX_COMPILES = 8;
static constexpr uint32_t OPT_MAX_LEARNING_COMPILES = 64;
static constexpr uint32_t OPT_COMPILE_BYTES = 1024;

static uint32_t opt_max_compiles(const sv_func_t *func) {
  return OPT_MAX_COMPILES + (uint32_t)func->code_len / OPT_COMPILE_BYTES;
}

typedef struct {
  uint32_t bc_off;
  uint32_t count;
  opt_field_memo_case_t cases[OPT_FIELD_MEMO_CASES];
} opt_field_memo_t;

typedef struct opt_func_state {
  struct opt_func_state *prev, *next;
  ir_exit_site_t *exit_sites;
  uint32_t exit_site_count, exit_site_cap;
  uint32_t exits;
  uint32_t compiles;
  uint32_t fruitless;
  bool disabled;
  opt_field_memo_t *memos;
  uint32_t memo_count, memo_cap;
  opt_code_t *codes;
} opt_func_state_t;

struct opt_state {
  opt_func_state_t *states;
  asm_code_heap_t *heap;
  opt_code_t **scan;
  uint32_t scan_len, scan_cap;
};

static _Thread_local uint64_t opt_codes_freed;

#if defined(__x86_64__)
static bool opt_host_supported(void) {
  static int supported = -1;
  if (supported < 0) {
    unsigned eax, ebx, ecx, edx;
    supported = __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_SSE4_1);
  }
  return supported;
}
#else
static bool opt_host_supported(void) {
  return true;
}
#endif

bool opt_tier_enabled(void) {
  return OPT_TIER_SUPPORTED && opt_host_supported() && !sv_debug_enabled(SV_DEBUG_OPT_OFF);
}

static bool opt_verify_enabled(void) {
  return sv_debug_enabled(SV_DEBUG_OPT_VERIFY);
}

static bool opt_stats_enabled(void) {
  return sv_debug_enabled(SV_DEBUG_DUMP_OPT_STATS);
}

static _Thread_local uint64_t opt_generic_step_counts[OP__COUNT];

static void opt_print_generic_counts(void) {
  for (uint32_t shown = 0; shown < 12; shown++) {
    uint32_t top = 0;
    for (uint32_t op = 1; op < OP__COUNT; op++)
      if (opt_generic_step_counts[op] > opt_generic_step_counts[top]) top = op;
    if (!opt_generic_step_counts[top]) break;
    fprintf(stderr, "jit: opt generic steps op=%s count=%llu\n", sv_op_names[top], (unsigned long long)opt_generic_step_counts[top]);
    opt_generic_step_counts[top] = 0;
  }
}

typedef struct {
  const uint8_t *ip;
  uint64_t count;
  uint32_t bc_off, line;
  uint8_t op;
  char name[40];
} opt_generic_site_t;

static _Thread_local opt_generic_site_t opt_generic_sites[256];

static void opt_count_generic_site(sv_func_t *func, const uint8_t *ip) {
  uint32_t h = (uint32_t)(((uintptr_t)ip >> 2) * 0x9E3779B1u) & 255;
  for (uint32_t i = 0; i < 256; i++, h = (h + 1) & 255) {
    opt_generic_site_t *site = &opt_generic_sites[h];
    if (site->ip != ip && site->ip) continue;
    if (!site->ip) {
      uint32_t col = 0;
      site->ip = ip;
      snprintf(site->name, sizeof site->name, "%s", func->debug && func->debug->name ? func->debug->name : "<anonymous>");
      site->op = *ip;
      site->bc_off = (uint32_t)(ip - func->code);
      (void)sv_lookup_srcpos(func, (int)site->bc_off, &site->line, &col);
    }
    site->count++;
    return;
  }
}

static void opt_print_generic_sites(void) {
  for (uint32_t shown = 0; shown < 40; shown++) {
    opt_generic_site_t *top = NULL;
    for (uint32_t i = 0; i < 256; i++)
      if (opt_generic_sites[i].count && (!top || opt_generic_sites[i].count > top->count)) top = &opt_generic_sites[i];
    if (!top) break;
    fprintf(stderr, "jit: opt generic site op=%s func=%s bc=%u line=%u count=%llu\n", sv_op_names[top->op], top->name,
      top->bc_off, top->line, (unsigned long long)top->count);
    top->count = 0;
  }
}

static void opt_count_step(sv_func_t *func, const uint8_t *ip) {
  opt_generic_step_counts[*ip]++;
  opt_count_generic_site(func, ip);
}

static ant_value_t opt_rt_generic_step_counted(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip, uint32_t depth) {
  opt_count_step(frame->func, ip);
  return sv_generic_step(vm, frame, ip, depth);
}

uintptr_t opt_tier_generic_step(void) {
  return opt_stats_enabled() ? (uintptr_t)opt_rt_generic_step_counted : (uintptr_t)sv_generic_step;
}

static const char *opt_func_name(const sv_func_t *func) {
  return func->debug && func->debug->name ? func->debug->name : "<anonymous>";
}

static uint32_t opt_func_line(sv_func_t *func) {
  uint32_t line = 0, col = 0;
  (void)sv_lookup_srcpos(func, 0, &line, &col);
  return line;
}

typedef struct {
  uint32_t total;
  uint32_t primary;
  uint32_t in_loops;
  uint16_t first;
} opt_generic_counts_t;

static opt_generic_counts_t opt_generic_counts(const ir_graph_t *graph) {
  opt_generic_counts_t counts = {.first = OP__COUNT};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->body.len; i++) {
      const ir_op_t *op = &ir_node(graph, data->body.items[i])->op;
      if (op->code != IR_OP_GENERIC) continue;
      counts.total++;
      if (data->is_cold) continue;
      if (!counts.primary++) counts.first = op->slow.opcode;
      if (op->slow.in_loop && !counts.in_loops++) counts.first = op->slow.opcode;
    }
  }
  return counts;
}

static const char *opt_unprofitable(const opt_generic_counts_t *counts, bool sole_tier) {
  if (sole_tier || sv_debug_enabled(SV_DEBUG_OPT_FORCE)) return NULL;
  if (counts->in_loops) return "generic-in-hot-path";
  return counts->primary ? "generic" : NULL;
}

static struct opt_state *opt_state_of(sv_jit_ctx_t *jc) {
  if (jc && !jc->opt) jc->opt = calloc(1, sizeof *jc->opt);
  return jc ? jc->opt : NULL;
}

static opt_func_state_t *opt_func_state(ant_t *js, sv_func_t *func, bool create) {
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  if (sidecar && sidecar->opt_state) return sidecar->opt_state;
  struct opt_state *all = create ? opt_state_of(js->jit_ctx) : NULL;
  if (!all || !(sidecar = sv_func_ensure_sidecar(func))) return NULL;
  opt_func_state_t *state = calloc(1, sizeof *state);
  if (!state) return NULL;
  state->next = all->states;
  if (all->states) all->states->prev = state;
  all->states = state;
  sidecar->opt_state = state;
  return state;
}

opt_code_t *opt_code_new(sv_func_t *func) {
  opt_code_t *code = calloc(1, sizeof *code);
  if (!code) opt_out_of_memory();
  code->func = func;
  return code;
}

static void opt_code_invalidate_dependent(void *code) {
  opt_code_invalidate(code);
}

void opt_code_free(opt_code_t *code) {
  if (!code) return;
  for (uint32_t i = 0; i < code->watch_count; i++) ant_proto_watch_forget(code->watches[i], code);
  free(code->watches);
  asm_executable_free(&code->executable);
  free(code->sites);
  free(code->values);
  free(code->chunks);
  free(code->refs);
  free(code->frames);
  free(code->call_sites);
  free(code->roots);
  free(code->funcs);
  free(code->handlers);
  free(code->patches);
  free(code);
}

static void opt_free_codes(opt_func_state_t *func_state) {
  for (opt_code_t *code = func_state->codes, *next; code; code = next) {
    next = code->func_next;
    opt_code_free(code);
  }
  func_state->codes = NULL;
}

void opt_tier_destroy(sv_jit_ctx_t *jc) {
  if (!jc || !jc->opt) return;
  struct opt_state *state = jc->opt;
  for (opt_func_state_t *func_state = state->states; func_state; func_state = func_state->next) {
    for (opt_code_t *code = func_state->codes; code; code = code->func_next) code->executable = (asm_executable_t){0};
    opt_free_codes(func_state);
  }
  asm_code_heap_destroy(state->heap);
  free(state->scan);
  free(state);
  jc->opt = NULL;
}

static bool opt_hold_roots(opt_code_t *code, const ir_graph_t *graph) {
  if (graph->retained.len) {
    code->roots = malloc(graph->retained.len * sizeof *code->roots);
    if (!code->roots) return false;
    memcpy(code->roots, graph->retained.items, graph->retained.len * sizeof *code->roots);
    code->root_count = graph->retained.len;
  }
  if (graph->retained_funcs.len) {
    code->funcs = malloc(graph->retained_funcs.len * sizeof *code->funcs);
    if (!code->funcs) return false;
    memcpy(code->funcs, graph->retained_funcs.items, graph->retained_funcs.len * sizeof *code->funcs);
    code->func_count = graph->retained_funcs.len;
  }
  if (graph->dependencies.len) {
    code->watches = malloc(graph->dependencies.len * sizeof *code->watches);
    if (!code->watches) return false;
    for (uint32_t i = 0; i < graph->dependencies.len; i++) {
      ant_proto_watch_t *watch = (ant_proto_watch_t *)(uintptr_t)graph->dependencies.items[i].a;
      if (!ant_proto_watch_depend(watch, code, opt_code_invalidate_dependent)) return false;
      code->watches[code->watch_count++] = watch;
    }
  }
  return true;
}

void opt_tier_func_free(ant_t *js, sv_func_t *func) {
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  opt_func_state_t *func_state = sidecar ? sidecar->opt_state : NULL;
  if (!func_state) return;
  sv_jit_ctx_t *jc = js->jit_ctx;
  struct opt_state *state = jc ? jc->opt : NULL;
  if (state) {
    if (func_state->prev) func_state->prev->next = func_state->next;
    else state->states = func_state->next;
    if (func_state->next) func_state->next->prev = func_state->prev;
  }
  for (opt_code_t *code = func_state->codes; code; code = code->func_next) opt_codes_freed++;
  opt_free_codes(func_state);
  free(func_state->exit_sites);
  free(func_state->memos);
  free(func_state);
  sidecar->opt_state = NULL;
}

static void opt_mark_code(ant_t *js, opt_code_t *code, bool weak) {
  for (uint32_t i = 0; i < code->func_count; i++) gc_mark_func_ref(js, code->funcs[i]);
  for (uint32_t i = 0; i < code->root_count; i++)
    if (!weak || !gc_value_is_weak(code->roots[i])) gc_mark_value(js, code->roots[i]);
}

void opt_tier_func_trace(ant_t *js, sv_func_t *func) {
  opt_func_state_t *func_state = sv_func_sidecar(func)->opt_state;
  for (opt_code_t *code = func_state->codes; code; code = code->func_next) opt_mark_code(js, code, true);
}

static int opt_code_address_order(const void *a, const void *b) {
  uintptr_t x = (uintptr_t)(*(opt_code_t *const *)a)->executable.address;
  uintptr_t y = (uintptr_t)(*(opt_code_t *const *)b)->executable.address;
  return x < y ? -1 : x > y;
}

__attribute__((no_sanitize("address")))
static void opt_scan_stack_range(uintptr_t from, uintptr_t to, opt_code_t **codes, uint32_t count, uint64_t epoch) {
  uintptr_t lo = from < to ? from : to, hi = from < to ? to : from;
  uintptr_t first = (uintptr_t)codes[0]->executable.address;
  uintptr_t last = (uintptr_t)codes[count - 1]->executable.address + codes[count - 1]->executable.size;
  for (uintptr_t at = (lo + 7) & ~(uintptr_t)7; at + sizeof(uintptr_t) <= hi; at += sizeof(uintptr_t)) {
    uintptr_t word = *(const uintptr_t *)at;
    if (word < first || word >= last) continue;
    uint32_t low = 0, high = count;
    while (low < high) {
      uint32_t mid = (low + high) / 2;
      if ((uintptr_t)codes[mid]->executable.address <= word) low = mid + 1;
      else high = mid;
    }
    if (low && word < (uintptr_t)codes[low - 1]->executable.address + codes[low - 1]->executable.size) codes[low - 1]->active_epoch = epoch;
  }
}

static bool opt_scan_reserve(struct opt_state *state, uint32_t count) {
  if (count < state->scan_cap) return true;
  uint32_t cap = state->scan_cap ? state->scan_cap * 2 : 64;
  opt_code_t **scan = realloc(state->scan, cap * sizeof *scan);
  if (!scan) return false;
  state->scan = scan;
  state->scan_cap = cap;
  return true;
}

void opt_tier_gc_mark_roots(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  struct opt_state *state = jc ? jc->opt : NULL;
  if (!state) return;
  uint64_t epoch = js->gc.epoch;
  uint32_t count = 0;
  bool scan = js->cstk.base != NULL;
  for (opt_func_state_t *func_state = state->states; func_state; func_state = func_state->next)
    for (opt_code_t *code = func_state->codes; code && scan; code = code->func_next)
      if ((scan = opt_scan_reserve(state, count))) state->scan[count++] = code;
  if (!scan) {
    count = 0;
    for (opt_func_state_t *func_state = state->states; func_state; func_state = func_state->next)
      for (opt_code_t *code = func_state->codes; code; code = code->func_next) {
        code->active_epoch = epoch;
        gc_mark_func_ref(js, code->func);
        opt_mark_code(js, code, false);
      }
    state->scan_len = 0;
    return;
  }
  state->scan_len = count;
  if (!count) return;
  qsort(state->scan, count, sizeof *state->scan, opt_code_address_order);
  opt_scan_stack_range(gc_native_sp(), (uintptr_t)js->cstk.base, state->scan, count, epoch);
  if (js->cstk.main_base && js->cstk.main_lo)
    opt_scan_stack_range((uintptr_t)js->cstk.main_lo, (uintptr_t)js->cstk.main_base, state->scan, count, epoch);
  for (uint32_t i = 0; i < count; i++) {
    opt_code_t *code = state->scan[i];
    if (code->active_epoch != epoch) continue;
    gc_mark_func_ref(js, code->func);
    opt_mark_code(js, code, true);
  }
}

static bool opt_code_roots_live(ant_t *js, const opt_code_t *code) {
  for (uint32_t i = 0; i < code->root_count; i++)
    if (!gc_value_is_marked(js, code->roots[i])) return false;
  return true;
}

static void opt_drop_code(opt_code_t *code);

static void opt_drop_dead(opt_func_state_t *state, opt_code_t *code) {
  if (code->func->jit_code != code->entry) return;
  if (state) state->fruitless++;
  if (sv_jit_warn_unlikely) fprintf(stderr, "jit: opt dropped func=%s reason=dead-value\n", opt_func_name(code->func));
  opt_drop_code(code);
}

static void opt_weak_codes(ant_t *js, opt_func_state_t *func_state) {
  uint64_t epoch = js->gc.epoch;
  for (opt_code_t **link = &func_state->codes; *link;) {
    opt_code_t *code = *link;
    bool keep = code->active_epoch == epoch || sv_code_unit_dying(code->func->unit, epoch);
    if (!keep && code->func->jit_code == code->entry) {
      keep = opt_code_roots_live(js, code);
      if (!keep) opt_drop_dead(func_state, code);
    }
    if (keep) {
      code->slow_path_runs >>= 1;
      link = &code->func_next;
      continue;
    }
    *link = code->func_next;
    jit_code_unregister(js->jit_ctx, (const void *)code->entry);
    opt_code_free(code);
    opt_codes_freed++;
  }
}

static void opt_prune_memos(ant_t *js, opt_func_state_t *func_state) {
  for (uint32_t i = 0; i < func_state->memo_count; i++) {
    opt_field_memo_t *memo = &func_state->memos[i];
    uint32_t kept = 0;
    for (uint32_t k = 0; k < memo->count; k++) {
      const opt_field_memo_case_t *seen = &memo->cases[k];
      bool holder_live = seen->holder && (seen->holder->flags.gc_permanent || gc_obj_is_marked(js, seen->holder));
      if (!seen->holder || (holder_live && gc_value_is_marked(js, seen->proto))) memo->cases[kept++] = *seen;
    }
    memo->count = kept;
  }
}

void opt_tier_gc_doom(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  struct opt_state *state = jc ? jc->opt : NULL;
  if (!state) return;
  uint64_t epoch = js->gc.epoch;
  for (uint32_t i = 0; i < state->scan_len; i++) {
    opt_code_t *code = state->scan[i];
    if (code->active_epoch == epoch) opt_mark_code(js, code, false);
  }
}

void opt_tier_gc_weak(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  struct opt_state *state = jc ? jc->opt : NULL;
  if (!state) return;
  for (opt_func_state_t *func_state = state->states; func_state; func_state = func_state->next) {
    opt_prune_memos(js, func_state);
    if (func_state->codes) opt_weak_codes(js, func_state);
  }
  state->scan_len = 0;
}

static bool opt_record_exit_site(opt_func_state_t *state, uint32_t bc_off, opt_exit_kind_t kind, uint32_t slot) {
  for (uint32_t i = 0; i < state->exit_site_count; i++) {
    const ir_exit_site_t *site = &state->exit_sites[i];
    if (site->bc_off == bc_off && site->kind == kind && site->slot == slot) return false;
  }
  if (state->exit_site_count == state->exit_site_cap) {
    uint32_t cap = state->exit_site_cap ? state->exit_site_cap * 2 : 8;
    ir_exit_site_t *sites = realloc(state->exit_sites, cap * sizeof *sites);
    if (!sites) return false;
    state->exit_sites = sites;
    state->exit_site_cap = cap;
  }
  state->exit_sites[state->exit_site_count++] = (ir_exit_site_t){.bc_off = bc_off, .kind = kind, .slot = slot};
  return true;
}

static bool opt_exit_learned(opt_func_state_t *state, const sv_func_t *func, uint32_t bc_off, opt_exit_kind_t kind, uint32_t slot) {
  if (kind == OPT_EXIT_INVALIDATED) return false;
  bool recorded = opt_record_exit_site(state, bc_off, kind, slot);
  return recorded || (kind == OPT_EXIT_BAD_SHAPE && slot && opt_learned_shapes(func, bc_off) >= slot);
}

static void opt_note_discard(opt_func_state_t *state, const opt_code_t *code, bool learned) {
  if (!state || code->dropped) return;
  state->fruitless = learned ? 0 : state->fruitless + 1;
}

static opt_field_memo_t *opt_field_memo(opt_func_state_t *state, uint32_t bc_off, bool create) {
  for (uint32_t i = 0; i < state->memo_count; i++)
    if (state->memos[i].bc_off == bc_off) return &state->memos[i];
  if (!create) return NULL;
  if (state->memo_count == state->memo_cap) {
    uint32_t cap = state->memo_cap ? state->memo_cap * 2 : 8;
    opt_field_memo_t *memos = realloc(state->memos, cap * sizeof *memos);
    if (!memos) return NULL;
    state->memos = memos;
    state->memo_cap = cap;
  }
  opt_field_memo_t *memo = &state->memos[state->memo_count++];
  *memo = (opt_field_memo_t){.bc_off = bc_off};
  return memo;
}

uint32_t opt_tier_field_memo(ant_t *js, sv_func_t *func, uint32_t bc_off, opt_field_memo_case_t *out) {
  opt_func_state_t *state = opt_func_state(js, func, false);
  opt_field_memo_t *memo = state ? opt_field_memo(state, bc_off, false) : NULL;
  if (!memo) return 0;
  memcpy(out, memo->cases, memo->count * sizeof *out);
  return memo->count;
}

void opt_tier_field_remember(ant_t *js, sv_func_t *func, uint32_t bc_off, const opt_field_memo_case_t *cases, uint32_t count) {
  opt_func_state_t *state = opt_func_state(js, func, true);
  opt_field_memo_t *memo = state ? opt_field_memo(state, bc_off, true) : NULL;
  if (!memo) return;
  memo->count = count < OPT_FIELD_MEMO_CASES ? count : OPT_FIELD_MEMO_CASES;
  memcpy(memo->cases, cases, memo->count * sizeof *cases);
}

static const ir_exit_site_t *opt_exit_sites_of(ant_t *js, sv_func_t *func, uint32_t *count) {
  opt_func_state_t *state = opt_func_state(js, func, false);
  *count = state ? state->exit_site_count : 0;
  return state ? state->exit_sites : NULL;
}

static _Thread_local struct {
  uint64_t compiled, inlined_calls, exits, inline_exits;
  uint64_t compiled_ns, rejected_ns, rejected;
  uint64_t code_bytes, mapped;
  uint64_t drops, catches, catch_leaves, catch_entries, unwinds;
} opt_stats;

static void opt_print_stats(void) {
  opt_print_generic_counts();
  opt_print_generic_sites();
  fprintf(stderr, "jit: opt stats compiled=%llu inlined_calls=%llu exits=%llu inline_exits=%llu compile_ms=%.1f rejected=%llu rejected_ms=%.1f code_bytes=%llu freed=%llu mapped_peak=%llu\n",
    (unsigned long long)opt_stats.compiled, (unsigned long long)opt_stats.inlined_calls,
    (unsigned long long)opt_stats.exits, (unsigned long long)opt_stats.inline_exits,
    (double)opt_stats.compiled_ns / 1e6, (unsigned long long)opt_stats.rejected, (double)opt_stats.rejected_ns / 1e6,
    (unsigned long long)opt_stats.code_bytes, (unsigned long long)opt_codes_freed, (unsigned long long)opt_stats.mapped);
  fprintf(stderr, "jit: opt catch stats drops=%llu caught_in_code=%llu caught_in_interpreter=%llu interpreter_entries=%llu unwinds=%llu\n",
    (unsigned long long)opt_stats.drops, (unsigned long long)opt_stats.catches,
    (unsigned long long)opt_stats.catch_leaves, (unsigned long long)opt_stats.catch_entries,
    (unsigned long long)opt_stats.unwinds);
}

static void opt_drop_code(opt_code_t *code) {
  sv_func_t *func = code->func;
  if (!code->dropped) opt_stats.drops++;
  code->dropped = true;
  if (func->jit_code == code->entry) {
    func->jit_code = NULL;
    func->jit_code_cold = false;
    func->jit_code_retier = false;
  }
  func->back_edge_count = 0;
  func->call_count = SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY;
}

void opt_code_invalidate(opt_code_t *code) {
  if (code->invalidated) return;
  code->invalidated = true;
  for (uint32_t i = 0; i < code->patch_count; i++) {
    const opt_patch_t *patch = &code->patches[i];
    if (!asm_executable_patch(&code->executable, patch->offset, patch->bytes, patch->len)) opt_out_of_memory();
  }
  opt_drop_code(code);
}

static void opt_warn_exit(const opt_func_state_t *state, sv_func_t *func, uint32_t bc_off, opt_exit_kind_t kind, uint32_t slot, const opt_code_t *code, uint32_t depth) {
  const char *op_name = "entry";
  if (bc_off < (uint32_t)func->code_len) {
    uint8_t op = func->code[bc_off];
    if (op < OP__COUNT && sv_op_names[op]) op_name = sv_op_names[op];
  }
  uint32_t line = 0, col = 0;
  (void)sv_lookup_srcpos(func, (int)bc_off, &line, &col);
  fprintf(stderr,
    "jit: bailout %u/%u tfb=%u func=%s op=%s bc=%u at %s:%u:%u reason=opt-exit kind=%s",
    state ? state->exits : 0, opt_max_compiles(func), func->tfb_version, opt_func_name(func), op_name, bc_off,
    func->debug && func->debug->filename ? func->debug->filename : "<unknown>", line, col,
    opt_exit_kind_name(kind)
  );
  if (slot && kind != OPT_EXIT_BAD_SHAPE) fprintf(stderr, " slot=%u", slot);
  if (depth) fprintf(stderr, " inlined_in=%s depth=%u", opt_func_name(code->func), depth);
  fputc('\n', stderr);
}

ant_value_t opt_rt_enter_frame(sv_vm_t *vm, opt_entry_t *entry, const opt_code_t *code) {
  ant_t *js = vm->js;
  sv_func_t *func = code->func;
  sv_closure_t *closure = entry->closure;
  int argc = (int)entry->argc;
  int arg_slots = argc > func->param_count ? argc : func->param_count;
  int need = arg_slots + func->max_locals;
  bool room = vm->fp + 1 < vm->max_frames && vm->sp + need + func->max_stack <= vm->stack_size;
  if (!room && !sv_vm_reserve(vm, need + func->max_stack))
    return Ant_Error_CreateStackOverflow(js);
  ant_value_t this_val = sv_normalize_this_for_frame(js, func, entry->this_val);
  int prev_sp = vm->sp;
  ant_value_t *bp, *lp;
  ant_value_t staged = sv_vm_stage_frame(vm, func, entry->args, argc, &bp, &lp);
  if (is_err(staged)) return staged;

  sv_frame_t *frame = &vm->frames[vm->fp + 1];
  frame->ip = func->code;
  frame->bp = bp;
  frame->lp = lp;
  frame->func = func;
  frame->callee = sv_closure_heap_value(js, closure);
  frame->this = this_val;
  frame->new_target = entry->new_target;
  frame->super_val = entry->super_val;
  frame->prev_sp = prev_sp;
  frame->argc = argc;
  frame->completion.kind = SV_COMPLETION_NONE;
  frame->completion.value = js_mkundef();
  frame->upvalues = closure->upvalues;
  frame->upvalue_count = func->upvalue_count;
  frame->handler_base = (uint16_t)vm->handler_depth;
  frame->handler_top = (uint16_t)vm->handler_depth;
  frame->arguments_obj = js_mkundef();
  frame->eval_env = sv_closure_eval_env(closure);
  vm->fp++;

  entry->seg = (gc_vm_seg_t){.prev = js->vm_segs, .jit_depth = UINT32_MAX, .entry_vm_fp = vm->fp};
  js->vm_segs = &entry->seg;
  entry->frame = frame;
  entry->locals = lp;
  entry->params = bp;
  entry->owns_frame = true;
  return 0;
}

void opt_rt_borrow_frame(sv_vm_t *vm, opt_entry_t *entry) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  entry->frame = frame;
  entry->locals = frame->lp;
  entry->owns_frame = false;
}

ant_value_t opt_rt_leave_frame(ant_value_t result, sv_vm_t *vm, opt_entry_t *entry) {
  sv_frame_t *frame = entry->frame;
  if (!frame) return result;
  ant_t *js = vm->js;
  if (vm->open_upvalues) sv_close_upvalues_from_slot(vm, frame->bp);
  if (vtype(frame->arguments_obj) != kTypeUndefined) {
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, result);
    js_arguments_detach(js, frame->arguments_obj);
    frame->arguments_obj = js_mkundef();
    GC_ROOT_RESTORE(js, mark);
  }
  if (entry->owns_frame) {
    vm->sp = frame->prev_sp;
    vm->fp--;
    js->vm_segs = entry->seg.prev;
  }
  entry->frame = NULL;
  return result;
}

ant_value_t opt_rt_exit(
  sv_vm_t *vm, opt_code_t *code, const opt_site_t *site, opt_entry_t *entry,
  ant_value_t *params, ant_value_t *locals, ant_value_t *stack, sv_jit_finally_t *finally
) {
  sv_func_t *func = code->func;
  if (site->kind == OPT_EXIT_UNWIND) {
    opt_stats.unwinds++;
    return opt_rt_resume(vm, code, entry, site->bc_off, site->depth, params, locals, stack, finally);
  }
  opt_func_state_t *state = opt_func_state(vm->js, func, true);
  if (state) {
    opt_note_discard(state, code, opt_exit_learned(state, func, site->bc_off, site->kind, site->slot));
    state->exits++;
  }
  opt_stats.exits++;
  opt_drop_code(code);
  if (sv_jit_warn_unlikely) opt_warn_exit(state, func, site->bc_off, site->kind, site->slot, code, 0);
  return opt_rt_resume(vm, code, entry, site->bc_off, site->depth, params, locals, stack, finally);
}

ant_value_t opt_rt_resume(
  sv_vm_t *vm, opt_code_t *code, opt_entry_t *entry, uint32_t bc_off, uint32_t depth,
  ant_value_t *params, ant_value_t *locals, ant_value_t *stack, sv_jit_finally_t *finally
) {
  ant_value_t arguments = vm->jit.resume.arguments_obj;
  if (entry->frame) {
    sv_frame_t *frame = entry->frame;
    sv_rebase_open_upvalues(&vm->open_upvalues, frame->bp, params, code->param_count);
    sv_rebase_open_upvalues(&vm->open_upvalues, frame->lp, locals, code->local_count);
    if (is_object_type(frame->arguments_obj)) arguments = frame->arguments_obj;
    frame->arguments_obj = js_mkundef();
    opt_rt_leave_frame(0, vm, entry);
  }
  vm->jit.resume.arguments_obj = arguments;
  return jit_resume_in_interpreter(
    vm, entry->closure, entry->this_val, entry->new_target, entry->super_val,
    entry->args, (int)entry->argc, stack, depth, params, code->param_count,
    locals, code->local_count, bc_off, false, finally
  );
}

static bool opt_push_waiting_frame(sv_vm_t *vm, sv_func_t *func, ant_value_t callee, uint32_t call_off) {
  if (vm->fp + 1 >= vm->max_frames && !sv_vm_reserve(vm, 0)) return false;
  sv_frame_t *frame = &vm->frames[++vm->fp];
  ant_value_t undefined = js_mkundef();
  *frame = (sv_frame_t){
    .ip = func->code + call_off + 1,
    .bp = &vm->stack[vm->sp],
    .lp = &vm->stack[vm->sp],
    .func = func,
    .callee = callee,
    .this = undefined,
    .new_target = undefined,
    .super_val = undefined,
    .prev_sp = vm->sp,
    .completion = {.kind = SV_COMPLETION_NONE, .value = undefined},
    .handler_base = (uint16_t)vm->handler_depth,
    .handler_top = (uint16_t)vm->handler_depth,
    .arguments_obj = undefined,
    .eval_env = undefined,
  };
  return true;
}

ant_value_t opt_rt_exit_frames(sv_vm_t *vm, opt_code_t *code, const opt_site_t *site, opt_entry_t *entry, opt_exit_frame_t *frames, uint32_t count) {
  ant_t *js = vm->js;
  sv_func_t *func = code->func;
  const opt_site_frame_t *inner = frames[0].site;
  opt_func_state_t *state = opt_func_state(js, frames[0].func, true);
  bool learned = state && opt_exit_learned(state, frames[0].func, inner->bc_off, site->kind, site->slot);
  opt_func_state_t *root_state = opt_func_state(js, func, true);
  opt_note_discard(root_state, code, learned);
  if (root_state) root_state->exits++;
  opt_stats.exits++;
  opt_stats.inline_exits++;
  opt_drop_code(code);
  if (sv_jit_warn_unlikely) opt_warn_exit(state, frames[0].func, inner->bc_off, site->kind, site->slot, code, count - 1);

  opt_exit_frame_t *root = &frames[count - 1];
  uint32_t params = code->param_count, locals = code->local_count;
  ant_value_t arguments = root->arguments;
  bool borrowed = entry->frame && !entry->owns_frame;
  if (entry->frame) {
    sv_frame_t *frame = entry->frame;
    sv_rebase_open_upvalues(&vm->open_upvalues, frame->bp, root->params, params);
    sv_rebase_open_upvalues(&vm->open_upvalues, frame->lp, root->locals, locals);
    if (is_object_type(frame->arguments_obj)) arguments = frame->arguments_obj;
    frame->arguments_obj = js_mkundef();
    opt_rt_leave_frame(0, vm, entry);
  }

  gc_vm_seg_t seg = {.prev = js->vm_segs, .jit_depth = UINT32_MAX - 1, .entry_vm_fp = vm->fp + 1};
  js->vm_segs = &seg;
  bool waits[count];
  bool room = true;
  for (uint32_t k = count; k-- > 1;) {
    ant_value_t callee = frames[k].callee;
    if (k == count - 1) callee = sv_closure_heap_value(js, entry->closure);
    waits[k] = room && !(k == count - 1 && borrowed) && opt_push_waiting_frame(vm, frames[k].func, callee, frames[k].site->call_off);
    room &= waits[k] || (k == count - 1 && borrowed);
  }

  ant_value_t result = js_mkundef();
  for (uint32_t k = 0; k < count; k++) {
    opt_exit_frame_t *frame = &frames[k];
    const opt_site_frame_t *at = frame->site;
    uint32_t bc_off = at->bc_off, depth = at->depth;
    bool throwing = false;
    if (k > 0) {
      if (waits[k]) vm->fp--;
      if (is_err(result)) {
        uint32_t resume, handler_depth;
        if (js->terminating || !sv_func_catch_at(frame->func, at->call_off, &resume, &handler_depth, &throwing) || handler_depth >= at->depth) continue;
        frame->stack[handler_depth] = js_take_thrown(js, result);
        bc_off = resume;
        depth = handler_depth + 1;
      } else {
        if (frames[k - 1].site->construct && !is_object_type(result)) result = frames[k - 1].this_val;
        if (at->mode == OPT_RESUME_RETURN) continue;
        frame->stack[at->depth - 1] = result;
      }
    }
    vm->jit.resume.throwing = throwing;
    if (k == count - 1) {
      vm->jit.resume.arguments_obj = arguments;
      result = jit_resume_in_interpreter(
        vm, entry->closure, entry->this_val, entry->new_target, entry->super_val,
        entry->args, (int)entry->argc, frame->stack, depth, frame->params, params,
        frame->locals, locals, bc_off, false, frame->finally
      );
    } else {
      sv_func_t *frame_func = frame->func;
      uint32_t frame_params = frame_func->param_count, frame_locals = (uint32_t)frame_func->max_locals;
      uint32_t argc = is_object_type(frame->arguments) ? at->argc : at->argc < frame_params ? at->argc : frame_params;
      vm->jit.resume.arguments_obj = frame->arguments;
      result = jit_resume_in_interpreter(
        vm, frame->closure, frame->this_val, at->construct ? frame->callee : js_mkundef(), frame->closure->super_val,
        frame->params, (int)argc, frame->stack, depth, frame->params, frame_params,
        frame->locals, frame_locals, bc_off, false, NULL
      );
    }
  }
  js->vm_segs = seg.prev;
  return result;
}

ant_value_t opt_rt_call(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc) {
  ant_t *js = vm->js;
  if (vtype(callee) == kTypeBuiltin && js->vm_exec_depth && vtype(js->exception) == kTypeUndefined && !ant_realm_multiple(js)) {
    ant_cfunc_t fn = ((const ant_cfunc_meta_t *)vptr(callee))->fn;
    ant_value_t saved = js->this_val;
    js->this_val = vtype(this_val) == kTypeNull ? js_mkundef() : this_val;
    ant_value_t result = fn(js, args, (int)argc, js_mkundef());
    js->this_val = saved;
    if (is_err(result) || vtype(js->exception) != kTypeUndefined) result = jit_helper_native_finish(js, result);
    return result;
  }
  return sv_vm_call(vm, js, callee, this_val, args, (int)argc, NULL, js_mkundef());
}

ant_value_t opt_rt_char_code_at(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_call_char_code_at(vm, vm->js, callee, this_val, args, (int)argc);
}

ant_value_t opt_rt_array_includes(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_call_array_includes(vm, vm->js, callee, this_val, args, (int)argc);
}

ant_value_t opt_rt_string_intrinsic(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_call_string_intrinsic(vm, vm->js, (ant_string_intrinsic_kind_t)kind, callee, this_val, args, (int)argc);
}

ant_value_t opt_rt_stable_builtin(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_call_stable_builtin(vm, vm->js, (int)kind, callee, this_val, args, (int)argc);
}

ant_value_t opt_rt_concat(sv_vm_t *vm, ant_value_t lhs, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t kind) {
  if (kind & IR_CONCAT_INT32_LHS) lhs = tov((double)(int32_t)(uint32_t)lhs);
  if (kind & IR_CONCAT_INT32_RHS) rhs = tov((double)(int32_t)(uint32_t)rhs);
  return jit_helper_concat(vm->js, lhs, rhs);
}

uint64_t opt_rt_string_length(sv_vm_t *vm, ant_value_t string, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  return (uint32_t)str_utf16_len(vm->js, string);
}

ant_value_t opt_rt_instanceof(sv_vm_t *vm, ant_value_t lhs, ant_value_t rhs, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_instanceof(vm, vm->js, lhs, rhs, site->func, (int32_t)site->bc_off);
}

ant_value_t opt_rt_regexp_exec_truthy(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_regexp_exec_truthy(vm, vm->js, callee, this_val, args[0]);
}

ant_value_t opt_rt_call_call(sv_vm_t *vm, ant_value_t callee, ant_value_t arg, ant_value_t *args, int64_t argc, int64_t kind) {
  ant_value_t base[2 + SV_JIT_ARGS_BUF_CAP];
  base[0] = callee;
  base[1] = arg;
  memcpy(base + 2, args, (size_t)argc * sizeof *args);
  return jit_helper_call_call(vm, vm->js, base, 1, (int32_t)argc);
}

ant_value_t opt_rt_to_string(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t defer_number) {
  if (vtype(value) == kTypeString || (defer_number && vtype(value) == kTypeNumber)) return value;
  return js_template_to_string(vm->js, value);
}

ant_value_t opt_rt_typeof(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_typeof(vm, vm->js, value);
}

ant_value_t opt_rt_in(sv_vm_t *vm, ant_value_t key, ant_value_t object, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_in(vm, vm->js, key, object);
}

ant_value_t opt_rt_for_in(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_for_in(vm, vm->js, object);
}

ant_value_t opt_rt_in_field(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  sv_func_t *func = site->func;
  sv_atom_t *atom = &func->atoms[sv_get_u32(func->code + site->bc_off + 1)];
  return jit_helper_in_field(vm->js, object, atom, func, (int32_t)site->bc_off);
}

ant_value_t opt_rt_set_proto(sv_vm_t *vm, ant_value_t object, ant_value_t proto, ant_value_t *args, int64_t argc, int64_t kind) {
  js_set_literal_proto(vm->js, object, proto);
  return js_mkundef();
}

ant_value_t opt_rt_builder_read(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  return sv_string_builder_read_value(vm->js, value);
}

static ant_value_t opt_str_append_at(sv_vm_t *vm, ant_value_t current, ant_value_t rhs, const ant_value_t *lhs, bool boxed) {
  ant_t *js = vm->js;
  sv_upvalue_t *box = boxed ? (sv_upvalue_t *)(uintptr_t)current : NULL;
  ant_value_t value = current;
  ant_value_t *slot = box ? box->location : &value;
  ant_value_t result = lhs
    ? sv_string_builder_append_snapshot_at(vm, js, slot, NULL, NULL, 0, *lhs, rhs)
    : sv_string_builder_append_at(vm, js, slot, NULL, NULL, 0, rhs);
  if (is_err(result)) return result;
  if (box) gc_upvalue_write_barrier(js, box, *slot);
  return *slot;
}

ant_value_t opt_rt_str_append(sv_vm_t *vm, ant_value_t current, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t boxed) {
  return opt_str_append_at(vm, current, rhs, NULL, boxed != 0);
}

ant_value_t opt_rt_str_append_snapshot(sv_vm_t *vm, ant_value_t current, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t boxed) {
  return opt_str_append_at(vm, current, rhs, &args[0], boxed != 0);
}

ant_value_t opt_rt_regexp(sv_vm_t *vm, ant_value_t pattern, ant_value_t flags, ant_value_t *args, int64_t argc, int64_t kind) {
  return regexp_create_literal(vm->js, pattern, flags);
}

ant_value_t opt_rt_construct(sv_vm_t *vm, ant_value_t callee, ant_value_t new_target, ant_value_t *args, int64_t argc) {
  return jit_helper_new(vm, vm->js, callee, new_target, args, (int)argc);
}

ant_value_t opt_rt_new_this(sv_vm_t *vm, ant_value_t callee) {
  ant_t *js = vm->js;
  if (vtype(callee) != kTypeFunction) return T_EMPTY;
  sv_closure_t *closure = js_func_closure(callee);
  if (!sv_closure_is_plain_sync(closure) || closure->func->is_derived_ctor || !is_object_type(closure->func_obj)) return T_EMPTY;
  ant_object_t *func_obj = js_obj_ptr(closure->func_obj);
  ant_value_t proto;
  if (!func_obj->flags.is_constructor || !sv_construct_prototype_data(js, func_obj, closure->func, &proto)) return T_EMPTY;
  ant_value_t object = js_mkobj_with_proto(js, proto);
  if (vtype(object) != kTypeObject) return T_EMPTY;
  return object;
}

ant_value_t opt_rt_allocate_object(sv_vm_t *vm, ant_shape_t *shape, ant_value_t proto, uint64_t property_count) {
  ant_value_t object = Ant_Object_CreateBare(vm->js);
  if (vtype(object) != kTypeObject) return is_err(object) ? object : js_mkerr(vm->js, "oom");
  ant_object_t *ptr = js_obj_ptr(object);
  ant_shape_retain(shape);
  ant_shape_release(ptr->shape);
  ptr->shape = shape;
  ptr->prop_count = (uint32_t)property_count;
  ptr->proto = proto;
  return object;
}

ant_value_t opt_rt_allocate_array(sv_vm_t *vm, uint64_t count) {
  ant_value_t *data;
  ant_value_t array = js_mkarr_dense_uninit(vm->js, (uint32_t)count, &data);
  if (is_err(array)) return array;
  for (uint64_t i = 0; i < count; i++) data[i] = js_mkundef();
  return array;
}

ant_value_t opt_rt_object(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_object(vm, vm->js, site->func, sv_obj_site_for_offset(site->func, site->bc_off));
}

ant_value_t opt_rt_array(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t kind) {
  return js_mkarr_dense_literal(vm->js, args, (uint32_t)argc);
}

ant_value_t opt_rt_closure(sv_vm_t *vm, sv_closure_t *parent, ant_value_t this_val, ant_value_t *args, int64_t argc, const opt_rt_closure_site_t *site) {
  return jit_helper_closure_inherited(vm->js, site->func, parent, this_val, site->index, site->name, site->name_len);
}

ant_value_t opt_rt_closure_boxes(sv_vm_t *vm, sv_closure_t *parent, ant_value_t this_val, ant_value_t *args, int64_t argc, const opt_rt_closure_site_t *site) {
  sv_upvalue_t *boxes[SV_JIT_ARGS_BUF_CAP];
  for (int64_t k = 0; k < argc; k++) {
    if (!(site->fresh & (1u << k))) {
      boxes[k] = (sv_upvalue_t *)(uintptr_t)args[k];
      continue;
    }
    sv_upvalue_t *box = js_upvalue_alloc(vm->js);
    if (!box) return js_mkerr_typed(vm->js, JS_ERR_RANGE | JS_ERR_NO_STACK, "out of memory");
    box->closed = args[k];
    box->location = &box->closed;
    boxes[k] = box;
  }
  return jit_helper_closure_boxes(vm->js, site->func, parent, this_val, site->index, site->name, site->name_len, boxes);
}

ant_value_t opt_rt_new_box(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  sv_upvalue_t *box = js_upvalue_alloc(vm->js);
  if (!box) return js_mkerr_typed(vm->js, JS_ERR_RANGE | JS_ERR_NO_STACK, "out of memory");
  box->closed = value;
  box->location = &box->closed;
  box->next = NULL;
  return (ant_value_t)(uintptr_t)box;
}

ant_value_t opt_rt_osr_box(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t slot) {
  ant_value_t *location = sv_frame_slot_ptr(&vm->frames[vm->fp], (uint16_t)slot);
  ant_value_t value = location ? *location : js_mkundef();
  for (sv_upvalue_t **pp = &vm->open_upvalues; location && *pp; pp = &(*pp)->next) {
    sv_upvalue_t *box = *pp;
    if ((uintptr_t)box->location < (uintptr_t)location) break;
    if (box->location != location) continue;
    *pp = box->next;
    box->next = NULL;
    box->closed = value;
    box->location = &box->closed;
    gc_upvalue_write_barrier(vm->js, box, value);
    return (ant_value_t)(uintptr_t)box;
  }
  return opt_rt_new_box(vm, value, unused, args, argc, 0);
}

void opt_rt_reopen_box(sv_vm_t *vm, sv_upvalue_t *box, ant_value_t *slot) {
  *slot = box->closed;
  box->location = slot;
  sv_upvalue_t **pp = &vm->open_upvalues;
  while (*pp && (uintptr_t)(*pp)->location > (uintptr_t)slot) pp = &(*pp)->next;
  box->next = *pp;
  *pp = box;
}

void opt_rt_close_storage(sv_vm_t *vm, const ant_value_t *from, const ant_value_t *to) {
  for (sv_upvalue_t **pp = &vm->open_upvalues; *pp;) {
    sv_upvalue_t *box = *pp;
    if ((uintptr_t)box->location < (uintptr_t)from) break;
    if ((uintptr_t)box->location >= (uintptr_t)to) {
      pp = &box->next;
      continue;
    }
    box->closed = *box->location;
    box->location = &box->closed;
    *pp = box->next;
    box->next = NULL;
    gc_upvalue_write_barrier(vm->js, box, box->closed);
  }
}

ant_value_t opt_rt_arguments_object(sv_vm_t *vm, ant_value_t callee, ant_value_t *args, int argc, bool is_strict) {
  sv_frame_t frame = {.bp = args};
  return js_create_arguments_object(vm->js, vm, callee, &frame, argc, 0, is_strict);
}

ant_value_t opt_rt_close_upvalues(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t slot) {
  jit_helper_close_frame_upvalues(vm, (uint32_t)slot);
  return js_mkundef();
}

ant_value_t opt_rt_get_global(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  const sv_atom_t *atom = &site->func->atoms[sv_get_u32(site->func->code + site->bc_off + 1)];
  return jit_helper_get_global(vm->js, atom->str, site->func, (int32_t)site->bc_off);
}

ant_value_t opt_rt_get_field(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_slow_get_field(vm->js, object, site->func, site->func->code + site->bc_off);
}

ant_value_t opt_rt_put_field(sv_vm_t *vm, ant_value_t object, ant_value_t value, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_slow_put_field(vm->js, object, value, site->func, site->func->code + site->bc_off);
}

ant_value_t opt_rt_put_field_adding(sv_vm_t *vm, ant_value_t object, ant_value_t value, ant_value_t *args, int64_t argc, const opt_rt_add_site_t *site) {
  const opt_rt_site_t *at = &site->site;
  return jit_helper_slow_put_field_adding(vm->js, object, value, at->func, at->func->code + at->bc_off, site->adds, site->count);
}

ant_value_t opt_rt_get_elem(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_slow_get_elem(vm->js, object, key, site->func, site->func->code + site->bc_off);
}

ant_value_t opt_rt_put_elem(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_slow_put_elem(vm->js, object, key, args[0], site->func, site->func->code + site->bc_off);
}

ant_value_t opt_rt_put_elem_plain(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  if (site->func->is_strict) return jit_helper_put_elem_strict(vm, vm->js, object, key, args[0]);
  return jit_helper_put_elem(vm, vm->js, object, key, args[0]);
}

ant_value_t opt_rt_get_length(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  return jit_helper_get_length(vm->js, object);
}

static void opt_count_slow_call(const opt_rt_site_t *site) {
  opt_count_step(site->func, site->func->code + site->bc_off);
}

typedef ant_value_t (*opt_rt_access_fn)(sv_vm_t *vm, ant_value_t object, ant_value_t operand, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);

static opt_rt_access_fn opt_access_helper_of(sv_op_t op) {
  switch (op) {
    case OP_GET_FIELD:
    case OP_GET_FIELD2: return opt_rt_get_field;
    case OP_PUT_FIELD: return opt_rt_put_field;
    case OP_GET_ELEM: return opt_rt_get_elem;
    case OP_PUT_ELEM: return opt_rt_put_elem;
    case OP_GET_LENGTH: return opt_rt_get_length;
    default: return NULL;
  }
}

static ant_value_t opt_rt_access_counted(sv_vm_t *vm, ant_value_t object, ant_value_t operand, ant_value_t *args, int64_t argc, const opt_rt_site_t *site) {
  opt_count_slow_call(site);
  return opt_access_helper_of((sv_op_t)site->func->code[site->bc_off])(vm, object, operand, args, argc, site);
}

static ant_value_t opt_rt_put_field_adding_counted(sv_vm_t *vm, ant_value_t object, ant_value_t value, ant_value_t *args, int64_t argc, const opt_rt_add_site_t *site) {
  opt_count_slow_call(&site->site);
  return opt_rt_put_field_adding(vm, object, value, args, argc, site);
}

uintptr_t opt_tier_access_helper(sv_op_t op, bool adding) {
  bool counted = opt_stats_enabled();
  if (op == OP_PUT_FIELD && adding) return counted ? (uintptr_t)opt_rt_put_field_adding_counted : (uintptr_t)opt_rt_put_field_adding;
  opt_rt_access_fn helper = opt_access_helper_of(op);
  if (!helper) return 0;
  return counted ? (uintptr_t)opt_rt_access_counted : (uintptr_t)helper;
}

ant_value_t opt_rt_throw(ant_value_t value, sv_vm_t *vm) {
  return is_err(value) ? value : js_throw(vm->js, value);
}

static void opt_catch_missed(ant_t *js, opt_code_t *code, uint32_t catch_off) {
  if (++code->catch_misses < OPT_CATCH_MISS_LIMIT || code->dropped) return;
  opt_note_discard(opt_func_state(js, code->func, true), code, true);
  opt_drop_code(code);
  if (sv_jit_warn_unlikely)
    fprintf(stderr, "jit: opt dropped func=%s reason=catch-without-entry catch=%u line=%u\n", opt_func_name(code->func), catch_off,
      opt_func_line(code->func));
}

void opt_rt_caught(sv_vm_t *vm, opt_code_t *code, const opt_handler_t *handler, bool in_place) {
  if (in_place) {
    opt_stats.catches++;
    return;
  }
  opt_stats.catch_leaves++;
  if (handler->learnable) opt_catch_missed(vm->js, code, handler->catch_off);
}

bool opt_tier_has_catch_entry(ant_t *js, sv_func_t *func, uint32_t catch_off) {
  opt_func_state_t *state = opt_func_state(js, func, false);
  opt_code_t *code = state ? state->codes : NULL;
  while (code && code->entry != func->jit_code) code = code->func_next;
  if (!code || !func->jit_code) return false;
  for (uint32_t i = 0; i < code->handler_count; i++) {
    const opt_handler_t *handler = &code->handlers[i];
    if (handler->catch_off != catch_off) continue;
    if (handler->entry == UINT32_MAX) {
      if (handler->learnable) opt_catch_missed(js, code, catch_off);
      return false;
    }
    opt_stats.catch_entries++;
    return true;
  }
  return false;
}

ant_value_t opt_rt_for_of(sv_vm_t *vm, ant_value_t iterable, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_for_of(vm, vm->js, iterable, args);
}

ant_value_t opt_rt_iter_next(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t hint) {
  return jit_helper_iter_next(vm, vm->js, args - 2, (int)hint);
}

ant_value_t opt_rt_destructure_next(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_destructure_next(vm, vm->js, args - 2);
}

ant_value_t opt_rt_destructure_rest(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t kind) {
  return jit_helper_destructure_rest(vm, vm->js, args - 2);
}

ant_value_t opt_rt_iter_close(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t suppress) {
  return jit_helper_destructure_close(vm, vm->js, args - 2, (int)suppress);
}

ant_value_t opt_rt_stack_overflow(sv_vm_t *vm) {
  return Ant_Error_CreateStackOverflow(vm->js);
}

void opt_rt_slow_paths_hot(sv_vm_t *vm, opt_code_t *code, sv_func_t *func, uint64_t bc_off) {
  opt_func_state_t *state = func && bc_off < (uint64_t)func->code_len ? opt_func_state(vm->js, func, true) : NULL;
  bool learned = false;
  if (state) {
    sv_op_t op = (sv_op_t)func->code[bc_off];
    bool again = op == OP_GET_ELEM || op == OP_PUT_ELEM;
    for (uint32_t i = 0; i < state->exit_site_count && !again; i++) {
      const ir_exit_site_t *site = &state->exit_sites[i];
      again = site->bc_off == bc_off && site->kind == OPT_EXIT_SLOW_PATH && site->slot == 1;
    }
    learned = opt_record_exit_site(state, (uint32_t)bc_off, OPT_EXIT_SLOW_PATH, again ? 0 : 1);
  }
  opt_note_discard(opt_func_state(vm->js, code->func, true), code, learned);
  opt_drop_code(code);
  if (sv_jit_warn_unlikely)
    fprintf(stderr, "jit: opt dropped func=%s reason=slow-paths runs=%llu line=%u\n", opt_func_name(code->func),
      (unsigned long long)code->slow_path_runs, opt_func_line(code->func));
}

ant_value_t opt_rt_normalize_this(sv_vm_t *vm, sv_func_t *func, ant_value_t this_val) {
  return sv_normalize_this_for_frame(vm->js, func, this_val);
}

uint64_t opt_rt_to_boolean(ant_value_t value, sv_vm_t *vm) {
  return js_truthy(vm->js, value) ? 1 : 0;
}

uint64_t opt_rt_strict_equals(ant_value_t lhs, ant_value_t rhs, sv_vm_t *vm) {
  return strict_eq_values(vm->js, lhs, rhs) ? 1 : 0;
}

void opt_rt_write_barrier(sv_vm_t *vm, ant_object_t *object, ant_value_t value) {
  gc_write_barrier_prop(vm->js, object, value);
}

void opt_rt_element_barrier(sv_vm_t *vm, ant_object_t *array, uint32_t index, ant_value_t value) {
  gc_write_barrier_elem(vm->js, array, index, value);
}

void opt_rt_upvalue_barrier(sv_vm_t *vm, sv_upvalue_t *upvalue, ant_value_t value) {
  jit_helper_upval_flagged(vm->js, upvalue, value);
}

void opt_rt_shape_transition(sv_vm_t *vm, ant_object_t *object, ant_shape_t *to_shape) {
  jit_helper_shape_transition(vm->js, object, to_shape);
}

static void opt_register_code(sv_jit_ctx_t *jc, opt_code_t *code) {
  uint32_t count = code->call_site_count;
  jit_call_site_info_t *infos = calloc(count + 1, sizeof *infos);
  uint32_t *returns = calloc(count + 1, sizeof *returns);
  if (infos && returns) {
    for (uint32_t i = 0; i < count; i++) {
      const opt_call_site_t *site = &code->call_sites[i];
      infos[i].bc_off = site->bc_off;
      infos[i].inl.depth = (int)site->depth;
      for (uint32_t d = 0; d < site->depth; d++) infos[i].inl.frames[d] = site->frames[d];
      returns[i] = site->ret_off;
    }
    uint32_t resume = code->exit_ret_off;
    jit_code_register(jc, code->func, (const void *)code->entry, code->code_bytes, 16, infos, returns, count,
      &resume, resume != UINT32_MAX);
  }
  free(infos);
  free(returns);
  free(code->call_sites);
  code->call_sites = NULL;
  code->call_site_count = 0;
}

static uint64_t opt_ns_since(const struct timespec *t0) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (uint64_t)((int64_t)(t1.tv_sec - t0->tv_sec) * 1000000000 + (t1.tv_nsec - t0->tv_nsec));
}

static void opt_note_rejected(const struct timespec *t0) {
  if (!opt_stats_enabled()) return;
  if (!opt_stats.compiled && !opt_stats.rejected) atexit(opt_print_stats);
  opt_stats.rejected++;
  opt_stats.rejected_ns += opt_ns_since(t0);
}

static void opt_print_text(const char *title, const opt_text_t *text) {
  fprintf(stderr, "=== %s\n%s", title, text->data ? text->data : "");
}

sv_jit_func_t opt_tier_compile(ant_t *js, sv_func_t *func, sv_closure_t *closure, bool sole_tier) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  if (!jc) return NULL;
  opt_func_state_t *state = opt_func_state(js, func, true);
  if (!state || state->disabled) return NULL;
  if (state->compiles >= OPT_MAX_LEARNING_COMPILES || state->fruitless >= opt_max_compiles(func)) {
    state->disabled = true;
    if (sv_jit_warn_unlikely) fprintf(stderr, "jit: opt disabled func=%s after %u compiles\n", opt_func_name(func), state->compiles);
    return NULL;
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  bool dump = sv_debug_enabled(SV_DEBUG_DUMP_OPT_GRAPH);
  if (dump) sv_disasm(js, func, opt_func_name(func));

  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_frame_layout_t layout;
  opt_build_failure_t failure;
  opt_build_stats_t build_stats = {0};
  if (!opt_build_graph(js, func, closure, opt_exit_sites_of, &graph, &layout, &failure, &build_stats)) {
    if (sv_jit_warn_unlikely) {
      sv_func_t *at = failure.func ? failure.func : func;
      uint32_t off = failure.bc_off;
      uint8_t op = off < (uint32_t)at->code_len ? at->code[off] : OP__COUNT;
      fprintf(stderr, "jit: opt rejected func=%s op=%s bc=%u reason=%s%s%s line=%u\n", opt_func_name(func),
        op < OP__COUNT && sv_op_names[op] ? sv_op_names[op] : "-", off, failure.reason ? failure.reason : "?",
        at != func ? " in=" : "", at != func ? opt_func_name(at) : "", opt_func_line(func));
    }
    state->disabled = true;
    opt_build_release(js, &graph);
    ir_graph_free(&graph);
    opt_note_rejected(&t0);
    return NULL;
  }

  opt_text_t text = {0};
  if (dump) {
    ir_dump(&graph, &layout, &text);
    opt_print_text("built", &text);
    opt_text_free(&text);
  }
  pass_remove_unreachable_blocks(&graph);
  opt_generic_counts_t counts = opt_generic_counts(&graph);
  const char *unprofitable = opt_unprofitable(&counts, sole_tier);
  if (!unprofitable) {
    pass_optimize(&graph, &layout, opt_verify_enabled());
    counts = opt_generic_counts(&graph);
    unprofitable = opt_unprofitable(&counts, sole_tier);
  }
  uint32_t nodes = ir_node_count(&graph);
  if (unprofitable) {
    if (sv_jit_warn_unlikely) {
      uint16_t op = counts.first;
      fprintf(stderr, "jit: opt rejected func=%s op=%s generics=%u in_loops=%u reason=%s line=%u\n", opt_func_name(func),
        op < OP__COUNT && sv_op_names[op] ? sv_op_names[op] : "-", counts.primary, counts.in_loops, unprofitable, opt_func_line(func));
    }
    state->disabled = true;
    opt_build_release(js, &graph);
    ir_graph_free(&graph);
    opt_note_rejected(&t0);
    return NULL;
  }
  if (dump) {
    ir_dump(&graph, &layout, &text);
    opt_print_text("optimized", &text);
    opt_text_free(&text);
  }

  const char *codegen_failure = NULL;
  struct opt_state *codes = opt_state_of(jc);
  if (codes && !codes->heap) codes->heap = asm_code_heap_new();
  opt_code_t *code = codes && codes->heap ? opt_cg_generate(js, &graph, &layout, codes->heap, opt_verify_enabled(), dump ? &text : NULL, &codegen_failure) : NULL;
  if (dump && text.data) {
    opt_print_text("allocation", &text);
    opt_text_free(&text);
  }
  if (code && !func->unit)
    for (uint32_t i = 0; i < graph.retained_funcs.len; i++) sv_code_depend(js, func, graph.retained_funcs.items[i]);
  if (code && !opt_hold_roots(code, &graph)) {
    opt_code_free(code);
    code = NULL;
    codegen_failure = "out-of-memory";
  }
  if (!code) opt_build_release(js, &graph);
  ir_graph_free(&graph);
  if (!code) {
    if (sv_jit_warn_unlikely)
      fprintf(stderr, "jit: opt codegen-failed func=%s reason=%s line=%u\n", opt_func_name(func),
        codegen_failure ? codegen_failure : "-", opt_func_line(func));
    state->disabled = true;
    opt_note_rejected(&t0);
    return NULL;
  }

  opt_register_code(jc, code);
  code->func_next = state->codes;
  state->codes = code;
  state->compiles++;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (opt_stats_enabled()) {
    if (!opt_stats.compiled++ && !opt_stats.rejected) atexit(opt_print_stats);
    opt_stats.compiled_ns += opt_ns_since(&t0);
    opt_stats.inlined_calls += build_stats.inlined_calls;
    opt_stats.code_bytes += code->code_bytes;
    asm_code_heap_stats_t heap = asm_code_heap_stats(codes->heap);
    if (heap.mapped > opt_stats.mapped) opt_stats.mapped = heap.mapped;
  }
  if (sv_jit_warn_unlikely) fprintf(
    stderr, "jit: opt compiled func=%s nodes=%u generics=%u slow_paths=%u inlined=%u code_bytes=%zu ms=%.3f line=%u\n",
    opt_func_name(func), nodes, counts.primary, counts.total - counts.primary,
    build_stats.inlined_calls, code->code_bytes,
    (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6, opt_func_line(func)
  );
  return code->entry;
}
