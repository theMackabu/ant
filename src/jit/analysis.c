#include "jit_internal.h"
#include "../silver/ops/globals.h"
#include "silver/feedback.h"

int jit_back_edges(const sv_func_t *func, int (**out)[2]) {
  int n = 0, cap = 0;
  *out = NULL;
  for (int off = 0; off < func->code_len;) {
    sv_op_t op = func->code[off];
    if (op >= OP__COUNT) goto fail;
    int size = sv_op_size[op];
    if (!size || size > func->code_len - off) goto fail;
    uint32_t flags = sv_op_flags[op];
    int64_t target = -1;
    if (flags & SV_OPF_JIT_BRANCH32)
      target = (int64_t)off + size + sv_get_i32(func->code + off + 1);
    else if (flags & SV_OPF_JIT_BRANCH8)
      target = (int64_t)off + size + (int8_t)sv_get_i8(func->code + off + 1);
    if (target >= 0 && target <= off) {
      if (n == cap) {
        cap = cap ? cap * 2 : 16;
        int (*grown)[2] = realloc(*out, (size_t)cap * sizeof(**out));
        if (!grown) goto fail;
        *out = grown;
      }
      (*out)[n][0] = (int)target;
      (*out)[n++][1] = off;
    }
    off += size;
  }
  return n;
fail:
  free(*out);
  *out = NULL;
  return -1;
}

int jit_hot_loop_upvalue(const sv_func_t *func) {
  if (!func || !func->code || func->upvalue_count <= 0) return -1;
  struct { unsigned depth, reads; } scores[32] = {{0}};
  int (*loops)[2] = NULL;
  int loop_count = jit_back_edges(func, &loops);
  if (loop_count <= 0 || loop_count > 64) {
    free(loops);
    return -1;
  }
  for (int off = 0; off < func->code_len; off += sv_op_size[func->code[off]]) {
    if (func->code[off] != OP_GET_UPVAL) continue;
    uint16_t index = sv_get_u16(func->code + off + 1);
    if (index >= func->upvalue_count || index >= sizeof scores / sizeof *scores) continue;
    unsigned depth = 0;
    for (int i = 0; i < loop_count; i++)
      if (off >= loops[i][0] && off <= loops[i][1]) depth++;
    if (depth > scores[index].depth) {
      scores[index].depth = depth;
      scores[index].reads = 1;
    } else if (depth && depth == scores[index].depth) scores[index].reads++;
  }
  free(loops);
  int best = -1;
  for (unsigned i = 0; i < sizeof scores / sizeof *scores; i++) {
    if (!scores[i].depth) continue;
    if (best < 0 || scores[i].depth > scores[best].depth ||
        (scores[i].depth == scores[best].depth && scores[i].reads > scores[best].reads))
      best = (int)i;
  }
  return best;
}

static int jit_local_write_target(const uint8_t *ip) {
  switch ((sv_op_t)*ip) {
    case OP_PUT_LOCAL: case OP_SET_LOCAL: case OP_PUT_LOCAL_CHK: case OP_SET_LOCAL_UNDEF:
      return sv_get_u16(ip + 1);
    case OP_PUT_LOCAL8: case OP_SET_LOCAL8:
    case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL:
      return sv_get_u8(ip + 1);
    default:
      return -1;
  }
}

uint8_t *jit_locals_written_in_loops(const sv_func_t *func) {
  if (!func || !func->code || func->max_locals <= 0) return NULL;
  uint8_t *written = calloc((size_t)func->max_locals, 1);
  if (!written) return NULL;
  int (*loops)[2] = NULL;
  int loop_count = jit_back_edges(func, &loops);
  if (loop_count < 0) {
    free(written);
    return NULL;
  }
  for (int i = 0; i < loop_count; i++)
    for (int off = loops[i][0]; off <= loops[i][1]; off += sv_op_size[func->code[off]]) {
      int local = jit_local_write_target(func->code + off);
      if (local >= 0 && local < func->max_locals) written[local] = 1;
    }
  free(loops);
  return written;
}

