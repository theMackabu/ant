#include "compiler_internal.h"

static void reject_super(sv_compiler_t *c) {
  js_mkerr_typed(c->js, JS_ERR_SYNTAX, "'super' keyword unexpected here");
  emit_op(c, OP_UNDEF);
}

void emit_super(sv_compiler_t *c) {
  if (c->super_local >= 0) {
    c->super_referenced = true;
    emit_get_local(c, c->super_local);
    return;
  }

  int upval = c->is_arrow ? resolve_super_upvalue(c) : -1;
  if (upval >= 0) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    return;
  }

  if (!lexical_owner(c)->eval_super_binding) {
    reject_super(c);
    return;
  }

  emit_atom_op(c, OP_GET_EVAL_GLOBAL, "super", 5);
}

void emit_new_target(sv_compiler_t *c) {
  if (c->is_arrow) {
    emit_lexical_new_target(c);
    return;
  }
  
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_NEW_TARGET);
}

void emit_lexical_new_target(sv_compiler_t *c) {
  if (c->new_target_local >= 0) {
    emit_get_local(c, c->new_target_local);
    return;
  }

  int upval = resolve_new_target_upvalue(c);
  if (upval >= 0) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    return;
  }

  emit_op(c, OP_UNDEF);
}

typedef enum {
  THIS_BINDING_NONE,
  THIS_BINDING_LOCAL,
  THIS_BINDING_UPVALUE,
  THIS_BINDING_NAMED,
} this_binding_t;

static this_binding_t this_binding(sv_compiler_t *c, bool reading, int *slot) {
  sv_compiler_t *owner = lexical_owner(c);
  
  if (owner->this_local >= 0) {
    if (reading && owner->this_bound) return THIS_BINDING_NONE;
    if (owner == c) {
      *slot = c->this_local;
      return THIS_BINDING_LOCAL;
    }
    *slot = resolve_this_upvalue(c);
    return *slot >= 0 ? THIS_BINDING_UPVALUE : THIS_BINDING_NONE;
  }
  
  return owner->eval_this_binding ? THIS_BINDING_NAMED : THIS_BINDING_NONE;
}

static void emit_this_binding_get(sv_compiler_t *c, this_binding_t kind, int slot) {
  if (kind == THIS_BINDING_LOCAL) emit_get_local(c, slot);
  else if (kind == THIS_BINDING_NAMED) emit_get_var(c, "this", 4);
  else {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)slot);
  }
}

static void emit_this_binding_put(sv_compiler_t *c, this_binding_t kind, int slot) {
  if (kind == THIS_BINDING_LOCAL) emit_put_local(c, slot);
  else if (kind == THIS_BINDING_NAMED) emit_set_var(c, "this", 4, false);
  else {
    emit_op(c, OP_PUT_UPVAL);
    emit_u16(c, (uint16_t)slot);
  }
}

static void emit_this_check(sv_compiler_t *c) {
  static const char message[] = "Must call super constructor in derived class before accessing 'this' or returning from derived constructor";
  if (c->this_check_atom < 0) c->this_check_atom = add_atom(c, message, sizeof(message) - 1);
  
  int ok = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_THROW_ERROR);
  emit_u32(c, (uint32_t)c->this_check_atom);
  emit(c, (uint8_t)JS_ERR_REFERENCE);
  patch_jump(c, ok);
}

void emit_this(sv_compiler_t *c) {
  int slot = -1;
  this_binding_t kind = this_binding(c, true, &slot);
  if (kind == THIS_BINDING_NONE) {
    emit_op(c, OP_THIS);
    return;
  }
  
  emit_this_binding_get(c, kind, slot);
  if (c->this_checked) return;
  emit_this_check(c);
  c->this_checked = true;
}

