#include <compat.h> // IWYU pragma: keep

#include "compiler_internal.h"

typedef struct {
  uint32_t *slots;
  uint32_t capacity;
} sv_eval_binding_set_t;

static bool eval_binding_set_init(
  sv_compiler_t *c, sv_eval_binding_set_t *set, size_t candidate_count
) {
  memset(set, 0, sizeof(*set));
  if (candidate_count == 0) return true;

  if (candidate_count > UINT32_MAX / 4) {
    js_mkerr(c->js, "too many bindings while capturing direct eval scope");
    return false;
  }

  uint32_t capacity = 16;
  uint32_t needed = (uint32_t)candidate_count * 2;

  while (capacity < needed) {
    if (capacity > UINT32_MAX / 2) {
      js_mkerr(c->js, "too many bindings while capturing direct eval scope");
      return false;
    }
    capacity *= 2;
  }

  if ((size_t)capacity > SIZE_MAX / sizeof(uint32_t)) {
    js_mkerr(c->js, "too many bindings while capturing direct eval scope");
    return false;
  }

  uint32_t *slots = malloc((size_t)capacity * sizeof(uint32_t));
  if (!slots) {
    js_mkerr(c->js, "out of memory while indexing direct eval scope");
    return false;
  }

  memset(slots, 0xff, (size_t)capacity * sizeof(uint32_t));
  set->slots = slots;
  set->capacity = capacity;

  return true;
}

static uint32_t *eval_binding_set_slot(
  sv_eval_binding_set_t *set, const sv_eval_scope_builder_t *scope,
  const char *name, uint32_t len
) {
  if (!set->slots || set->capacity == 0) return NULL;
  uint32_t slot = sv_compile_ctx_hash_local_name(name, len) & (set->capacity - 1);

  for (;;) {
    uint32_t binding_index = set->slots[slot];
    if (binding_index == UINT32_MAX) return &set->slots[slot];
    const sv_runtime_binding_t *binding = &scope->bindings[binding_index];
    if (binding->len == len && memcmp(binding->name, name, len) == 0)
      return &set->slots[slot];
    slot = (slot + 1) & (set->capacity - 1);
  }
}

static bool eval_scope_builder_has_binding(
  sv_eval_binding_set_t *set, const sv_eval_scope_builder_t *scope,
  const char *name, uint32_t len
) {
  uint32_t *slot = eval_binding_set_slot(set, scope, name, len);
  return slot && *slot != UINT32_MAX;
}

static bool eval_scope_builder_add_binding(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set,
  const char *name, uint32_t len,
  uint8_t kind, uint16_t index, bool is_const, const sv_binding_meta_t *meta
) {
  if (!name || len == 0) return true;
  uint32_t *lookup_slot = eval_binding_set_slot(set, scope, name, len);
  if (!lookup_slot || *lookup_slot != UINT32_MAX) return true;

  if (scope->count >= scope->capacity) {
    uint32_t capacity = scope->capacity ? scope->capacity * 2 : 8;
    sv_runtime_binding_t *bindings = realloc(
      scope->bindings,
      (size_t)capacity * sizeof(sv_runtime_binding_t));
    if (!bindings) {
      js_mkerr(c->js, "out of memory while capturing direct eval scope");
      return false;
    }
    scope->bindings = bindings;
    scope->capacity = capacity;
  }
  uint32_t binding_index = scope->count++;
  if (len == 9 && memcmp(name, "arguments", 9) == 0) scope->binds_arguments = true;
  if (meta->import_kind == SV_IMPORT_BIND_DEFAULT) kind |= SV_EVAL_BIND_IMPORT_DEFAULT;
  if (meta->import_kind == SV_IMPORT_BIND_NAMED) kind |= SV_EVAL_BIND_IMPORT_NAMED;
  
  scope->bindings[binding_index] = (sv_runtime_binding_t){
    name, len, index, kind, is_const,
    meta->import_name ? intern_string(c->js, meta->import_name, meta->import_name_len) : NULL,
    meta->import_name_len
  };
  
  *lookup_slot = binding_index;
  return true;
}

static inline bool eval_local_is_visible(const sv_local_t *local) {
  return local && local->name && local->name_len > 0;
}

static bool eval_scope_builder_capture_local(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_local_t *local, int local_index
) {
  if (!eval_local_is_visible(local) || eval_scope_builder_has_binding(
        set, scope, local->name, local->name_len)) return true;

  uint8_t kind = local->depth == -1
    ? SV_EVAL_BIND_PARAM : SV_EVAL_BIND_LOCAL;
  uint16_t index = kind == SV_EVAL_BIND_PARAM
    ? (uint16_t)local_index
    : (uint16_t)(local_index - c->param_locals);

  if (!eval_scope_builder_add_binding(
        c, scope, set, local->name, local->name_len,
        kind | local->eval_flags,
        index, local->is_const, &local->binding)) return false;
  local->captured = true;
  c->type_child_count++;
  return true;
}