bool jit_offset_in_loop(const sv_func_t *func, int bc_off) {
  int (*loops)[2] = NULL;
  int loop_count = jit_back_edges(func, &loops);
  bool inside = false;
  for (int i = 0; i < loop_count && !inside; i++)
    inside = bc_off >= loops[i][0] && bc_off <= loops[i][1];
  free(loops);
  return inside;
}

bool jit_loop_has_await(const sv_func_t *func, int head) {
  int (*loops)[2] = NULL;
  int loop_count = jit_back_edges(func, &loops);
  bool found = false;
  for (int i = 0; i < loop_count && !found; i++) {
    if (loops[i][0] != head) continue;
    for (int off = loops[i][0]; off <= loops[i][1] && !found; off += sv_op_size[func->code[off]]) {
      found = jit_op_exits_to_interpreter((sv_op_t)func->code[off]);
    }
  }
  free(loops);
  return found;
}

static int jit_await_map_scan(const sv_func_t *func, const int *depth, sv_jit_await_map_t *map) {
  uint32_t open[SV_TRY_MAX];
  int sites = 0, open_count = 0;
  for (int off = 0; off < func->code_len;) {
    sv_op_t op = (sv_op_t)func->code[off];
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if ((op == OP_TRY_PUSH || op == OP_TRY_PUSH_FINALLY) && open_count < SV_TRY_MAX)
      open[open_count++] = (uint32_t)(off + sz + sv_get_i32(func->code + off + 1));
    else if (op == OP_TRY_POP && open_count > 0) open_count--;
    else if ((op == OP_AWAIT || op == OP_AWAIT_RETURN || op == OP_AWAIT_ITER_NEXT) && depth[off] >= 0) {
      bool in_try = false;
      for (int i = 0; i < open_count && !in_try; i++) in_try = open[i] > (uint32_t)off;
      if (map) map->sites[sites] = (sv_jit_await_site_t){
        .bc_off = (uint32_t)off, .resumable = op == OP_AWAIT, .in_try = in_try,
      };
      sites++;
    }
    off += sz;
  }
  return sites;
}

typedef struct {
  int reads[2], nreads;
  int write;
  bool all;
} jit_local_effect_t;

static jit_local_effect_t jit_local_effect(const sv_func_t *func, int off) {
  const uint8_t *ip = func->code + off;
  sv_op_t op = (sv_op_t)*ip;
  jit_local_effect_t e = { .nreads = 0, .write = -1, .all = false };
  switch (op) {
    case OP_GET_LOCAL: case OP_GET_LOCAL_CHK: e.reads[e.nreads++] = sv_get_u16(ip + 1); break;
    case OP_GET_LOCAL8: e.reads[e.nreads++] = sv_get_u8(ip + 1); break;
    case OP_PUT_LOCAL: case OP_SET_LOCAL: case OP_SET_LOCAL_UNDEF: e.write = sv_get_u16(ip + 1); break;
    case OP_PUT_LOCAL8: case OP_SET_LOCAL8: e.write = sv_get_u8(ip + 1); break;
    case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL: e.reads[e.nreads++] = sv_get_u8(ip + 1); break;
    case OP_PUT_LOCAL_CHK: e.reads[e.nreads++] = sv_get_u16(ip + 1); break;
    default: {
      if (sv_op_is_yield_star(op)) {
        e.all = true;
        break;
      }
      uint8_t fmt = sv_op_fmts[op];
      if (fmt != SVF_loc && fmt != SVF_loc8 && fmt != SVF_loc_atom) break;
      int idx = fmt == SVF_loc8 ? sv_get_u8(ip + 1) : sv_get_u16(ip + 1);
      e.reads[e.nreads++] = idx;
      if (idx >= func->param_count) e.reads[e.nreads++] = idx - func->param_count;
      break;
    }
  }
  return e;
}

static void jit_live_apply(const sv_func_t *func, int off, uint64_t *live, int words) {
  jit_local_effect_t e = jit_local_effect(func, off);
  if (e.all) {
    memset(live, 0xff, (size_t)words * sizeof(uint64_t));
    return;
  }
  if (e.write >= 0 && e.write < func->max_locals) live[e.write >> 6] &= ~(1ull << (e.write & 63));
  for (int i = 0; i < e.nreads; i++)
    if (e.reads[i] >= 0 && e.reads[i] < func->max_locals) live[e.reads[i] >> 6] |= 1ull << (e.reads[i] & 63);
}