void emit_this_store(sv_compiler_t *c) {
  static const char message[] = "Super constructor may only be called once";
  int slot = -1;
  this_binding_t kind = this_binding(c, false, &slot);
  if (kind == THIS_BINDING_NONE) return;
  
  if (!c->super_runs_once) {
    emit_this_binding_get(c, kind, slot);
    emit_op(c, OP_IS_UNDEF);
    int first = emit_jump(c, OP_JMP_TRUE);
    emit_throw_error(c, JS_ERR_REFERENCE, message, sizeof(message) - 1);
    patch_jump(c, first);
  }
  
  emit_op(c, OP_DUP);
  emit_this_binding_put(c, kind, slot);
  c->this_checked = true;
}

bool reject_forbidden_arguments(sv_compiler_t *c) {
  const sv_compiler_t *owner = c;
  while (owner->is_arrow && owner->enclosing) owner = owner->enclosing;
  const sv_compiler_t *root = owner->enclosing ? owner->enclosing : owner;
  if (root->enclosing || !root->eval_no_arguments) return false;
  js_mkerr_typed(c->js, JS_ERR_SYNTAX, "'arguments' is not allowed in class field initializer or static initialization block");
  emit_op(c, OP_UNDEF);
  return true;
}

static bool in_field_initializer(const sv_compiler_t *c) {
  for (; c; c = c->is_arrow ? c->enclosing : NULL)
    if (c->in_field_init) return true;
  return false;
}

bool reject_super_identifier(sv_compiler_t *c, const sv_ast_t *node) {
  if (!is_ident_name(node, "super")) return false;
  reject_super(c);
  return true;
}

bool reject_misplaced_super_call(sv_compiler_t *c) {
  sv_compiler_t *owner = lexical_owner(c);
  bool allowed = (owner->is_derived_ctor || owner->eval_this_binding) && !in_field_initializer(c);
  if (allowed) return false;
  
  reject_super(c);
  return true;
}

void emit_super_receiver(sv_compiler_t *c) {
  int slot = -1;
  this_binding_t kind = this_binding(c, false, &slot);
  
  if (kind == THIS_BINDING_NONE || c->super_runs_once) {
    emit_op(c, OP_THIS);
    return;
  }
  
  emit_this_binding_get(c, kind, slot);
  emit_op(c, OP_IS_UNDEF);
  int first = emit_jump(c, OP_JMP_TRUE);
  
  emit_op(c, OP_OBJECT);
  emit_new_target(c);
  emit_atom_op(c, OP_GET_FIELD, "prototype", 9);
  emit_op(c, OP_SET_PROTO);
  int done = emit_jump(c, OP_JMP);
  
  patch_jump(c, first);
  emit_op(c, OP_THIS);
  patch_jump(c, done);
}

bool derived_return_needs_check(const sv_compiler_t *c) {
  return c->this_local >= 0 && !c->this_bound;
}

void emit_derived_return_check(sv_compiler_t *c) {
  int slot = -1;
  this_binding_t kind = this_binding(c, false, &slot);
  emit_op(c, OP_DUP);
  emit_op(c, OP_IS_UNDEF);
  int defined = emit_jump(c, OP_JMP_FALSE);
  emit_op(c, OP_POP);
  emit_this_binding_get(c, kind, slot);
  emit_this_check(c);
  patch_jump(c, defined);
}

bool compiler_has_this_binding(const sv_compiler_t *c) {
  const sv_compiler_t *owner = lexical_owner((sv_compiler_t *)c);
  return owner->this_local >= 0 || owner->eval_this_binding;
}


typedef struct {
  int super_calls;
  bool reads_this;
  bool returns;
  bool has_eval;
  bool has_finally;
} ctor_scan_t;

static void ctor_scan(const sv_ast_t *node, ctor_scan_t *scan, bool in_arrow) {
  if (!node) return;
  
  switch (node->type) {
    case N_IDENT: case N_NUMBER: case N_STRING: case N_BOOL: case N_NULL: case N_UNDEF: return;
    case N_THIS: scan->reads_this = true; return;
    case N_CLASS: scan->reads_this = true; break;
    case N_FUNC:
      if (!(node->flags & FN_ARROW)) return;
      in_arrow = true;
      break;
    case N_RETURN: scan->returns |= !in_arrow; break;
    case N_TRY: scan->has_finally |= node->finally_body && !in_arrow; break;
    case N_MEMBER: scan->reads_this |= ast_is_ident(node->left, "super", 5); break;
    case N_CALL:
      scan->super_calls += ast_is_ident(node->left, "super", 5);
      scan->has_eval |= ast_is_ident(node->left, "eval", 4);
      break;
    default: break;
  }
  
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child) ctor_scan(child, scan, in_arrow);
}

