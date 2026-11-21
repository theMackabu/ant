#include "parser_internal.h"

static bool class_modifier_here(sv_parser_t *p) {
  uint8_t la = sv_lexer_lookahead(&p->lx);
  return la != TOK_LPAREN && la != TOK_ASSIGN && la != TOK_SEMICOLON && la != TOK_RBRACE && la != TOK_EOF;
}

typedef struct {
  bool observes;
  bool suspends;
} class_expr_facts_t;

static void class_expr_facts(const sv_ast_t *node, class_expr_facts_t *facts) {
  if (!node || (facts->suspends && facts->observes)) return;
  switch (node->type) {
    case N_YIELD:
    case N_AWAIT:
      facts->suspends = true;
      break;
    case N_FUNC:
      facts->observes = true;
      return;
    case N_METHOD:
      class_expr_facts(node->left, facts);
      return;
    case N_STATIC_BLOCK:
      return;
    case N_ASSIGN:
    case N_UPDATE:
    case N_DELETE:
      facts->observes = true;
      break;
    case N_CALL:
      if (ast_is_ident(node->left, "eval", 4)) facts->observes = true;
      break;
    default: break;
  }

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child) class_expr_facts(child, facts);
}

static sv_ast_t *class_strict_expr(sv_parser_t *p, sv_ast_t *expr, bool outer_strict) {
  if (outer_strict || !expr) return expr;
  class_expr_facts_t facts = {0};
  class_expr_facts(expr, &facts);
  if (!facts.observes) return expr;

  if (facts.suspends) {
    sv_ast_t *strict = mk(N_STRICT_EXPR);
    strict->right = expr;
    strict->src_off = expr->src_off;
    strict->src_end = expr->src_end;
    return strict;
  }

  sv_ast_t *fn = mk(N_FUNC);
  fn->flags = FN_ARROW | FN_CLASS_BODY | FN_SYNTHETIC;
  fn->body = expr;
  fn->src_off = expr->src_off;
  fn->src_end = expr->src_end;

  sv_ast_t *call = mk(N_CALL);
  call->left = finish_arrow(p, fn);
  call->src_off = expr->src_off;
  call->src_end = expr->src_end;
  return call;
}

