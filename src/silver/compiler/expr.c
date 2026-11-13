#include "compiler_internal.h"

static inline bool is_invalid_cooked_string(const sv_ast_t *node) {
  return is_template_segment(node) && (node->flags & FN_INVALID_COOKED);
}

bool template_has_valid_cooked_segments(const sv_ast_t *node) {
  if (!node || node->type != N_TEMPLATE) return false;
  for (int i = 0; i < node->args.count; i++)
    if (is_invalid_cooked_string(node->args.items[i])) return false;
  return true;
}

uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return SV_TI_UNDEF;

  switch (node->type) {
    case N_NUMBER:   return SV_TI_NUM;
    case N_STRING:   return SV_TI_STR;
    case N_BOOL:     return SV_TI_BOOL;
    case N_NULL:     return SV_TI_NULL;
    case N_ARRAY:    return SV_TI_ARR;
    case N_OBJECT:   return SV_TI_OBJ;
    case N_TEMPLATE: return SV_TI_STR;
    case N_TYPEOF:   return SV_TI_STR;
    case N_VOID:     return SV_TI_UNDEF;
    case N_NEW:      return SV_TI_OBJ;
    case N_UNDEF:    return undefined_shadowed(c) ? SV_TI_UNKNOWN : SV_TI_UNDEF;

    case N_IDENT: {
      int local = resolve_local(c, node->str, node->len);
      if (local < 0 || local_with_shadowed(c, node->str, node->len)) return SV_TI_UNKNOWN;
      return get_local_inferred_type(c, local);
    }

    case N_SEQUENCE:
      return infer_expr_type(c, node->right);

    case N_TERNARY: {
      uint8_t lt = infer_expr_type(c, node->left);
      uint8_t rhs_type = infer_expr_type(c, node->right);
      if (lt == rhs_type && sv_type_is_known(lt)) return lt;
      return SV_TI_UNKNOWN;
    }

    case N_UNARY: {
      uint8_t rhs_type = infer_expr_type(c, node->right);
      switch (node->op) {
        case TOK_UPLUS:
        case TOK_UMINUS:
          return sv_type_is_num(rhs_type) ? SV_TI_NUM : SV_TI_UNKNOWN;
        case TOK_NOT:
          return SV_TI_BOOL;
        default:
          return SV_TI_UNKNOWN;
      }
    }

    case N_BINARY: {
      uint8_t lt = infer_expr_type(c, node->left);
      uint8_t rhs_type = infer_expr_type(c, node->right);
      switch (node->op) {
        case TOK_PLUS:
          if (lt == SV_TI_NUM && rhs_type == SV_TI_NUM) return SV_TI_NUM;
          if (lt == SV_TI_STR && rhs_type == SV_TI_STR) return SV_TI_STR;
          return SV_TI_UNKNOWN;
        case TOK_MINUS:
        case TOK_MUL:
        case TOK_DIV:
          return (lt == SV_TI_NUM && rhs_type == SV_TI_NUM) ? SV_TI_NUM : SV_TI_UNKNOWN;
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE:
        case TOK_EQ:
        case TOK_NE:
        case TOK_SEQ:
        case TOK_SNE:
        case TOK_INSTANCEOF:
        case TOK_IN:
          return SV_TI_BOOL;
        case TOK_LAND:
        case TOK_LOR:
        case TOK_NULLISH:
          if (lt == rhs_type && sv_type_is_known(lt)) return lt;
          return SV_TI_UNKNOWN;
        default:
          return SV_TI_UNKNOWN;
      }
    }

    default:
      return SV_TI_UNKNOWN;
  }
}

static void compile_yield_star_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (node->right) compile_expr(c, node->right);
  else emit_op(c, OP_UNDEF);

  int base_local = reserve_hidden_locals(c, 4);
  uint16_t base_slot = (uint16_t)(base_local - c->param_locals);

  if (c->is_async_generator) {
    emit_op(c, OP_YIELD_STAR_INIT_ASYNC);
    emit_u16(c, base_slot);
    emit_op(c, OP_YIELD_STAR_ASYNC_CALL);
    emit_u16(c, base_slot);
    emit_op(c, OP_AWAIT);
    emit_op(c, OP_YIELD_STAR_ASYNC_RESULT);
    emit_u16(c, base_slot);
  } else {
    emit_op(c, OP_YIELD_STAR_INIT);
    emit_u16(c, base_slot);
  }

  emit_op(c, OP_YIELD_STAR_NEXT);
  emit_u16(c, base_slot);
}

