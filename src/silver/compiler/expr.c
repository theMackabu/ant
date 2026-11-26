#include "private/codegen.h"

static inline bool is_invalid_cooked_string(const sv_ast_t *node) {
  return is_template_segment(node) && (node->flags & FN_INVALID_COOKED);
}

bool template_has_valid_cooked_segments(const sv_ast_t *node) {
  if (!node || node->type != N_TEMPLATE) return false;
  for (int i = 0; i < node->args.count; i++)
    if (is_invalid_cooked_string(node->args.items[i])) return false;
  return true;
}

static inline bool is_additive_chain_op(uint8_t op) {
  return op == TOK_PLUS || op == TOK_MINUS || op == TOK_MUL || op == TOK_DIV;
}

static inline bool is_numeric_result_op(uint8_t op) {
  switch (op) {
    case TOK_MINUS: case TOK_MUL: case TOK_DIV: case TOK_REM: case TOK_EXP:
    case TOK_SHL: case TOK_SHR: case TOK_ZSHR: case TOK_AND: case TOK_XOR: case TOK_OR:
      return true;
    default:
      return false;
  }
}

static uint8_t infer_arithmetic_type(sv_compiler_t *c, sv_ast_t *node) {
  uint8_t want = SV_TI_NUM;
  if (node->op == TOK_PLUS) {
    want = infer_expr_type(c, node->right);
    if (want != SV_TI_NUM && want != SV_TI_STR) return SV_TI_UNKNOWN;
    node = node->left;
  }

  while (node && node->type == N_BINARY && is_additive_chain_op(node->op)) {
    uint8_t right = infer_expr_type(c, node->right);
    if (node->op != TOK_PLUS) {
      if (want != SV_TI_NUM) return SV_TI_UNKNOWN;
      if (right == SV_TI_NUM) return SV_TI_NUM;
    } else if (right != want) return SV_TI_UNKNOWN;
    node = node->left;
  }

  return infer_expr_type(c, node) == want ? want : SV_TI_UNKNOWN;
}

static uint8_t binary_op_for_assign(uint8_t op) {
  switch (op) {
    case TOK_PLUS_ASSIGN:  return TOK_PLUS;
    case TOK_MINUS_ASSIGN: return TOK_MINUS;
    case TOK_MUL_ASSIGN:   return TOK_MUL;
    case TOK_DIV_ASSIGN:   return TOK_DIV;
    case TOK_REM_ASSIGN:   return TOK_REM;
    case TOK_SHL_ASSIGN:   return TOK_SHL;
    case TOK_SHR_ASSIGN:   return TOK_SHR;
    case TOK_ZSHR_ASSIGN:  return TOK_ZSHR;
    case TOK_AND_ASSIGN:   return TOK_AND;
    case TOK_XOR_ASSIGN:   return TOK_XOR;
    case TOK_OR_ASSIGN:    return TOK_OR;
    case TOK_EXP_ASSIGN:   return TOK_EXP;
    default:               return 0;
  }
}

static uint8_t infer_assign_type_bounded(sv_compiler_t *c, sv_ast_t *node) {
  if (node->op == TOK_ASSIGN) return infer_expr_type(c, node->right);
  uint8_t right = infer_expr_type(c, node->right);
  uint8_t op = binary_op_for_assign(node->op);
  if (op && op != TOK_PLUS && right == SV_TI_NUM) return SV_TI_NUM;
  if (!sv_type_is_known(right) && op == TOK_PLUS) return SV_TI_UNKNOWN;
  uint8_t left = infer_expr_type(c, node->left);
  if (!op) return left == right ? left : SV_TI_UNKNOWN;
  if (op == TOK_PLUS) return left == SV_TI_NUM && right == SV_TI_NUM ? SV_TI_NUM : SV_TI_UNKNOWN;
  return left == SV_TI_NUM ? SV_TI_NUM : SV_TI_UNKNOWN;
}

static uint8_t infer_assign_type(sv_compiler_t *c, sv_ast_t *node) {
  sv_type_scratch_t *types = c->types;
  if (types->assign_depth >= SV_TYPE_INFER_DEPTH) return SV_TI_UNKNOWN;
  types->assign_depth++;
  uint8_t type = infer_assign_type_bounded(c, node);
  types->assign_depth--;
  return type;
}

uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return SV_TI_UNDEF;

  sv_type_scratch_t *types = c->types;
  if (
    node == types->cache_node && types->cache_owner == c && types->cache_code_len == c->code_len &&
    types->cache_dep_seq == types->dep_seq && types->cache_dep_base == types->dep_count
  ) {
    types->dep_count = types->cache_dep_count;
    return types->cache_type;
  }

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
      if (!c->type_any) return SV_TI_UNKNOWN;
      int local = resolve_local(c, node->str, node->len);
      if (local < 0 || local_with_shadowed(c, node->str, node->len)) return SV_TI_UNKNOWN;
      uint8_t type = get_local_inferred_type(c, local);
      if (c->type_loop_depth && sv_type_is_known(type)) type_dep_add(c, local);
      return type;
    }

    case N_SEQUENCE:
      return infer_expr_type(c, node->right);

    case N_ASSIGN:
      return infer_assign_type(c, node);

    case N_UPDATE:
      return node->right && node->right->type == N_IDENT && infer_expr_type(c, node->right) == SV_TI_NUM
        ? SV_TI_NUM : SV_TI_UNKNOWN;

    case N_TERNARY: {
      uint8_t lt = infer_expr_type(c, node->left);
      uint8_t rhs_type = infer_expr_type(c, node->right);
      if (lt == rhs_type && sv_type_is_known(lt)) return lt;
      return SV_TI_UNKNOWN;
    }

    case N_UNARY: {
      if (node->op == TOK_NOT) return SV_TI_BOOL;
      if (node->op == TOK_UPLUS) return SV_TI_NUM;
      if (node->op != TOK_UMINUS && node->op != TOK_TILDA) return SV_TI_UNKNOWN;
      return sv_type_is_num(infer_expr_type(c, node->right)) ? SV_TI_NUM : SV_TI_UNKNOWN;
    }

    case N_BINARY: {
      if (is_additive_chain_op(node->op)) return infer_arithmetic_type(c, node);
      switch (node->op) {
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
        default: break;
      }
      uint8_t rhs_type = infer_expr_type(c, node->right);
      if (is_numeric_result_op(node->op)) {
        if (rhs_type == SV_TI_NUM) return SV_TI_NUM;
        return infer_expr_type(c, node->left) == SV_TI_NUM ? SV_TI_NUM : SV_TI_UNKNOWN;
      }
      if (node->op != TOK_LAND && node->op != TOK_LOR && node->op != TOK_NULLISH) return SV_TI_UNKNOWN;
      uint8_t lt = infer_expr_type(c, node->left);
      return lt == rhs_type && sv_type_is_known(lt) ? lt : SV_TI_UNKNOWN;
    }

    default:
      return SV_TI_UNKNOWN;
  }
}

static void emit_strict_enter(sv_compiler_t *c) {
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_STRICT_ENTER);
  emit_put_local(c, c->strict_saved_local);
}

static void emit_strict_leave(sv_compiler_t *c) {
  emit_get_local(c, c->strict_saved_local);
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_STRICT_LEAVE);
  emit_put_local(c, c->strict_saved_local);
  emit_op(c, OP_POP);
}

static void compile_strict_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (c->in_strict_region) {
    compile_expr(c, node->right);
    return;
  }

  c->strict_saved_local = add_hidden_local(c);
  int err_local = add_hidden_local(c);
  emit_op(c, OP_UNDEF);
  emit_put_local(c, c->strict_saved_local);

  c->in_strict_region = true;
  c->has_strict_region = true;
  int try_jump = emit_jump(c, OP_TRY_PUSH);
  c->try_depth++;

  emit_strict_enter(c);
  compile_expr(c, node->right);
  emit_strict_leave(c);

  emit_op(c, OP_TRY_POP);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, try_jump);
  int catch_tag = emit_jump(c, OP_CATCH);

  emit_put_local(c, err_local);
  emit_strict_leave(c);
  emit_get_local(c, err_local);
  emit_op(c, OP_THROW);

  patch_jump(c, catch_tag);
  patch_jump(c, end_jump);
  c->try_depth--;
  c->in_strict_region = false;
}