static bool eval_scope_builder_capture_upvalue(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_local_t *local, bool own_arguments
) {
  if (!eval_local_is_visible(local) || eval_scope_builder_has_binding(
        set, scope, local->name, local->name_len)) return true;
  if (own_arguments && is_ident_str(local->name, local->name_len, "arguments", 9)) return true;

  if (is_ident_str(local->name, local->name_len, "super", 5)) return true;
  int upvalue = resolve_upvalue_raw(c, local->name, local->name_len);
  if (upvalue < 0) return true;
  return eval_scope_builder_add_binding(
    c, scope, set, local->name, local->name_len,
    SV_EVAL_BIND_UPVALUE, (uint16_t)upvalue,
    c->upval_descs[upvalue].is_const, &c->upval_bindings[upvalue]);
}

static _Atomic uint64_t with_marker_sequence;

static bool eval_scope_builder_capture_with(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_compiler_t *owner, sv_with_scope_t *with
) {
  static const sv_binding_meta_t no_meta = {0};
  if (!with->marker) {
    char name[32];
    int len = snprintf(
      name, sizeof(name), with->implicit ? "\x01scope:%llu" : "\x01with:%llu",
      (unsigned long long)++with_marker_sequence);
    with->marker = intern_string(c->js, name, (size_t)len);
    with->marker_len = (uint32_t)len;
    if (!with->marker) return false;
  }

  if (owner == c) {
    c->locals[with->local].captured = true;
    return eval_scope_builder_add_binding(
      c, scope, set, with->marker, with->marker_len,
      SV_EVAL_BIND_LOCAL, (uint16_t)(with->local - c->param_locals), true, &no_meta);
  }

  int upvalue = resolve_upvalue_to(c, owner, with->local);
  return upvalue < 0 || eval_scope_builder_add_binding(
    c, scope, set, with->marker, with->marker_len,
    SV_EVAL_BIND_UPVALUE, (uint16_t)upvalue, true, &no_meta);
}

static bool eval_scope_builder_capture_locals(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set
) {
  int with = c->with_depth - 1;
  for (int i = c->local_count - 1; i >= 0; i--) {
    for (; with >= 0 && c->with_scopes[with].depth > c->locals[i].depth; with--)
      if (!eval_scope_builder_capture_with(c, scope, set, c, &c->with_scopes[with])) return false;
    if (!eval_scope_builder_capture_local(
          c, scope, set, &c->locals[i], i)) return false;
  }
  for (; with >= 0; with--)
    if (!eval_scope_builder_capture_with(c, scope, set, c, &c->with_scopes[with])) return false;
  return true;
}

static bool compiler_has_super_binding(sv_compiler_t *c) {
  if (c->super_local >= 0 || lexical_owner(c)->eval_super_binding) return true;
  return c->is_arrow && resolve_super_upvalue(c) >= 0;
}

static bool eval_scope_builder_capture_upvalues(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_compiler_t *enclosing, bool own_arguments
) {
  int with = enclosing->with_depth - 1;
  for (int i = enclosing->local_count - 1; i >= 0; i--) {
    for (; with >= 0 && enclosing->with_scopes[with].depth > enclosing->locals[i].depth; with--)
      if (!eval_scope_builder_capture_with(c, scope, set, enclosing, &enclosing->with_scopes[with])) return false;
    if (!eval_scope_builder_capture_upvalue(
          c, scope, set, &enclosing->locals[i], own_arguments)) return false;
  }
  for (; with >= 0; with--)
    if (!eval_scope_builder_capture_with(c, scope, set, enclosing, &enclosing->with_scopes[with])) return false;
  return true;
}

static bool eval_scope_builder_capture_keywords(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope, sv_eval_binding_set_t *set
) {
  static const sv_binding_meta_t no_meta = {0};
  int this_upvalue = resolve_this_upvalue(c);
  if (this_upvalue >= 0 && !eval_scope_builder_add_binding(
        c, scope, set, "this", 4, SV_EVAL_BIND_UPVALUE, (uint16_t)this_upvalue, false, &no_meta)) return false;
  int thunk_upvalue = resolve_upvalue_raw(c, SV_FIELD_THUNK_NAME, SV_FIELD_THUNK_LEN);
  if (thunk_upvalue >= 0 && !eval_scope_builder_add_binding(
        c, scope, set, SV_FIELD_THUNK_NAME, SV_FIELD_THUNK_LEN, SV_EVAL_BIND_UPVALUE, (uint16_t)thunk_upvalue, false, &no_meta)) return false;
  int super_upvalue = resolve_super_upvalue(c);
  return super_upvalue < 0 || eval_scope_builder_add_binding(
    c, scope, set, "super", 5, SV_EVAL_BIND_UPVALUE, (uint16_t)super_upvalue, false, &no_meta);
}