static bool is_super_call_stmt(const sv_ast_t *stmt) {
  return stmt && stmt->type == N_CALL && is_ident_name(stmt->left, "super");
}

typedef enum {
  THIS_FLOW_UNBOUND,
  THIS_FLOW_BOUND,
  THIS_FLOW_MAYBE,
  THIS_FLOW_DEAD,
} this_flow_t;

static this_flow_t this_flow_join(this_flow_t a, this_flow_t b) {
  if (a == THIS_FLOW_DEAD) return b;
  if (b == THIS_FLOW_DEAD) return a;
  return a == b ? a : THIS_FLOW_MAYBE;
}

static bool this_flow_opaque(const sv_ast_t *node, this_flow_t state) {
  ctor_scan_t scan = {0};
  ctor_scan(node, &scan, false);
  if (scan.super_calls || scan.has_eval) return false;
  return !(scan.reads_this || scan.returns) || state == THIS_FLOW_BOUND;
}

static bool this_flow_args(const sv_ast_t *call, this_flow_t state) {
  for (int i = 0; i < call->args.count; i++)
    if (!this_flow_opaque(call->args.items[i], state)) return false;
  return true;
}

static bool this_flow_expr(const sv_ast_t *node, this_flow_t *state) {
  if (!node) return true;
  
  if (is_super_call_stmt(node)) {
    if (*state != THIS_FLOW_UNBOUND || !this_flow_args(node, *state)) return false;
    *state = THIS_FLOW_BOUND;
    return true;
  }
  
  if (node->type == N_SEQUENCE)
    return this_flow_expr(node->left, state) && this_flow_expr(node->right, state);
  
  if (node->type == N_TERNARY) {
    if (!this_flow_opaque(node->cond, *state)) return false;
    this_flow_t taken = *state, skipped = *state;
    if (!this_flow_expr(node->left, &taken) || !this_flow_expr(node->right, &skipped)) return false;
    *state = this_flow_join(taken, skipped);
    return true;
  }
  
  return this_flow_opaque(node, *state);
}

static bool this_flow_stmt(const sv_ast_t *node, this_flow_t *state) {
  if (!node || *state == THIS_FLOW_DEAD) return true;
  
  if (node->type == N_BLOCK) {
    for (int i = 0; i < node->args.count; i++)
      if (!this_flow_stmt(node->args.items[i], state)) return false;
    return true;
  }
  
  if (node->type == N_IF) {
    if (!this_flow_opaque(node->cond, *state)) return false;
    this_flow_t taken = *state, skipped = *state;
    if (!this_flow_stmt(node->left, &taken) || !this_flow_stmt(node->right, &skipped)) return false;
    *state = this_flow_join(taken, skipped);
    return true;
  }
  
  if (node->type == N_VAR) {
    for (int i = 0; i < node->args.count; i++) {
      const sv_ast_t *decl = node->args.items[i];
      if (!decl || decl->type != N_VARDECL) return this_flow_opaque(node, *state);
      if (!this_flow_opaque(decl->left, *state) || !this_flow_expr(decl->right, state)) return false;
    }
    return true;
  }
  
  if (node->type == N_RETURN || node->type == N_THROW) {
    if (!this_flow_opaque(node, *state)) return false;
    *state = THIS_FLOW_DEAD;
    return true;
  }
  
  return this_flow_expr(node, state);
}

static bool this_flow_proves_body(const sv_ast_t *body) {
  this_flow_t state = THIS_FLOW_UNBOUND;
  return this_flow_stmt(body, &state) && (state == THIS_FLOW_BOUND || state == THIS_FLOW_DEAD);
}

