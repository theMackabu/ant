#include "compiler_internal.h"

static bool sv_func_compute_has_try(const sv_func_t *func) {
  for (const uint8_t *ip = func->code, *end = func->code + func->code_len; ip < end;) {
    int sz = sv_op_size[*ip];
    if (sz <= 0) return false;
    if (*ip == OP_TRY_PUSH || *ip == OP_TRY_PUSH_FINALLY) return true;
    ip += sz;
  }
  return false;
}

static int sv_func_compute_max_stack(const sv_func_t *func);

void sv_func_init_code_and_map_templates(
  const sv_compiler_t *c, sv_func_t *func
) {
  Ant_Assert(c != NULL && func != NULL, "function code requires compiler state");
  Ant_Assert(c->code_len > 0, "function bytecode cannot be empty");

  size_t table_size =
    (size_t)c->map_template_count * sizeof(sv_map_template_desc_t);
  
  size_t prefix_size = c->map_template_count > 0
    ? table_size + sizeof(sv_map_template_table_header_t) : 0;
  
  Ant_Assert(
    prefix_size <= SIZE_MAX - (size_t)c->code_len,
    "function bytecode sidecar size overflow"
  );

  uint8_t *storage = sv_code_bump(c->js, prefix_size + (size_t)c->code_len);
  Ant_Assert(storage != NULL, "failed to allocate function bytecode");
  
  if (c->map_template_count > 0) {
    memcpy(storage, c->map_templates, table_size);
    sv_map_template_table_header_t *header =
    (sv_map_template_table_header_t *)(void *)(storage + table_size);
    header->count = (uint32_t)c->map_template_count;
    header->magic = SV_MAP_TEMPLATE_TABLE_MAGIC;
    func->code = (uint8_t *)(void *)(header + 1);
    func->has_map_templates = true;
  } else func->code = storage;
  
  memcpy(func->code, c->code, (size_t)c->code_len);
  func->code_len = c->code_len;
  func->has_try = sv_func_compute_has_try(func);
  func->jit_osr_threshold = sv_jit_osr_threshold_for(func->code_len);

  int max_stack = sv_func_compute_max_stack(func);
  if (max_stack < 0) {
    if (sv_debug_unlikely(SV_DEBUG_JIT_WARN)) fprintf(
      stderr, "jit: operand depth analysis failed for %s\n",
      func->debug && func->debug->name ? func->debug->name : "<anonymous>"
    );
    max_stack = func->code_len + 64;
  }
  func->max_stack = max_stack;
}

uint16_t alloc_ic_idx(sv_compiler_t *c) {
  if (!c || c->ic_count >= (int)UINT16_MAX) return UINT16_MAX;
  return (uint16_t)c->ic_count++;
}

void sv_func_init_obj_sites(const sv_compiler_t *c, sv_func_t *func) {
  if (!func || !func->code || func->code_len <= 0) return;

  uint32_t count = 0;
  for (int pc = 0; pc < func->code_len; ) {
    sv_op_t op = (sv_op_t)func->code[pc];
    if (op == OP_OBJECT) count++;
    uint8_t size = (op < OP__COUNT) ? sv_op_size[op] : 1;
    pc += (size > 0) ? size : 1;
  }
  if (count == 0) return;

  func->obj_sites = sv_code_bump(c->js, (size_t)count * sizeof(sv_obj_site_cache_t));
  memset(func->obj_sites, 0, (size_t)count * sizeof(sv_obj_site_cache_t));
  func->obj_site_count = count;

  uint32_t idx = 0;
  for (int pc = 0; pc < func->code_len && idx < count; ) {
    sv_op_t op = (sv_op_t)func->code[pc];
    if (op == OP_OBJECT) func->obj_sites[idx++].bc_off = (uint32_t)pc;
    uint8_t size = (op < OP__COUNT) ? sv_op_size[op] : 1;
    pc += (size > 0) ? size : 1;
  }

  if (c->shaped_site_count > 0) {
    uint32_t i = 0;
    for (int s = 0; s < c->shaped_site_count; s++) {
      uint16_t kc = c->shaped_sites[s].key_count;
      if (kc == 0) continue;
      
      while (i < count && func->obj_sites[i].bc_off < c->shaped_sites[s].bc_off) i++;
      
      if (i >= count) break;
      if (func->obj_sites[i].bc_off != c->shaped_sites[s].bc_off) continue;
      
      uint32_t *ka = sv_code_bump(c->js, (size_t)kc * sizeof(uint32_t));
      if (!ka) continue;
      
      memcpy(ka, c->shaped_keys + c->shaped_sites[s].first_key, (size_t)kc * sizeof(uint32_t));
      func->obj_sites[i].key_atoms = ka;
      func->obj_sites[i].key_count = kc;
      
      i++;
    }
  }
}