static int64_t jit_live_branch_target(const sv_func_t *func, int off, sv_op_t op, int sz) {
  uint32_t flags = sv_op_flags[op];
  if (flags & SV_OPF_JIT_BRANCH32) return (int64_t)off + sz + sv_get_i32(func->code + off + 1);
  if (flags & SV_OPF_JIT_BRANCH8) return (int64_t)off + sz + sv_get_i8(func->code + off + 1);
  if (op == OP_TRY_PUSH || op == OP_TRY_PUSH_FINALLY || op == OP_CATCH || op == OP_UNWIND_JMP)
    return (int64_t)off + 5 + sv_get_i32(func->code + off + 1);
  return -1;
}

static uint64_t *jit_await_liveness(const sv_func_t *func, const sv_jit_await_map_t *map, int words) {
  int len = func->code_len;
  uint8_t *leader = calloc((size_t)len + 1, 1);
  int *block_of = malloc((size_t)(len + 1) * sizeof(int));
  uint64_t *result = calloc((size_t)map->site_count * (size_t)words, sizeof(uint64_t));
  if (!leader || !block_of || !result) goto fail;

  leader[0] = 1;
  for (int off = 0; off < len;) {
    sv_op_t op = (sv_op_t)func->code[off];
    int sz = sv_op_size[op];
    if (sz == 0 || off + sz > len) goto fail;
    int64_t target = jit_live_branch_target(func, off, op, sz);
    if (target > len) goto fail;
    if (target >= 0) leader[target] = 1;
    if (target >= 0 || (sv_op_flags[op] & SV_OPF_TERMINAL)) leader[off + sz] = 1;
    off += sz;
  }

  int nblocks = 0;
  for (int off = 0; off < len; off++) {
    if (leader[off]) nblocks++;
    block_of[off] = nblocks - 1;
  }
  block_of[len] = -1;

  int *start = malloc((size_t)nblocks * sizeof(int));
  int *succ = malloc((size_t)nblocks * 2 * sizeof(int));
  uint64_t *gen = calloc((size_t)nblocks * (size_t)words, sizeof(uint64_t));
  uint64_t *kill = calloc((size_t)nblocks * (size_t)words, sizeof(uint64_t));
  uint64_t *in = calloc((size_t)nblocks * (size_t)words, sizeof(uint64_t));
  uint64_t *out = calloc((size_t)nblocks * (size_t)words, sizeof(uint64_t));
  uint64_t *tmp = calloc((size_t)words, sizeof(uint64_t));
  int region_cap = 0;
  for (int off = 0; off < len; off += sv_op_size[func->code[off]])
    if (func->code[off] == OP_TRY_PUSH || func->code[off] == OP_TRY_PUSH_FINALLY) region_cap++;
  struct { int try_off, pop_off, catch_block; } *regions = region_cap ? malloc((size_t)region_cap * sizeof(*regions)) : NULL;
  bool ok = start && succ && gen && kill && in && out && tmp && (!region_cap || regions);

  for (int off = 0, b = 0; ok && off < len; off++) if (leader[off]) start[b++] = off;

  for (int b = 0; ok && b < nblocks; b++) {
    int end = b + 1 < nblocks ? start[b + 1] : len;
    succ[b * 2] = succ[b * 2 + 1] = -1;
    uint64_t *g = gen + (size_t)b * words, *k = kill + (size_t)b * words;
    for (int off = start[b]; off < end;) {
      sv_op_t op = (sv_op_t)func->code[off];
      int sz = sv_op_size[op];
      jit_local_effect_t e = jit_local_effect(func, off);
      if (e.all) memset(g, 0xff, (size_t)words * sizeof(uint64_t));
      for (int i = 0; i < e.nreads; i++) {
        int r = e.reads[i];
        if (r >= 0 && r < func->max_locals && !(k[r >> 6] & (1ull << (r & 63)))) g[r >> 6] |= 1ull << (r & 63);
      }
      if (e.write >= 0 && e.write < func->max_locals) k[e.write >> 6] |= 1ull << (e.write & 63);
      if (off + sz == end) {
        int64_t target = jit_live_branch_target(func, off, op, sz);
        if (!(sv_op_flags[op] & SV_OPF_TERMINAL) && end < len) succ[b * 2] = block_of[end];
        if (target >= 0 && target < len) succ[b * 2 + 1] = block_of[target];
      }
      off += sz;
    }
  }

  int region_count = 0, open[SV_TRY_MAX], open_count = 0;
  for (int off = 0; ok && off < len; off += sv_op_size[func->code[off]]) {
    sv_op_t op = (sv_op_t)func->code[off];
    if (op == OP_TRY_PUSH || op == OP_TRY_PUSH_FINALLY) {
      int catch_off = (int)jit_live_branch_target(func, off, op, sv_op_size[op]);
      if (open_count == SV_TRY_MAX || catch_off < 0 || catch_off >= len) { ok = false; break; }
      regions[region_count] = (typeof(*regions)){ .try_off = off, .pop_off = len, .catch_block = block_of[catch_off] };
      open[open_count++] = region_count++;
    } else if (op == OP_TRY_POP && open_count > 0) regions[open[--open_count]].pop_off = off;
  }

  for (bool changed = ok; changed;) {
    changed = false;
    for (int b = nblocks - 1; b >= 0; b--) {
      uint64_t *o = out + (size_t)b * words;
      memset(tmp, 0, (size_t)words * sizeof(uint64_t));
      for (int k = 0; k < 2; k++) {
        int s = succ[b * 2 + k];
        if (s < 0) continue;
        for (int w = 0; w < words; w++) tmp[w] |= in[(size_t)s * words + w];
      }
      int b_end = b + 1 < nblocks ? start[b + 1] : len;
      for (int r = 0; r < region_count; r++) {
        if (regions[r].try_off >= b_end || regions[r].pop_off < start[b]) continue;
        int s = regions[r].catch_block;
        for (int w = 0; w < words; w++) tmp[w] |= in[(size_t)s * words + w];
      }
      memcpy(o, tmp, (size_t)words * sizeof(uint64_t));
      for (int w = 0; w < words; w++) {
        uint64_t v = gen[(size_t)b * words + w] | (o[w] & ~kill[(size_t)b * words + w]);
        if (v != in[(size_t)b * words + w]) { in[(size_t)b * words + w] = v; changed = true; }
      }
    }
  }

  for (int i = 0; ok && i < map->site_count; i++) {
    int site_off = (int)map->sites[i].bc_off;
    int b = block_of[site_off];
    int end = b + 1 < nblocks ? start[b + 1] : len;
    uint64_t *live = result + (size_t)i * words;
    memcpy(live, out + (size_t)b * words, (size_t)words * sizeof(uint64_t));
    int offs[256], n = 0;
    for (int off = site_off; off < end && n < 256; off += sv_op_size[func->code[off]]) offs[n++] = off;
    if (n == 256) { memset(live, 0xff, (size_t)words * sizeof(uint64_t)); continue; }
    for (int k = n - 1; k >= 0; k--) jit_live_apply(func, offs[k], live, words);
  }

  free(start); free(succ); free(gen); free(kill); free(in); free(out); free(tmp); free(regions);
  free(leader); free(block_of);
  if (ok) return result;
  free(result);
  return NULL;

fail:
  free(leader); free(block_of); free(result);
  return NULL;
}

