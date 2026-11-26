#include "private/codegen.h"

void emit_import_binding_resolve(
  sv_compiler_t *c,
  uint8_t import_kind,
  const char *import_name,
  uint32_t import_name_len
) {
  if (import_kind == SV_IMPORT_BIND_DEFAULT) emit_op(c, OP_IMPORT_DEFAULT);
  else if (import_kind == SV_IMPORT_BIND_NAMED && import_name)
    emit_atom_op(c, OP_IMPORT_NAMED, import_name, import_name_len);
}

int resolve_local(sv_compiler_t *c, const char *name, uint32_t len) {
  if (c->local_lookup_heads && c->local_lookup_cap > 0) {
    uint32_t hash = sv_compile_ctx_hash_local_name(name, len);
    int bucket = (int)(hash & (uint32_t)(c->local_lookup_cap - 1));
    for (int i = c->local_lookup_heads[bucket]; i != -1; i = c->locals[i].lookup_next) {
      sv_local_t *loc = &c->locals[i];
      if (
        loc->name_hash == hash &&
        loc->name_len == len &&
        memcmp(loc->name, name, len) == 0
      ) return i;
    }
    return -1;
  }

  for (int i = c->local_count - 1; i >= 0; i--) {
    sv_local_t *loc = &c->locals[i];
    if (loc->name_len == len && memcmp(loc->name, name, len) == 0) return i;
  }

  return -1;
}

int resolve_local_at_depth(sv_compiler_t *c, const char *name, uint32_t len, int depth) {
  if (c->local_lookup_heads && c->local_lookup_cap > 0) {
    uint32_t hash = sv_compile_ctx_hash_local_name(name, len);
    int bucket = (int)(hash & (uint32_t)(c->local_lookup_cap - 1));
    for (int i = c->local_lookup_heads[bucket]; i != -1; i = c->locals[i].lookup_next) {
      sv_local_t *loc = &c->locals[i];
      if (loc->depth == depth && loc->name_hash == hash && loc->name_len == len && memcmp(loc->name, name, len) == 0) return i;
    }
    return -1;
  }

  for (int i = c->local_count - 1; i >= 0; i--) {
    sv_local_t *loc = &c->locals[i];
    if (
      loc->depth == depth &&
      loc->name_len == len &&
      memcmp(loc->name, name, len) == 0
    ) return i;
  }
  return -1;
}

static void ensure_slot_type_cap(sv_compiler_t *c, int slot) {
  if (slot < 0) return;
  if (slot < c->slot_type_cap) return;
  
  int old_cap = c->slot_type_cap;
  sv_type_info_t *next = Silver_Compiler_Grow(
    c, c->slot_types, &c->slot_type_cap, slot + 1, 16, sizeof(sv_type_info_t));
  if (!next) return;
  
  memset(next + old_cap, 0, (size_t)(c->slot_type_cap - old_cap) * sizeof(sv_type_info_t));
  c->slot_types = next;
}

static void mark_slot_bound(sv_compiler_t *c, int slot) {
  if (slot < 0) return;
  ensure_slot_type_cap(c, slot);
  
  if (!c->slot_types || slot >= c->slot_type_cap) return;
  sv_type_info_t *info = &c->slot_types[slot];
  
  if (info->bound) info->shared_slot = true;
  info->bound = true;
}

int add_local(
  sv_compiler_t *c, const char *name, uint32_t len, bool is_const, int depth
) {
  sv_compile_ctx_ensure_local_lookup_capacity(c, c->local_count + 1);
  
  if (c->local_count >= c->local_cap) {
    sv_local_t *locals = Silver_Compiler_Grow(c, c->locals, &c->local_cap, c->local_count + 1, 16, sizeof(sv_local_t));
    if (!locals) {
      if (c->local_count > 0) return c->local_count - 1;
      c->locals = &c->local_sink;
      return 0;
    }
    c->locals = locals;
  }
  
  int idx = c->local_count++;
  
  if (idx > UINT16_MAX && !Ant_Exception_Pending(c->js)) js_mkerr_typed(
    c->js, JS_ERR_SYNTAX,
    "Too many variables declared (only %d allowed)", UINT16_MAX
  );
  if (c->local_count > c->max_local_count) c->max_local_count = c->local_count;
  if (depth >= 0) mark_slot_bound(c, idx - c->param_locals);
  
  c->locals[idx] = (sv_local_t){
    .name = name, .name_len = len,
    .name_hash = sv_compile_ctx_hash_local_name(name, len),
    .lookup_next = -1,
    .depth = depth, .is_const = is_const, .captured = false,
    .inferred_type = SV_TI_UNKNOWN,
  };
  
  sv_compile_ctx_local_lookup_insert(c, idx);
  
  return idx;
}