bool compile_stack_overflow(sv_compiler_t *c) {
  if (!sv_check_c_stack_overflow(c->js)) return false;
  if (!Ant_Exception_Pending(c->js)) js_mkerr_typed(c->js, JS_ERR_RANGE, "Maximum call stack size exceeded");
  return true;
}

void compile_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (!node || compile_stack_overflow(c)) { emit_op(c, OP_UNDEF); return; }
  emit_srcpos(c, node);

  switch (node->type) {
    case N_NUMBER:
      emit_number(c, node->num);
      break;

    case N_STRING: {
      emit_constant(c, ast_string_const(c, node));
      break;
    }

    case N_BIGINT: {
      bool neg = false;
      const char *digits = node->str;
      uint32_t dlen = node->len;
      if (dlen > 0 && digits[0] == '-') { neg = true; digits++; dlen--; }
      if (dlen > 0 && digits[dlen - 1] == 'n') dlen--;
      ant_value_t bi = sv_const_bigint(c, digits, dlen, neg);
      emit_constant(c, bi);
      break;
    }

    case N_BOOL:
      emit_op(c, node->num != 0.0 ? OP_TRUE : OP_FALSE);
      break;

    case N_NULL:
      emit_op(c, OP_NULL);
      break;

    case N_UNDEF:
      if (undefined_shadowed(c)) emit_get_var(c, "undefined", 9);
      else emit_op(c, OP_UNDEF);
      break;

    case N_THIS:
      emit_op(c, OP_THIS);
      break;

    case N_GLOBAL_THIS:
      if (global_this_shadowed(c)) emit_get_var(c, "globalThis", 10);
      else emit_op(c, OP_GLOBAL);
      break;

    case N_NEW_TARGET: {
      emit_lexical_new_target(c);
      break;
    }

    case N_IDENT:
      if (is_private_name_node(node)) {
        js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private names may only be used as class member names");
        emit_op(c, OP_UNDEF);
        break;
      }
      emit_get_var(c, node->str, node->len);
      break;

    case N_BINARY:
      compile_binary(c, node);
      break;

    case N_UNARY:
      compile_unary(c, node);
      break;

    case N_UPDATE:
      compile_update(c, node);
      break;

    case N_ASSIGN:
      compile_assign(c, node);
      break;

    case N_TERNARY:
      compile_ternary(c, node);
      break;

    case N_CALL:
      compile_call(c, node);
      break;

    case N_NEW:
      compile_new(c, node);
      break;

    case N_MEMBER:
      compile_member(c, node);
      break;

    case N_OPTIONAL:
      compile_optional(c, node);
      break;

    case N_ARRAY:
      compile_array(c, node);
      break;

    case N_OBJECT:
      compile_object(c, node);
      break;

    case N_FUNC:
      compile_func_expr(c, node);
      break;

    case N_CLASS:
      compile_class(c, node);
      break;

    case N_SEQUENCE:
      compile_expr(c, node->left);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      break;

    case N_TYPEOF:
      compile_typeof(c, node);
      break;

    case N_VOID:
      compile_expr(c, node->right);
      emit_op(c, OP_VOID);
      break;

    case N_DELETE:
      compile_delete(c, node);
      break;

    case N_SPREAD:
      compile_expr(c, node->right);
      break;

    case N_TEMPLATE:
      compile_template(c, node);
      break;

    case N_AWAIT:
      if (!await_allowed(c)) js_mkerr_typed(c->js, JS_ERR_SYNTAX,
        "await is only valid in async functions and the top level bodies of modules");
      compile_expr(c, node->right);
      emit_op(c, OP_AWAIT);
      if (c->enclosing && !c->enclosing->enclosing)
        c->is_tla = true;
      break;

    case N_YIELD:
      if (node->flags & FN_YIELD_STAR) compile_yield_star_expr(c, node);
      else {
        if (node->right) compile_expr(c, node->right);
        else emit_op(c, OP_UNDEF);
        if (c->is_async_generator) emit_op(c, OP_ASYNC_YIELD);
        emit_op(c, OP_YIELD);
      }
      break;

    case N_THROW:
      compile_expr(c, node->right);
      emit_op(c, OP_THROW);
      break;

    case N_TAGGED_TEMPLATE: {
      sv_call_kind_t tag_kind = compile_call_setup_non_optional(c, node->left);
      sv_ast_t *tpl = node->right;
      int n = tpl->args.count;
      int n_strings = 0, n_exprs = 0;
      for (int i = 0; i < n; i++) {
        if (is_template_segment(tpl->args.items[i])) n_strings++;
        else n_exprs++;
      }
      int cache_idx = add_constant(c, Ant_Value_Undefined);
      emit_op(c, OP_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      int skip_jump = emit_jump(c, OP_JMP_TRUE_PEEK);
      emit_op(c, OP_POP);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (!is_template_segment(item)) continue;
        if (is_invalid_cooked_string(item))
          emit_op(c, OP_UNDEF);
        else emit_constant(c, ast_string_const(c, item));
      }
      emit_op(c, OP_ARRAY);
      emit_u16(c, (uint16_t)n_strings);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (!is_template_segment(item)) continue;
        const char *raw = item->aux ? item->aux : item->str;
        uint32_t raw_len = item->aux ? item->aux_len : item->len;
        emit_constant(c, sv_const_str(c, raw ? raw : "", raw_len));
      }
      emit_op(c, OP_ARRAY);
      emit_u16(c, (uint16_t)n_strings);
      emit_atom_op(c, OP_GET_GLOBAL, "Object", 6);
      emit_atom_op(c, OP_GET_FIELD2, "freeze", 6);
      emit_op(c, OP_ROT3L);
      emit_op(c, OP_CALL_METHOD);
      emit_u16(c, 1);
      emit_atom_op(c, OP_DEFINE_FIELD, "raw", 3);
      emit_atom_op(c, OP_GET_GLOBAL, "Object", 6);
      emit_atom_op(c, OP_GET_FIELD2, "freeze", 6);
      emit_op(c, OP_ROT3L);
      emit_op(c, OP_CALL_METHOD);
      emit_u16(c, 1);
      emit_op(c, OP_SET_BRAND);
      emit(c, BRAND_TEMPLATE_OBJECT);
      emit_op(c, OP_DUP);
      emit_op(c, OP_PUT_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      patch_jump(c, skip_jump);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (is_template_segment(item)) continue;
        compile_expr(c, item);
      }
      emit_op(c, tag_kind == SV_CALL_METHOD ? OP_CALL_METHOD : OP_CALL);
      emit_u16(c, (uint16_t)(1 + n_exprs));
      break;
    }

    case N_IMPORT:
      if (has_module_import_binding(c)) {
        emit_get_module_import_binding(c);
        compile_expr(c, node->right);
        if (node->left) compile_expr(c, node->left);
        else emit_op(c, OP_UNDEF);
        emit_op(c, OP_CALL);
        emit_u16(c, 2);
      } else {
        compile_expr(c, node->right);
        if (node->left) compile_expr(c, node->left);
        else emit_op(c, OP_UNDEF);
        emit_op(c, OP_IMPORT);
      }
      break;

    case N_REGEXP:
      emit_constant(c, sv_const_str(c, node->str ? node->str : "", node->len));
      emit_constant(c, sv_const_str(c, node->aux ? node->aux : "", node->aux_len));
      emit_op(c, OP_REGEXP);
      break;

    default:
      emit_op(c, OP_UNDEF);
      break;
  }
}