sv_jit_await_map_t *jit_await_map_build(const sv_func_t *func, const int *depth) {
  int sites = jit_await_map_scan(func, depth, NULL);
  sv_jit_await_map_t probe = { .site_count = sites };
  probe.sites = sites ? malloc((size_t)sites * sizeof(sv_jit_await_site_t)) : NULL;
  
  if (sites && !probe.sites) return NULL;
  jit_await_map_scan(func, depth, &probe);

  int n_locals = func->max_locals;
  int words = (n_locals + 63) / 64;
  uint64_t *bits = sites && n_locals ? jit_await_liveness(func, &probe, words) : NULL;
  int live_total = 0;
  for (int i = 0; i < sites; i++) for (int l = 0; l < n_locals; l++)
    if (!bits || (bits[(size_t)i * words + (l >> 6)] & (1ull << (l & 63)))) live_total++;

  sv_jit_await_map_t *map = malloc(sizeof(*map) + (size_t)sites * sizeof(sv_jit_await_site_t) + (size_t)live_total * sizeof(uint16_t));
  if (map) {
    map->site_count = sites;
    map->sites = (sv_jit_await_site_t *)(map + 1);
    map->live = (uint16_t *)(map->sites + sites);
    if (sites) memcpy(map->sites, probe.sites, (size_t)sites * sizeof(sv_jit_await_site_t));
    int k = 0;
    for (int i = 0; i < sites; i++) {
      map->sites[i].live_start = (uint32_t)k;
      for (int l = 0; l < n_locals; l++)
        if (!bits || (bits[(size_t)i * words + (l >> 6)] & (1ull << (l & 63)))) map->live[k++] = (uint16_t)l;
      map->sites[i].live_count = (uint16_t)(k - (int)map->sites[i].live_start);
    }
  }
  free(bits);
  free(probe.sites);
  return map;
}