typedef struct {
  sv_compiler_t *c;
  sv_eval_scope_builder_t *scope;
  sv_eval_binding_set_t *set;
} private_capture_t;

static bool eval_scope_builder_capture_private(
  void *ctx, const char *name, uint32_t len, uint8_t kind, uint16_t index
) {
  static const sv_binding_meta_t no_meta = {0};
  private_capture_t *capture = ctx;
  return eval_scope_builder_add_binding(
    capture->c, capture->scope, capture->set, name, len, kind, index, true, &no_meta);
}

bool arguments_forbidden(const sv_compiler_t *c) {
  for (const sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (cur->in_field_init || cur->no_arguments_scope || cur->eval_no_arguments) return true;
    if (!cur->is_arrow) return cur->enclosing && !cur->enclosing->enclosing && cur->enclosing->eval_no_arguments;
  }
  return false;
}

static const sv_private_scope_t *eval_private_scope(const sv_compiler_t *c) {
  const sv_private_scope_t *scope = c->private_scope;
  while (scope && scope->count == 0) scope = scope->parent;
  return scope;
}

static sv_eval_scope_flags_t eval_scope_flags(sv_compiler_t *c) {
  const sv_compiler_t *root = c;
  while (root->enclosing) root = root->enclosing;
  return (sv_eval_scope_flags_t){
    .this_binding = compiler_has_this_binding(c),
    .super_binding = compiler_has_super_binding(c),
    .is_static = c->is_static,
    .no_arguments = arguments_forbidden(c),
    .has_with = has_active_with_scope(c),
    .has_private = eval_private_scope(c) != NULL || root->eval_private,
  };
}

void eval_scope_locals_popped(sv_compiler_t *c) {
  while (c->eval_scope_shareable && c->eval_scopes[c->eval_scope_shared].local_count > c->local_count) {
    uint32_t parent = c->eval_scopes[c->eval_scope_shared].parent;
    if (!parent) c->eval_scope_shareable = false;
    else c->eval_scope_shared = (int)parent - 1;
  }
}

static bool eval_scope_reserve(sv_compiler_t *c) {
  if (c->eval_scope_count < c->eval_scope_cap) return true;
  int capacity = c->eval_scope_cap ? c->eval_scope_cap * 2 : 4;
  sv_eval_scope_builder_t *scopes = realloc(
    c->eval_scopes,
    (size_t)capacity * sizeof(sv_eval_scope_builder_t));
  if (!scopes) {
    js_mkerr(c->js, "out of memory while growing direct eval scopes");
    return false;
  }
  c->eval_scopes = scopes;
  c->eval_scope_cap = capacity;
  return true;
}

static void eval_scope_publish(sv_compiler_t *c, uint32_t scope_index) {
  c->eval_scopes[scope_index].local_count = c->local_count;
  c->eval_scope_shared = (int)scope_index;
  c->eval_scope_shared_withs = c->with_depth;
  c->eval_scope_shared_private = eval_private_scope(c);
  c->eval_scope_shareable = true;
}

static bool capture_local_range_scope(
  sv_compiler_t *c, uint32_t parent, int first_local, int end_local,
  sv_eval_scope_flags_t flags, uint32_t *scope_index_out
) {
  if (!eval_scope_reserve(c)) return false;
  uint32_t scope_index = (uint32_t)c->eval_scope_count++;
  sv_eval_scope_builder_t *scope = &c->eval_scopes[scope_index];
  memset(scope, 0, sizeof(*scope));
  scope->parent = parent;
  scope->flags = flags;
  scope->local_count = end_local;

  sv_eval_binding_set_t binding_set;
  if (!eval_binding_set_init(c, &binding_set, (size_t)(end_local - first_local))) return false;
  bool ok = true;
  for (int i = end_local - 1; ok && i >= first_local; i--)
    ok = eval_scope_builder_capture_local(c, scope, &binding_set, &c->locals[i], i);
  free(binding_set.slots);
  if (!ok) return false;
  *scope_index_out = scope_index;
  return true;
}

static constexpr size_t SV_EVAL_SCOPE_FLAT_BUDGET = 65536;

static size_t eval_scope_flat_cost(const sv_compiler_t *c) {
  size_t budget = SV_EVAL_SCOPE_FLAT_BUDGET - c->eval_scope_flat_used;
  size_t count = (size_t)c->local_count + (size_t)c->with_depth + (size_t)private_eval_binding_count(c);
  for (const sv_compiler_t *enc = c->enclosing; enc && count <= budget; enc = enc->enclosing)
    count += (size_t)enc->local_count + (size_t)enc->with_depth;
  return count <= budget ? count : 0;
}

