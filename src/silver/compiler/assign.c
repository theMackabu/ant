#include "compiler_internal.h"

typedef enum {
  SELF_APPEND_EXPR,
  SELF_APPEND_TEMPLATE_REST,
} self_append_rhs_kind_t;

typedef struct {
  int local;
  uint16_t slot;
  sv_ast_t *rhs;
  self_append_rhs_kind_t rhs_kind;
} self_append_match_t;

static bool same_ident(const sv_ast_t *a, const sv_ast_t *b) {
  return a && b &&
    a->type == N_IDENT &&
    b->type == N_IDENT &&
    a->str && b->str &&
    a->len == b->len &&
    memcmp(a->str, b->str, a->len) == 0;
}

static sv_ast_t *match_binary_self_append(sv_ast_t *lhs, sv_ast_t *rhs) {
  if (!rhs || rhs->type != N_BINARY || rhs->op != TOK_PLUS) return NULL;
  if (!same_ident(rhs->left, lhs)) return NULL;
  return rhs->right;
}

static bool match_template_self_append(
  sv_compiler_t *c, int local, sv_ast_t *lhs, sv_ast_t *rhs
) {
  if (!rhs || rhs->type != N_TEMPLATE || rhs->args.count < 3) return false;
  if (!is_template_segment(rhs->args.items[0]) || rhs->args.items[0]->len != 0)
    return false;
  if (!same_ident(rhs->args.items[1], lhs)) return false;
  if (!template_has_valid_cooked_segments(rhs)) return false;
  return get_local_inferred_type(c, local) == SV_TI_STR;
}

static bool match_self_append_local(
  sv_compiler_t *c, sv_ast_t *node, self_append_match_t *match
) {
  if (!c || !node || node->type != N_ASSIGN || !node->left || node->left->type != N_IDENT)
    return false;
  if (c->with_depth > 0) return false;

  int local = resolve_local(c, node->left->str, node->left->len);
  if (local < 0 || c->locals[local].is_const) return false;
  if (c->locals[local].is_tdz) return false;
  if (c->locals[local].depth == -1 && c->arguments_local >= 0) return false;

  sv_ast_t *rhs = NULL;
  self_append_rhs_kind_t rhs_kind = SELF_APPEND_EXPR;
  switch (node->op) {
    case TOK_PLUS_ASSIGN:
      rhs = node->right;
      break;
    case TOK_ASSIGN:
      rhs = match_binary_self_append(node->left, node->right);
      if (!rhs && match_template_self_append(c, local, node->left, node->right)) {
        rhs = node->right;
        rhs_kind = SELF_APPEND_TEMPLATE_REST;
      }
      break;
    default:
      return false;
  }

  if (!rhs) return false;
  uint8_t local_type = get_local_inferred_type(c, local);
  if (rhs_kind == SELF_APPEND_EXPR && local_type != SV_TI_STR &&
      !c->locals[local].str_accumulating && infer_expr_type(c, rhs) != SV_TI_STR)
    return false;

  if (match) {
    match->local = local;
    match->slot = (uint16_t)local_to_frame_slot(c, local);
    match->rhs = rhs;
    match->rhs_kind = rhs_kind;
  }

  return true;
}

static bool is_self_append_inplace_safe_ident(sv_compiler_t *c, sv_ast_t *node) {
  if (!c || !node || node->type != N_IDENT) return false;

  if (resolve_local(c, node->str, node->len) != -1) return true;
  if (resolve_upvalue(c, node->str, node->len) != -1) return true;

  if (is_ident_str(node->str, node->len, "arguments", 9) && has_implicit_arguments_obj(c)) return true;

  if (c->is_arrow && is_ident_str(node->str, node->len, "super", 5))
    return resolve_super_upvalue(c) != -1;

  if (has_module_import_binding(c) && is_ident_str(node->str, node->len, "import", 6))
    return true;

  return false;
}

static bool is_self_append_inplace_safe_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return false;

  switch (node->type) {
    case N_NUMBER:
    case N_STRING:
    case N_BIGINT:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
      return true;

    case N_IDENT:
      return is_self_append_inplace_safe_ident(c, node);

    case N_BINARY: return 
      is_self_append_inplace_safe_expr(c, node->left) &&
      is_self_append_inplace_safe_expr(c, node->right);
      
    case N_UNARY:
    case N_TYPEOF:
    case N_VOID:
      return is_self_append_inplace_safe_expr(c, node->left);

    default:
      return false;
  }
}

void compile_template_items(sv_compiler_t *c, sv_ast_t *node, int start) {
  bool has_value = false;
  for (int i = start; i < node->args.count; i++) {
    sv_ast_t *item = node->args.items[i];
    if (is_template_segment(item) && item->len == 0) continue;
    compile_expr(c, item);
    if (!is_template_segment(item)) emit_op(c, OP_TO_STRING);
    if (has_value) emit_op(c, OP_ADD);
    has_value = true;
  }
  if (!has_value) emit_constant(c, sv_const_str(c, "", 0));
}