int reserve_hidden_locals(sv_compiler_t *c, int count) {
  int base = add_local(c, "", 0, false, c->scope_depth);
  for (int i = 1; i < count; i++)
    add_local(c, "", 0, false, c->scope_depth);
  return base;
}

static void mark_slot_type(sv_compiler_t *c, int slot, uint8_t type) {
  if (slot < 0) return;
  ensure_slot_type_cap(c, slot);
  
  if (!c->slot_types || slot >= c->slot_type_cap) return;
  uint8_t old = c->slot_types[slot].type;
  
  if (!sv_type_is_known(type)) {
    c->slot_types[slot].type = SV_TI_UNKNOWN;
    return;
  }
  
  if (!sv_type_is_known(old)) c->slot_types[slot].type = type;
  else if (old != type) c->slot_types[slot].type = SV_TI_UNKNOWN;
}

void set_local_declared_type(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base) {
  if (local_idx < 0 || local_idx >= c->local_count) return;
  type_assign_local(c, local_idx, type, dep_base);
  if (c->locals[local_idx].depth == -1) return;
  mark_slot_type(c, local_idx - c->param_locals, c->locals[local_idx].inferred_type);
}

void set_local_inferred_type(sv_compiler_t *c, int local_idx, uint8_t type) {
  set_local_declared_type(c, local_idx, type, c->types->dep_count);
}

void set_local_stored_type(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base) {
  type_assign_local(c, local_idx, type, dep_base);
  if (c->locals[local_idx].depth == -1) return;
  mark_slot_type(c, local_idx - c->param_locals, SV_TI_UNKNOWN);
}

const char *typeof_name_for_type(uint8_t type) {
  switch (type) {
    case SV_TI_NUM:   return "number";
    case SV_TI_STR:   return "string";
    case SV_TI_BOOL:  return "boolean";
    case SV_TI_UNDEF: return "undefined";
    case SV_TI_ARR:
    case SV_TI_NULL:  return "object";
    default:          return NULL;
  }
}

uint8_t iter_hint_for_type(uint8_t type) {
  switch (type) {
    case SV_TI_ARR: return SV_ITER_HINT_ARRAY;
    case SV_TI_STR: return SV_ITER_HINT_STRING;
    default:        return SV_ITER_HINT_GENERIC;
  }
}

int ensure_local_at_depth(
  sv_compiler_t *c, const char *name, uint32_t len,
  bool is_const, int depth
) {
  int local = resolve_local(c, name, len);
  if (local != -1 && c->locals[local].depth == depth)
    return local;
  return add_local(c, name, len, is_const, depth);
}

int local_to_frame_slot(sv_compiler_t *c, int local_idx) {
  if (c->locals[local_idx].depth == -1) return local_idx;
  return c->param_count + (local_idx - c->param_locals);
}

int resolve_local_slot(sv_compiler_t *c, const char *name, uint32_t len) {
  int local = resolve_local(c, name, len);
  if (local == -1 || c->locals[local].depth == -1 || c->locals[local].is_tdz) return -1;
  int slot = local - c->param_locals;
  return (slot >= 0 && slot <= 255) ? slot : -1;
}

static inline uint32_t upvalue_index_slot(const sv_compiler_t *c, uint16_t index, bool is_local) {
  uint32_t key = ((uint32_t)index << 1) | (is_local ? 1u : 0u);
  return (key * 0x9E3779B1u) >> 8 & (uint32_t)(c->upvalue_index_cap - 1);
}

static void upvalue_index_insert(sv_compiler_t *c, int upvalue) {
  uint32_t mask = (uint32_t)c->upvalue_index_cap - 1;
  uint32_t slot = upvalue_index_slot(c, c->upval_descs[upvalue].index, c->upval_descs[upvalue].is_local);
  while (c->upvalue_index[slot] >= 0) slot = (slot + 1) & mask;
  c->upvalue_index[slot] = upvalue;
}