static int share_dynamic_eval_scope(sv_compiler_t *c, uint32_t *scope_index_out) {
  if (
    !c->eval_scope_shareable || c->eval_scope_shared_withs != c->with_depth ||
    c->eval_scope_shared_private != eval_private_scope(c)
  ) return 0;

  sv_eval_scope_builder_t *shared = &c->eval_scopes[c->eval_scope_shared];
  sv_eval_scope_flags_t flags = eval_scope_flags(c);
  if (c->super_local >= 0) c->super_referenced = true;

  bool same_locals = shared->local_count == c->local_count;
  if (same_locals && memcmp(&flags, &shared->flags, sizeof(flags)) == 0) {
    c->type_child_count++;
    *scope_index_out = (uint32_t)c->eval_scope_shared;
    return 1;
  }
  if (eval_scope_flat_cost(c)) return 0;

  uint32_t scope_index;
  if (!capture_local_range_scope(
        c, (uint32_t)c->eval_scope_shared + 1, shared->local_count, c->local_count, flags, &scope_index)) return -1;
  if (same_locals) c->type_child_count++;
  else eval_scope_publish(c, scope_index);
  *scope_index_out = scope_index;
  return 1;
}

static int eval_scope_base_locals(const sv_compiler_t *c) {
  if (c->with_depth > 0) return c->local_count;
  for (int i = 1; i < c->local_count; i++)
    if (c->locals[i].depth > c->locals[i - 1].depth && c->locals[i].depth > 0) return i;
  return c->local_count;
}

bool capture_dynamic_eval_scope(
  sv_compiler_t *c, uint32_t *scope_index_out
) {
  int shared = share_dynamic_eval_scope(c, scope_index_out);
  if (shared) return shared > 0;
  if (!eval_scope_reserve(c)) return false;

  uint32_t scope_index = (uint32_t)c->eval_scope_count++;
  sv_eval_scope_builder_t *scope = &c->eval_scopes[scope_index];
  memset(scope, 0, sizeof(*scope));

  size_t candidate_count = (size_t)c->local_count + (size_t)c->with_depth + (size_t)private_eval_binding_count(c);
  for (sv_compiler_t *enc = c->enclosing; enc; enc = enc->enclosing) {
    if ((size_t)enc->local_count + (size_t)enc->with_depth > SIZE_MAX - candidate_count) {
      js_mkerr(c->js, "too many bindings while capturing direct eval scope");
      return false;
    }
    candidate_count += (size_t)enc->local_count + (size_t)enc->with_depth;
  }

  sv_eval_binding_set_t binding_set;
  if (!eval_binding_set_init(c, &binding_set, candidate_count)) return false;

  size_t flat_cost = eval_scope_flat_cost(c);
  int base_locals = flat_cost ? c->local_count : eval_scope_base_locals(c);
  c->eval_scope_flat_used += (uint32_t)flat_cost;
  if (base_locals == c->local_count) {
    if (!eval_scope_builder_capture_locals(
          c, scope, &binding_set)) goto capture_failed;
  } else {
    for (int i = base_locals - 1; i >= 0; i--)
      if (!eval_scope_builder_capture_local(c, scope, &binding_set, &c->locals[i], i)) goto capture_failed;
  }

  scope->flags = eval_scope_flags(c);
  if (c->super_local >= 0) c->super_referenced = true;
  if (c->is_arrow && !eval_scope_builder_capture_keywords(c, scope, &binding_set)) goto capture_failed;

  bool own_arguments = has_implicit_arguments_obj(c);
  for (sv_compiler_t *enc = c->inherits_eval_env ? NULL : c->enclosing; enc; enc = enc->enclosing)
    if (!eval_scope_builder_capture_upvalues(
          c, scope, &binding_set, enc, own_arguments)) goto capture_failed;
  private_capture_t private_capture = { c, scope, &binding_set };
  if (!private_eval_bindings_each(c, eval_scope_builder_capture_private, &private_capture)) goto capture_failed;
  free(binding_set.slots);
  for (int first = base_locals; first < c->local_count;) {
    int end = first + 1;
    while (end < c->local_count && c->locals[end].depth <= c->locals[end - 1].depth) end++;
    c->eval_scopes[scope_index].local_count = first;
    if (!capture_local_range_scope(c, scope_index + 1, first, end, c->eval_scopes[scope_index].flags, &scope_index)) return false;
    first = end;
  }
  eval_scope_publish(c, scope_index);
  *scope_index_out = scope_index;
  return true;

capture_failed:
  free(binding_set.slots);
  return false;
}

bool is_sloppy_eval(const sv_compiler_t *c) {
  return (sv_compile_mode_is_eval(c->mode) && !c->is_strict) || is_repl_root(c);
}

static void eval_var_index_insert(sv_compiler_t *c, uint32_t entry) {
  uint32_t mask = c->eval_var_index_cap - 1;
  uint32_t slot = sv_compile_ctx_hash_local_name(c->eval_vars[entry].str, c->eval_vars[entry].len) & mask;
  while (c->eval_var_index[slot] != UINT32_MAX) slot = (slot + 1) & mask;
  c->eval_var_index[slot] = entry;
}