static void compile_self_append_rhs(
  sv_compiler_t *c, const self_append_match_t *match
) {
  if (match->rhs_kind == SELF_APPEND_TEMPLATE_REST)
    compile_template_items(c, match->rhs, 2);
  else
    compile_expr(c, match->rhs);
}

static void compile_self_append(
  sv_compiler_t *c, const self_append_match_t *match
) {
  if (match->rhs_kind == SELF_APPEND_TEMPLATE_REST) {
    emit_slot_op(c, OP_GET_SLOT_RAW, match->slot);
    emit_op(c, OP_TO_STRING);
    compile_self_append_rhs(c, match);
    emit_slot_op(c, OP_STR_ALC_SNAPSHOT, match->slot);
    set_local_inferred_type(c, match->local, SV_TI_STR);
    return;
  }
  if (is_self_append_inplace_safe_expr(c, match->rhs)) {
    compile_self_append_rhs(c, match);
    emit_slot_op(c, OP_STR_APPEND_LOCAL, match->slot);
  } else {
    emit_slot_op(c, OP_GET_SLOT_RAW, match->slot);
    compile_self_append_rhs(c, match);
    emit_slot_op(c, OP_STR_ALC_SNAPSHOT, match->slot);
  }
  set_local_inferred_type(c, match->local, SV_TI_UNKNOWN);
  c->locals[match->local].str_accumulating = true;
}

bool compile_self_append_stmt(sv_compiler_t *c, sv_ast_t *node) {
  self_append_match_t match;
  if (!match_self_append_local(c, node, &match)) return false;
  compile_self_append(c, &match);
  return true;
}

static sv_op_t compound_assign_op(uint8_t op) {
  switch (op) {
    case TOK_PLUS_ASSIGN:  return OP_ADD;
    case TOK_MINUS_ASSIGN: return OP_SUB;
    case TOK_MUL_ASSIGN:   return OP_MUL;
    case TOK_DIV_ASSIGN:   return OP_DIV;
    case TOK_REM_ASSIGN:   return OP_MOD;
    case TOK_SHL_ASSIGN:   return OP_SHL;
    case TOK_SHR_ASSIGN:   return OP_SHR;
    case TOK_ZSHR_ASSIGN:  return OP_USHR;
    case TOK_AND_ASSIGN:   return OP_BAND;
    case TOK_XOR_ASSIGN:   return OP_BXOR;
    case TOK_OR_ASSIGN:    return OP_BOR;
    case TOK_EXP_ASSIGN:   return OP_EXP;
    default:               return OP_INVALID;
  }
}

static void emit_compound_assign_op(sv_compiler_t *c, uint8_t op) {
  sv_op_t aop = compound_assign_op(op);
  if (aop != OP_INVALID) emit_op(c, aop);
}

static bool is_super_member(const sv_ast_t *target) {
  return target && target->type == N_MEMBER && is_ident_name(target->left, "super");
}

bool reject_super_private(sv_compiler_t *c, const sv_ast_t *member) {
  if ((member->flags & 1) || !is_private_name_node(member->right)) return false;
  js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot access private member through super");
  emit_op(c, OP_UNDEF);
  return true;
}

void emit_super_key(sv_compiler_t *c, sv_ast_t *member) {
  if (member->flags & 1) compile_expr(c, member->right);
  else emit_constant(c, sv_const_str(c, member->right->str, member->right->len));
}

void emit_super_reference(sv_compiler_t *c, sv_ast_t *member) {
  emit_op(c, OP_THIS);
  emit_get_var(c, "super", 5);
  emit_super_key(c, member);
}

static void emit_super_reference_read(sv_compiler_t *c) {
  emit_op(c, OP_DUP2);
  emit_op(c, OP_THIS);
  emit_op(c, OP_ROT3R);
  emit_op(c, OP_GET_SUPER_VAL);
}

static void compile_super_update(sv_compiler_t *c, sv_ast_t *target, bool prefix, bool is_inc) {
  if (reject_super_private(c, target)) return;
  emit_super_reference(c, target);
  emit_super_reference_read(c);
  
  if (prefix) {
    emit_op(c, is_inc ? OP_INC : OP_DEC);
    emit_op(c, OP_PUT_SUPER_VAL);
    return;
  }
  
  int old_local = reserve_hidden_locals(c, 1);
  emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
  emit_op(c, OP_SWAP);
  emit_put_local(c, old_local);
  emit_op(c, OP_PUT_SUPER_VAL);
  emit_op(c, OP_POP);
  emit_get_local(c, old_local);
}