static bool ast_returns_under_finally(const sv_ast_t *node, bool under) {
  if (!node || node->type == N_FUNC) return false;
  if (node->type == N_RETURN) return under;
  if (node->type == N_TRY) return
    ast_returns_under_finally(node->body, under || node->finally_body) ||
    ast_returns_under_finally(node->catch_body, under || node->finally_body) ||
    ast_returns_under_finally(node->finally_body, under);
  
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_returns_under_finally(child, under)) return true;
  
  return false;
}

static bool super_call_ignores_this(const sv_ast_t *call) {
  ctor_scan_t args = {0};
  for (int i = 0; i < call->args.count; i++) ctor_scan(call->args.items[i], &args, false);
  return !args.reads_this;
}

static bool sole_super_opens_plain_ctor(const sv_ast_t *ctor) {
  const sv_ast_t *body = ctor->body;
  if (!(ctor->flags & FN_SOLE_SUPER_CALL) || (ctor->flags & FN_DIRECT_EVAL)) return false;
  if (!body || body->type != N_BLOCK) return false;
  
  int at = 0;
  for (; at < body->args.count && !is_super_call_stmt(body->args.items[at]); at++) {
    ctor_scan_t before = {0};
    ctor_scan(body->args.items[at], &before, false);
    if (before.reads_this || before.returns || before.super_calls || before.has_eval) return false;
  }
  if (at == body->args.count) return false;
  
  for (int i = 0; i < ctor->args.count; i++)
    if (ctor->args.items[i]->type != N_IDENT) return false;
  
  return super_call_ignores_this(body->args.items[at]);
}

derived_ctor_plan_t derived_ctor_plan(const sv_ast_t *ctor) {
  if (sole_super_opens_plain_ctor(ctor)) return (derived_ctor_plan_t){0};
  
  const sv_ast_t *body = ctor->body;
  bool block_body = body && body->type == N_BLOCK;
  
  ctor_scan_t all = {0};
  for (int i = 0; i < ctor->args.count; i++) ctor_scan(ctor->args.items[i], &all, false);
  bool params_are_inert = !all.reads_this && !all.super_calls && !all.has_eval;
  if (params_are_inert && block_body && this_flow_proves_body(body)) return (derived_ctor_plan_t){0};
  
  ctor_scan(body, &all, false);
  derived_ctor_plan_t plan = {
    .needs_this_binding = true,
    .has_eval = all.has_eval,
    .returns_through_finally = all.returns && all.has_finally && ast_returns_under_finally(body, false),
  };
  
  bool sole_super = params_are_inert && block_body && !all.has_eval && all.super_calls == 1;
  for (int i = 0; sole_super && i < body->args.count && !plan.sole_super_stmt; i++)
    if (is_super_call_stmt(body->args.items[i])) plan.sole_super_stmt = body->args.items[i];
  
  return plan;
}

static void emit_slot_atom_op(sv_compiler_t *c, sv_op_t op, int local, const char *name, uint32_t len) {
  int ai = add_atom(c, name, len);
  emit_op(c, op);
  emit_u16(c, (uint16_t)(local - c->param_locals));
  emit_u32(c, (uint32_t)ai);
}

