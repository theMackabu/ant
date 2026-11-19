#include "parser_internal.h"

bool lookahead_crosses_line_terminator(sv_parser_t *p) {
  return sv_lexer_lookahead_newline(&p->lx);
}

void sv_parse_stop(sv_parser_t *p) {
  p->lx.st.tok = TOK_EOF;
  p->lx.st.consumed = 0;
  p->lx.st.toff = p->lx.st.pos = p->lx.clen;
  p->lx.st.tlen = 0;
}

bool sv_reject_ident_name_slow(sv_parser_t *p, const char *s, uint32_t len) {
  if (p->class_initializer && len == 9 && memcmp(s, "arguments", 9) == 0) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "'arguments' is not allowed in class field initializer or static initialization block");
    return true;
  }
  if (len == 5 && memcmp(s, "await", 5) == 0 && ((p->static_block && !p->in_async) || sv_await_reserved(p))) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unexpected reserved word");
    return true;
  }
  if ((p->lx.strict && is_strict_reserved_name(s, len)) || (p->in_generator && len == 5 && memcmp(s, "yield", 5) == 0)) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unexpected strict mode reserved word");
    return true;
  }
  return false;
}

static bool pattern_name_ok(sv_parser_t *p, const sv_ast_t *node) {
  if (node->type == N_UNDEF || node->type == N_GLOBAL_THIS) return node->str != NULL;
  if (node->type != N_IDENT || !node->str || node->str[0] == '#') return false;
  return !(p->lx.strict && is_eval_or_arguments_name(node->str, node->len));
}

static bool target_chain_ok(const sv_ast_t *node) {
  for (const sv_ast_t *cur = node->left; cur; cur = cur->left) {
    if (cur->type == N_OPTIONAL) return false;
    if (cur->flags & FN_PAREN) return true;
    if (cur->type != N_MEMBER && cur->type != N_CALL) return true;
  }
  return true;
}

static bool simple_target_ok(sv_parser_t *p, const sv_ast_t *node, bool allow_call) {
  switch (node->type) {
    case N_IDENT:
    case N_UNDEF:
    case N_GLOBAL_THIS:
      return pattern_name_ok(p, node);
    case N_MEMBER:
      if (ast_is_ident(node->left, "import", 6) && !(node->left->flags & FN_PAREN)) return false;
      return target_chain_ok(node);
    case N_CALL:
      return allow_call && !ast_is_ident(node->left, "super", 5) && target_chain_ok(node);
    default:
      return false;
  }
}

static bool pattern_ok(sv_parser_t *p, const sv_ast_t *node, bool binding, bool top);

static bool pattern_element_ok(sv_parser_t *p, const sv_ast_t *node, bool binding) {
  if (!node) return false;
  if (node->type == N_ASSIGN && node->op == TOK_ASSIGN && !(node->flags & FN_PAREN)) node = node->left;
  else if (node->type == N_ASSIGN_PAT) node = node->left;
  if (!node) return false;
  if (node->type == N_ARRAY || node->type == N_OBJECT || node->type == N_ARRAY_PAT || node->type == N_OBJECT_PAT)
    return !(node->flags & FN_PAREN) && pattern_ok(p, node, binding, false);
  if (binding) return !(node->flags & FN_PAREN) && pattern_name_ok(p, node);
  return simple_target_ok(p, node, false);
}

static bool pattern_ok(sv_parser_t *p, const sv_ast_t *node, bool binding, bool top) {
  if (node->flags & FN_REST_COMMA) return false;
  bool is_array = node->type == N_ARRAY || node->type == N_ARRAY_PAT;

  for (int i = 0; i < node->args.count; i++) {
    const sv_ast_t *item = node->args.items[i];
    if (!item) return false;
    if (item->type == N_EMPTY && is_array) continue;

    if (item->type == N_SPREAD || item->type == N_REST) {
      const sv_ast_t *rest = item->right;
      if (i != node->args.count - 1 || !rest) return false;
      if (is_array) {
        if (rest->type == N_ASSIGN || rest->type == N_ASSIGN_PAT || !pattern_element_ok(p, rest, binding)) return false;
      } else if (binding ? ((rest->flags & FN_PAREN) || !pattern_name_ok(p, rest)) : !simple_target_ok(p, rest, false)) return false;
      continue;
    }

    if (is_array) {
      if (!pattern_element_ok(p, item, binding)) return false;
      continue;
    }

    if (item->type != N_PROPERTY || (item->flags & (FN_GETTER | FN_SETTER | FN_GENERATOR | FN_ASYNC))) return false;
    if (!(item->flags & FN_COLON) && item->right && item->right->type == N_FUNC) return false;
    if (!pattern_element_ok(p, item->right, binding)) return false;
  }

  return true;
}

