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
  return true;
}

static bool eval_scope_builder_capture_upvalue(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_local_t *local
) {
  if (!eval_local_is_visible(local) || eval_scope_builder_has_binding(
        set, scope, local->name, local->name_len)) return true;

  int upvalue = resolve_upvalue_raw(c, local->name, local->name_len);
  if (upvalue < 0) return true;
  return eval_scope_builder_add_binding(
    c, scope, set, local->name, local->name_len,
    SV_EVAL_BIND_UPVALUE, (uint16_t)upvalue,
    c->upval_descs[upvalue].is_const, &c->upval_bindings[upvalue]);
}

static bool eval_scope_builder_capture_locals(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set
) {
  for (int i = c->local_count - 1; i >= 0; i--)
    if (!eval_scope_builder_capture_local(
          c, scope, set, &c->locals[i], i)) return false;
  return true;
}

static bool eval_scope_builder_capture_upvalues(
  sv_compiler_t *c, sv_eval_scope_builder_t *scope,
  sv_eval_binding_set_t *set, sv_compiler_t *enclosing
) {
  for (int i = enclosing->local_count - 1; i >= 0; i--)
    if (!eval_scope_builder_capture_upvalue(
          c, scope, set, &enclosing->locals[i])) return false;
  return true;
}

bool capture_dynamic_eval_scope(
  sv_compiler_t *c, uint32_t *scope_index_out
) {
  if (c->eval_scope_count >= c->eval_scope_cap) {
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
  }

  uint32_t scope_index = (uint32_t)c->eval_scope_count++;
  sv_eval_scope_builder_t *scope = &c->eval_scopes[scope_index];
  memset(scope, 0, sizeof(*scope));

  size_t candidate_count = (size_t)c->local_count;
  for (sv_compiler_t *enc = c->enclosing; enc; enc = enc->enclosing) {
    if ((size_t)enc->local_count > SIZE_MAX - candidate_count) {
      js_mkerr(c->js, "too many bindings while capturing direct eval scope");
      return false;
    }
    candidate_count += (size_t)enc->local_count;
  }

  sv_eval_binding_set_t binding_set;
  if (!eval_binding_set_init(c, &binding_set, candidate_count)) return false;

  if (!eval_scope_builder_capture_locals(
        c, scope, &binding_set)) goto capture_failed;

  for (sv_compiler_t *enc = c->inherits_eval_env ? NULL : c->enclosing; enc; enc = enc->enclosing)
    if (!eval_scope_builder_capture_upvalues(
          c, scope, &binding_set, enc)) goto capture_failed;
  free(binding_set.slots);
  *scope_index_out = scope_index;
  return true;

capture_failed:
  free(binding_set.slots);
  return false;
}

bool is_sloppy_eval(const sv_compiler_t *c) {
  return (sv_compile_mode_is_eval(c->mode) && !c->is_strict) || is_repl_root(c);
}

void add_eval_var(sv_compiler_t *c, const char *name, uint32_t len, bool annex_b) {
  int local = resolve_local_at_depth(c, name, len, 0);
  if (annex_b && local >= 0 && (c->locals[local].eval_flags & SV_EVAL_BIND_LEXICAL)) return;
  for (uint32_t i = 0; i < c->eval_var_count; i++)
    if (c->eval_vars[i].len == len && !memcmp(c->eval_vars[i].str, name, len)) {
      c->eval_vars[i].annex_b &= annex_b;
      return;
    }
  sv_eval_decl_t *vars = realloc(c->eval_vars, ((size_t)c->eval_var_count + 1) * sizeof(*vars));
  if (!vars) { js_mkerr(c->js, "out of memory while declaring eval variables"); return; }
  c->eval_vars = vars;
  int atom = add_atom(c, name, len);
  c->eval_vars[c->eval_var_count++] = (sv_eval_decl_t){ .str = c->atoms[atom].str, .len = len, .annex_b = annex_b };
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

static bool inline_eval_can_compile_without_early_errors(ant_t *js, sv_ast_t *node, bool strict) {
  if (!node) return true;
  if (node->type == N_CLASS) return false;
  
  if (node->type == N_FUNC) {
    bool has_own_use_strict = false;
    if (!(node->flags & (FN_ARROW | FN_PAREN))) return false;
    if (!check_function_param_early_errors(js, node, strict, &has_own_use_strict, NULL, false)) return false;
    strict = strict || has_own_use_strict || (node->flags & FN_CLASS_BODY);
  }

  sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (!inline_eval_can_compile_without_early_errors(js, child, strict)) return false;

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
    inline_eval_can_compile_without_early_errors(c->js, program->args.items[0], c->is_strict) &&
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
    emit_u16(c, (uint16_t)node->args.count);
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
  emit_u16(c, (uint16_t)node->args.count);
  
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, direct_jump);
  
  if (node->args.count == 0) {
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
  } else {
    for (int i = 1; i < node->args.count; i++) emit_op(c, OP_POP);
    emit_op(c, OP_NIP);
    emit_lexical_new_target(c);
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
    !inline_eval_can_compile_without_early_errors(c->js, expr, c->is_strict) ||
    ast_contains_direct_suspend(expr, NULL) ||
    ast_has_own_eval(c, expr);
}

static bool eval_source_needs_env(sv_compiler_t *c, const sv_ast_t *source) {
  GC_ROOT_SAVE(exception_mark, c->js);
  ant_value_t saved_exception = Ant_Exception_Peek(c->js);
  GC_ROOT_PIN(c->js, saved_exception);
  code_arena_mark_t mark = parse_arena_mark(c->js);
  
  sv_ast_t *program = sv_parse(c->js, source->str ? source->str : "", source->len, SV_PARSE_SCRIPT, c->is_strict);
  bool needs_env = eval_program_needs_env(c, program);
  
  parse_arena_rewind(c->js, mark);
  Ant_Exception_Set(c->js, saved_exception);
  GC_ROOT_RESTORE(c->js, exception_mark);
  
  return needs_env;
}

static bool eval_call_needs_env(sv_compiler_t *c, const sv_ast_t *node) {
  if (node->type != N_CALL || !is_ident_name(node->left, "eval") || node->args.count == 0) return false;
  if (resolve_local(c, "eval", 4) != -1 || resolve_upvalue_raw(c, "eval", 4) != -1) return false;
  if (call_has_spread_arg(node)) return true;
  const sv_ast_t *arg = node->args.items[0];
  if (eval_arg_is_definitely_non_string(arg)) return false;
  
  return !arg || arg->type != N_STRING || eval_source_needs_env(c, arg);
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