static void emit_get_var_with(
  sv_compiler_t *c, const char *name, uint32_t len,
  sv_op_t global_op, sv_op_t eval_op
) {
  if (is_ident_str(name, len, "super", 5)) {
    reject_super(c);
    return;
  }

  int local = resolve_local(c, name, len);
  if (local != -1) {
    if (c->locals[local].depth == -1) {
      emit_op(c, OP_GET_ARG);
      emit_u16(c, (uint16_t)local);
    } else {
      int slot = local - c->param_locals;
      if (c->locals[local].is_tdz) {
        emit_slot_atom_op(c, OP_GET_LOCAL_CHK, local, name, len);
      } else if (slot <= 255) {
        emit_op(c, OP_GET_LOCAL8);
        emit(c, (uint8_t)slot);
      } else {
        emit_op(c, OP_GET_LOCAL);
        emit_u16(c, (uint16_t)slot);
      }
    }
    emit_import_binding_resolve(
      c,
      c->locals[local].binding.import_kind,
      c->locals[local].binding.import_name,
      c->locals[local].binding.import_name_len
    );
    return;
  }
  
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    emit_op(c, c->upval_descs[upval].by_value ? OP_GET_UPVAL_VAL : OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    sv_binding_meta_t *meta = &c->upval_bindings[upval];
    emit_import_binding_resolve(
      c,
      meta->import_kind,
      meta->import_name,
      meta->import_name_len
    );
    return;
  }
  
  if (is_ident_str(name, len, "arguments", 9)) {
    if (reject_forbidden_arguments(c)) return;
    if (c->owns_eval_env || sv_compile_mode_is_eval(c->mode)) {
      emit_arguments_eval_env(c, name, len);
      emit_atom_op(c, eval_op, name, len);
      return;
    }
    if (has_implicit_arguments_obj(c)) {
      emit_op(c, OP_SPECIAL_OBJ);
      emit(c, SV_SPECIAL_ARGUMENTS);
      return;
    }
  }
  
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_get_module_import_binding(c);
    return;
  }
  
  emit_atom_op(c, env_resolution_op(c, global_op, eval_op), name, len);
}

void emit_get_var_plain(sv_compiler_t *c, const char *name, uint32_t len) {
  emit_get_var_with(c, name, len, OP_GET_GLOBAL, OP_GET_EVAL_GLOBAL);
}

void emit_get_var_typeof(sv_compiler_t *c, const char *name, uint32_t len) {
  emit_get_var_with(c, name, len, OP_GET_GLOBAL_UNDEF, OP_GET_EVAL_GLOBAL_UNDEF);
}

void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len) {
  sv_with_refs_t shadows = {0};
  sv_patch_list_t found = {0};
  
  with_shadows(c, name, len, &shadows);
  emit_with_tries(c, &shadows, OP_WITH_TRY_GET, name, len, 1, &found);
  emit_get_var_plain(c, name, len);
  patch_list_resolve(c, &found);
}

static void emit_export_dups(sv_compiler_t *c, const sv_export_name_t *exports) {
  for (const sv_export_name_t *e = exports; e; e = e->next)
    emit_op(c, OP_DUP);
}

static void emit_export_writes(sv_compiler_t *c, const sv_export_name_t *exports) {
  for (const sv_export_name_t *e = exports; e; e = e->next)
    emit_atom_op(c, OP_EXPORT, e->name, e->len);
}

void emit_export_writes_from_stack(sv_compiler_t *c, const sv_export_name_t *exports) {
  if (!exports) return;
  for (const sv_export_name_t *e = exports->next; e; e = e->next)
    emit_op(c, OP_DUP);
  emit_export_writes(c, exports);
}

void emit_local_export_writes(sv_compiler_t *c, int local) {
  if (local < 0) return;
  const sv_export_name_t *exports = c->locals[local].binding.exports;
  if (!exports) return;
  emit_get_local(c, local);
  emit_export_writes_from_stack(c, exports);
}

