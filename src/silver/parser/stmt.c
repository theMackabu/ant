#include "parser_internal.h"

static bool is_function_decl_stmt(const sv_ast_t *n) {
  return n && n->type == N_FUNC && !(n->flags & (FN_ARROW | FN_PAREN));
}

static bool is_class_decl_stmt(const sv_ast_t *n) {
  return n && n->type == N_CLASS && !(n->flags & FN_PAREN);
}

static bool is_body_declaration(const sv_ast_t *n) {
  const sv_ast_t *body = n;
  while (body && body->type == N_LABEL) body = body->body;
  return is_lexical_decl_stmt((sv_ast_t *)n) || is_class_decl_stmt(n) || is_function_decl_stmt(body);
}

static bool let_declaration_ahead(sv_parser_t *p) {
  uint8_t la = sv_lexer_lookahead(&p->lx);
  return la == TOK_LBRACKET || la == TOK_LBRACE || is_binding_ident_tok(la);
}

static sv_ast_t *parse_stmt(sv_parser_t *p);

static sv_ast_t *parse_body_stmt(sv_parser_t *p) {
  p->body_stmt = true;
  sv_ast_t *body = parse_stmt(p);
  p->body_stmt = false;
  return body;
}

static void check_loop_body(sv_parser_t *p, const sv_ast_t *body) {
  if (is_body_declaration(body) && !Ant_Exception_Pending(p->js))
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
}

static bool check_if_branch(sv_parser_t *p, const sv_ast_t *branch) {
  const sv_ast_t *body = branch;
  while (body && body->type == N_LABEL) body = body->body;

  if (is_lexical_decl_stmt((sv_ast_t *)branch) || is_class_decl_stmt(branch)) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
    return false;
  }
  if (is_function_decl_stmt(body) &&
      (body != branch || p->lx.strict || (body->flags & (FN_ASYNC | FN_GENERATOR)))) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "In strict mode code, functions can only be declared at top level or inside a block");
    return false;
  }
  return true;
}

static bool prologue_has_legacy_escape(const sv_parser_t *p, const sv_ast_list_t *list) {
  for (int i = 0; i < list->count; i++) {
    const sv_ast_t *stmt = list->items[i];
    if (!stmt || stmt->type != N_STRING || stmt->src_end <= stmt->src_off + 1) continue;
    const char *raw = &p->lx.code[stmt->src_off];
    uint32_t len = stmt->src_end - stmt->src_off;
    for (uint32_t k = 1; k + 1 < len; k++) {
      if (raw[k] != '\\') continue;
      char next = raw[++k];
      if (next >= '1' && next <= '9') return true;
      if (next == '0' && k + 1 < len && raw[k + 1] >= '0' && raw[k + 1] <= '9') return true;
    }
  }
  return false;
}

void sv_parse_stmt_list(sv_parser_t *p, sv_ast_list_t *out, bool stop_at_rbrace, bool directive_ctx, bool module_items) {
  bool strict_mode = p->lx.strict;
  bool saved_lexer_strict = p->lx.strict;
  bool in_directive_prologue = directive_ctx;
  p->lx.strict = strict_mode;

  for (;;) {
    sv_lexer_next(&p->lx);

    if (p->lx.st.tok == TOK_EOF) break;
    if (stop_at_rbrace && p->lx.st.tok == TOK_RBRACE) break;

    if (p->lx.st.tok == TOK_ERR) {
      sv_parse_unexpected_token(p);
      break;
    }

    p->module_item = module_items;
    sv_ast_t *stmt = parse_stmt(p);
    if (stmt) sv_ast_list_push(p->js, out, stmt);
    if (Ant_Exception_Pending(p->js)) break;
    if (!in_directive_prologue) continue;
    if (!stmt || stmt->type == N_EMPTY) continue;
    
    if (!sv_ast_can_be_expression_statement(stmt)) {
      in_directive_prologue = false;
      continue;
    }
    
    if (sv_ast_is_use_strict(p->js, stmt)) {
      if (!strict_mode && prologue_has_legacy_escape(p, out)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Octal escape sequences are not allowed in strict mode");
        break;
      }
      strict_mode = true;
      p->lx.strict = true;
      continue;
    }

    if (stmt->type == N_STRING && !(stmt->flags & FN_PAREN)) continue;
    in_directive_prologue = false;
  }

  p->lx.strict = saved_lexer_strict;
}