static void upvalue_index_add(sv_compiler_t *c) {
  if (c->upvalue_count < 32) return;
  if (c->upvalue_index_cap && c->upvalue_count * 2 < c->upvalue_index_cap) {
    upvalue_index_insert(c, c->upvalue_count - 1);
    return;
  }

  int cap = c->upvalue_index_cap ? c->upvalue_index_cap * 2 : 128;
  int *index = realloc(c->upvalue_index, (size_t)cap * sizeof(*index));
  if (!index) { c->upvalue_index_cap = 0; return; }
  memset(index, 0xff, (size_t)cap * sizeof(*index));
  c->upvalue_index = index;
  c->upvalue_index_cap = cap;
  for (int i = 0; i < c->upvalue_count; i++) upvalue_index_insert(c, i);
}

static int find_upvalue(sv_compiler_t *c, uint16_t index, bool is_local) {
  if (c->upvalue_index_cap) {
    uint32_t mask = (uint32_t)c->upvalue_index_cap - 1;
    for (uint32_t slot = upvalue_index_slot(c, index, is_local);; slot = (slot + 1) & mask) {
      int upvalue = c->upvalue_index[slot];
      if (upvalue < 0) return -1;
      if (c->upval_descs[upvalue].index == index && c->upval_descs[upvalue].is_local == is_local) return upvalue;
    }
  }
  for (int i = 0; i < c->upvalue_count; i++)
    if (c->upval_descs[i].index == index && c->upval_descs[i].is_local == is_local) return i;
  return -1;
}

static void merge_binding_meta(sv_binding_meta_t *dst, const sv_binding_meta_t *src) {
  if (!dst || !src) return;

  if (src->import_kind != SV_IMPORT_BIND_NONE) {
    dst->import_kind = src->import_kind;
    dst->import_name = src->import_name;
    dst->import_name_len = src->import_name_len;
  }

  if (!dst->exports && src->exports) dst->exports = src->exports;
  dst->may_tdz |= src->may_tdz;
}

static bool ensure_upvalue_capacity(sv_compiler_t *c) {
  if (c->upvalue_count < c->upvalue_cap) return true;

  int old_cap = c->upvalue_cap;
  int desc_cap = old_cap;
  int binding_cap = old_cap;
  
  sv_upval_desc_t *descs = Silver_Compiler_Grow(
    c, c->upval_descs, &desc_cap, c->upvalue_count + 1, 8, sizeof(sv_upval_desc_t));
  if (!descs) return false;
  c->upval_descs = descs;
  
  sv_binding_meta_t *bindings = Silver_Compiler_Grow(
    c, c->upval_bindings, &binding_cap, c->upvalue_count + 1, 8, sizeof(sv_binding_meta_t));
  if (!bindings) return false;
  c->upval_bindings = bindings;

  memset(bindings + old_cap, 0, (size_t)(binding_cap - old_cap) * sizeof(sv_binding_meta_t));
  c->upvalue_cap = binding_cap;
  return true;
}

int add_upvalue_binding(
  sv_compiler_t *c,
  uint16_t index,
  bool is_local,
  bool is_const,
  bool by_value,
  bool write_once,
  const sv_binding_meta_t *binding
) {
  int existing = find_upvalue(c, index, is_local);
  
  if (existing != -1) {
    merge_binding_meta(&c->upval_bindings[existing], binding);
    if (binding && binding->may_tdz) c->upval_descs[existing].may_tdz = true;
    if (!write_once) c->upval_descs[existing].write_once = false;
    return existing;
  }

  if (!ensure_upvalue_capacity(c)) return -1;
  int idx = c->upvalue_count++;
  
  if (idx > UINT16_MAX && !Ant_Exception_Pending(c->js)) js_mkerr_typed(
    c->js, JS_ERR_SYNTAX,
    "Too many captured variables (only %d allowed)", UINT16_MAX
  );
  
  c->upval_descs[idx] = (sv_upval_desc_t){
    .index = index, 
    .is_local = is_local, 
    .is_const = is_const,
    .may_tdz = binding && binding->may_tdz,
    .by_value = by_value,
    .write_once = write_once,
  };
  
  if (binding) c->upval_bindings[idx] = *binding;
  upvalue_index_add(c);
  return idx;
}

