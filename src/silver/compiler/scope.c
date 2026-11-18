#include "compiler_internal.h"

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
  
  int new_cap = c->slot_type_cap ? c->slot_type_cap : 16;
  while (new_cap <= slot) new_cap *= 2;
  
  sv_type_info_t *next = realloc(c->slot_types, (size_t)new_cap * sizeof(sv_type_info_t));
  if (!next) return;
  
  memset(next + c->slot_type_cap, 0, (size_t)(new_cap - c->slot_type_cap) * sizeof(sv_type_info_t));
  c->slot_types = next;
  c->slot_type_cap = new_cap;
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
    c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
    c->locals = realloc(c->locals, (size_t)c->local_cap * sizeof(sv_local_t));
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
  int base = c->local_count;
  for (int i = 0; i < count; i++)
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

void set_local_inferred_type(sv_compiler_t *c, int local_idx, uint8_t type) {
  if (local_idx < 0 || local_idx >= c->local_count) return;
  c->locals[local_idx].inferred_type = type;
  c->locals[local_idx].str_accumulating = false;
  if (c->locals[local_idx].depth == -1) return;
  int slot = local_idx - c->param_locals;
  mark_slot_type(c, slot, type);
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

static int find_upvalue(sv_compiler_t *c, uint16_t index, bool is_local) {
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

static void ensure_upvalue_capacity(sv_compiler_t *c) {
  if (c->upvalue_count < c->upvalue_cap) return;

  int old_cap = c->upvalue_cap;
  c->upvalue_cap = c->upvalue_cap ? c->upvalue_cap * 2 : 8;
  
  c->upval_descs = realloc(
    c->upval_descs,
    (size_t)c->upvalue_cap * sizeof(sv_upval_desc_t)
  );
  
  c->upval_bindings = realloc(
    c->upval_bindings,
    (size_t)c->upvalue_cap * sizeof(sv_binding_meta_t)
  );

  if (c->upval_bindings) memset(
    c->upval_bindings + old_cap, 0,
    (size_t)(c->upvalue_cap - old_cap) * sizeof(sv_binding_meta_t)
  );
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

  ensure_upvalue_capacity(c);
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
  return idx;
}

int add_upvalue_tdz(sv_compiler_t *c, uint16_t index, bool is_local, bool may_tdz) {
  sv_binding_meta_t binding = {.may_tdz = may_tdz};
  return add_upvalue_binding(c, index, is_local, false, false, false, &binding);
}

static int resolve_upvalue_to(sv_compiler_t *c, sv_compiler_t *owner, int local) {
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

int resolve_arguments_upvalue(sv_compiler_t *c) {
  for (sv_compiler_t *enc = c->enclosing; enc; enc = enc->enclosing) {
    if (enc->owns_eval_env || sv_compile_mode_is_eval(enc->mode)) return -1;
    if (!enc->is_arrow) return resolve_upvalue_to(c, enc, enc->strict_args_local);
  }
  return -1;
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
    if (!c->is_arrow && !sv_compile_mode_is_eval(c->mode) && is_ident_str(name, len, "arguments", 9)) return -1;
    if (c->owns_eval_env) return -1;
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

void unwind_push(sv_compiler_t *c, uint8_t kind) {
  if (c->unwind_count >= c->unwind_cap) {
    c->unwind_cap = c->unwind_cap ? c->unwind_cap * 2 : 8;
    c->unwind_kinds = realloc(c->unwind_kinds, (size_t)c->unwind_cap);
  }
  c->unwind_kinds[c->unwind_count++] = kind;
}

void unwind_pop(sv_compiler_t *c) {
  if (c->unwind_count > 0) c->unwind_count--;
}

void push_loop(
  sv_compiler_t *c, int loop_start,
  const char *label, uint32_t label_len,
  sv_loop_kind_t kind
) {
  if (c->loop_count >= c->loop_cap) {
    c->loop_cap = c->loop_cap ? c->loop_cap * 2 : 4;
    c->loops = realloc(c->loops, (size_t)c->loop_cap * sizeof(sv_loop_t));
  }
  if (!label && c->pending_label) {
    label = c->pending_label;
    label_len = c->pending_label_len;
    c->pending_label = NULL;
    c->pending_label_len = 0;
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
  };
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
  sv_loop_t *loop = &c->loops[--c->loop_count];
  patch_list_resolve(c, &loop->breaks);
  free(loop->continues.offsets);
}

void begin_scope(sv_compiler_t *c) {
  c->scope_depth++;
}

void end_scope(sv_compiler_t *c) {
  while (
    c->local_count > 0 &&
    c->locals[c->local_count - 1].depth >= c->scope_depth
  ) {
    sv_local_t *loc = &c->locals[c->local_count - 1];
    if (loc->captured) {
      int frame_slot = local_to_frame_slot(c, c->local_count - 1);
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)frame_slot);
    }
    sv_compile_ctx_local_lookup_remove(c, c->local_count - 1);
    c->local_count--;
  }
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

static void with_refs_push(sv_with_refs_t *out, bool is_upval, int idx) {
  if (idx < 0) return;
  if (out->count >= out->cap) {
    out->cap = out->cap ? out->cap * 2 : 4;
    out->refs = realloc(out->refs, (size_t)out->cap * sizeof(*out->refs));
  }
  out->refs[out->count++] = (sv_with_ref_t){ .is_upval = is_upval, .idx = (uint16_t)idx };
}

void with_shadows(sv_compiler_t *c, const char *name, uint32_t len, sv_with_refs_t *out) {
  out->count = 0;
  if (!has_active_with_scope(c) || is_ident_str(name, len, "super", 5)) return;

  for (sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    int local = resolve_local(cur, name, len);
    for (int i = cur->with_depth - 1; i >= 0; i--) {
      sv_with_scope_t *scope = &cur->with_scopes[i];
      if (local >= 0 && cur->locals[local].depth >= scope->depth) continue;
      if (cur == c) {
        with_refs_push(out, false, local_to_frame_slot(c, scope->local) - c->param_count);
      } else with_refs_push(out, true, resolve_upvalue_to(c, cur, scope->local));
    }
    if (local >= 0 || cur->owns_eval_env) break;
  }
}

bool local_with_shadowed(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!has_active_with_scope(c) || is_ident_str(name, len, "super", 5)) return false;

  for (sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    int local = resolve_local(cur, name, len);
    for (int i = cur->with_depth - 1; i >= 0; i--)
      if (local < 0 || cur->locals[local].depth < cur->with_scopes[i].depth) return true;
    if (local >= 0 || cur->owns_eval_env) break;
  }
  return false;
}

static void emit_with_ref_load(sv_compiler_t *c, const sv_with_ref_t *ref) {
  if (ref->is_upval) {
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

static int emit_with_try(sv_compiler_t *c, sv_op_t op, const sv_with_ref_t *ref, const char *name, uint32_t len) {
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
    patch_list_add(out, emit_with_try(c, op, &shadows->refs[i], name, len));
    for (int p = 0; p < pops; p++) emit_op(c, OP_POP);
  }
  
  free(shadows->refs);
  *shadows = (sv_with_refs_t){0};
}