void sv_func_finalize_type_data(
  sv_func_t *func, const sv_compiler_t *comp, int local_type_count
) {
  Ant_Assert(func != NULL, "function metadata requires a function");
  Ant_Assert(comp != NULL, "function metadata requires a compiler context");
  Ant_Assert(local_type_count >= 0, "function local type count cannot be negative");
  Ant_Assert(!func->has_dynamic_eval, "function metadata cannot be initialized twice");
  
  Ant_Assert(
    func->type_data.local_types == NULL,
    "function local types cannot be replaced"
  );

  func->needs_eval_env = comp->inherits_eval_env;
  func->is_eval = sv_compile_mode_is_eval(comp->mode);
  func->local_type_count = local_type_count;
  if (comp->eval_scope_count > 0 || comp->eval_var_count > 0 || comp->global_lexical_count > 0) {
    size_t metadata_size = offsetof(sv_func_metadata_t, local_types) +
      (size_t)local_type_count * sizeof(sv_type_info_t);
    sv_func_metadata_t *metadata = sv_code_bump(comp->js, metadata_size);
    Ant_Assert(metadata != NULL, "failed to allocate function metadata");
    Ant_Assert(
      ((uintptr_t)metadata % _Alignof(sv_func_metadata_t)) == 0,
      "code arena returned misaligned function metadata"
    );
    memset(metadata, 0, metadata_size);
    sv_eval_scope_t *eval_scopes = sv_code_bump(comp->js, 
      (size_t)comp->eval_scope_count * sizeof(sv_eval_scope_t));
    Ant_Assert(eval_scopes != NULL, "failed to allocate eval scopes");
    metadata->eval_scopes = eval_scopes;
    metadata->eval_scope_count = (uint32_t)comp->eval_scope_count;
    metadata->eval_var_count = comp->eval_var_count;
    if (comp->eval_var_count) {
      size_t size = (size_t)comp->eval_var_count * sizeof(sv_eval_decl_t);
      metadata->eval_vars = sv_code_bump(comp->js, size);
      memcpy(metadata->eval_vars, comp->eval_vars, size);
    }
    metadata->global_lexical_count = comp->global_lexical_count;
    if (comp->global_lexical_count) {
      size_t size = (size_t)comp->global_lexical_count * sizeof(sv_eval_decl_t);
      metadata->global_lexicals = sv_code_bump(comp->js, size);
      memcpy(metadata->global_lexicals, comp->global_lexicals, size);
    }

    size_t binding_count = 0;
    for (int i = 0; i < comp->eval_scope_count; i++)
      binding_count += comp->eval_scopes[i].count;

    sv_runtime_binding_t *bindings = binding_count > 0
      ? sv_code_bump(comp->js, binding_count * sizeof(sv_runtime_binding_t))
      : NULL;
    Ant_Assert(
      binding_count == 0 || bindings != NULL,
      "failed to allocate eval bindings"
    );

    size_t binding_offset = 0;
    for (int i = 0; i < comp->eval_scope_count; i++) {
      const sv_eval_scope_builder_t *src = &comp->eval_scopes[i];
      sv_eval_scope_t *dst = &metadata->eval_scopes[i];
      dst->count = src->count;
      dst->bindings = src->count > 0
        ? bindings + binding_offset : NULL;
      if (src->count > 0) {
        memcpy(bindings + binding_offset, src->bindings,
          (size_t)src->count * sizeof(sv_runtime_binding_t));
        for (uint32_t b = 0; b < src->count; b++) {
          sv_runtime_binding_t *binding = &bindings[binding_offset + b];
          const char *name = copy_name(comp->js, binding->name, binding->len);
          Ant_Assert(name != NULL || binding->len == 0, "failed to copy eval binding name");
          binding->name = name;
        }
        binding_offset += src->count;
      }
    }

    if (local_type_count > 0 && comp->slot_types) {
      int ncopy = local_type_count < comp->slot_type_cap
        ? local_type_count : comp->slot_type_cap;
      memcpy(metadata->local_types, comp->slot_types,
        (size_t)ncopy * sizeof(sv_type_info_t));
    }
    func->type_data.metadata = metadata;
    func->has_dynamic_eval = true;
    return;
  }

  if (local_type_count == 0) return;
  sv_type_info_t *local_types = sv_code_bump(comp->js, 
    (size_t)local_type_count * sizeof(sv_type_info_t));
  Ant_Assert(local_types != NULL, "failed to allocate function local types");
  memset(local_types, 0, (size_t)local_type_count * sizeof(sv_type_info_t));
  if (comp->slot_types) {
    int ncopy = local_type_count < comp->slot_type_cap
      ? local_type_count : comp->slot_type_cap;
    memcpy(local_types, comp->slot_types,
      (size_t)ncopy * sizeof(sv_type_info_t));
  }
  func->type_data.local_types = local_types;
}