int resolve_upvalue_to(sv_compiler_t *c, sv_compiler_t *owner, int local) {
  sv_compiler_t *enc = c->enclosing;
  if (!enc || local < 0) return -1;

  if (enc == owner) {
    enc->locals[local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(enc, local);
    return add_upvalue(c, slot, true, enc->locals[local].is_const);
  }

  int upvalue = resolve_upvalue_to(enc, owner, local);
  if (upvalue == -1) return -1;

  return add_upvalue(c, (uint16_t)upvalue, false, owner->locals[local].is_const);
}

sv_compiler_t *lexical_owner(sv_compiler_t *c) {
  while (c->is_arrow && c->enclosing) c = c->enclosing;
  return c;
}

static sv_compiler_t *enclosing_non_arrow(sv_compiler_t *c) {
  sv_compiler_t *enc = c->enclosing;
  while (enc && enc->is_arrow) enc = enc->enclosing;
  return enc;
}

int resolve_super_upvalue(sv_compiler_t *c) {
  sv_compiler_t *owner = enclosing_non_arrow(c);
  if (!owner || owner->super_local < 0) return -1;
  owner->super_referenced = true;
  return resolve_upvalue_to(c, owner, owner->super_local);
}

int resolve_this_upvalue(sv_compiler_t *c) {
  sv_compiler_t *owner = enclosing_non_arrow(c);
  if (!owner || owner->this_local < 0) return -1;
  return resolve_upvalue_to(c, owner, owner->this_local);
}

int resolve_new_target_upvalue(sv_compiler_t *c) {
  for (sv_compiler_t *enc = c->enclosing; enc; enc = enc->enclosing)
    if (enc->new_target_local >= 0) return resolve_upvalue_to(c, enc, enc->new_target_local);
  return -1;
}

int resolve_upvalue_raw(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!c->enclosing) return -1;

  int local = resolve_local(c->enclosing, name, len);
  if (local != -1) {
    sv_local_t *loc = &c->enclosing->locals[local];
    c->enclosing->locals[local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(c->enclosing, local);
    sv_binding_meta_t binding = loc->binding;
    binding.may_tdz = loc->is_tdz;
    bool by_value = !loc->is_tdz && param_captured_by_value(c->enclosing, local);
    return add_upvalue_binding(c, slot, true, loc->is_const, by_value, local_written_once(c->enclosing, local), &binding);
  }

  int upvalue = resolve_upvalue_raw(c->enclosing, name, len);
  if (upvalue != -1) {
    sv_upval_desc_t *uv = &c->enclosing->upval_descs[upvalue];
    sv_binding_meta_t *meta = &c->enclosing->upval_bindings[upvalue];
    return add_upvalue_binding(c, (uint16_t)upvalue, false, uv->is_const, uv->by_value, uv->write_once, meta);
  }

  return -1;
}

int resolve_upvalue(sv_compiler_t *c, const char *name, uint32_t len) {
  if (name && len) {
    bool lexical_arguments = !sv_compile_mode_is_eval(c->mode) && is_ident_str(name, len, "arguments", 9);
    if (lexical_arguments && !c->is_arrow) return -1;
    if (c->owns_eval_env) return -1;
    for (sv_compiler_t *enc = lexical_arguments ? c->enclosing : NULL; enc; enc = enc->enclosing) {
      if (resolve_local(enc, name, len) >= 0) break;
      if (!enc->is_arrow) return -1;
    }
    for (sv_compiler_t *enc = c->enclosing; enc; enc = enc->enclosing) {
      if (resolve_local(enc, name, len) >= 0) break;
      if (enc->owns_eval_env) return -1;
    }
  }
  return resolve_upvalue_raw(c, name, len);
}

static sv_compiler_t *global_name_shadowed(sv_compiler_t *c, const char *name, uint32_t len) {
  if (has_active_with_scope(c)) return NULL;
  sv_compiler_t *root = c;
  for (sv_compiler_t *cur = c; cur; root = cur, cur = cur->enclosing)
    if (resolve_local(cur, name, len) >= 0) return NULL;
  return root;
}

bool await_allowed(const sv_compiler_t *c) {
  if (c->is_async) return true;
  return c->enclosing && !c->enclosing->enclosing && !c->enclosing->function_ctor_root;
}

bool undefined_shadowed(sv_compiler_t *c) {
  sv_compiler_t *root = global_name_shadowed(c, "undefined", 9);
  return !root || root->eval_shadows_undefined;
}

bool global_this_shadowed(sv_compiler_t *c) {
  sv_compiler_t *root = global_name_shadowed(c, "globalThis", 10);
  return !root || root->eval_shadows_global_this;
}

static bool unwind_reserve(sv_compiler_t *c) {
  int kind_cap = c->unwind_cap;
  int mark_cap = c->unwind_cap;
  
  uint8_t *kinds = Silver_Compiler_Grow(c, c->unwind_kinds, &kind_cap, c->unwind_count + 1, 8, 1);
  if (!kinds) return false;
  c->unwind_kinds = kinds;
  
  sv_unwind_mark_t *marks = Silver_Compiler_Grow(
    c, c->unwind_marks, &mark_cap, c->unwind_count + 1, 8, sizeof(sv_unwind_mark_t));
  if (!marks) return false;
  c->unwind_marks = marks;
  
  c->unwind_cap = mark_cap;
  return true;
}

void unwind_push(sv_compiler_t *c, uint8_t kind) {
  if (c->unwind_count >= c->unwind_cap && !unwind_reserve(c)) {
    c->unwind_lost++;
    return;
  }
  c->unwind_marks[c->unwind_count] = (sv_unwind_mark_t){
    .local_count = c->local_count,
    .using_count = c->using_cleanup_count,
  };
  c->unwind_kinds[c->unwind_count++] = kind;
}

void unwind_pop(sv_compiler_t *c) {
  if (c->unwind_lost) c->unwind_lost--;
  else if (c->unwind_count > 0) c->unwind_count--;
}

void push_loop(
  sv_compiler_t *c, int loop_start,
  const char *label, uint32_t label_len,
  sv_loop_kind_t kind
) {
  bool lost = false;
  if (c->loop_count >= c->loop_cap) {
    sv_loop_t *loops = Silver_Compiler_Grow(c, c->loops, &c->loop_cap, c->loop_count + 1, 4, sizeof(sv_loop_t));
    if (loops) c->loops = loops;
    else lost = true;
  }
  if (!label && c->pending_label) {
    label = c->pending_label;
    label_len = c->pending_label_len;
    c->pending_label = NULL;
    c->pending_label_len = 0;
  }
  if (lost) {
    if (c->loop_lost++ == 0) c->loop_sink = (sv_loop_t){ .loop_start = loop_start, .iter_unwind_index = -1, .iter_completion_local = -1 };
    return;
  }
  c->loops[c->loop_count++] = (sv_loop_t){
    .loop_start = loop_start,
    .scope_depth = c->scope_depth,
    .unwind_depth = c->unwind_count,
    .label = label, .label_len = label_len,
    .is_switch = kind == SV_LOOP_SWITCH || kind == SV_LOOP_RETURN_LANDING,
    .is_block = kind == SV_LOOP_LABEL || kind == SV_LOOP_LABEL_OF_LOOP || kind == SV_LOOP_RETURN_LANDING,
    .labels_loop = kind == SV_LOOP_LABEL_OF_LOOP,
    .iter_unwind_index = -1,
    .iter_completion_local = -1,
    .iterates = kind == SV_LOOP_ITERATION,
    .type_child_mark = c->type_child_count,
    .type_use_start = c->types->use_count,
    .type_fact_mark = c->types->fact_next,
  };
  if (kind == SV_LOOP_ITERATION) c->type_loop_depth++;
}

int find_loop_label(const sv_compiler_t *c, const char *label, uint32_t label_len) {
  for (int i = c->loop_count - 1; i >= 0; i--) if (
    c->loops[i].label &&
    c->loops[i].label_len == label_len &&
    memcmp(c->loops[i].label, label, label_len) == 0
  ) return i;
  return -1;
}

void pop_loop(sv_compiler_t *c) {
  if (c->loop_lost) {
    if (--c->loop_lost) return;
    patch_list_resolve(c, &c->loop_sink.breaks);
    free(c->loop_sink.continues.offsets);
    c->loop_sink = (sv_loop_t){0};
    return;
  }
  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  if (loop->iterates) {
    if (loop->type_child_mark != c->type_child_count) type_loop_leave(c);
    c->type_loop_depth--;
  }
  c->loop_count--;
  patch_list_resolve(c, &loop->breaks);
  free(loop->continues.offsets);
}

void begin_scope(sv_compiler_t *c) {
  c->scope_depth++;
}

static inline bool spill_allowed(const sv_compiler_t *c) {
  if (c->spill_disabled || !c->enclosing) return false;
  if (c->scope_depth == 0 && (is_repl_root(c) || sv_compile_mode_is_eval(c->mode))) return false;
  if (c->scope_depth == 0 && c->mode == SV_COMPILE_MODULE && !c->enclosing->enclosing) return false;
  return c->local_count - c->param_locals >= SV_SPILL_THRESHOLD;
}

int resolve_spill_slow(const sv_compiler_t *c, const char *name, uint32_t len) {
  if (!c->spill_count) return -1;
  uint32_t hash = sv_compile_ctx_hash_local_name(name, len);
  for (int i = c->spill_heads[hash & (uint32_t)(c->spill_head_cap - 1)]; i != -1; i = c->spills[i].next) {
    const sv_spill_binding_t *spill = &c->spills[i];
    if (spill->hash == hash && spill->len == len && memcmp(spill->name, name, len) == 0) return i;
  }
  return -1;
}

static int spill_scope_for(sv_compiler_t *c) {
  int block_depth = c->scope_depth;
  for (int i = c->with_depth - 1; i >= 0; i--) {
    if (c->with_scopes[i].implicit && c->with_scopes[i].block_depth == block_depth) return i;
    if (c->with_scopes[i].block_depth < block_depth) break;
  }

  int local = add_hidden_local(c);
  emit_op(c, OP_OBJECT);
  emit_op(c, OP_NULL);
  emit_op(c, OP_SET_PROTO);
  emit_put_local(c, local);

  if (!push_with_scope(c, (sv_with_scope_t){
    .local = local, .depth = block_depth == 0 ? -1 : block_depth,
    .block_depth = block_depth, .implicit = true,
  })) return -1;
  return c->with_depth - 1;
}

static bool spill_table_grow(sv_compiler_t *c) {
  if (c->spill_count >= c->spill_cap) {
    sv_spill_binding_t *spills = Silver_Compiler_Grow(c, c->spills, &c->spill_cap, c->spill_count + 1, 1024, sizeof(*c->spills));
    if (!spills) return false;
    c->spills = spills;
  }
  if (c->spill_count * 2 < c->spill_head_cap) return true;

  int *heads = Silver_Compiler_Grow(c, c->spill_heads, &c->spill_head_cap, c->spill_count * 2 + 1, 2048, sizeof(*c->spill_heads));
  if (!heads) return false;
  c->spill_heads = heads;
  memset(c->spill_heads, 0xff, (size_t)c->spill_head_cap * sizeof(*c->spill_heads));
  for (int i = 0; i < c->spill_count; i++) {
    int bucket = (int)(c->spills[i].hash & (uint32_t)(c->spill_head_cap - 1));
    c->spills[i].next = c->spill_heads[bucket];
    c->spill_heads[bucket] = i;
  }
  return true;
}

void emit_spill_define(sv_compiler_t *c, int spill) {
  emit_get_local(c, c->with_scopes[c->spills[spill].scope].local);
  emit_op(c, OP_SWAP);
  emit_atom_op(c, OP_DEFINE_FIELD, c->spills[spill].name, c->spills[spill].len);
  emit_op(c, OP_POP);
}

bool declare_spilled_slow(sv_compiler_t *c, const char *name, uint32_t len, bool is_const, bool define) {
  int existing = resolve_spill(c, name, len);
  if (existing >= 0 && c->spills[existing].depth == c->scope_depth) return true;
  if (!spill_allowed(c)) return false;

  if (!spill_table_grow(c)) return false;
  int scope = spill_scope_for(c);
  if (scope < 0) return false;
  uint32_t hash = sv_compile_ctx_hash_local_name(name, len);
  int bucket = (int)(hash & (uint32_t)(c->spill_head_cap - 1));
  int index = c->spill_count++;
  c->spills[index] = (sv_spill_binding_t){
    .name = name, .len = len, .hash = hash,
    .depth = c->scope_depth, .scope = scope,
    .next = c->spill_heads[bucket], .is_const = is_const,
  };
  c->spill_heads[bucket] = index;

  if (define) {
    emit_op(c, OP_UNDEF);
    emit_spill_define(c, index);
  }
  return true;
}

static const sv_spill_binding_t *nearest_spill(sv_compiler_t *c, const char *name, uint32_t len) {
  for (sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    int local = resolve_local(cur, name, len);
    int spill = resolve_spill(cur, name, len);
    if (spill >= 0 && (local < 0 || cur->locals[local].depth < cur->spills[spill].depth)) return &cur->spills[spill];
    if (local >= 0 || cur->owns_eval_env) break;
  }
  return NULL;
}

bool spilled_const_assignment(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!has_active_with_scope(c)) return false;
  const sv_spill_binding_t *spill = nearest_spill(c, name, len);
  return spill && spill->is_const;
}