static void compile_typeof_op(sv_compiler_t *c, sv_ast_t *node, int test_type);

static bool compile_primitive_type_compare(sv_compiler_t *c, sv_ast_t *node) {
  if (node->op != TOK_SEQ && node->op != TOK_SNE && node->op != TOK_EQ && node->op != TOK_NE) return false;
  sv_ast_t *type = node->left, *name = node->right;
  
  if (name->type == N_TYPEOF) { type = node->right; name = node->left; }
  if (type->type != N_TYPEOF || name->type != N_STRING) return false;
  
  static const struct { const char *name; uint8_t type; } primitives[] = {
    {"string", kTypeString}, {"number", kTypeNumber}, {"boolean", kTypeBool},
    {"undefined", kTypeUndefined}, {"symbol", kTypeSymbol}, {"bigint", kTypeBigInt},
    {"object", kTypeObject}, {"function", kTypeFunction},
  };
  
  for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++) {
    if (!is_ident_str(name->str, name->len, primitives[i].name, (uint32_t)strlen(primitives[i].name))) continue;
    compile_typeof_op(c, type, primitives[i].type);
    if (node->op == TOK_SNE || node->op == TOK_NE) emit_op(c, OP_NOT);
    return true;
  }
  
  return false;
}

void compile_binary(sv_compiler_t *c, sv_ast_t *node) {
  uint8_t op = node->op;
  if (compile_primitive_type_compare(c, node)) return;

  if (op == TOK_LAND) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_FALSE_PEEK);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }
  
  if (op == TOK_LOR) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_TRUE_PEEK);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }
  
  if (op == TOK_NULLISH) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }

  if (op == TOK_IN && node->left->type == N_IDENT &&
      node->left->len > 0 && node->left->str[0] == '#') {
    compile_expr(c, node->right);
    emit_private_token(c, node->left);
    emit_op(c, OP_HAS_PRIVATE);
    return;
  }

  sv_ast_t *key = node->left;
  if (
    op == TOK_IN && key->type == N_STRING && key->str &&
    !(key->len > 0 && key->str[0] >= '0' && key->str[0] <= '9') &&
    !(key->len == 6 && !memcmp(key->str, "length", 6))
  ) {
    compile_expr(c, node->right);
    emit_atom_op(c, OP_IN_FIELD, key->str, key->len);
    return;
  }

  uint8_t left_type = SV_TI_UNKNOWN;
  uint8_t right_type = SV_TI_UNKNOWN;
  
  if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_MUL || op == TOK_DIV) {
    left_type = infer_expr_type(c, node->left);
    right_type = infer_expr_type(c, node->right);
  }

  compile_expr(c, node->left);
  compile_expr(c, node->right);

  switch (op) {
    case TOK_PLUS:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_ADD_NUM : OP_ADD);
      break;
    case TOK_MINUS:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_SUB_NUM : OP_SUB);
      break;
    case TOK_MUL:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_MUL_NUM : OP_MUL);
      break;
    case TOK_DIV:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_DIV_NUM : OP_DIV);
      break;
    case TOK_REM:        emit_op(c, OP_MOD); break;
    case TOK_EXP:        emit_op(c, OP_EXP); break;
    case TOK_LT:         emit_op(c, OP_LT);  break;
    case TOK_LE:         emit_op(c, OP_LE);  break;
    case TOK_GT:         emit_op(c, OP_GT);  break;
    case TOK_GE:         emit_op(c, OP_GE);  break;
    case TOK_EQ:         emit_op(c, OP_EQ);  break;
    case TOK_NE:         emit_op(c, OP_NE);  break;
    case TOK_SEQ:        emit_op(c, OP_SEQ); break;
    case TOK_SNE:        emit_op(c, OP_SNE); break;
    case TOK_AND:        emit_op(c, OP_BAND); break;
    case TOK_OR:         emit_op(c, OP_BOR);  break;
    case TOK_XOR:        emit_op(c, OP_BXOR); break;
    case TOK_SHL:        emit_op(c, OP_SHL);  break;
    case TOK_SHR:        emit_op(c, OP_SHR);  break;
    case TOK_ZSHR:       emit_op(c, OP_USHR); break;
    case TOK_INSTANCEOF:
      emit_op(c, OP_INSTANCEOF);
      emit_u16(c, alloc_ic_idx(c));
      break;
    case TOK_IN:         emit_op(c, OP_IN);  break;
    default:             emit_op(c, OP_UNDEF); break;
  }
}