static void compile_super_assign(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *target = node->left;
  uint8_t op = node->op;
  if (reject_super_private(c, target)) return;
  emit_super_reference(c, target);
  
  if (op == TOK_ASSIGN) {
    compile_expr(c, node->right);
    emit_op(c, OP_PUT_SUPER_VAL);
    return;
  }
  
  emit_super_reference_read(c);
  
  if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN || op == TOK_NULLISH_ASSIGN) {
    int skip = emit_jump(c,
      op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
      op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    emit_op(c, OP_PUT_SUPER_VAL);
    int end = emit_jump(c, OP_JMP);
    patch_jump(c, skip);
    emit_op(c, OP_NIP);
    emit_op(c, OP_NIP);
    emit_op(c, OP_NIP);
    patch_jump(c, end);
    return;
  }
  
  compile_expr(c, node->right);
  emit_compound_assign_op(c, op);
  emit_op(c, OP_PUT_SUPER_VAL);
}

void compile_update(sv_compiler_t *c, sv_ast_t *node) {
  bool prefix = (node->flags & 1);
  bool is_inc = (node->op == TOK_POSTINC);
  sv_ast_t *target = node->right;

  if (is_super_member(target)) {
    compile_super_update(c, target, prefix, is_inc);
    return;
  }

  if (target->type == N_IDENT) {
    if (prefix) {
      emit_get_var(c, target->str, target->len);
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_set_var(c, target->str, target->len, true);
    } else {
      emit_get_var(c, target->str, target->len);
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_set_var(c, target->str, target->len, false);
    }
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_private_token(c, target->right);
    emit_op(c, OP_GET_PRIVATE);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_SWAP_UNDER);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      emit_op(c, OP_POP);
    }
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    int atom = add_atom(c, target->right->str, target->right->len);
    emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_SWAP_UNDER);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
    }
  } else if (target->type == N_MEMBER && (target->flags & 1)) {
    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_DUP2);
    emit_op(c, OP_GET_ELEM);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_ROT4_UNDER);
      emit_op(c, OP_PUT_ELEM);
    }
  } else {
    emit_op(c, OP_UNDEF);
  }
}