bool spilled_binding_visible(sv_compiler_t *c, const char *name, uint32_t len) {
  return has_active_with_scope(c) && nearest_spill(c, name, len) != NULL;
}

static void end_spill_scopes(sv_compiler_t *c) {
  while (c->spill_count > 0 && c->spills[c->spill_count - 1].depth >= c->scope_depth) {
    sv_spill_binding_t *spill = &c->spills[--c->spill_count];
    c->spill_heads[spill->hash & (uint32_t)(c->spill_head_cap - 1)] = spill->next;
  }
  while (
    c->with_depth > 0 && c->with_scopes[c->with_depth - 1].implicit &&
    c->with_scopes[c->with_depth - 1].block_depth >= c->scope_depth
  ) c->with_depth--;
  c->eval_scope_shareable = false;
}

void end_scope(sv_compiler_t *c) {
  if (c->spill_count) end_spill_scopes(c);
  while (
    c->local_count > 0 &&
    c->locals[c->local_count - 1].depth >= c->scope_depth
  ) {
    sv_local_t *loc = &c->locals[c->local_count - 1];
    if (loc->captured) {
      int frame_slot = local_to_frame_slot(c, c->local_count - 1);
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)frame_slot);
      if (!c->handler_close_floor || frame_slot < c->handler_close_floor - 1)
        c->handler_close_floor = frame_slot + 1;
    }
    sv_compile_ctx_local_lookup_remove(c, c->local_count - 1);
    c->local_count--;
  }
  if (c->eval_scope_shareable) eval_scope_locals_popped(c);
  c->scope_depth--;
}