void compile_unary(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->right);
  switch (node->op) {
    case TOK_NOT:    emit_op(c, OP_NOT);   break;
    case TOK_TILDA:  emit_op(c, OP_BNOT);  break;
    case TOK_UPLUS:  emit_op(c, OP_UPLUS); break;
    case TOK_UMINUS: emit_op(c, OP_NEG);   break;
    default: break;
  }
}

void compile_ternary(sv_compiler_t *c, sv_ast_t *node) {
  compile_truthy_test_expr(c, node->cond);
  int else_jump = emit_jump(c, OP_JMP_FALSE);
  compile_expr(c, node->left);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, else_jump);
  compile_expr(c, node->right);
  patch_jump(c, end_jump);
}

static void compile_typeof_op(sv_compiler_t *c, sv_ast_t *node, int test_type) {
  sv_ast_t *arg = node->right;
  sv_patch_list_t found = {0};
  
  if (arg->type == N_IDENT) {
    sv_with_refs_t shadows = {0};
    with_shadows(c, arg->str, arg->len, &shadows);
    
    bool shadowed = shadows.count > 0;
    emit_with_tries(c, &shadows, OP_WITH_TRY_GET, arg->str, arg->len, 1, &found);
    
    int local = resolve_local(c, arg->str, arg->len);
    if (local != -1) {
      uint8_t inferred = get_local_inferred_type(c, local);
      const char *known = typeof_name_for_type(inferred);
      if (test_type < 0 && known && !shadowed && c->locals[local].is_const) {
        emit_constant(c, sv_const_str(c, known, strlen(known)));
        return;
      }
      emit_get_var_plain(c, arg->str, arg->len);
    } else {
      int upval = resolve_upvalue(c, arg->str, arg->len);
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
      } else if (
          !c->owns_eval_env && has_implicit_arguments_obj(c) &&
          is_ident_str(arg->str, arg->len, "arguments", 9)
        ) {
        if (c->strict_args_local >= 0) {
          emit_get_local(c, c->strict_args_local);
        } else {
          emit_op(c, OP_SPECIAL_OBJ);
          emit(c, SV_SPECIAL_ARGUMENTS);
        }
      } else emit_atom_op(
        c, env_resolution_op(
          c, OP_GET_GLOBAL_UNDEF, OP_GET_EVAL_GLOBAL_UNDEF),
        arg->str, arg->len);
    }
    
    patch_list_resolve(c, &found);
  } else compile_expr(c, arg);
  
  if (test_type >= 0) {
    emit_op(c, OP_IS_PRIMITIVE_TYPE);
    emit(c, (uint8_t)test_type);
  } else emit_op(c, OP_TYPEOF);
}