static void emit_set_var_plain(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  int local = resolve_local(c, name, len);
  if (local != -1) {
    bool tdz = c->locals[local].is_tdz && c->locals[local].depth != -1;
    if (c->locals[local].is_const) {
      if (tdz) {
        emit_slot_atom_op(c, OP_GET_LOCAL_CHK, local, name, len);
        emit_op(c, OP_POP);
      }
      emit_const_assign_error(c, name, len);
      return;
    }
    uint8_t old_type = c->locals[local].inferred_type;
    if (!sv_type_is_known(old_type)) set_local_stored_type(c, local, SV_TI_UNKNOWN, c->types->dep_count);
    else {
      int dep_base = c->store_type ? c->store_dep_base : c->types->dep_count;
      uint8_t stored = c->store_type;
      if (c->store_node) stored = infer_expr_type(c, (sv_ast_t *)c->store_node);
      set_local_stored_type(c, local, stored == old_type ? stored : SV_TI_UNKNOWN, dep_base);
      if (!c->store_type) c->types->dep_count = dep_base;
    }
    const sv_export_name_t *exports = c->locals[local].binding.exports;
    emit_export_dups(c, exports);

    if (c->locals[local].depth == -1) {
      emit_op(c, keep ? OP_SET_ARG : OP_PUT_ARG);
      emit_u16(c, (uint16_t)local);
    } else if (tdz) {
      if (keep) emit_op(c, OP_DUP);
      emit_slot_atom_op(c, OP_PUT_LOCAL_CHK, local, name, len);
    } else {
      int slot = local - c->param_locals;
      sv_op_t op = keep
        ? (slot <= 255 ? OP_SET_LOCAL8 : OP_SET_LOCAL)
        : (slot <= 255 ? OP_PUT_LOCAL8 : OP_PUT_LOCAL);
      emit_op(c, op);
      if (slot <= 255) emit(c, (uint8_t)slot);
      else emit_u16(c, (uint16_t)slot);
    }
    emit_export_writes(c, exports);
    return;
  }
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    Ant_Assert(!c->upval_descs[upval].by_value, "assignment to a parameter captured by value");
    if (c->upval_descs[upval].is_const) {
      if (c->upval_descs[upval].may_tdz) {
        emit_op(c, OP_GET_UPVAL);
        emit_u16(c, (uint16_t)upval);
        emit_op(c, OP_POP);
      }
      emit_const_assign_error(c, name, len);
      return;
    }
    const sv_export_name_t *exports = c->upval_bindings[upval].exports;
    emit_export_dups(c, exports);
    emit_op(c, keep ? OP_SET_UPVAL : OP_PUT_UPVAL);
    emit_u16(c, (uint16_t)upval);
    emit_export_writes(c, exports);
    return;
  }
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_const_assign_error(c, name, len);
    return;
  }
  if (keep) emit_op(c, OP_DUP);
  emit_arguments_eval_env(c, name, len);
  emit_atom_op(
    c, env_resolution_op(c, OP_PUT_GLOBAL, OP_PUT_EVAL_GLOBAL),
    name, len);
}

void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  if (is_ident_str(name, len, "super", 5)) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "'super' keyword unexpected here");
    return;
  }
  if (spilled_const_assignment(c, name, len)) {
    emit_const_assign_error(c, name, len);
    return;
  }
  sv_with_refs_t shadows = {0};
  with_shadows(c, name, len, &shadows);
  if (shadows.count == 0) {
    emit_set_var_plain(c, name, len, keep);
    return;
  }

  if (keep) emit_op(c, OP_DUP);
  sv_patch_list_t stored = {0};
  emit_with_tries(c, &shadows, OP_WITH_TRY_PUT, name, len, 0, &stored);
  emit_set_var_plain(c, name, len, false);
  
  int done = emit_jump(c, OP_JMP);
  patch_list_resolve(c, &stored);
  emit_op(c, OP_POP);
  patch_jump(c, done);
}

void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_PUT_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_PUT_LOCAL); emit_u16(c, (uint16_t)slot); }
  set_local_declared_type(c, local_idx, type, dep_base);
}

void emit_put_local(sv_compiler_t *c, int local_idx) {
  emit_put_local_typed(c, local_idx, SV_TI_UNKNOWN, c->types->dep_count);
}

void emit_set_completion_from_stack(sv_compiler_t *c) {
  if (has_completion_accumulator(c)) emit_put_local(c, c->completion_local);
  else emit_op(c, OP_POP);
}

void emit_set_completion_undefined(sv_compiler_t *c) {
  if (!has_completion_accumulator(c)) return;
  emit_op(c, OP_UNDEF);
  emit_put_local(c, c->completion_local);
}

void emit_get_local(sv_compiler_t *c, int local_idx) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_GET_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_GET_LOCAL); emit_u16(c, (uint16_t)slot); }
}