bool sv_binding_pattern_ok(sv_parser_t *p, const sv_ast_t *node, bool top) {
  if (!node) return false;
  if (node->type == N_ASSIGN_PAT) node = node->left;
  else if (node->type == N_ASSIGN && node->op == TOK_ASSIGN && (top || !(node->flags & FN_PAREN))) node = node->left;
  if (!node) return false;
  if (node->type == N_REST || node->type == N_SPREAD) {
    node = node->right;
    if (!node || node->type == N_ASSIGN || node->type == N_ASSIGN_PAT || (node->flags & FN_PAREN)) return false;
    top = false;
  }
  if (node->type == N_ARRAY || node->type == N_OBJECT || node->type == N_ARRAY_PAT || node->type == N_OBJECT_PAT)
    return (top || !(node->flags & FN_PAREN)) && pattern_ok(p, node, true, false);
  return (top || !(node->flags & FN_PAREN)) && pattern_name_ok(p, node);
}

bool sv_assign_target_ok(sv_parser_t *p, const sv_ast_t *node, sv_target_kind_t kind) {
  if (!node) return false;
  bool pattern = node->type == N_ARRAY || node->type == N_OBJECT || node->type == N_ARRAY_PAT || node->type == N_OBJECT_PAT;
  if (pattern) return (kind == SV_TARGET_ASSIGN || kind == SV_TARGET_FOR) && !(node->flags & FN_PAREN) && pattern_ok(p, node, false, true);
  return simple_target_ok(p, node, kind != SV_TARGET_LOGICAL);
}

bool sv_reject_binding_pattern(sv_parser_t *p, const sv_ast_t *node) {
  if (Ant_Exception_Pending(p->js)) return true;
  if (sv_binding_pattern_ok(p, node, false)) return false;
  bool strict_name = p->lx.strict && node && ast_pattern_binds_eval_or_arguments(node);
  SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, strict_name ? "Unexpected eval or arguments in strict mode" : "Invalid destructuring assignment target");
  return true;
}

bool sv_reject_assign_target_slow(sv_parser_t *p, const sv_ast_t *node, sv_target_kind_t kind) {
  if (Ant_Exception_Pending(p->js)) return true;
  if (sv_assign_target_ok(p, node, kind)) return false;
  bool pattern = node && (node->type == N_ARRAY || node->type == N_OBJECT) && !(node->flags & FN_PAREN);
  bool strict_name = p->lx.strict && node && (pattern || node->type == N_IDENT) && ast_pattern_binds_eval_or_arguments(node);
  SV_MKERR_TYPED(
    p->js, JS_ERR_SYNTAX,
    strict_name ? "Unexpected eval or arguments in strict mode" :
    pattern && (kind == SV_TARGET_ASSIGN || kind == SV_TARGET_FOR) ? "Invalid destructuring assignment target" :
    kind == SV_TARGET_UPDATE ? "Invalid left-hand side expression in update operation" :
    kind == SV_TARGET_FOR ? "Invalid left-hand side in for-loop" : "Invalid left-hand side in assignment"
  );
  return true;
}

static bool program_has_module_syntax(const sv_ast_t *program) {
  if (!program || program->type != N_PROGRAM) return false;

  for (int i = 0; i < program->args.count; i++) {
    const sv_ast_t *stmt = program->args.items[i];
    if (!stmt) continue;
    if (stmt->type == N_IMPORT_DECL || stmt->type == N_EXPORT) return true;
  }

  return false;
}