void compile_typeof(sv_compiler_t *c, sv_ast_t *node) {
  compile_typeof_op(c, node, -1);
}

bool sv_node_has_optional_base(sv_ast_t *n) {
  while (n) {
    if (n->type == N_OPTIONAL) return true;
    if (n->type == N_MEMBER || n->type == N_CALL) n = n->left;
    else break;
  }
  return false;
}

static void compile_delete_optional(sv_compiler_t *c, sv_ast_t *arg) {
  compile_expr(c, arg->left);
  int ok_jump = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_POP);
  emit_op(c, OP_TRUE);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, ok_jump);
  if (arg->flags & 1) {
    compile_expr(c, arg->right);
  } else {
    ant_value_t key = sv_const_str(c, arg->right->str, arg->right->len);
    emit_constant(c, key);
  }
  emit_op(c, OP_DELETE);
  patch_jump(c, end_jump);
}

void compile_delete(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *arg = node->right;
  if ((arg->type == N_MEMBER || arg->type == N_OPTIONAL) &&
      arg->right && is_private_name_node(arg->right)) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot delete private fields");
    emit_op(c, OP_TRUE);
    return;
  }
  if (arg->type == N_OPTIONAL) {
    compile_delete_optional(c, arg);
  } else if (arg->type == N_MEMBER && sv_node_has_optional_base(arg->left)) {
    compile_delete_optional(c, arg);
  } else if (arg->type == N_MEMBER && !(arg->flags & 1)) {
    compile_expr(c, arg->left);
    ant_value_t key = sv_const_str(c, arg->right->str, arg->right->len);
    emit_constant(c, key);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_MEMBER && (arg->flags & 1)) {
    compile_expr(c, arg->left);
    compile_expr(c, arg->right);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_IDENT) {
    sv_with_refs_t shadows = {0};
    sv_patch_list_t deleted = {0};
    with_shadows(c, arg->str, arg->len, &shadows);
    emit_with_tries(c, &shadows, OP_WITH_TRY_DEL, arg->str, arg->len, 1, &deleted);

    bool binding_is_local = false;
    for (sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
      if (resolve_local(cur, arg->str, arg->len) != -1) {
        binding_is_local = true;
        break;
      }
      if (cur->owns_eval_env) break;
    }
    
    if (binding_is_local) emit_op(c, OP_FALSE);
    else emit_atom_op(c, env_resolution_op(c, OP_DELETE_VAR, OP_DELETE_EVAL_VAR), arg->str, arg->len);
    
    patch_list_resolve(c, &deleted);
  } else {
    compile_expr(c, arg);
    emit_op(c, OP_POP);
    emit_op(c, OP_TRUE);
  }
}