void emit_close_upvals(sv_compiler_t *c) {
  for (int i = 0; i < c->local_count; i++) {
  if (c->locals[i].captured) {
    int frame_slot = local_to_frame_slot(c, i);
    emit_op(c, OP_CLOSE_UPVAL);
    emit_u16(c, (uint16_t)frame_slot);
    return;
  }}
}

bool push_with_scope(sv_compiler_t *c, sv_with_scope_t scope) {
  if (c->with_depth >= c->with_cap) {
    sv_with_scope_t *scopes = Silver_Compiler_Grow(c, c->with_scopes, &c->with_cap, c->with_depth + 1, 4, sizeof(*scopes));
    if (!scopes) return false;
    c->with_scopes = scopes;
  }
  c->with_scopes[c->with_depth++] = scope;
  return true;
}

typedef struct {
  sv_compiler_t *c;
  sv_with_refs_t *out;
} with_refs_sink_t;

static void with_refs_push(sv_compiler_t *c, sv_with_refs_t *out, sv_with_ref_t ref) {
  if (out->count >= out->cap) {
    sv_with_ref_t *refs = Silver_Compiler_Grow(c, out->refs, &out->cap, out->count + 1, 4, sizeof(*out->refs));
    if (!refs) return;
    out->refs = refs;
  }
  out->refs[out->count++] = ref;
}

