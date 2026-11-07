#include "parser_internal.h"

sv_ast_t *parse_member_suffix(sv_parser_t *p, sv_ast_t *left, uint8_t la) {
  if (la == TOK_DOT) {
    p->lx.st.consumed = 1;
    sv_lexer_next(&p->lx);
    sv_ast_t *mem = mk(N_MEMBER);
    mem->left = left;
    if (p->lx.st.tok == TOK_HASH) {
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
      if (!is_private_ident_like_tok(p->lx.st.tok)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "private field name expected");
        return mk(N_EMPTY);
      }
      mem->right = mk_private_ident_from_tok(p);
      p->lx.st.consumed = 1;
    } else {
      mem->right = parse_dot_property_name(p);
      if (!mem->right) return mk(N_EMPTY);
    }
    mem->src_off = left ? left->src_off : mem->src_off;
    mem->src_end = mem->right ? mem->right->src_end : (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return mem;
  }

  if (la == TOK_LBRACKET) {
    p->lx.st.consumed = 1;
    sv_ast_t *mem = mk(N_MEMBER);
    mem->left = left;
    mem->right = parse_with_in(p, parse_expr);
    mem->flags = 1;
    expect(p, TOK_RBRACKET);
    mem->src_off = left ? left->src_off : mem->src_off;
    mem->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return mem;
  }

  return NULL;
}

static sv_ast_t *parse_call(sv_parser_t *p) {
  sv_ast_t *n = parse_primary(p);

  for (;;) {
    uint8_t la = sv_lexer_next(&p->lx);
    if (la == TOK_LPAREN) {
      p->lx.st.consumed = 1;
      sv_ast_t *call = mk(N_CALL);
      call->left = n;
      while (sv_lexer_next(&p->lx) != TOK_RPAREN && p->lx.st.tok != TOK_EOF) {
        if (p->lx.st.tok == TOK_REST) {
          p->lx.st.consumed = 1;
          sv_ast_t *spread = mk(N_SPREAD);
          spread->right = parse_with_in(p, parse_assign);
          sv_ast_list_push(p->js, &call->args, spread);
        } else {
          sv_ast_list_push(p->js, &call->args, parse_with_in(p, parse_assign));
        }
        if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
        else break;
      }
      expect(p, TOK_RPAREN);
      call->src_off = n ? n->src_off : call->src_off;
      call->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
      n = call;
    } else if (la == TOK_DOT || la == TOK_LBRACKET) {
      n = parse_member_suffix(p, n, la);
      if (n && n->type == N_EMPTY) return n;
    } else if (la == TOK_OPTIONAL_CHAIN) {
      p->lx.st.consumed = 1;
      sv_ast_t *opt = mk(N_OPTIONAL);
      opt->left = n;
      if (sv_lexer_next(&p->lx) == TOK_LBRACKET) {
        p->lx.st.consumed = 1;
        opt->right = parse_with_in(p, parse_expr);
        opt->flags = 1;
        expect(p, TOK_RBRACKET);
        opt->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
      } else if (p->lx.st.tok == TOK_LPAREN) {
        sv_ast_t *call = mk(N_CALL);
        call->left = opt;
        p->lx.st.consumed = 1;
        while (sv_lexer_next(&p->lx) != TOK_RPAREN && p->lx.st.tok != TOK_EOF) {
          sv_ast_list_push(p->js, &call->args, parse_with_in(p, parse_assign));
          if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
          else break;
        }
        expect(p, TOK_RPAREN);
        call->src_off = n ? n->src_off : call->src_off;
        call->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
        n = call;
        continue;
      } else if (p->lx.st.tok == TOK_HASH) {
        p->lx.st.consumed = 1;
        sv_lexer_next(&p->lx);
        if (!is_private_ident_like_tok(p->lx.st.tok)) {
          SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "private field name expected");
          return mk(N_EMPTY);
        }
        opt->right = mk_private_ident_from_tok(p);
        p->lx.st.consumed = 1;
      } else {
        opt->right = parse_dot_property_name(p);
        if (!opt->right) return mk(N_EMPTY);
      }
      opt->src_off = n ? n->src_off : opt->src_off;
      if (opt->src_end <= opt->src_off && opt->right) opt->src_end = opt->right->src_end;
      n = opt;
    } else if (la == TOK_TEMPLATE) {
      sv_ast_t *tagged = mk(N_TAGGED_TEMPLATE);
      tagged->left = n;
      p->tagged_template = true;
      tagged->right = parse_primary(p);
      n = tagged;
    } else break;
  }
  return n;
}