void compile_template(sv_compiler_t *c, sv_ast_t *node) {
  int n = node->args.count;
  if (n == 0) {
    emit_constant(c, sv_const_str(c, "", 0));
    return;
  }
  compile_template_items(c, node, 0);
}

static bool is_direct_length_member(sv_ast_t *node) {
  return !(node->flags & 1) && !is_private_name_node(node->right) &&
    node->right->len == 6 && memcmp(node->right->str, "length", 6) == 0;
}

static int resolve_raw_length_local(sv_compiler_t *c, sv_ast_t *base) {
  if (c->with_depth != 0 || base->type != N_IDENT) return -1;

  int local = resolve_local(c, base->str, base->len);
  if (local < 0 || c->locals[local].is_tdz) return -1;
  return local_to_frame_slot(c, local);
}

void compile_member(sv_compiler_t *c, sv_ast_t *node) {
  if (is_ident_name(node->left, "super")) {
    if (!(node->flags & 1) && is_private_name_node(node->right)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot access private member through super");
      emit_op(c, OP_UNDEF);
      return;
    }
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (node->flags & 1)
      compile_expr(c, node->right);
    else
      emit_constant(c, sv_const_str(c, node->right->str, node->right->len));
    emit_op(c, OP_GET_SUPER_VAL);
    return;
  }

  if (
    c->args_length_only && !c->owns_eval_env &&
    has_implicit_arguments_obj(c) && ast_is_arguments_length(node)
  ) {
    emit_op(c, OP_SPECIAL_OBJ);
    emit(c, SV_SPECIAL_ARGC);
    return;
  }

  bool direct_length = is_direct_length_member(node);
  int raw_length_local = direct_length ? resolve_raw_length_local(c, node->left) : -1;

  if (raw_length_local >= 0)
    emit_slot_op(c, OP_GET_SLOT_RAW, (uint16_t)raw_length_local);
  else compile_expr(c, node->left);

  int ok_jump = -1, end_jump = -1;
  if (sv_node_has_optional_base(node->left)) {
    ok_jump  = emit_jump(c, OP_JMP_NOT_NULLISH);
    end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, ok_jump);
  }

  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE);
  } else {
    if (direct_length) emit_get_length(c);
    else {
      emit_srcpos(c, node->right);
      emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
    }
  }

  if (end_jump >= 0) patch_jump(c, end_jump);
}

void compile_optional_get(sv_compiler_t *c, sv_ast_t *node) {
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM_OPT);
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE_OPT);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD_OPT, node->right->str, node->right->len);
  }
}

void compile_optional(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  compile_optional_get(c, node);
}