static void eval_var_index_rebuild(sv_compiler_t *c) {
  uint32_t cap = 64;
  while (cap < c->eval_var_count * 2 + 2) cap *= 2;
  uint32_t *index = realloc(c->eval_var_index, (size_t)cap * sizeof(*index));
  if (!index) { c->eval_var_index_cap = 0; return; }
  memset(index, 0xff, (size_t)cap * sizeof(*index));
  c->eval_var_index = index;
  c->eval_var_index_cap = cap;
  for (uint32_t i = 0; i < c->eval_var_count; i++) eval_var_index_insert(c, i);
}

int find_eval_var(const sv_compiler_t *c, const char *name, uint32_t len) {
  if (!c->eval_var_index_cap) {
    for (uint32_t i = 0; i < c->eval_var_count; i++)
      if (c->eval_vars[i].len == len && !memcmp(c->eval_vars[i].str, name, len)) return (int)i;
    return -1;
  }

  uint32_t mask = c->eval_var_index_cap - 1;
  for (uint32_t slot = sv_compile_ctx_hash_local_name(name, len) & mask;; slot = (slot + 1) & mask) {
    uint32_t entry = c->eval_var_index[slot];
    if (entry == UINT32_MAX) return -1;
    if (c->eval_vars[entry].len == len && !memcmp(c->eval_vars[entry].str, name, len)) return (int)entry;
  }
}

void add_eval_var(sv_compiler_t *c, const char *name, uint32_t len, bool annex_b) {
  int local = resolve_local_at_depth(c, name, len, 0);
  if (annex_b && local >= 0 && (c->locals[local].eval_flags & SV_EVAL_BIND_LEXICAL)) return;
  int existing = find_eval_var(c, name, len);
  if (existing >= 0) {
    c->eval_vars[existing].annex_b &= annex_b;
    return;
  }
  if (c->eval_var_count >= c->eval_var_cap) {
    uint32_t cap = c->eval_var_cap ? c->eval_var_cap * 2 : 8;
    sv_eval_decl_t *vars = realloc(c->eval_vars, (size_t)cap * sizeof(*vars));
    if (!vars) { js_mkerr(c->js, "out of memory while declaring eval variables"); return; }
    c->eval_vars = vars;
    c->eval_var_cap = cap;
  }
  int atom = add_atom(c, name, len);
  c->eval_vars[c->eval_var_count++] = (sv_eval_decl_t){ .str = c->atoms[atom].str, .len = len, .annex_b = annex_b };
  if (c->eval_var_count < 16) return;
  if (c->eval_var_index_cap && c->eval_var_count * 2 < c->eval_var_index_cap) eval_var_index_insert(c, c->eval_var_count - 1);
  else eval_var_index_rebuild(c);
}

static bool is_inline_literal_eval_expr(sv_ast_t *node) {
  if (!node) return false;
  switch (node->type) {
    case N_NUMBER:
    case N_STRING:
    case N_BIGINT:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
    case N_THIS:
    case N_GLOBAL_THIS:
    case N_TEMPLATE:
    case N_REGEXP:
    case N_IDENT:
    case N_BINARY:
    case N_UNARY:
    case N_UPDATE:
    case N_ASSIGN:
    case N_TERNARY:
    case N_CALL:
    case N_NEW:
    case N_MEMBER:
    case N_OPTIONAL:
    case N_ARRAY:
    case N_OBJECT:
    case N_SPREAD:
    case N_SEQUENCE:
    case N_ARROW:
    case N_YIELD:
    case N_AWAIT:
    case N_TYPEOF:
    case N_DELETE:
    case N_VOID:
    case N_TAGGED_TEMPLATE:
    case N_FUNC:
    case N_CLASS:
    case N_NEW_TARGET:
      return true;
    default:
      return false;
  }
}