void compile_assign(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *target = node->left;
  uint8_t op = node->op;
  if (is_super_member(target)) {
    compile_super_assign(c, node);
    return;
  }
  
  if (target->type == N_UNDEF && undefined_shadowed(c)) {
    target->type = N_IDENT;
    target->str = "undefined";
    target->len = 9;
  }

  if (target->type == N_UNDEF && op != TOK_ASSIGN) {
    static const char message[] = "Cannot assign to read only property 'undefined' of object '#<Object>'";
    emit_op(c, OP_UNDEF);
    if (op == TOK_LAND_ASSIGN) return;
    if (op == TOK_LOR_ASSIGN || op == TOK_NULLISH_ASSIGN) {
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
    } else {
      compile_expr(c, node->right);
      emit_compound_assign_op(c, op);
    }
    if (c->is_strict) emit_throw_type_error(c, message, sizeof(message) - 1);
    return;
  }

  self_append_match_t append;
  bool can_append_builder = match_self_append_local(c, node, &append);

  if (op == TOK_ASSIGN) {
    if (
      target->type == N_MEMBER && !(target->flags & 1) &&
      target->right && target->right->str &&
      is_ident_str(target->right->str, target->right->len, "exec", 4)
    ) c->regexp_exec_write_seen = true;
    if (
      target->type == N_MEMBER && !(target->flags & 1) &&
      target->right && target->right->str &&
      is_ident_str(target->right->str, target->right->len, "replace", 7)
    ) c->regexp_replace_write_seen = true;

    if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
      compile_expr(c, target->left);
      compile_expr(c, node->right);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      return;
    }

    if (target->type == N_MEMBER && !(target->flags & 1)) {
      int atom = add_atom(c, target->right->str, target->right->len);
      compile_expr(c, target->left);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
      return;
    }
    
    if (target->type == N_MEMBER && (target->flags & 1)) {
      compile_expr(c, target->left);
      compile_expr(c, target->right);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
      return;
    }
    
    if (can_append_builder) {
      compile_self_append(c, &append);
      emit_get_var(c, target->str, target->len);
      return;
    }

    compile_expr(c, node->right);
    compile_lhs_set(c, target, true);
    return;
  }

  if (target->type == N_IDENT) {
    bool shadowed = local_with_shadowed(c, target->str, target->len);
    int lhs_local = shadowed ? -1 : resolve_local(c, target->str, target->len);
    uint8_t lhs_type = (lhs_local >= 0) ? get_local_inferred_type(c, lhs_local) : SV_TI_UNKNOWN;
    uint8_t rhs_type = infer_expr_type(c, node->right);

    if (can_append_builder) {
      compile_self_append(c, &append);
      emit_get_var(c, target->str, target->len);
      return;
    }

    if (op == TOK_PLUS_ASSIGN && !shadowed) {
      int slot = resolve_local_slot(c, target->str, target->len);
      if (slot >= 0 && !c->locals[slot + c->param_locals].is_const) {
        sv_ast_t *rhs = node->right;
        bool rhs_pure = rhs && (
          rhs->type == N_NUMBER || rhs->type == N_STRING ||
          rhs->type == N_BOOL || rhs->type == N_NULL ||
          rhs->type == N_UNDEF || rhs->type == N_IDENT
        );
        if (rhs_pure) {
          compile_expr(c, rhs);
          emit_op(c, OP_ADD_LOCAL);
          emit(c, (uint8_t)slot);
          emit_get_local(c, c->param_locals + slot);
          set_local_inferred_type(c, c->param_locals + slot, SV_TI_UNKNOWN);
          return;
        }
      }
    }

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      emit_get_var(c, target->str, target->len);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_set_var(c, target->str, target->len, true);
      patch_jump(c, skip);
      return;
    }

    emit_get_var(c, target->str, target->len);
    compile_expr(c, node->right);
    switch (op) {
      case TOK_PLUS_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_ADD_NUM : OP_ADD);
        break;
      case TOK_MINUS_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_SUB_NUM : OP_SUB);
        break;
      case TOK_MUL_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_MUL_NUM : OP_MUL);
        break;
      case TOK_DIV_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_DIV_NUM : OP_DIV);
        break;
      default:               emit_compound_assign_op(c, op); break;
    }
    emit_set_var(c, target->str, target->len, true);
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      emit_op(c, OP_DUP);
      emit_private_token(c, target->right);
      emit_op(c, OP_GET_PRIVATE);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_private_token(c, target->right);
    emit_op(c, OP_GET_PRIVATE);
    compile_expr(c, node->right);
    emit_compound_assign_op(c, op);
    emit_private_token(c, target->right);
    emit_op(c, OP_PUT_PRIVATE);
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    int atom = add_atom(c, target->right->str, target->right->len);

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      emit_op(c, OP_DUP);
      emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
    compile_expr(c, node->right);
    emit_compound_assign_op(c, op);
    emit_op(c, OP_INSERT2);
    emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
  } else if (target->type == N_MEMBER && (target->flags & 1)) {

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      compile_expr(c, target->right);
      emit_op(c, OP_DUP2);
      emit_op(c, OP_GET_ELEM);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP2);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_DUP2);
    emit_op(c, OP_GET_ELEM);
    compile_expr(c, node->right);
    emit_compound_assign_op(c, op);
    emit_op(c, OP_INSERT3);
    emit_op(c, OP_PUT_ELEM);
  } else {
    compile_expr(c, node->right);
  }
}

void compile_lhs_set(sv_compiler_t *c, sv_ast_t *target, bool keep) {
  if (is_super_member(target)) {
    if (reject_super_private(c, target)) return;
    emit_op(c, OP_THIS);
    emit_op(c, OP_SWAP);
    emit_get_var(c, "super", 5);
    emit_op(c, OP_SWAP);
    emit_super_key(c, target);
    emit_op(c, OP_SWAP);
    emit_op(c, OP_PUT_SUPER_VAL);
    if (!keep) emit_op(c, OP_POP);
  } else if (target->type == N_IDENT) {
    emit_set_var(c, target->str, target->len, keep);
  } else if (target->type == N_UNDEF && undefined_shadowed(c)) {
    emit_set_var(c, "undefined", 9, keep);
  } else if (target->type == N_UNDEF && c->is_strict) {
    static const char message[] = "Cannot assign to read only property 'undefined' of object '#<Object>'";
    emit_throw_type_error(c, message, sizeof(message) - 1);
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    compile_expr(c, target->left);
    emit_op(c, OP_SWAP);
    emit_private_token(c, target->right);
    emit_op(c, OP_PUT_PRIVATE);
    if (!keep) emit_op(c, OP_POP);
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    if (keep) emit_op(c, OP_DUP);
    compile_expr(c, target->left);
    emit_op(c, OP_SWAP);
    emit_atom_op(c, OP_PUT_FIELD, target->right->str, target->right->len);
  } else if (target->type == N_MEMBER && (target->flags & 1)) {
    if (keep) emit_op(c, OP_DUP);
    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_ROT3L);
    emit_op(c, OP_PUT_ELEM);
  } else if (target->type == N_ARRAY_PAT || target->type == N_ARRAY) {
    compile_array_destructure(c, target, keep);
  } else if (target->type == N_OBJECT_PAT || target->type == N_OBJECT) {
    compile_object_destructure(c, target, keep);
  }
}