static void with_refs_push_marker(void *ctx, const char *marker, uint32_t len) {
  with_refs_sink_t *sink = ctx;
  with_refs_push(sink->c, sink->out, (sv_with_ref_t){
    .kind = SV_WITH_REF_ENV, .implicit = len > 1 && marker[1] == 's',
    .marker = marker, .marker_len = len,
  });
}

static void with_marker_seen(void *ctx, const char *marker, uint32_t len) {
  *(bool *)ctx = true;
}

static bool eval_declares_var(const sv_compiler_t *body, const char *name, uint32_t len) {
  return body && find_eval_var(body, name, len) >= 0;
}

void with_shadows(sv_compiler_t *c, const char *name, uint32_t len, sv_with_refs_t *out) {
  out->count = 0;
  if (!has_active_with_scope(c) || is_ident_str(name, len, "super", 5)) return;

  sv_compiler_t *inner = NULL;
  for (sv_compiler_t *cur = c; cur; inner = cur, cur = cur->enclosing) {
    int local = resolve_local(cur, name, len);
    for (int i = cur->with_depth - 1; i >= 0; i--) {
      sv_with_scope_t *scope = &cur->with_scopes[i];
      if (local >= 0 && cur->locals[local].depth >= scope->depth) continue;
      int slot = cur == c
        ? local_to_frame_slot(c, scope->local) - c->param_count
        : resolve_upvalue_to(c, cur, scope->local);
      if (slot >= 0) with_refs_push(c, out, (sv_with_ref_t){
        .kind = cur == c ? SV_WITH_REF_LOCAL : SV_WITH_REF_UPVALUE,
        .implicit = scope->implicit, .idx = (uint16_t)slot,
      });
    }
    if (local >= 0 || cur->owns_eval_env) break;
    if (cur->eval_with) sv_eval_env_each_with(
      c->js, cur->eval_env, name, len, eval_declares_var(inner, name, len), with_refs_push_marker,
      &(with_refs_sink_t){ .c = c, .out = out });
  }
}