bool jit_loop_reads_global(const sv_func_t *func) {
  if (!func || !func->code) return false;
  int (*loops)[2] = NULL;
  int loop_count = jit_back_edges(func, &loops);
  bool found = false;
  for (int i = 0; i < loop_count && !found; i++)
    for (int off = loops[i][0]; off <= loops[i][1] && !found; off += sv_op_size[func->code[off]])
      found = func->code[off] == OP_GET_GLOBAL || func->code[off] == OP_GET_GLOBAL_UNDEF;
  free(loops);
  return found;
}

static const uint8_t forward_arguments_prefix[] = {
  OP_SPECIAL_OBJ,  // arguments object (SV_SPECIAL_ARGUMENTS)
  OP_PUT_LOCAL8,   // -> local 0
  OP_THIS,
  OP_GET_FIELD,    // this.method
  OP_DUP,
  OP_GET_FIELD,    // .apply
  OP_THIS,
  OP_GET_LOCAL8,   // arguments (local 0)
};
enum {
  FORWARD_PREFIX_OPS = sizeof forward_arguments_prefix,
  FORWARD_CALL_OP = FORWARD_PREFIX_OPS,
  FORWARD_RETURN_FORM_OPS = FORWARD_CALL_OP + 2,
  FORWARD_STATEMENT_FORM_OPS = FORWARD_CALL_OP + 3,
};
static const char forward_apply_name[] = "apply";

bool jit_can_forward_arguments(sv_func_t *func) {
  if (!func->is_strict || func->is_async || func->is_generator ||
      func->is_derived_ctor || func->param_count != 0 || func->max_locals != 1)
    return false;

  const uint8_t *ops[FORWARD_STATEMENT_FORM_OPS];
  int count = 0;
  for (const uint8_t *ip = func->code, *end = ip + func->code_len; ip < end;) {
    int size = sv_op_size[*ip];
    if (!size || ip + size > end || count == FORWARD_STATEMENT_FORM_OPS) return false;
    ops[count++] = ip;
    ip += size;
  }
  if (count != FORWARD_RETURN_FORM_OPS && count != FORWARD_STATEMENT_FORM_OPS) return false;

  for (int i = 0; i < FORWARD_PREFIX_OPS; i++)
    if (*ops[i] != forward_arguments_prefix[i]) return false;
  if (ops[0][1] != 0 || ops[1][1] != 0 || ops[7][1] != 0) return false;
  uint32_t atom = sv_get_u32(ops[5] + 1);
  if (atom >= (uint32_t)func->atom_count ||
      func->atoms[atom].len != sizeof forward_apply_name - 1 ||
      memcmp(func->atoms[atom].str, forward_apply_name, sizeof forward_apply_name - 1))
    return false;

  const uint8_t *call = ops[FORWARD_CALL_OP];
  if (sv_get_u16(call + 1) != 2) return false;  // apply(this, arguments)
  if (count == FORWARD_RETURN_FORM_OPS)
    return *call == OP_TAIL_CALL_METHOD && *ops[FORWARD_CALL_OP + 1] == OP_RETURN_UNDEF;
  return *call == OP_CALL_METHOD &&
    (*ops[FORWARD_CALL_OP + 1] == OP_POP || *ops[FORWARD_CALL_OP + 1] == OP_RETURN) &&
    *ops[FORWARD_CALL_OP + 2] == OP_RETURN_UNDEF;
}