static bool inline_eval_can_compile_without_early_errors(sv_compiler_t *c, sv_ast_t *node, bool strict) {
  ant_t *js = c->js;
  if (!node) return true;
  if (node->type == N_CLASS) return false;
  if (is_private_name_node(node) && !private_name_resolvable(c, node)) return false;
  if (is_ident_name(node, "arguments") && arguments_forbidden(c)) return false;
  if (node->type == N_CALL && is_ident_name(node->left, "super") && !lexical_owner(c)->is_derived_ctor) return false;
  if (is_ident_name(node, "super") && !compiler_has_super_binding(c)) return false;
  if (node->type == N_OBJECT && (node->flags & FN_PATTERN_ONLY)) return false;
  if (node->type == N_IDENT && is_private_name_node(node)) return false;
  if ((node->type == N_MEMBER || node->type == N_OPTIONAL) && !(node->flags & 1) && node->right && is_private_name_node(node->right)) {
    if (is_ident_name(node->left, "super") || !private_name_resolvable(c, node->right)) return false;
    return inline_eval_can_compile_without_early_errors(c, node->left, strict);
  }
  if (node->type == N_BINARY && node->op == TOK_IN && node->left && node->left->type == N_IDENT && is_private_name_node(node->left)) {
    if (!private_name_resolvable(c, node->left)) return false;
    return inline_eval_can_compile_without_early_errors(c, node->right, strict);
  }
  if (node->type == N_DELETE && node->right && (node->right->type == N_MEMBER || node->right->type == N_OPTIONAL) &&
      node->right->right && !(node->right->flags & 1) && is_private_name_node(node->right->right)) return false;

  if (node->type == N_FUNC) {
    bool has_own_use_strict = false;
    if (!(node->flags & (FN_ARROW | FN_PAREN))) return false;
    if (!check_function_param_early_errors(js, node, strict, &has_own_use_strict, NULL, false)) return false;
    strict = strict || has_own_use_strict || (node->flags & FN_CLASS_BODY);
  }

  sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (!inline_eval_can_compile_without_early_errors(c, child, strict)) return false;

  return true;
}

static bool compile_inline_literal_eval(sv_compiler_t *c, sv_ast_t *node) {
  if (!node || node->args.count == 0) return false;

  sv_ast_t *arg = node->args.items[0];
  if (!arg || arg->type != N_STRING) return false;

  GC_ROOT_SAVE(exception_mark, c->js);
  ant_value_t saved_exception = Ant_Exception_Peek(c->js);
  GC_ROOT_PIN(c->js, saved_exception);

  code_arena_mark_t mark = parse_arena_mark(c->js);
  const char *source = copy_source_text(c->js, arg->str ? arg->str : "", arg->len);
  
  if (!source) {
    parse_arena_rewind(c->js, mark);
    GC_ROOT_RESTORE(c->js, exception_mark);
    return false;
  }
  
  sv_ast_t *program = sv_parse(c->js, source, arg->len, SV_PARSE_SCRIPT, c->is_strict);

  if (!program) {
    parse_arena_rewind(c->js, mark);
    Ant_Exception_Set(c->js, saved_exception);
    GC_ROOT_RESTORE(c->js, exception_mark);
    return false;
  }

  sv_ast_t *expr = NULL;
  if (program->args.count == 0) {
    expr = NULL;
  } else if (
    program->args.count == 1 &&
    is_inline_literal_eval_expr(program->args.items[0]) &&
    (c->allows_new_target || !ast_contains_lexical_new_target(program)) &&
    inline_eval_can_compile_without_early_errors(c, program->args.items[0], c->is_strict) &&
    !ast_contains_direct_suspend(program->args.items[0], NULL)
  ) expr = program->args.items[0]; else {
    parse_arena_rewind(c->js, mark);
    Ant_Exception_Set(c->js, saved_exception);
    GC_ROOT_RESTORE(c->js, exception_mark);
    return false;
  }

  for (int i = 1; i < node->args.count; i++) {
    compile_expr(c, node->args.items[i]);
    emit_op(c, OP_POP);
  }

  if (expr) compile_expr(c, expr);
  else emit_op(c, OP_UNDEF);

  parse_arena_rewind(c->js, mark);
  GC_ROOT_RESTORE(c->js, exception_mark);

  return true;
}

static bool eval_arg_is_definitely_non_string(const sv_ast_t *node) {
  if (!node) return false;
  switch (node->type) {
    case N_NUMBER:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
    case N_OBJECT:
    case N_ARRAY:
    case N_FUNC:
    case N_CLASS:
    case N_REGEXP:
      return true;
    default:
      return false;
  }
}

static void emit_eval_of_first_arg(sv_compiler_t *c, uint32_t eval_scope) {
  emit_number(c, 0);
  emit_op(c, OP_GET_ELEM);
  emit_lexical_new_target(c);
  note_eval_reads_arguments(c);
  emit_op(c, OP_EVAL);
  emit_u32(c, eval_scope);
}

static void compile_inherited_direct_eval_spread(sv_compiler_t *c, sv_ast_t *node) {
  uint32_t eval_scope;
  if (!capture_dynamic_eval_scope(c, &eval_scope)) return;

  emit_atom_op(c, OP_GET_EVAL_GLOBAL, "eval", 4);
  emit_op(c, OP_DUP);
  emit_constant(c, js_builtin_eval(c->js));
  
  emit_op(c, OP_SEQ);
  emit_op(c, OP_UNDEF);
  emit_op(c, OP_SWAP);
  compile_call_args_array(c, node);
  emit_op(c, OP_SWAP);
  
  int direct_jump = emit_jump(c, OP_JMP_TRUE);
  emit_srcpos(c, node);
  emit_op(c, OP_APPLY);
  emit_u16(c, 1);
  
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, direct_jump);
  emit_eval_of_first_arg(c, eval_scope);
  
  emit_op(c, OP_NIP);
  emit_op(c, OP_NIP);
  patch_jump(c, end_jump);
}