static sv_ast_t *parse_postfix(sv_parser_t *p) {
  sv_ast_t *n = parse_call(p);
  uint8_t la = sv_lexer_next(&p->lx);
  if (is_leading_dot_number_token(p) && !p->lx.st.had_newline) {
    sv_parse_unexpected_token(p);
    return mk(N_EMPTY);
  }
  if ((la == TOK_POSTINC || la == TOK_POSTDEC) && !p->lx.st.had_newline) {
    if (sv_is_strict_restricted_assign_target(p, n)) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    p->lx.st.consumed = 1;
    sv_ast_t *u = mk(N_UPDATE);
    u->op = la;
    u->right = n;
    u->src_off = n ? n->src_off : u->src_off;
    u->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return u;
  }
  return n;
}

bool sv_parse_stack_overflow(sv_parser_t *p) {
  if (!sv_check_c_stack_overflow(p->js)) return false;
  SV_MKERR_TYPED(p->js, JS_ERR_RANGE, "Maximum call stack size exceeded");
  return true;
}

sv_ast_t *parse_unary(sv_parser_t *p) {
  if (sv_parse_stack_overflow(p)) return mk(N_EMPTY);
  uint8_t la = sv_lexer_next(&p->lx);
  if (la == TOK_NOT || la == TOK_TILDA ||
      la == TOK_UPLUS || la == TOK_UMINUS ||
      la == TOK_PLUS || la == TOK_MINUS) {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_UNARY);
    n->op = (la == TOK_PLUS) ? TOK_UPLUS :
            (la == TOK_MINUS) ? TOK_UMINUS : la;
    n->right = parse_unary(p);
    n->src_end = n->right ? n->right->src_end : n->src_end;
    if (sv_lexer_next(&p->lx) == TOK_EXP) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX,
       "Unary operator used immediately before exponentiation expression. "
       "Parenthesis must be used to disambiguate operator precedence");
      return mk(N_EMPTY);
    }
    return n;
  }
  if (la == TOK_POSTINC || la == TOK_POSTDEC) {
    p->lx.st.consumed = 1;
    sv_ast_t *target = parse_unary(p);
    if (sv_is_strict_restricted_assign_target(p, target)) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    sv_ast_t *n = mk(N_UPDATE);
    n->op = la;
    n->right = target;
    n->flags = 1;
    n->src_end = target ? target->src_end : n->src_end;
    return n;
  }
  if (la == TOK_THROW) {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_THROW);
    n->right = parse_assign(p);
    return n;
  }
  return parse_postfix(p);
}

static sv_ast_t *parse_binary(sv_parser_t *p, int min_prec) {
  sv_ast_t *left = parse_unary(p);

  for (;;) {
    uint8_t op = sv_lexer_next(&p->lx);
    if (op >= TOK_MAX) break;
    if (op == TOK_IN && p->no_in) break;
    int prec = prec_table[op];
    if (prec == 0 || prec < min_prec) break;
    p->lx.st.consumed = 1;
    int next_prec = (op == TOK_EXP) ? prec : prec + 1;
    sv_ast_t *right = parse_binary(p, next_prec);
    sv_ast_t *bin = mk(N_BINARY);
    bin->op = op;
    bin->left = left;
    bin->right = right;
    bin->src_off = left ? left->src_off : bin->src_off;
    bin->src_end = right ? right->src_end : bin->src_end;
    left = bin;
  }
  return left;
}