bool local_with_shadowed(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!has_active_with_scope(c) || is_ident_str(name, len, "super", 5)) return false;

  sv_compiler_t *inner = NULL;
  for (sv_compiler_t *cur = c; cur; inner = cur, cur = cur->enclosing) {
    int local = resolve_local(cur, name, len);
    for (int i = cur->with_depth - 1; i >= 0; i--)
      if (local < 0 || cur->locals[local].depth < cur->with_scopes[i].depth) return true;
    if (local >= 0 || cur->owns_eval_env) break;
    if (cur->eval_with) {
      bool seen = false;
      sv_eval_env_each_with(c->js, cur->eval_env, name, len, eval_declares_var(inner, name, len), with_marker_seen, &seen);
      if (seen) return true;
    }
  }
  return false;
}

static void emit_with_ref_load(sv_compiler_t *c, const sv_with_ref_t *ref) {
  if (ref->kind == SV_WITH_REF_ENV) {
    emit_atom_op(c, OP_GET_EVAL_GLOBAL, ref->marker, ref->marker_len);
  } else if (ref->kind == SV_WITH_REF_UPVALUE) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, ref->idx);
  } else if (ref->idx <= 255) {
    emit_op(c, OP_GET_LOCAL8);
    emit(c, (uint8_t)ref->idx);
  } else {
    emit_op(c, OP_GET_LOCAL);
    emit_u16(c, ref->idx);
  }
}

int emit_with_try(sv_compiler_t *c, sv_op_t op, const sv_with_ref_t *ref, const char *name, uint32_t len) {
  emit_with_ref_load(c, ref);
  emit_op(c, op);
  emit_u32(c, (uint32_t)add_atom(c, name, len));
  int jump = c->code_len;
  emit_i32(c, 0);
  return jump;
}

void emit_with_tries(
  sv_compiler_t *c, sv_with_refs_t *shadows, sv_op_t op,
  const char *name, uint32_t len, int pops, sv_patch_list_t *out
) {
  for (int i = 0; i < shadows->count; i++) {
    patch_list_add(c, out, emit_with_try(c, op, &shadows->refs[i], name, len));
    for (int p = 0; p < pops; p++) emit_op(c, OP_POP);
  }
  
  free(shadows->refs);
  *shadows = (sv_with_refs_t){0};
}