bool scan_branch_targets(sv_func_t *func, jit_label_map_t *lm, MIR_context_t ctx) {
  lm->index_len = func->code_len + 1;
  lm->index = calloc((size_t)lm->index_len, sizeof(*lm->index));
  if (!lm->index) return false;

  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    uint32_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_BRANCH32) != 0) {
      int off = (int)(ip - func->code) + sv_get_i32(ip + 1) + sz;
      if (!label_for_offset(ctx, lm, off)) return false;
    } else if ((flags & SV_OPF_JIT_BRANCH8) != 0) {
      int off = (int)(ip - func->code) + (int8_t)sv_get_i8(ip + 1) + sz;
      if (!label_for_offset(ctx, lm, off)) return false;
    }
    ip += sz;
  }
  return true;
}

static bool jit_local_has_numeric_hint(sv_func_t *func, int idx) {
  if (!func || idx < 0) return false;
  if (idx >= func->max_locals) return false;
  sv_type_info_t *local_types = sv_func_local_types(func);
  if (local_types && idx < func->local_type_count &&
      local_types[idx].type == SV_TI_NUM)
    return true;
  if (func->local_type_feedback) {
    uint8_t ltf = func->local_type_feedback[idx];
    if (ltf && !(ltf & ~SV_TFB_NUM)) return true;
  }
  return false;
}

jit_features_t jit_prescan_features(sv_func_t *func, int n_slots, const sv_closure_t *hint) {
  jit_features_t f = {
    .needs_new_target = func->is_derived_ctor,
    .needs_super = func->is_method || func->is_static || func->is_derived_ctor,
  };
  if (n_slots > 0)
    f.builder_target_slots = calloc((size_t)n_slots, sizeof(bool));
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if (op == OP_CHECK_CTOR) f.needs_new_target = true;
    if (op == OP_SPECIAL_OBJ) {
      uint8_t which = sv_get_u8(ip + 1);
      if (which == SV_SPECIAL_NEW_TARGET) f.needs_new_target = true;
      if (which == SV_SPECIAL_SUPER) f.needs_super = true;
    }
    if (op == OP_GET_GLOBAL) {
      const sv_atom_t *atom = &func->atoms[sv_get_u32(ip + 1)];
      if (atom->len == 5 && memcmp(atom->str, "super", 5) == 0) f.needs_super = true;
    }
    uint32_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_NEEDS_BAILOUT) != 0) f.needs_bailout = true;
    if ((flags & SV_OPF_JIT_NEEDS_INC_LOCAL) != 0) f.needs_inc_local = true;
    if ((flags & SV_OPF_JIT_NEEDS_ARGS_BUF) != 0) f.needs_args_buf = true;
    if ((flags & SV_OPF_JIT_NEEDS_TCO_ARGS) != 0) f.needs_tco_args = true;
    if ((flags & SV_OPF_JIT_NEEDS_ITER_ROOTS) != 0) f.needs_iter_roots = true;
    if ((flags & SV_OPF_JIT_NEEDS_CLOSE_UPVAL) != 0) f.needs_close_upval = true;
    if ((flags & SV_OPF_JIT_NEEDS_IC_EPOCH) != 0) f.needs_ic_epoch = true;
    if (op == OP_THIS || op == OP_CLOSURE || op == OP_EVAL)
      f.needs_this = true;
    if ((op == OP_GET_UPVAL || op == OP_PUT_UPVAL || op == OP_SET_UPVAL) &&
        jit_upvalue_may_tdz(func, sv_get_u16(ip + 1)) &&
        !jit_hint_cell_initialized(hint, func, sv_get_u16(ip + 1)))
      f.needs_bailout = true;
    if ((flags & SV_OPF_BUILDER_TARGET) != 0 && f.builder_target_slots) {
      uint16_t slot = sv_get_u16(ip + 1);
      if ((int)slot < n_slots) f.builder_target_slots[slot] = true;
    }
    if ((flags & SV_OPF_JIT_LOCAL_NUMERIC_BAILOUT) != 0) {
      if (op == OP_PUT_LOCAL || op == OP_SET_LOCAL) {
        uint16_t idx = sv_get_u16(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      } else {
        uint8_t idx = sv_get_u8(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      }
    }
    ip += sz;
  }
  if (f.needs_bailout) f.needs_args_buf = true;
  return f;
}