static sv_ast_t *parse_class_strict(sv_parser_t *p, bool outer_strict) {
  sv_ast_t *cls = mk(N_CLASS);

  sv_lexer_next(&p->lx);
  if (binding_ident_here(p) && !(p->lx.st.tlen == 7 && memcmp(tok_str(p), "extends", 7) == 0)) {
    cls->str = tok_ident_str(p, &cls->len);
    if (sv_reject_binding_name(p, cls->str, cls->len)) return cls;
    p->lx.st.consumed = 1;
  }

  if (sv_lexer_next(&p->lx) == TOK_IDENTIFIER && p->lx.st.tlen == 7 && memcmp(tok_str(p), "extends", 7) == 0) {
    p->lx.st.consumed = 1;
    sv_ast_t *heritage = parse_lhs(p);
    bool unary = heritage->type == N_VOID || heritage->type == N_TYPEOF || heritage->type == N_DELETE ||
      heritage->type == N_UNARY || heritage->type == N_YIELD || heritage->type == N_AWAIT ||
      (heritage->type == N_FUNC && (heritage->flags & FN_ARROW));
    if (unary && !(heritage->flags & FN_PAREN)) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unexpected token in class heritage");
      return cls;
    }
    cls->left = class_strict_expr(p, heritage, outer_strict);
  }

  expect(p, TOK_LBRACE);
  while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
    if (p->lx.st.tok == TOK_SEMICOLON) { p->lx.st.consumed = 1; continue; }

    uint8_t flags = 0;

    if (p->lx.st.tok == TOK_STATIC && class_modifier_here(p)) {
      flags |= FN_STATIC;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);

      if (p->lx.st.tok == TOK_LBRACE) {
        sv_parse_context_t saved = sv_parse_context_save(p);
        bool saved_strict = p->lx.strict;
        uint32_t saved_depth = p->function_depth;
        p->lx.strict = true;
        p->in_async = false;
        p->static_block = true;
        p->class_initializer = true;
        p->in_generator = false;
        p->no_yield = false;
        p->function_depth = 0;
        sv_ast_t *block = parse_block(p, false);
        sv_parse_context_restore(p, saved);
        p->lx.strict = saved_strict;
        p->function_depth = saved_depth;
        block->type = N_STATIC_BLOCK;
        block->flags = FN_STATIC | FN_CLASS_BODY;
        sv_ast_list_push(p->js, &cls->args, block);
        continue;
      }
    }

    sv_ast_t *method = mk(N_METHOD);
    uint32_t method_src_off = (uint32_t)p->lx.st.toff;

    if (p->lx.st.tok == TOK_ASYNC && class_modifier_here(p) && !lookahead_crosses_line_terminator(p)) {
      flags |= FN_ASYNC;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
    }

    if (p->lx.st.tok == TOK_MUL) {
      flags |= FN_GENERATOR;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
    }

    if (!(flags & (FN_ASYNC | FN_GENERATOR)) && p->lx.st.tok == TOK_IDENTIFIER && p->lx.st.tlen == 3 &&
        (memcmp(tok_str(p), "get", 3) == 0 || memcmp(tok_str(p), "set", 3) == 0) &&
        class_modifier_here(p) && sv_lexer_lookahead(&p->lx) != TOK_MUL) {
      flags |= (tok_str(p)[0] == 'g') ? FN_GETTER : FN_SETTER;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
    }

    if (p->lx.st.tok == TOK_LBRACKET) {
      p->lx.st.consumed = 1;
      method->left = class_strict_expr(p, parse_assign(p), outer_strict);
      expect(p, TOK_RBRACKET);
      flags |= FN_COMPUTED;
    } else if (p->lx.st.tok == TOK_HASH) {
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);
      if (!is_private_ident_like_tok(p->lx.st.tok)) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "private field name expected");
        return mk(N_EMPTY);
      }
      method->left = mk_private_ident_from_tok(p);
      p->lx.st.consumed = 1;
    } else if (p->lx.st.tok == TOK_NUMBER) {
      p->lx.st.consumed = 1;
      method->left = mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
    } else if (p->lx.st.tok == TOK_STRING) {
      method->left = mk(N_STRING);
      sv_ast_set_string(method->left, sv_lexer_str_literal(&p->lx));
      method->left->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
      p->lx.st.consumed = 1;
    } else if (p->lx.st.tok == TOK_BIGINT || is_private_ident_like_tok(p->lx.st.tok)) {
      method->left = mk_ident_from_tok(p);
      p->lx.st.consumed = 1;
    } else {
      sv_parse_unexpected_token(p);
      return cls;
    }

    method->flags = flags;

    if (!(flags & FN_STATIC) && (flags & FN_GENERATOR) &&
        method->left && method->left->type == N_IDENT &&
        method->left->len == 11 &&
        memcmp(method->left->str, "constructor", 11) == 0) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Class constructor may not be a generator");
      return cls;
    }

    if (sv_lexer_next(&p->lx) == TOK_LPAREN) {
      bool saved_strict = p->lx.strict;
      p->lx.strict = true;
      method->right = parse_method_func(p, (flags & FN_ASYNC) != 0, (flags & FN_GENERATOR) != 0);
      p->lx.strict = saved_strict;
      if (!validate_accessor_params(p, method->right, method->flags)) return cls;
      method->right->flags |= (flags & (FN_ASYNC | FN_GENERATOR)) | FN_METHOD | FN_CLASS_BODY;
      method->right->src_off = method_src_off;
    } else if (flags & (FN_ASYNC | FN_GENERATOR | FN_GETTER | FN_SETTER)) {
      sv_parse_unexpected_token(p);
      return cls;
    } else if (p->lx.st.tok == TOK_ASSIGN) {
      p->lx.st.consumed = 1;
      sv_parse_context_t saved = sv_parse_context_save(p);
      p->in_async = false;
      p->static_block = (flags & FN_STATIC) != 0;
      p->class_initializer = true;
      p->in_generator = false;
      p->no_yield = false;
      method->right = parse_assign(p);
      sv_parse_context_restore(p, saved);
      consume_semicolon(p);
    } else {
      method->right = mk(N_UNDEF);
      consume_semicolon(p);
    }

    sv_ast_list_push(p->js, &cls->args, method);
  }
  
  expect(p, TOK_RBRACE);
  cls->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  
  return cls;
}

sv_ast_t *parse_class(sv_parser_t *p) {
  bool saved_strict = p->lx.strict;
  p->lx.strict = true;
  sv_ast_t *cls = parse_class_strict(p, saved_strict);
  p->lx.strict = saved_strict;
  return cls;
}