static int emit_replaced_eval_call(sv_compiler_t *c, sv_ast_t *node, bool has_spread) {
  emit_op(c, OP_EVAL_INTACT);
  int intact_jump = emit_jump(c, OP_JMP_TRUE);
  
  compile_expr(c, node->left);
  emit_op(c, OP_DUP);
  emit_constant(c, js_builtin_eval(c->js));
  emit_op(c, OP_SEQ);
  int builtin_jump = emit_jump(c, OP_JMP_TRUE);
  
  if (has_spread) {
    emit_op(c, OP_UNDEF);
    compile_call_args_array(c, node);
    emit_srcpos(c, node);
    emit_op(c, OP_APPLY);
    emit_u16(c, 1);
  } else {
    for (int i = 0; i < node->args.count; i++) compile_expr(c, node->args.items[i]);
    emit_srcpos(c, node);
    emit_op(c, OP_CALL);
    emit_argc(c, node->args.count);
  }
  
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, builtin_jump);
  emit_op(c, OP_POP);
  patch_jump(c, intact_jump);
  
  return end_jump;
}

static void compile_own_direct_eval(sv_compiler_t *c, sv_ast_t *node, bool has_spread) {
  uint32_t eval_scope;
  
  if (has_spread) {
    if (!capture_dynamic_eval_scope(c, &eval_scope)) return;
    compile_call_args_array(c, node);
    emit_eval_of_first_arg(c, eval_scope);
    return;
  }
  
  if (node->args.count == 0) {
    emit_op(c, OP_UNDEF);
    return;
  }

  bool may_inline = !(c->js->codegen_blocked & ANT_CODEGEN_NO_STRINGS);
  if (may_inline && compile_inline_literal_eval(c, node)) return;

  if (eval_arg_is_definitely_non_string(node->args.items[0])) {
    compile_expr(c, node->args.items[0]);
    for (int i = 1; i < node->args.count; i++) {
      compile_expr(c, node->args.items[i]);
      emit_op(c, OP_POP);
    }
    return;
  }

  if (!capture_dynamic_eval_scope(c, &eval_scope)) return;

  compile_expr(c, node->args.items[0]);
  for (int i = 1; i < node->args.count; i++) {
    compile_expr(c, node->args.items[i]);
    emit_op(c, OP_POP);
  }

  emit_lexical_new_target(c);
  note_eval_reads_arguments(c);
  emit_op(c, OP_EVAL);
  emit_u32(c, eval_scope);
}

static void compile_inherited_direct_eval(sv_compiler_t *c, sv_ast_t *node) {
  uint32_t eval_scope;
  if (!capture_dynamic_eval_scope(c, &eval_scope)) return;
  
  emit_atom_op(c, OP_GET_EVAL_GLOBAL, "eval", 4);
  emit_op(c, OP_DUP);
  emit_constant(c, js_builtin_eval(c->js));
  emit_op(c, OP_SEQ);
  
  for (int i = 0; i < node->args.count; i++) {
    compile_expr(c, node->args.items[i]);
    emit_op(c, OP_SWAP);
  }
  
  int direct_jump = emit_jump(c, OP_JMP_TRUE);
  emit_op(c, OP_CALL);
  emit_argc(c, node->args.count);
  
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, direct_jump);
  
  if (node->args.count == 0) {
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
  } else {
    for (int i = 1; i < node->args.count; i++) emit_op(c, OP_POP);
    emit_op(c, OP_NIP);
    emit_lexical_new_target(c);
    note_eval_reads_arguments(c);
    emit_op(c, OP_EVAL);
    emit_u32(c, eval_scope);
  }
  
  patch_jump(c, end_jump);
}

bool compile_direct_eval_call(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  sv_ast_t *callee = node->left;
  if (
    !is_ident_name(callee, "eval") ||
    resolve_local(c, "eval", 4) != -1 ||
    resolve_upvalue_raw(c, "eval", 4) != -1
  ) return false;

  if (c->inherits_eval_env) {
    if (has_spread) compile_inherited_direct_eval_spread(c, node);
    else compile_inherited_direct_eval(c, node);
    return true;
  }

  int end_jump = emit_replaced_eval_call(c, node, has_spread);
  compile_own_direct_eval(c, node, has_spread);
  patch_jump(c, end_jump);
  
  return true;
}

static bool ast_decl_binds_eval(const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_EXPORT) return ast_decl_binds_eval(node->left);
  if (node->type == N_VAR) {
    for (int i = 0; i < node->args.count; i++)
      if (ast_pattern_binds(node->args.items[i]->left, "eval")) return true;
  }
  if ((node->type == N_FUNC && !(node->flags & (FN_ARROW | FN_PAREN))) ||
      (node->type == N_CLASS && (node->flags & FN_CLASS_DECL)))
    return is_ident_str(node->str, node->len, "eval", 4);
  return false;
}