sv_ast_t *parse_binding_pattern(sv_parser_t *p) {
  sv_lexer_next(&p->lx);
  if (p->lx.st.tok == TOK_LBRACKET || p->lx.st.tok == TOK_LBRACE) {
    sv_ast_t *pattern = p->lx.st.tok == TOK_LBRACKET ? parse_with_in(p, parse_array) : parse_with_in(p, parse_object);
    sv_reject_binding_pattern(p, pattern);
    return pattern;
  }
  if (binding_ident_here(p)) {
    sv_ast_t *id = mk_ident_from_tok(p);
    sv_reject_binding_name(p, id->str, id->len);
    p->lx.st.consumed = 1;
    return id;
  }
  sv_parse_unexpected_token(p);
  return mk(N_EMPTY);
}

sv_ast_t *parse_block(sv_parser_t *p, bool directive_ctx) {
  expect(p, TOK_LBRACE);
  sv_ast_t *block = mk(N_BLOCK);
  if (directive_ctx) p->function_depth++;
  sv_parse_stmt_list(p, &block->args, true, directive_ctx, false);
  if (directive_ctx) p->function_depth--;
  if (Ant_Exception_Pending(p->js)) return block;
  expect(p, TOK_RBRACE);
  block->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  return block;
}

sv_ast_t *parse_var_decl(sv_parser_t *p, sv_var_kind_t kind, bool allow_uninit_const) {
  sv_ast_t *var = mk(N_VAR);
  var->var_kind = kind;

  do {
    sv_lexer_next(&p->lx);
    sv_ast_t *decl = mk(N_VARDECL);

    if (p->lx.st.tok == TOK_LBRACKET || p->lx.st.tok == TOK_LBRACE) {
      decl->left = p->lx.st.tok == TOK_LBRACKET ? parse_with_in(p, parse_array) : parse_with_in(p, parse_object);
      if (sv_reject_binding_pattern(p, decl->left)) return var;
    } else if (!binding_ident_here(p)) {
      sv_parse_unexpected_token(p);
      return var;
    } else {
      decl->left = mk_ident_from_tok(p);
      if (sv_reject_binding_name(p, decl->left->str, decl->left->len)) return var;
      p->lx.st.consumed = 1;
    }

    if (kind != SV_VAR_VAR && ast_pattern_binds(decl->left, "let")) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "let is disallowed as a lexically bound name");
      return var;
    }

    if (sv_lexer_next(&p->lx) == TOK_ASSIGN) {
      p->lx.st.consumed = 1;
      decl->right = parse_assign(p);
    } else if ((kind == SV_VAR_CONST || kind == SV_VAR_USING || kind == SV_VAR_AWAIT_USING) && !allow_uninit_const) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing initializer in const declaration");
    } else if (decl->left->type != N_IDENT && !allow_uninit_const) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing initializer in destructuring declaration");
    }

    sv_ast_list_push(p->js, &var->args, decl);
  } while (sv_lexer_next(&p->lx) == TOK_COMMA && (p->lx.st.consumed = 1, 1));

  return var;
}