static sv_ast_t *parse_ternary(sv_parser_t *p) {
  sv_ast_t *cond = parse_binary(p, 1);
  if (sv_lexer_next(&p->lx) == TOK_Q) {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_TERNARY);
    n->cond = cond;
    n->left = parse_with_in(p, parse_assign);
    expect(p, TOK_COLON);
    n->right = parse_assign(p);
    n->src_off = cond ? cond->src_off : n->src_off;
    n->src_end = n->right ? n->right->src_end : n->src_end;
    return n;
  }
  return cond;
}

static bool is_assign_op(uint8_t tok) {
  return tok >= TOK_ASSIGN && tok <= TOK_NULLISH_ASSIGN;
}

sv_ast_t *parse_assign(sv_parser_t *p) {
  sv_ast_t *left = parse_ternary(p);
  uint8_t op = sv_lexer_next(&p->lx);
  
  if (op == TOK_ARROW) {
    if (p->lx.st.had_newline) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Line terminator not allowed before arrow");
      return mk(N_EMPTY);
    }
    
    p->lx.st.consumed = 1;
    sv_ast_t *fn = mk(N_FUNC);
    fn->flags = FN_ARROW;
    fn->src_off = left->src_off;
    
    if (arrow_param_is_name(left) && !(left->flags & FN_PAREN)) {
      push_arrow_params_from_expr(p, fn, left);
    } else if (left->flags & FN_PAREN) {
      if (left->type != N_UNDEF || left->str) push_arrow_params_from_expr(p, fn, left);
    } else {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Malformed arrow function parameter list");
      return mk(N_EMPTY);
    }
    
    fn->body = parse_arrow_body(p, false);
    fn->src_end = node_src_end(p, fn->body);
    
    return finish_arrow(p, fn);
  }
  
  if (is_assign_op(op)) {
    if (left->type == N_NEW_TARGET) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Invalid left-hand side in assignment");
      return mk(N_EMPTY);
    }
    
    if ((left->flags & FN_PAREN) && op == TOK_ASSIGN &&
        (left->type == N_OBJECT || left->type == N_OBJECT_PAT ||
         left->type == N_ARRAY || left->type == N_ARRAY_PAT)) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX,
        "Invalid destructuring assignment target");
      return mk(N_EMPTY);
    }
    
    if (sv_is_strict_restricted_assign_target(p, left)) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_ASSIGN);
    n->op = op;
    n->left = left;
    n->right = parse_assign(p);
    n->src_off = left ? left->src_off : n->src_off;
    n->src_end = n->right ? n->right->src_end : n->src_end;
    return n;
  }
  
  return left;
}

sv_ast_t *parse_expr(sv_parser_t *p) {
  sv_ast_t *left = parse_assign(p);
  while (sv_lexer_next(&p->lx) == TOK_COMMA) {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_SEQUENCE);
    n->left = left;
    n->right = parse_assign(p);
    n->src_off = left ? left->src_off : n->src_off;
    n->src_end = n->right ? n->right->src_end : n->src_end;
    left = n;
  }
  return left;
}

sv_ast_t *parse_paren_list(sv_parser_t *p, bool trailing_comma) {
  sv_ast_t *left = parse_assign(p);
  while (sv_lexer_next(&p->lx) == TOK_COMMA) {
    p->lx.st.consumed = 1;
    if (sv_lexer_next(&p->lx) == TOK_RPAREN && (trailing_comma || sv_lexer_lookahead(&p->lx) == TOK_ARROW)) break;

    sv_ast_t *n = mk(N_SEQUENCE);
    n->left = left;
    n->right = parse_assign(p);
    left = n;
  }
  return left;
}

sv_ast_t *parse_paren_expr(sv_parser_t *p) {
  return parse_paren_list(p, false);
}