bool ast_has_eval_var(const sv_ast_t *node) {
  if (!node || node->type == N_FUNC || node->type == N_CLASS) return false;
  if (node->type == N_VAR && node->var_kind == SV_VAR_VAR) return ast_decl_binds_eval(node);

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_has_eval_var(child)) return true;
  
  return false;
}

static bool stmts_bind_eval(const sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++)
    if (ast_decl_binds_eval(stmts->items[i])) return true;
  return false;
}

static bool scope_binds_eval(const sv_ast_t *node) {
  switch (node->type) {
    case N_BLOCK: return stmts_bind_eval(&node->args);
    case N_SWITCH:
      for (int i = 0; i < node->args.count; i++)
        if (node->args.items[i] && stmts_bind_eval(&node->args.items[i]->args)) return true;
      return false;
    case N_FOR: return ast_decl_binds_eval(node->init);
    case N_FOR_IN: case N_FOR_OF: case N_FOR_AWAIT_OF: return ast_decl_binds_eval(node->left);
    default: return false;
  }
}

static bool eval_program_needs_env(sv_compiler_t *c, const sv_ast_t *program) {
  if (!program) return true;
  if (program->args.count == 0) return false;
  if (program->args.count != 1) return true;
  sv_ast_t *expr = program->args.items[0];
  return
    !is_inline_literal_eval_expr(expr) ||
    !inline_eval_can_compile_without_early_errors(c, expr, c->is_strict) ||
    ast_contains_direct_suspend(expr, NULL) ||
    ast_has_own_eval(c, expr);
}

static bool eval_program_mentions_arguments(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_IDENT) return is_ident_str(node->str, node->len, "arguments", 9);
  for (int i = 0; node->type == N_PROGRAM && i < node->args.count; i++)
    if (eval_program_mentions_arguments(c, node->args.items[i])) return true;

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (eval_program_mentions_arguments(c, child)) return true;

  return false;
}

static bool eval_source_test(
  sv_compiler_t *c, const sv_ast_t *source,
  bool (*test)(sv_compiler_t *c, const sv_ast_t *program)
) {
  GC_ROOT_SAVE(exception_mark, c->js);
  ant_value_t saved_exception = Ant_Exception_Peek(c->js);
  GC_ROOT_PIN(c->js, saved_exception);
  code_arena_mark_t mark = parse_arena_mark(c->js);
  
  sv_ast_t *program = sv_parse(c->js, source->str ? source->str : "", source->len, SV_PARSE_SCRIPT, c->is_strict);
  bool result = test(c, program);
  
  parse_arena_rewind(c->js, mark);
  Ant_Exception_Set(c->js, saved_exception);
  GC_ROOT_RESTORE(c->js, exception_mark);
  
  return result;
}

bool ast_literal_eval_mentions_arguments(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || (node->type == N_FUNC && !(node->flags & FN_ARROW)) || node->type == N_CLASS) return false;

  const sv_ast_t *source = node->type == N_CALL && is_ident_name(node->left, "eval") && node->args.count > 0
    ? node->args.items[0] : NULL;
  if (
    source && source->type == N_STRING && source->str &&
    memmem(source->str, source->len, "arguments", 9) &&
    eval_source_test(c, source, eval_program_mentions_arguments)
  ) return true;

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_literal_eval_mentions_arguments(c, child)) return true;

  return false;
}

static bool eval_call_needs_env(sv_compiler_t *c, const sv_ast_t *node) {
  if (node->type != N_CALL || !is_ident_name(node->left, "eval") || node->args.count == 0) return false;
  if (resolve_local(c, "eval", 4) != -1 || resolve_upvalue_raw(c, "eval", 4) != -1) return false;
  if (call_has_spread_arg(node)) return true;
  const sv_ast_t *arg = node->args.items[0];
  if (eval_arg_is_definitely_non_string(arg)) return false;
  
  return !arg || arg->type != N_STRING || eval_source_test(c, arg, eval_program_needs_env);
}

bool ast_has_own_eval(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || node->type == N_FUNC || node->type == N_CLASS) return false;
  if (node->type == N_SWITCH && ast_has_own_eval(c, node->cond)) return true;
  if (scope_binds_eval(node)) return false;

  if (node->type == N_TRY) return
    ast_has_own_eval(c, node->body) ||
    (!ast_pattern_binds(node->catch_param, "eval") && ast_has_own_eval(c, node->catch_body)) ||
    ast_has_own_eval(c, node->finally_body);
  
  if (eval_call_needs_env(c, node)) return true;
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_has_own_eval(c, child)) return true;
    
  return false;
}