static bool jit_value_is_closure(
    ant_value_t value, const sv_closure_t *expected) {
  if (vtype(value) != kTypeFunction) return false;
  return js_func_closure(value) == expected;
}

static bool jit_try_get_global_data(
    ant_t *js, sv_func_t *func, uint8_t *ip,
    const sv_atom_t *atom, ant_value_t *out) {
  if (!js || !func || !ip || !atom || !out) return false;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  return sv_global_ic_try_get_hit(js, js->global, ic, atom->str, out) ||
         sv_global_ic_try_fill_unshadowed(js, js->global, ic, atom->str, out);
}

bool jit_mark_self_binding_guards(
    ant_t *js, sv_func_t *func, sv_closure_t *hint_closure,
    uint8_t *guard_sites) {
  if (!js || !func || !hint_closure || !guard_sites) return false;
  if (func->has_bound_closure) return false;

  bool found = false;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;

    if (op == OP_GET_UPVAL) {
      uint16_t idx = sv_get_u16(ip + 1);
      if (idx < (uint16_t)func->upvalue_count && func->upval_descs &&
          hint_closure->upvalues && hint_closure->upvalues[idx] &&
          jit_value_is_closure(
              *hint_closure->upvalues[idx]->location, hint_closure)) {
        guard_sites[ip - func->code] = 1;
        found = true;
      }
    } else if (op == OP_GET_GLOBAL || op == OP_GET_GLOBAL_UNDEF) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx < (uint32_t)func->atom_count) {
        sv_atom_t *atom = &func->atoms[idx];
        ant_value_t value;
        if (jit_try_get_global_data(js, func, ip, atom, &value) &&
            jit_value_is_closure(value, hint_closure)) {
          guard_sites[ip - func->code] = 1;
          found = true;
        }
      }
    }

    ip += sz;
  }
  return found;
}

bool jit_is_eligible(sv_func_t *func) {
  if (func->is_generator) return false;
  
  if (func->code_len > SV_JIT_MAX_CODE_BYTES) {
    if (sv_jit_warn_unlikely) fprintf(
      stderr, "jit: %s too large (%d bytes > %d)\n",
      func->debug->name ? func->debug->name : "<anonymous>",
      func->code_len, SV_JIT_MAX_CODE_BYTES
    );
    return false;
  }

  bool eligible = true;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    if ((sv_op_flags[op] & SV_OPF_JIT_ELIGIBLE) == 0) {
      if ((sv_op_flags[op] & SV_OPF_JIT_QUIET_INELIGIBLE) == 0) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op %s in %s\n",
                  (op < OP__COUNT && sv_op_names[op]) ? sv_op_names[op] : "???",
                  func->debug->name ? func->debug->name : "<anonymous>");
      }
      eligible = false;
    } else if (op == OP_CLOSURE) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx >= (uint32_t)func->const_count) return false;
      ant_value_t cv = func->constants[idx];
      if (vtype(cv) != kTypeFunctionInfo) return false;
    } else if (op == OP_SPECIAL_OBJ) {
      if (sv_get_u8(ip + 1) == SV_SPECIAL_ARGUMENTS && !func->is_strict) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op SPECIAL_OBJ(%d) in %s\n",
                  sv_get_u8(ip + 1),
                  func->debug->name ? func->debug->name : "<anonymous>");
        eligible = false;
      }
    }
    ip += sz;
  }
  return eligible;
}