bool using_declaration_here(sv_parser_t *p, bool for_head) {
  if (p->lx.st.tok != TOK_USING) return false;
  
  sv_lexer_state_t saved;
  sv_lexer_save_state(&p->lx, &saved);
  p->lx.st.consumed = 1;
  uint8_t next = sv_lexer_next(&p->lx);
  
  bool declares = !p->lx.st.had_newline && is_binding_ident_tok(next) && !(for_head && next == TOK_OF);
  sv_lexer_restore_state(&p->lx, &saved);
  
  return declares;
}

const char *bigint_key_decimal(ant_t *js, const char *lit, uint32_t len, uint32_t *out_len) {
  uint32_t radix = 10, i = 0;
  if (len > 2 && lit[0] == '0') {
    char tag = (char)(lit[1] | 0x20);
    radix = tag == 'x' ? 16 : tag == 'o' ? 8 : tag == 'b' ? 2 : 10;
    if (radix != 10) i = 2;
  }
  
  uint32_t limbs[len + 1];
  uint32_t used = 1;
  limbs[0] = 0;
  
  for (; i < len; i++) {
    char ch = lit[i];
    if (ch == '_' || ch == 'n') continue;
    uint32_t digit = ch <= '9' ? (uint32_t)(ch - '0') : (uint32_t)((ch | 0x20) - 'a' + 10);
    
    uint64_t carry = digit;
    for (uint32_t l = 0; l < used; l++) {
      uint64_t v = (uint64_t)limbs[l] * radix + carry;
      limbs[l] = (uint32_t)(v % 1000000000u);
      carry = v / 1000000000u;
    }
    
    if (carry) limbs[used++] = (uint32_t)carry;
  }
  
  char *out = parse_arena_bump(js, (size_t)used * 9 + 1);
  if (!out) return NULL;
  
  int n = snprintf(out, 10, "%u", limbs[used - 1]);
  for (uint32_t l = used - 1; l-- > 0;) n += snprintf(out + n, 10, "%09u", limbs[l]);
  
  *out_len = (uint32_t)n;
  return out;
}

void consume_semicolon(sv_parser_t *p) {
  if (sv_lexer_next(&p->lx) == TOK_SEMICOLON) p->lx.st.consumed = 1;
  else if (p->lx.st.tok != TOK_RBRACE && p->lx.st.tok != TOK_EOF && !p->lx.st.had_newline) sv_parse_unexpected_token(p);
}

static sv_ast_t *parse_program(
  ant_t *js, const char *code, ant_offset_t clen,
  sv_parse_goal_t goal, bool strict, bool *wants_module, bool function_body
) {
  sv_parser_t parser = {
    .js = js, .goal = goal,
    .in_async = goal == SV_PARSE_MODULE || goal == SV_PARSE_REPL,
    .function_depth = function_body ? 1 : 0,
  };
  
  sv_parser_t *p = &parser;
  sv_lexer_init(&p->lx, js, code, clen, strict || goal == SV_PARSE_MODULE);

  sv_ast_t *program = mk(N_PROGRAM);
  sv_parse_stmt_list(p, &program->args, false, true, true);
  
  if (sv_debug_unlikely(SV_DEBUG_PARSE)) fprintf(
    stderr, "[parse] after-stmt-list thrown=%d strict=%d body=%d\n",
    Ant_Exception_Pending(js) ? 1 : 0,
    p->lx.strict ? 1 : 0,
    program ? program->args.count : -1
  );

  if (wants_module) *wants_module = p->wants_module;
  if (Ant_Exception_Pending(js) || p->wants_module) return NULL;
  
  if (p->lx.strict) program->flags |= FN_PARSE_STRICT;
  if (program_has_module_syntax(program)) program->flags |= FN_MODULE_SYNTAX;

  program->flags |= scope_facts_eval_yield(&program->args, NULL);  
  return program;
}