const char *const sv_op_names[OP__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = #name,
#include "silver/opcode.h"
};

bool sv_op_stack_effect(const sv_func_t *func, const uint8_t *ip, int *pops, int *pushes) {
  uint8_t op = *ip;
  if (op >= OP__COUNT || sv_op_size[op] == 0) return false;
  
  int n = sv_op_npop[op];
  switch (sv_op_fmts[op]) {
    case SVF_npop:       n += sv_get_u16(ip + 1); break;
    case SVF_u8_npop:    n += sv_get_u16(ip + 2); break;
    case SVF_npop_u8_u8: n += ip[1] + ip[2]; break;
    
    case SVF_map_template: {
      const sv_map_template_desc_t *desc =
        sv_map_template_desc_at(func, sv_get_u32(ip + 1));
      if (!desc) return false;
      n += (int)desc->substitution_count;
      break;
    }
    
    default: break;
  }
  
  *pops = n;
  *pushes = sv_op_npush[op];
  
  return true;
}

static constexpr int STACK_DEPTH_SCRATCH = 512;

static int sv_func_stack_depths(const sv_func_t *func, int *depth) {
  const int len = func->code_len;
  const uint8_t *code = func->code;
  if (len <= 0) return 0;

  int work_small[STACK_DEPTH_SCRATCH];
  uint8_t queued_small[STACK_DEPTH_SCRATCH];
  bool small = len <= STACK_DEPTH_SCRATCH;
  int *work = small ? work_small : malloc((size_t)len * sizeof(int));
  uint8_t *queued = small ? queued_small : malloc((size_t)len);
  
  if (!work || !queued) {
    if (!small) { free(work); free(queued); }
    return -1;
  }
  memset(queued, 0, (size_t)len);
  
  for (int i = 0; i < len; i++) depth[i] = -1;
  int wn = 0, max = 0, off = -1;
  
  bool ok = true;
  long budget = 16L * len + 4096;
  
  #define VISIT(off_, d_) do {                                 \
    int vo_ = (off_), vd_ = (d_);                              \
    if (vo_ < 0 || vo_ >= len) { ok = false; break; }          \
    if (depth[vo_] < vd_) {                                    \
      depth[vo_] = vd_;                                        \
      if (!queued[vo_]) { queued[vo_] = 1; work[wn++] = vo_; } \
    }                                                          \
  } while (0)

  VISIT(0, 0);
  while (ok && wn > 0) {
    if (--budget < 0) { ok = false; break; }
    
    off = work[--wn];
    queued[off] = 0;
    
    int d = depth[off];
    uint8_t op = code[off];
    int sz = sv_op_size[op];
    if (sz == 0 || off + sz > len) { ok = false; break; }
    const uint8_t *ip = code + off;

    int pops, pushes;
    if (!sv_op_stack_effect(func, ip, &pops, &pushes)) { ok = false; break; }
    if (d < pops) { ok = false; break; }
    int after = d - pops + pushes;
    if (after > max) max = after;

    switch (op) {
      case OP_TRY_PUSH:
      case OP_TRY_PUSH_FINALLY:
        VISIT(off + sz + sv_get_i32(ip + 1), after + 1);
        break;
      case OP_CATCH:
        break;
      default:
        if (sv_op_fmts[op] == SVF_label)
          VISIT(off + sz + sv_get_i32(ip + 1), after);
        else if (sv_op_fmts[op] == SVF_atom_label)
          VISIT(off + sz + sv_get_i32(ip + 5), after);
        else if (sv_op_fmts[op] == SVF_label8)
          VISIT(off + sz + (int8_t)sv_get_i8(ip + 1), after);
        break;
    }
    if (!ok) break;
    if (!(sv_op_flags[op] & SV_OPF_TERMINAL)) VISIT(off + sz, after);
  }
  #undef VISIT

  if (ok) {
    // TODO: cleaner
    int cur = 0;
    for (int o = 0; o < len; ) {
      uint8_t op = code[o];
      int sz = sv_op_size[op];
      int pops, pushes;
      if (sz == 0 || o + sz > len || !sv_op_stack_effect(func, code + o, &pops, &pushes)) { 
        ok = false;
        break;
      }
      if (depth[o] >= 0) cur = depth[o];
      else if (cur < pops) cur = pops;
      cur = cur - pops + pushes;
      if (cur > max) max = cur;
      o += sz;
    }
  }

  if (!small) { free(work); free(queued); }
  return ok ? max : -1;
}