static sv_ast_t *parse_stmt(sv_parser_t *p) {
  if (sv_parse_stack_overflow(p)) return mk(N_EMPTY);
  bool module_item = p->module_item;
  bool body_stmt = p->body_stmt;
  p->module_item = false;
  p->body_stmt = false;
  sv_lexer_next(&p->lx);

  static const void *dispatch[TOK_MAX] = {
    [TOK_SEMICOLON] = &&l_semi,
    [TOK_LBRACE]    = &&l_block,
    [TOK_VAR]       = &&l_var,
    [TOK_LET]       = &&l_let,
    [TOK_CONST]     = &&l_const,
    [TOK_IF]        = &&l_if,
    [TOK_WHILE]     = &&l_while,
    [TOK_DO]        = &&l_do,
    [TOK_FOR]       = &&l_for,
    [TOK_RETURN]    = &&l_return,
    [TOK_THROW]     = &&l_throw,
    [TOK_BREAK]     = &&l_break,
    [TOK_CONTINUE]  = &&l_continue,
    [TOK_TRY]       = &&l_try,
    [TOK_SWITCH]    = &&l_switch,
    [TOK_DEBUGGER]  = &&l_debugger,
    [TOK_WITH]      = &&l_with,
    [TOK_FUNC]      = &&l_func,
    [TOK_CLASS]     = &&l_class,
    [TOK_ASYNC]     = &&l_async,
    [TOK_EXPORT]    = &&l_export,
    [TOK_IMPORT]    = &&l_import,
  };

  if (using_declaration_here(p, false)) {
    if (module_item && p->goal == SV_PARSE_SCRIPT && !p->function_depth) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Using declarations are not allowed at the top level of a script");
      return mk(N_EMPTY);
    }
    p->lx.st.consumed = 1;
    sv_ast_t *n = parse_var_decl(p, SV_VAR_USING, false);
    consume_semicolon(p);
    return n;
  }
  
  if (p->lx.st.tok == TOK_AWAIT) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    p->lx.st.consumed = 1;
    sv_lexer_next(&p->lx);
    if (!p->lx.st.had_newline && using_declaration_here(p, false)) {
      p->lx.st.consumed = 1;
      sv_ast_t *n = parse_var_decl(p, SV_VAR_AWAIT_USING, false);
      consume_semicolon(p);
      return n;
    }
    sv_lexer_restore_state(&p->lx, &saved);
  }

  if (p->lx.st.tok < TOK_MAX && dispatch[p->lx.st.tok])
    goto *dispatch[p->lx.st.tok];
  goto l_expr_stmt;

  l_semi:  { p->lx.st.consumed = 1; return mk(N_EMPTY); }
  l_block: return parse_block(p, false);

  l_var: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = parse_var_decl(p, SV_VAR_VAR, false);
    consume_semicolon(p);
    return n;
  }
  l_let: {
    if (body_stmt) {
      uint8_t la = sv_lexer_lookahead(&p->lx);
      bool same_line = !lookahead_crosses_line_terminator(p);
      if (la == TOK_LBRACKET || ((la == TOK_LBRACE || la == TOK_IDENTIFIER) && same_line)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
        return mk(N_EMPTY);
      }
      goto l_expr_stmt;
    }
    if (!let_declaration_ahead(p)) goto l_expr_stmt;
    p->lx.st.consumed = 1;
    sv_ast_t *n = parse_var_decl(p, SV_VAR_LET, false);
    consume_semicolon(p);
    return n;
  }
  l_const: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = parse_var_decl(p, SV_VAR_CONST, false);
    consume_semicolon(p);
    return n;
  }

  l_if: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_IF);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->left = parse_body_stmt(p);
    if (!check_if_branch(p, n->left)) return n;
    if (sv_lexer_next(&p->lx) == TOK_ELSE) {
      p->lx.st.consumed = 1;
      n->right = parse_body_stmt(p);
      if (!check_if_branch(p, n->right)) return n;
    }
    return n;
  }

  l_while: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_WHILE);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_body_stmt(p);
    check_loop_body(p, n->body);
    return n;
  }

  l_do: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_DO_WHILE);
    n->body = parse_body_stmt(p);
    check_loop_body(p, n->body);
    expect(p, TOK_WHILE);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    if (sv_lexer_next(&p->lx) == TOK_SEMICOLON) p->lx.st.consumed = 1;
    return n;
  }

  l_for: {
    p->lx.st.consumed = 1;
    bool is_for_await = false;
    if (sv_lexer_next(&p->lx) == TOK_AWAIT) {
      p->lx.st.consumed = 1;
      is_for_await = true;
    }
    expect(p, TOK_LPAREN);
    sv_ast_t *init_node = NULL;

    sv_lexer_next(&p->lx);
    if (p->lx.st.tok == TOK_AWAIT) {
      sv_lexer_state_t saved;
      sv_lexer_save_state(&p->lx, &saved);
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
      if (!p->lx.st.had_newline && using_declaration_here(p, true)) {
        p->lx.st.consumed = 1;
        p->no_in = true;
        init_node = parse_var_decl(p, SV_VAR_AWAIT_USING, true);
        p->no_in = false;
      } else sv_lexer_restore_state(&p->lx, &saved);
    }
    
    bool let_expr = false;
    if (!init_node && using_declaration_here(p, true)) {
      p->lx.st.consumed = 1;
      p->no_in = true;
      init_node = parse_var_decl(p, SV_VAR_USING, true);
      p->no_in = false;
    } else if (!init_node && (p->lx.st.tok == TOK_VAR || p->lx.st.tok == TOK_CONST || (p->lx.st.tok == TOK_LET && let_declaration_ahead(p)))) {
      sv_var_kind_t kind = (
        p->lx.st.tok == TOK_VAR) ? SV_VAR_VAR :
        (p->lx.st.tok == TOK_LET) ? SV_VAR_LET : SV_VAR_CONST;
      p->lx.st.consumed = 1;
      p->no_in = true;
      init_node = parse_var_decl(p, kind, true);
      p->no_in = false;
    } else if (!init_node && p->lx.st.tok != TOK_SEMICOLON) {
      let_expr = p->lx.st.tok == TOK_LET;
      bool async_of = p->lx.st.tok == TOK_ASYNC && !is_for_await && sv_lexer_lookahead(&p->lx) == TOK_OF;
      p->no_in = true;
      init_node = parse_expr(p);
      p->no_in = false;
      if (async_of && init_node->type == N_IDENT && !(init_node->flags & FN_PAREN) && sv_lexer_next(&p->lx) == TOK_OF) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "The left-hand side of a for-of loop may not be 'async'");
        return mk(N_EMPTY);
      }
    }
    uint8_t la = sv_lexer_next(&p->lx);
    bool is_of = la == TOK_OF;
    if (la == TOK_IN || is_of) {
      if (is_for_await && !is_of) {
        sv_parse_unexpected_token(p);
        return mk(N_EMPTY);
      }
      if (init_node && init_node->type == N_VAR) {
        sv_ast_t *decl = init_node->args.count == 1 ? init_node->args.items[0] : NULL;
        bool legacy_init = !is_of && !p->lx.strict && init_node->var_kind == SV_VAR_VAR && decl && decl->left && decl->left->type == N_IDENT;
        bool using_in = !is_of && (init_node->var_kind == SV_VAR_USING || init_node->var_kind == SV_VAR_AWAIT_USING);
        if (!decl || using_in || (decl->right && !legacy_init)) {
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Invalid left-hand side in for-loop");
          return mk(N_EMPTY);
        }
      } else if (!init_node || (is_of && let_expr) || sv_reject_assign_target(p, init_node, SV_TARGET_FOR)) {
        if (!Ant_Exception_Pending(p->js)) SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Invalid left-hand side in for-loop");
        return mk(N_EMPTY);
      }
      p->lx.st.consumed = 1;
      sv_ast_t *n = mk(is_of ? (is_for_await ? N_FOR_AWAIT_OF : N_FOR_OF) : N_FOR_IN);
      n->left = init_node;
      n->right = is_of ? parse_assign(p) : parse_expr(p);
      expect(p, TOK_RPAREN);
      n->body = parse_body_stmt(p);
      check_loop_body(p, n->body);
      return n;
    }
    if (is_for_await) {
      sv_parse_unexpected_token(p);
      return mk(N_EMPTY);
    }

    if (init_node && init_node->type == N_VAR) {
      bool needs_init =
        init_node->var_kind == SV_VAR_CONST ||
        init_node->var_kind == SV_VAR_USING ||
        init_node->var_kind == SV_VAR_AWAIT_USING;
      for (int i = 0; i < init_node->args.count; i++) {
        sv_ast_t *decl = init_node->args.items[i];
        if (!decl || decl->type != N_VARDECL || decl->right) continue;
        if (needs_init) {
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing initializer in const declaration");
          return mk(N_EMPTY);
        }
        if (decl->left && decl->left->type != N_IDENT) {
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing initializer in destructuring declaration");
          return mk(N_EMPTY);
        }
      }
    }

    sv_ast_t *n = mk(N_FOR);
    n->init = init_node;
    expect(p, TOK_SEMICOLON);
    if (sv_lexer_next(&p->lx) != TOK_SEMICOLON)
      n->cond = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    if (sv_lexer_next(&p->lx) != TOK_RPAREN)
      n->update = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_body_stmt(p);
    check_loop_body(p, n->body);
    return n;
  }

  l_return: {
    if (!p->function_depth) SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Illegal return statement");
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_RETURN);
    if (sv_lexer_next(&p->lx) != TOK_SEMICOLON && p->lx.st.tok != TOK_RBRACE && p->lx.st.tok != TOK_EOF &&
        !p->lx.st.had_newline)
      n->right = parse_expr(p);
    consume_semicolon(p);
    return n;
  }

  l_throw: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_THROW);
    n->right = parse_expr(p);
    consume_semicolon(p);
    return n;
  }

  l_break: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_BREAK);
    if (sv_lexer_next(&p->lx) != TOK_EOF && ident_tok_here(p) && !p->lx.st.had_newline) {
      n->str = tok_ident_str(p, &n->len);
      p->lx.st.consumed = 1;
    }
    consume_semicolon(p);
    return n;
  }

  l_continue: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_CONTINUE);
    if (sv_lexer_next(&p->lx) != TOK_EOF && ident_tok_here(p) && !p->lx.st.had_newline) {
      n->str = tok_ident_str(p, &n->len);
      p->lx.st.consumed = 1;
    }
    consume_semicolon(p);
    return n;
  }

  l_try: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_TRY);
    n->body = parse_block(p, false);
    if (sv_lexer_next(&p->lx) == TOK_CATCH) {
      p->lx.st.consumed = 1;
      if (sv_lexer_next(&p->lx) == TOK_LPAREN) {
        p->lx.st.consumed = 1;
        n->catch_param = parse_binding_pattern(p);
        if (n->catch_param->type != N_IDENT && ast_pattern_binds(n->catch_param, "let") && !Ant_Exception_Pending(p->js))
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "let is disallowed as a lexically bound name");
        expect(p, TOK_RPAREN);
      }
      n->catch_body = parse_block(p, false);
    }
    if (sv_lexer_next(&p->lx) == TOK_FINALLY) {
      p->lx.st.consumed = 1;
      n->finally_body = parse_block(p, false);
    }
    if (!n->catch_body && !n->finally_body && !Ant_Exception_Pending(p->js))
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing catch or finally after try");
    return n;
  }

  l_switch: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_SWITCH);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);
    while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
      sv_ast_t *c = mk(N_CASE);
      if (p->lx.st.tok == TOK_CASE) {
        p->lx.st.consumed = 1;
        c->left = parse_expr(p);
      } else if (p->lx.st.tok == TOK_DEFAULT) {
        p->lx.st.consumed = 1;
      }
      expect(p, TOK_COLON);
      while (sv_lexer_next(&p->lx) != TOK_CASE && p->lx.st.tok != TOK_DEFAULT &&
             p->lx.st.tok != TOK_RBRACE && p->lx.st.tok != TOK_EOF)
        sv_ast_list_push(p->js, &c->args, parse_stmt(p));
      sv_ast_list_push(p->js, &n->args, c);
    }
    expect(p, TOK_RBRACE);
    return n;
  }

  l_debugger: { p->lx.st.consumed = 1; consume_semicolon(p); return mk(N_DEBUGGER); }

  l_with: {
    if (p->lx.strict) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "with statement not allowed in strict mode");
      return mk(N_EMPTY);
    }
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_WITH);
    expect(p, TOK_LPAREN);
    n->left = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_body_stmt(p);
    check_loop_body(p, n->body);
    return n;
  }

  l_func: {
    p->lx.st.consumed = 1;
    sv_ast_t *fn = parse_func_decl(p, false);
    if (!fn->str && !Ant_Exception_Pending(p->js)) SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Function statements require a function name");
    return fn;
  }

  l_class: {
    uint32_t class_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    sv_ast_t *cls = parse_class(p);
    cls->src_off = class_off;
    cls->flags |= FN_CLASS_DECL;
    if (!cls->str && !Ant_Exception_Pending(p->js)) SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Class statements require a class name");
    return cls;
  }

  l_async: {
    uint8_t la = sv_lexer_lookahead(&p->lx);
    uint32_t async_off = (uint32_t)p->lx.st.toff;
    if (la == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
      sv_ast_t *fn = parse_func_decl(p, true);
      fn->flags |= FN_ASYNC;
      fn->src_off = async_off;
      if (!fn->str && !Ant_Exception_Pending(p->js)) SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Function statements require a function name");
      return fn;
    }
    goto l_expr_stmt;
  }

  l_import: {
    uint8_t la = sv_lexer_lookahead(&p->lx);
    if (la == TOK_LPAREN || la == TOK_DOT)
      goto l_expr_stmt;
    if (!module_item) {
      sv_parse_unexpected_token(p);
      return mk(N_EMPTY);
    }
    if (p->goal == SV_PARSE_DETECT) goto l_wants_module;
    p->lx.st.consumed = 1;
    return parse_import_stmt(p);
  }

  l_export: {
    if (!module_item) {
      sv_parse_unexpected_token(p);
      return mk(N_EMPTY);
    }
    if (p->goal == SV_PARSE_DETECT) goto l_wants_module;
    p->lx.st.consumed = 1;
    return parse_export_stmt(p);
  }

  l_wants_module: {
    p->wants_module = true;
    sv_parse_stop(p);
    return mk(N_EMPTY);
  }

  l_expr_stmt: {
    bool adjacent_colon = p->lx.st.tok == TOK_IDENTIFIER && p->lx.st.pos < p->lx.clen && p->lx.code[p->lx.st.pos] == ':';
    sv_ast_t *expr = adjacent_colon ? mk_ident_from_tok(p) : parse_expr(p);
    if (adjacent_colon) {
      if (sv_reject_ident_name(p, expr->str, expr->len)) return expr;
      p->lx.st.consumed = 1;
    }
    bool named = expr->type == N_IDENT || ((expr->type == N_UNDEF || expr->type == N_GLOBAL_THIS) && expr->str);
    if (named && sv_lexer_next(&p->lx) == TOK_COLON && !(expr->flags & FN_PAREN)) {
      sv_ast_t *label = expr;
      label->type = N_LABEL;
      label->src_end = 0;
      p->lx.st.consumed = 1;
      label->body = parse_body_stmt(p);
      if (is_lexical_decl_stmt(label->body) || is_class_decl_stmt(label->body)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      } else if (is_function_decl_stmt(label->body) &&
          (p->lx.strict || (label->body->flags & (FN_ASYNC | FN_GENERATOR)))) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "In strict mode code, functions can only be declared at top level or inside a block");
      }
      return label;
    }

    consume_semicolon(p);
    return expr;
  }
}
