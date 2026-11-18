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

static sv_ast_t *parse_stmt(sv_parser_t *p);

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
      strict_mode = true;
      p->lx.strict = true;
      continue;
    }

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
  p->module_item = false;
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
    n->left = parse_stmt(p);
    if (!check_if_branch(p, n->left)) return n;
    if (sv_lexer_next(&p->lx) == TOK_ELSE) {
      p->lx.st.consumed = 1;
      n->right = parse_stmt(p);
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
    n->body = parse_stmt(p);
    if (is_body_declaration(n->body))
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
    return n;
  }

  l_do: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_DO_WHILE);
    n->body = parse_stmt(p);
    if (is_body_declaration(n->body))
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
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
    
    if (!init_node && using_declaration_here(p, true)) {
      p->lx.st.consumed = 1;
      p->no_in = true;
      init_node = parse_var_decl(p, SV_VAR_USING, true);
      p->no_in = false;
    } else if (!init_node && (p->lx.st.tok == TOK_VAR || p->lx.st.tok == TOK_LET || p->lx.st.tok == TOK_CONST)) {
      sv_var_kind_t kind = (
        p->lx.st.tok == TOK_VAR) ? SV_VAR_VAR :
        (p->lx.st.tok == TOK_LET) ? SV_VAR_LET : SV_VAR_CONST;
      p->lx.st.consumed = 1;
      p->no_in = true;
      init_node = parse_var_decl(p, kind, true);
      p->no_in = false;
    } else if (!init_node && p->lx.st.tok != TOK_SEMICOLON) {
      p->no_in = true;
      init_node = parse_expr(p);
      p->no_in = false;
    }

    uint8_t la = sv_lexer_next(&p->lx);
    if (la == TOK_IN) {
      p->lx.st.consumed = 1;
      sv_ast_t *n = mk(N_FOR_IN);
      n->left = init_node;
      n->right = parse_expr(p);
      expect(p, TOK_RPAREN);
      n->body = parse_stmt(p);
      if (p->lx.strict && init_node && init_node->type == N_VAR && var_decl_has_initializer(init_node))
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "for-in loop variable declaration may not have an initializer in strict mode");
      if (is_body_declaration(n->body))
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      return n;
    }
    if (la == TOK_OF || (la == TOK_IDENTIFIER && p->lx.st.tlen == 2 &&
        memcmp(tok_str(p), "of", 2) == 0)) {
      p->lx.st.consumed = 1;
      sv_ast_t *n = mk(is_for_await ? N_FOR_AWAIT_OF : N_FOR_OF);
      n->left = init_node;
      n->right = parse_assign(p);
      expect(p, TOK_RPAREN);
      n->body = parse_stmt(p);
      if (is_body_declaration(n->body))
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      return n;
    }

    if (init_node && init_node->type == N_VAR && (
        init_node->var_kind == SV_VAR_CONST ||
        init_node->var_kind == SV_VAR_USING ||
        init_node->var_kind == SV_VAR_AWAIT_USING)) {
      for (int i = 0; i < init_node->args.count; i++) {
        sv_ast_t *decl = init_node->args.items[i];
        if (decl && decl->type == N_VARDECL && !decl->right) {
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Missing initializer in const declaration");
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
    n->body = parse_stmt(p);
    if (is_body_declaration(n->body))
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
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
        expect(p, TOK_RPAREN);
      }
      n->catch_body = parse_block(p, false);
    }
    if (sv_lexer_next(&p->lx) == TOK_FINALLY) {
      p->lx.st.consumed = 1;
      n->finally_body = parse_block(p, false);
    }
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
    n->body = parse_stmt(p);
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
    if ((p->lx.st.tok == TOK_IDENTIFIER || is_contextual_ident_tok(p->lx.st.tok)) && sv_lexer_lookahead(&p->lx) == TOK_COLON) {
        sv_ast_t *label = mk(N_LABEL);
        label->str = tok_ident_str(p, &label->len);
        p->lx.st.consumed = 1;
        sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
        label->body = parse_stmt(p);
      if (is_lexical_decl_stmt(label->body) || is_class_decl_stmt(label->body)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      } else if (is_function_decl_stmt(label->body) &&
          (p->lx.strict || (label->body->flags & (FN_ASYNC | FN_GENERATOR)))) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "In strict mode code, functions can only be declared at top level or inside a block");
      }
      return label;
    }

    sv_ast_t *expr = parse_expr(p);
    consume_semicolon(p);
    return expr;
  }
}