static void compile_yield_star_expr(sv_compiler_t *c) {
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
      emit_this(c);
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
      if (c->in_strict_region) emit_strict_leave(c);
      emit_op(c, OP_AWAIT);
      if (c->in_strict_region) emit_strict_enter(c);
      if (c->enclosing && !c->enclosing->enclosing)
        c->is_tla = true;
      break;

    case N_YIELD:
      if (node->right) compile_expr(c, node->right);
      else emit_op(c, OP_UNDEF);
      if (c->in_strict_region) emit_strict_leave(c);
      if (node->flags & FN_YIELD_STAR) compile_yield_star_expr(c);
      else {
        if (c->is_async_generator) emit_op(c, OP_ASYNC_YIELD);
        emit_op(c, OP_YIELD);
      }
      if (c->in_strict_region) emit_strict_enter(c);
      break;

    case N_STRICT_EXPR:
      compile_strict_expr(c, node);
      break;

    case N_THROW:
      compile_expr(c, node->right);
      emit_op(c, OP_THROW);
      break;

    case N_TAGGED_TEMPLATE: {
      sv_call_kind_t tag_kind = compile_tag_setup(c, node->left);
      sv_ast_t *tpl = node->right;
      int n = tpl->args.count;
      int n_strings = 0, n_exprs = 0;
      for (int i = 0; i < n; i++) {
        if (is_template_segment(tpl->args.items[i])) n_strings++;
        else n_exprs++;
      }
      if (n_strings > UINT16_MAX || n_exprs >= UINT16_MAX) {
        static const char message[] = "Maximum call stack size exceeded";
        emit_op(c, OP_POP);
        if (tag_kind == SV_CALL_METHOD) emit_op(c, OP_POP);
        emit_throw_error(c, JS_ERR_RANGE, message, sizeof(message) - 1);
        emit_op(c, OP_UNDEF);
        break;
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

    case N_IMPORT: {
      bool module_binding = has_module_import_binding(c);
      if (module_binding) emit_get_module_import_binding(c);
      compile_expr(c, node->right);
      if (node->flags & 1) emit_constant(c, sv_const_str(c, SV_IMPORT_SOURCE_PHASE, SV_IMPORT_SOURCE_PHASE_LEN));
      else if (node->left) compile_expr(c, node->left);
      else emit_op(c, OP_UNDEF);
      if (module_binding) {
        emit_op(c, OP_CALL);
        emit_u16(c, 2);
      } else emit_op(c, OP_IMPORT);
      break;
    }

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
static __attribute__((noinline)) bool compile_typeof_name_compare(sv_compiler_t *c, sv_ast_t *node, sv_ast_t *type, sv_ast_t *name);

static inline bool compile_primitive_type_compare(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *type = node->left, *name = node->right;
  
  if (name->type == N_TYPEOF) { type = node->right; name = node->left; }
  if (type->type != N_TYPEOF || name->type != N_STRING) return false;
  return compile_typeof_name_compare(c, node, type, name);
}

static bool compile_typeof_name_compare(sv_compiler_t *c, sv_ast_t *node, sv_ast_t *type, sv_ast_t *name) {
  
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

static void compile_arithmetic_chain(sv_compiler_t *c, sv_ast_t *node) {
  int depth = 1;
  for (sv_ast_t *cur = node->left; cur && cur->type == N_BINARY && is_additive_chain_op(cur->op); cur = cur->left) depth++;

  sv_ast_t *inline_nodes[16];
  sv_ast_t **nodes = depth <= 16 ? inline_nodes : malloc((size_t)depth * sizeof(*nodes));
  if (!nodes) {
    Ant_Error_CreateOutOfMemory(c->js);
    emit_op(c, OP_UNDEF);
    return;
  }

  sv_ast_t *cur = node;
  for (int i = 0; i < depth; i++, cur = cur->left) {
    nodes[i] = cur;
    if (i > 0) emit_srcpos(c, cur);
  }

  int dep_base = c->types->dep_count;
  bool exact = true;
  compile_expr(c, cur);
  uint8_t left_type = infer_expr_type(c, cur);
  
  for (int i = depth - 1; i >= 0; i--) {
    uint8_t op = nodes[i]->op;
    compile_expr(c, nodes[i]->right);
    int right_base = c->types->dep_count;
    bool right_matters = left_type == SV_TI_NUM || (i > 0 && (op != TOK_PLUS || left_type == SV_TI_STR));
    uint8_t right_type = right_matters ? infer_expr_type(c, nodes[i]->right) : SV_TI_UNKNOWN;
    if (!right_matters && op != TOK_PLUS) exact = false;
    bool numeric = left_type == SV_TI_NUM && right_type == SV_TI_NUM && type_deps_live(c, dep_base);
    
    if (numeric && !type_use_op(c, dep_base, c->code_len)) numeric = false;
    switch (op) {
      case TOK_PLUS:  emit_op(c, numeric ? OP_ADD_NUM : OP_ADD); break;
      case TOK_MINUS: emit_op(c, numeric ? OP_SUB_NUM : OP_SUB); break;
      case TOK_MUL:   emit_op(c, numeric ? OP_MUL_NUM : OP_MUL); break;
      default:        emit_op(c, numeric ? OP_DIV_NUM : OP_DIV); break;
    }
    
    if (numeric) continue;
    if (op != TOK_PLUS && left_type == SV_TI_NUM) {
      c->types->dep_count = right_base;
    } else if (op != TOK_PLUS && right_type == SV_TI_NUM) {
      int moved = c->types->dep_count - right_base;
      memmove(c->types->deps + dep_base, c->types->deps + right_base, (size_t)moved * sizeof(*c->types->deps));
      c->types->dep_count = dep_base + moved;
      left_type = SV_TI_NUM;
    } else {
      left_type = op == TOK_PLUS && left_type == SV_TI_STR && right_type == SV_TI_STR ? SV_TI_STR : SV_TI_UNKNOWN;
      c->types->dep_count = dep_base;
    }
  }

  sv_type_scratch_t *types = c->types;
  types->cache_node = exact ? node : NULL;
  types->cache_type = left_type;
  types->cache_owner = c;
  types->cache_code_len = c->code_len;
  types->cache_dep_seq = types->dep_seq;
  types->cache_dep_base = dep_base;
  types->cache_dep_count = types->dep_count;
  types->dep_count = dep_base;
  if (nodes != inline_nodes) free(nodes);
}

static inline bool is_logical_op(uint8_t op) {
  return op == TOK_LAND || op == TOK_LOR || op == TOK_NULLISH;
}

static void emit_plain_binary_op(sv_compiler_t *c, uint8_t op) {
  switch (op) {
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

static inline sv_op_t logical_jump_op(uint8_t op) {
  return op == TOK_LAND ? OP_JMP_FALSE_PEEK : op == TOK_LOR ? OP_JMP_TRUE_PEEK : OP_JMP_NOT_NULLISH;
}

static inline bool binary_spine_continues(const sv_ast_t *node) {
  return node->left && node->left->type == N_BINARY && !is_additive_chain_op(node->op);
}

static void compile_binary_spine(sv_compiler_t *c, sv_ast_t *node) {
  int depth = 0;
  for (sv_ast_t *cur = node; binary_spine_continues(cur); cur = cur->left) depth++;

  sv_ast_t *inline_nodes[16];
  sv_ast_t **nodes = depth <= 16 ? inline_nodes : malloc((size_t)depth * sizeof(*nodes));
  if (!nodes) {
    Ant_Error_CreateOutOfMemory(c->js);
    emit_op(c, OP_UNDEF);
    return;
  }

  sv_ast_t *cur = node;
  for (int i = 0; i < depth; i++, cur = cur->left) {
    nodes[i] = cur;
    if (i > 0) emit_srcpos(c, cur);
  }

  compile_expr(c, cur);
  for (int i = depth - 1; i >= 0; i--) {
    uint8_t op = nodes[i]->op;
    if (is_logical_op(op)) {
      int jump = emit_jump(c, logical_jump_op(op));
      emit_op(c, OP_POP);
      compile_expr(c, nodes[i]->right);
      patch_jump(c, jump);
      continue;
    }
    compile_expr(c, nodes[i]->right);
    emit_plain_binary_op(c, op);
  }

  if (nodes != inline_nodes) free(nodes);
}

void compile_binary(sv_compiler_t *c, sv_ast_t *node) {
  uint8_t op = node->op;
  if (binary_spine_continues(node)) {
    compile_binary_spine(c, node);
    return;
  }
  if ((op == TOK_SEQ || op == TOK_SNE || op == TOK_EQ || op == TOK_NE) && compile_primitive_type_compare(c, node)) return;

  if (is_logical_op(op)) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, logical_jump_op(op));
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

  if (is_additive_chain_op(op)) {
    compile_arithmetic_chain(c, node);
    return;
  }

  compile_expr(c, node->left);
  compile_expr(c, node->right);
  emit_plain_binary_op(c, op);
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
  if (reject_super_identifier(c, arg)) return;
  if (is_ident_name(arg, "arguments") && reject_forbidden_arguments(c)) return;
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
      if (test_type < 0 && known && !shadowed && c->locals[local].is_const && !c->locals[local].type_derived) {
        emit_constant(c, sv_const_str(c, known, strlen(known)));
        return;
      }
    }
    
    emit_get_var_typeof(c, arg->str, arg->len);
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
  if (!n || (n->flags & FN_PAREN)) return false;
  if (n->type == N_OPTIONAL) return true;
  return (n->type == N_MEMBER || n->type == N_CALL) && (n->flags & FN_OPTIONAL_BASE);
}

static void compile_delete_optional(sv_compiler_t *c, sv_ast_t *arg) {
  sv_patch_list_t skipped = {0};
  compile_chain_base(c, arg->left, &skipped);
  if (arg->type == N_OPTIONAL) {
    int ok_jump = emit_jump(c, OP_JMP_NOT_NULLISH);
    patch_list_add(c, &skipped, emit_jump(c, OP_JMP));
    patch_jump(c, ok_jump);
  }
  if (arg->flags & 1) {
    compile_expr(c, arg->right);
  } else {
    ant_value_t key = sv_const_str(c, arg->right->str, arg->right->len);
    emit_constant(c, key);
  }
  emit_op(c, OP_DELETE);
  int end_jump = emit_jump(c, OP_JMP);
  patch_list_resolve(c, &skipped);
  emit_op(c, OP_POP);
  emit_op(c, OP_TRUE);
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
  } else if (arg->type == N_MEMBER && is_ident_name(arg->left, "super")) {
    static const char message[] = "Unsupported reference to 'super'";
    emit_super(c);
    emit_op(c, OP_POP);
    emit_this(c);
    if (arg->flags & 1) compile_expr(c, arg->right);
    else emit_op(c, OP_UNDEF);
    emit_op(c, OP_POP);
    emit_op(c, OP_POP);
    emit_throw_error(c, JS_ERR_REFERENCE, message, sizeof(message) - 1);
    emit_op(c, OP_TRUE);
  } else if (arg->type == N_MEMBER && !(arg->flags & 1)) {
    compile_expr(c, arg->left);
    ant_value_t key = sv_const_str(c, arg->right->str, arg->right->len);
    emit_constant(c, key);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_MEMBER && (arg->flags & 1)) {
    compile_expr(c, arg->left);
    compile_expr(c, arg->right);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_IDENT && spilled_binding_visible(c, arg->str, arg->len)) {
    emit_op(c, OP_FALSE);
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
    else {
      emit_arguments_eval_env(c, arg->str, arg->len);
      emit_atom_op(c, env_resolution_op(c, OP_DELETE_VAR, OP_DELETE_EVAL_VAR), arg->str, arg->len);
    }
    
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
  sv_patch_list_t *outer = optional_chain_take(c);
  if (is_ident_name(node->left, "super")) {
    if (reject_super_private(c, node)) return;
    emit_super_reference(c, node);
    emit_super_get(c, node);
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

  sv_patch_list_t own = {0};
  if (raw_length_local >= 0)
    emit_slot_op(c, OP_GET_SLOT_RAW, (uint16_t)raw_length_local);
  else compile_chain_base(c, node->left, outer ? outer : &own);

  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else if (is_private_name_node(node->right)) {
    emit_private_get(c, node->right, OP_GET_PRIVATE);
  } else {
    if (direct_length) emit_get_length(c);
    else {
      emit_srcpos(c, node->right);
      emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
    }
  }

  if (own.count) patch_list_resolve(c, &own);
}

void compile_optional_get(sv_compiler_t *c, sv_ast_t *node) {
  if (node->flags & 1) {
    bool constant_key = node->right && (node->right->type == N_NUMBER || node->right->type == N_STRING);
    if (constant_key) {
      compile_expr(c, node->right);
      emit_op(c, OP_GET_ELEM_OPT);
      return;
    }
    
    int present = emit_jump(c, OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
    int done = emit_jump(c, OP_JMP);
    patch_jump(c, present);
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM_OPT);
    patch_jump(c, done);
  } else if (is_private_name_node(node->right)) {
    emit_private_get(c, node->right, OP_GET_PRIVATE_OPT);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD_OPT, node->right->str, node->right->len);
  }
}

void compile_optional(sv_compiler_t *c, sv_ast_t *node) {
  sv_patch_list_t *outer = optional_chain_take(c);
  sv_patch_list_t own = {0};
  compile_chain_base(c, node->left, outer ? outer : &own);
  
  if (!outer) {
    compile_optional_get(c, node);
    if (own.count) patch_list_resolve(c, &own);
    return;
  }

  emit_chain_short_circuit(c, outer);
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else if (is_private_name_node(node->right)) {
    emit_private_get(c, node->right, OP_GET_PRIVATE);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
  }
}