static sv_ast_t *parse_detect(ant_t *js, const char *code, ant_offset_t clen, bool strict) {
  code_arena_mark_t mark = parse_arena_mark(js);
  bool wants_module = false;
  
  sv_ast_t *program = parse_program(js, code, clen, SV_PARSE_DETECT, strict, &wants_module, false);
  if (program) return program;

  parse_arena_rewind(js, mark);
  Ant_Exception_Clear(js);
  
  program = parse_program(js, code, clen, SV_PARSE_MODULE, strict, NULL, false);
  if (program) {
    program->flags |= FN_MODULE_SYNTAX;
    return program;
  }
  
  if (wants_module) return NULL;
  parse_arena_rewind(js, mark);
  Ant_Exception_Clear(js);
  
  return parse_program(js, code, clen, SV_PARSE_SCRIPT, strict, NULL, false);
}

sv_ast_t *sv_parse(ant_t *js, const char *code, ant_offset_t clen, sv_parse_goal_t goal, bool strict) {
  if (sv_debug_unlikely(SV_DEBUG_PARSE)) fprintf(
    stderr, "[parse] start len=%u goal=%d strict=%d\n",
    (unsigned)clen, (int)goal, strict ? 1 : 0
  );

  sv_ast_t *program = goal == SV_PARSE_DETECT
    ? parse_detect(js, code, clen, strict)
    : parse_program(js, code, clen, goal, strict, NULL, false);

  if (sv_debug_unlikely(SV_DEBUG_PARSE)) fprintf(stderr,
    "[parse] return %s body=%d\n", program ? "program" : "null",
    program ? program->args.count : -1
  );
  return program;
}

sv_ast_t *sv_parse_expression(ant_t *js, const char *code, ant_offset_t clen) {
  sv_parser_t parser = { .js = js, .goal = SV_PARSE_SCRIPT };
  sv_parser_t *p = &parser;
  sv_lexer_init(&p->lx, js, code, clen, false);

  sv_ast_t *program = mk(N_PROGRAM);
  sv_ast_t *expr = parse_expr(p);
  if (!expr || Ant_Exception_Pending(js) || sv_lexer_next(&p->lx) != TOK_EOF) return NULL;

  sv_ast_list_push(js, &program->args, expr);
  program->flags |= scope_facts_eval_yield(&program->args, NULL);
  return program;
}

sv_ast_t *sv_parse_function_body(ant_t *js, const char *code, ant_offset_t clen, bool strict) {
  return parse_program(js, code, clen, SV_PARSE_SCRIPT, strict, NULL, true);
}

sv_ast_t *sv_parse_function_parts(ant_t *js, const sv_function_parts_t *parts) {
  sv_parser_t parser = {
    .js = js, .goal = SV_PARSE_SCRIPT,
    .in_async = parts->is_async, .in_generator = parts->is_generator,
    .no_yield = parts->is_generator, .function_depth = 1,
  };
  sv_parser_t *p = &parser;

  sv_ast_t *fn = mk_plain(N_FUNC);
  fn->flags = (parts->is_async ? FN_ASYNC : 0) | (parts->is_generator ? FN_GENERATOR : 0);
  fn->src_off = 0;
  fn->src_end = (uint32_t)parts->len;

  sv_lexer_init(&p->lx, js, parts->text, parts->params_end, false);
  p->lx.st.pos = parts->params_off;
  parse_formal_params(p, fn, TOK_EOF);
  if (sv_lexer_next(&p->lx) != TOK_EOF) sv_parse_unexpected_token(p);
  if (Ant_Exception_Pending(js)) return NULL;

  sv_lexer_init(&p->lx, js, parts->text, parts->body_end, false);
  p->lx.st.pos = parts->body_off;
  p->no_yield = false;
  fn->body = mk_plain(N_BLOCK);
  fn->body->src_off = (uint32_t)parts->body_off;
  fn->body->src_end = (uint32_t)parts->body_end;
  sv_parse_stmt_list(p, &fn->body->args, false, true, false);
  if (Ant_Exception_Pending(js)) return NULL;

  finish_func(fn);
  return fn;
}