static int sv_func_compute_max_stack(const sv_func_t *func) {
  if (func->code_len <= 0) return 0;
  int depth_small[STACK_DEPTH_SCRATCH];
  bool small = func->code_len <= STACK_DEPTH_SCRATCH;
  int *depth = small ? depth_small : malloc((size_t)func->code_len * sizeof(int));
  int max = depth ? sv_func_stack_depths(func, depth) : -1;
  if (!small) free(depth);
  return max;
}

int *sv_func_stack_depth_map(const sv_func_t *func) {
  if (func->code_len <= 0) return NULL;
  int *depth = malloc((size_t)func->code_len * sizeof(int));
  if (depth && sv_func_stack_depths(func, depth) < 0) {
    free(depth);
    return NULL;
  }
  return depth;
}

int *sv_func_try_map(const sv_func_t *func) {
  int len = func->code_len;
  const uint8_t *code = func->code;
  
  if (len <= 0) return NULL;
  int *map = malloc(2 * (size_t)len * sizeof(int));
  int *work = malloc((size_t)len * sizeof(int));
  
  if (!map || !work) {
    free(map);
    free(work);
    return NULL;
  }
  
  int *parent = map + len;
  for (int i = 0; i < len; i++) map[i] = parent[i] = -2;
  
  int wn = 0;
  bool ok = true;
  
  #define TRY_VISIT(off_, state_) do {      \
    int to_ = (off_), st_ = (state_);       \
    if (to_ < 0 || to_ >= len) ok = false;  \
    else if (map[to_] == -2) {              \
      map[to_] = st_;                       \
      work[wn++] = to_;                     \
    } else if (map[to_] != st_) ok = false; \
  } while (0)
  
  TRY_VISIT(0, -1);
  while (ok && wn > 0) {
    int off = work[--wn];
    int state = map[off];
    
    uint8_t op = code[off];
    int sz = sv_op_size[op];
    
    if (sz == 0 || off + sz > len) {
      ok = false;
      break;
    }
    
    int next = state;
    bool in_finally = state >= 0 && code[state] == OP_FINALLY;
    switch (op) {
      case OP_TRY_PUSH:
      case OP_TRY_PUSH_FINALLY:
        if (parent[off] != -2 && parent[off] != state) ok = false;
        parent[off] = state;
        TRY_VISIT(off + sz + sv_get_i32(code + off + 1), state);
        next = off;
        break;
      case OP_TRY_POP:
        if (state < 0 || in_finally) ok = false;
        else next = parent[state];
        break;
      case OP_FINALLY:
        if (parent[off] != -2 && parent[off] != state) ok = false;
        parent[off] = state;
        TRY_VISIT(off + sz + sv_get_i32(code + off + 1), state);
        next = off;
        break;
      case OP_FINALLY_RET:
        if (!in_finally) ok = false;
        break;
      case OP_FINALLY_DISCARD:
        if (!in_finally) ok = false;
        else next = parent[state];
        break;
      case OP_UNWIND_JMP:
        if (state < 0 || code[state] != OP_TRY_PUSH_FINALLY || code[off + 5] != 1 || code[off + 6] != 1) ok = false;
        else TRY_VISIT(off + sz + sv_get_i32(code + off + 1), parent[state]);
        break;
      default:
        if (sv_op_fmts[op] == SVF_label) TRY_VISIT(off + sz + sv_get_i32(code + off + 1), state);
        else if (sv_op_fmts[op] == SVF_atom_label) TRY_VISIT(off + sz + sv_get_i32(code + off + 5), state);
        else if (sv_op_fmts[op] == SVF_label8) TRY_VISIT(off + sz + (int8_t)sv_get_i8(code + off + 1), state);
        break;
    }
    
    if (ok && !(sv_op_flags[op] & SV_OPF_TERMINAL)) TRY_VISIT(off + sz, next);
  }
  
  #undef TRY_VISIT
  free(work);
  
  if (ok) return map;
  free(map);

  return NULL;
}

int sv_func_finally_index(const sv_func_t *func, int finally_off) {
  int index = 0;
  for (int off = 0; off < finally_off;) {
    int sz = sv_op_size[func->code[off]];
    if (sz == 0) return -1;
    if (func->code[off] == OP_FINALLY) index++;
    off += sz;
  }
  return index;
}
