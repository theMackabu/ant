#include "parser_internal.h"

static void template_set_raw(sv_parser_t *p, sv_ast_t *s, const uint8_t *in, size_t start, size_t end) {
  s->aux = (const char *)&in[start];
  s->aux_len = (uint32_t)(end - start);
  if (!memchr(in + start, '\r', end - start)) return;

  uint8_t *raw = parse_arena_bump(p->js, end - start);
  if (!raw) {
    (void)SV_MKERR(p->js, "oom");
    return;
  }

  size_t n = 0;
  for (size_t i = start; i < end; i++) {
    raw[n++] = in[i] == '\r' ? '\n' : in[i];
    if (in[i] == '\r' && i + 1 < end && in[i + 1] == '\n') i++;
  }
  
  s->aux = (const char *)raw;
  s->aux_len = (uint32_t)n;
}

static sv_tpl_cooked_t decode_template_segment(sv_parser_t *p, const uint8_t *in, size_t start, size_t end) {
  sv_tpl_cooked_t outv = { .str = NULL, .len = 0, .ok = true, .valid_cooked = true };
  size_t raw_len = (end > start) ? (end - start) : 0;
  
  if (raw_len == 0) {
    outv.str = "";
    return outv;
  }

  uint8_t *out = parse_arena_bump(p->js, raw_len);
  if (!out) {
    (void)SV_MKERR(p->js, "oom");
    outv.ok = false;
    return outv;
  }

  size_t out_len = 0;
  for (size_t i = start; i < end; i++) {
    if (in[i] == '\r') {
      out[out_len++] = '\n';
      if (i + 1 < end && in[i + 1] == '\n') i++;
      continue;
    }
    
    if (in[i] != '\\' || i + 1 >= end) {
      out[out_len++] = in[i];
      continue;
    }
    
    uint8_t flags = 0;
    uint8_t escape_char = in[i + 1];
    i += 1 + decode_escape(in, i, end, out, &out_len, '`', &flags);
    
    if (flags) {
      outv.valid_cooked = false;
      outv.error = escape_error_message(flags, escape_char, ESCAPE_CONTEXT_TEMPLATE);
      return outv;
    }
  }

  outv.str = (const char *)out;
  outv.len = (uint32_t)out_len;
  
  return outv;
}

sv_ast_t *parse_primary(sv_parser_t *p) {
  sv_lexer_next(&p->lx);

  static const void *dispatch[TOK_MAX] = {
    [TOK_NUMBER]     = &&l_number,
    [TOK_STRING]     = &&l_string,
    [TOK_BIGINT]     = &&l_bigint,
    [TOK_TRUE]       = &&l_true,
    [TOK_FALSE]      = &&l_false,
    [TOK_NULL]       = &&l_null,
    [TOK_UNDEF]      = &&l_undef,
    [TOK_THIS]       = &&l_this,
    [TOK_IDENTIFIER] = &&l_ident,
    [TOK_AS]         = &&l_ident,
    [TOK_FROM]       = &&l_ident,
    [TOK_OF]         = &&l_ident,
    [TOK_USING]      = &&l_ident,
    [TOK_LPAREN]     = &&l_paren,
    [TOK_LBRACKET]   = &&l_array,
    [TOK_LBRACE]     = &&l_object,
    [TOK_FUNC]       = &&l_func,
    [TOK_CLASS]      = &&l_class,
    [TOK_ASYNC]      = &&l_async,
    [TOK_TEMPLATE]   = &&l_template,
    [TOK_NEW]        = &&l_new,
    [TOK_TYPEOF]     = &&l_typeof,
    [TOK_VOID]       = &&l_void,
    [TOK_DELETE]     = &&l_delete,
    [TOK_YIELD]      = &&l_yield,
    [TOK_AWAIT]      = &&l_await,
    [TOK_SUPER]      = &&l_super,
    [TOK_REST]       = &&l_spread,
    [TOK_IMPORT]     = &&l_import_expr,
    [TOK_DIV]        = &&l_regex,
    [TOK_DIV_ASSIGN] = &&l_regex,
    [TOK_GLOBAL_THIS]= &&l_globalthis,
    [TOK_WINDOW]     = &&l_ident,
    [TOK_HASH]       = &&l_private_name,
  };

  if (p->lx.st.tok < TOK_MAX && dispatch[p->lx.st.tok])
    goto *dispatch[p->lx.st.tok];

  sv_parse_unexpected_token(p);
  return mk(N_EMPTY);

  l_number: {
    p->lx.st.consumed = 1;
    return mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
  }

  l_string: {
    sv_ast_t *n = mk(N_STRING);
    sv_ast_set_string(n, sv_lexer_str_literal(&p->lx));
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    p->lx.st.consumed = 1;
    return n;
  }

  l_bigint: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_BIGINT);
    n->str = tok_str(p);
    n->len = (uint32_t)p->lx.st.tlen;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_true: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_BOOL);
    n->num = 1;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_false: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_BOOL);
    n->num = 0;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_null: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_NULL);
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_undef: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_UNDEF);
    n->str = tok_str(p);
    n->len = (uint32_t)p->lx.st.tlen;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_this: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_THIS);
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_globalthis: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_GLOBAL_THIS);
    n->str = tok_str(p);
    n->len = (uint32_t)p->lx.st.tlen;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return n;
  }

  l_ident: {
    p->lx.st.consumed = 1;
    return mk_ident_from_tok(p);
  }

  l_paren: {
    uint32_t paren_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    if (sv_lexer_next(&p->lx) == TOK_RPAREN) {
      p->lx.st.consumed = 1;
      sv_ast_t *n = mk(N_UNDEF);
      n->flags |= FN_PAREN;
      n->src_off = paren_off;
      return n;
    }
    bool outer_no_in = p->no_in;
    p->no_in = false;
    sv_ast_t *expr = parse_paren_expr(p);
    p->no_in = outer_no_in;
    expect(p, TOK_RPAREN);
    expr->flags |= FN_PAREN;
    if (expr->type != N_FUNC && expr->type != N_CLASS) expr->src_off = paren_off;
    if (expr->type != N_FUNC && expr->type != N_CLASS) expr->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    return expr;
  }

  l_array:  return parse_with_in(p, parse_array);
  l_object: return parse_with_in(p, parse_object);
  l_func:   { p->lx.st.consumed = 1; return parse_func(p, false); }

  l_class: {
    uint32_t class_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    sv_ast_t *cls = parse_with_in(p, parse_class);
    cls->src_off = class_off;
    return cls;
  }

  l_async: {
    uint32_t async_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    bool has_line_term = lookahead_crosses_line_terminator(p);
    if (!has_line_term && sv_lexer_lookahead(&p->lx) == TOK_FUNC) {
      sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
      sv_ast_t *fn = parse_func(p, true);
      fn->flags |= FN_ASYNC;
      fn->src_off = async_off;
      return fn;
    }
    if (has_line_term) return mk_ident_from_tok(p);
    sv_ast_t *arrow = try_parse_async_arrow(p);
    if (arrow) return arrow;
    return mk_ident_from_tok(p);
  }

  l_template: {
    p->lx.st.consumed = 1;
    bool tagged = p->tagged_template;
    p->tagged_template = false;
    sv_ast_t *n = mk(N_TEMPLATE);
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    
    const uint8_t *in = (const uint8_t *)&p->lx.code[p->lx.st.toff];
    const uint32_t tpl_off = (uint32_t)p->lx.st.toff;
    
    size_t tpl_len = p->lx.st.tlen;
    size_t i = 1;

    for (;;) {
      size_t seg_start = i;
      while (i < tpl_len - 1) {
        if (in[i] == '\\' && i + 1 < tpl_len - 1) { i += 2; continue; }
        if (in[i] == '$' && i + 1 < tpl_len - 1 && in[i + 1] == '{') break;
        i++;
      }
      sv_ast_t *s = mk(N_STRING);
      s->flags |= FN_TEMPLATE_SEGMENT;
      template_set_raw(p, s, in, seg_start, i);
      sv_tpl_cooked_t cooked = decode_template_segment(p, in, seg_start, i);
      if (cooked.ok && cooked.valid_cooked) {
        s->str = cooked.str;
        s->len = cooked.len;
      } else if (cooked.ok && !tagged) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "%s", cooked.error);
        return n;
      } else if (cooked.ok) s->flags |= FN_INVALID_COOKED;
      
      sv_ast_list_push(p->js, &n->args, s);
      if (i >= tpl_len - 1 || in[i] != '$') break;

      i += 2;
      size_t expr_start = i;
      size_t expr_max_len = (
        tpl_len > 0 && expr_start < tpl_len - 1)
        ? (tpl_len - 1 - expr_start) : 0;

      const uint32_t expr_off = tpl_off + (uint32_t)expr_start;
      sv_lexer_checkpoint_t cp;
      
      sv_lexer_push_source(
        &p->lx, &cp, p->lx.code,
        (ant_offset_t)expr_off, (ant_offset_t)(expr_off + expr_max_len)
      );

      sv_ast_t *expr = parse_with_in(p, parse_expr);
      sv_ast_list_push(p->js, &n->args, expr);

      if (sv_lexer_next(&p->lx) != TOK_RBRACE) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unterminated template expression");
        sv_lexer_pop_source(&p->lx, &cp);
        return n;
      }
      p->lx.st.consumed = 1;

      size_t consumed_expr = (size_t)p->lx.st.pos - expr_off;
      sv_lexer_pop_source(&p->lx, &cp);

      if (consumed_expr == 0) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unterminated template expression");
        return n;
      } i = expr_start + consumed_expr;
    }
    return n;
  }

  l_new: {
    uint32_t new_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;

    if (sv_lexer_next(&p->lx) == TOK_DOT) {
      p->lx.st.consumed = 1;
      if (sv_lexer_next(&p->lx) == TOK_IDENTIFIER && p->lx.st.tlen == 6 && memcmp(tok_str(p), "target", 6) == 0) {
        p->lx.st.consumed = 1;
        sv_ast_t *target = mk(N_NEW_TARGET);
        target->src_off = new_off;
        target->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
        return target;
      }
      sv_parse_unexpected_token(p);
      return mk(N_EMPTY);
    }

    sv_ast_t *n = mk(N_NEW);
    n->src_off = new_off;
    sv_ast_t *callee = parse_primary(p);
    
    for (;;) {
      uint8_t la = sv_lexer_next(&p->lx);
      if (la == TOK_DOT || la == TOK_LBRACKET) callee = parse_member_suffix(p, callee, la);
      else break;
      if (callee && callee->type == N_EMPTY) return callee;
    }
    n->left = callee;

    if (sv_lexer_next(&p->lx) == TOK_LPAREN) {
      p->lx.st.consumed = 1;
      while (sv_lexer_next(&p->lx) != TOK_RPAREN && p->lx.st.tok != TOK_EOF) {
        if (p->lx.st.tok == TOK_REST) {
          p->lx.st.consumed = 1;
          sv_ast_t *spread = mk(N_SPREAD);
          spread->right = parse_with_in(p, parse_assign);
          sv_ast_list_push(p->js, &n->args, spread);
        } else sv_ast_list_push(p->js, &n->args, parse_with_in(p, parse_assign));
        if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
        else break;
      } expect(p, TOK_RPAREN);
      n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    }
    if (n->src_end <= n->src_off && n->left) n->src_end = n->left->src_end;
    return n;
  }

  l_typeof: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_TYPEOF);
    n->right = parse_unary(p);
    return n;
  }

  l_void: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_VOID);
    n->right = parse_unary(p);
    return n;
  }

  l_delete: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_DELETE);
    n->right = parse_unary(p);
    if (p->lx.strict && n->right && n->right->type == N_IDENT) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "cannot delete bindings in strict mode");
      return mk(N_EMPTY);
    }
    return n;
  }

  l_yield: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_YIELD);
    if (sv_lexer_next(&p->lx) == TOK_MUL) {
      p->lx.st.consumed = 1;
      n->flags |= FN_YIELD_STAR;
    }
    if (p->lx.st.tok != TOK_SEMICOLON && p->lx.st.tok != TOK_RBRACE &&
        p->lx.st.tok != TOK_RPAREN && p->lx.st.tok != TOK_RBRACKET &&
        p->lx.st.tok != TOK_EOF && p->lx.st.tok != TOK_COMMA)
      n->right = parse_assign(p);
    return n;
  }

  l_await: {
    if (!p->in_async && p->goal != SV_PARSE_MODULE) {
      uint8_t la = sv_lexer_lookahead(&p->lx);
      bool starts_operand = !lookahead_crosses_line_terminator(p) && (
        la == TOK_IDENTIFIER || la == TOK_NUMBER || la == TOK_STRING ||
        la == TOK_THIS || la == TOK_NEW || la == TOK_FUNC || la == TOK_CLASS
      );
      if (starts_operand) {
        SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "await is only valid in async functions and the top level bodies of modules");
        return mk(N_EMPTY);
      }
      p->lx.st.consumed = 1;
      return mk_ident_from_tok(p);
    }
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_AWAIT);
    n->right = parse_unary(p);
    return n;
  }

  l_super: {
    sv_ast_t *n = mk_ident(p, "super", 5);
    n->src_off = (uint32_t)p->lx.st.toff;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    p->lx.st.consumed = 1;
    return n;
  }

  l_spread: {
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_SPREAD);
    n->right = parse_assign(p);
    return n;
  }

  l_import_expr: {
    p->lx.st.consumed = 1;
    if (sv_lexer_next(&p->lx) == TOK_DOT && p->goal == SV_PARSE_DETECT) {
      p->wants_module = true;
      sv_parse_stop(p);
      return mk(N_EMPTY);
    }
    if (sv_lexer_next(&p->lx) != TOK_LPAREN) return mk_ident(p, "import", 6);
    p->lx.st.consumed = 1;
    sv_ast_t *n = mk(N_IMPORT);
    n->right = parse_assign(p);
    if (sv_lexer_next(&p->lx) == TOK_COMMA && (p->lx.st.consumed = 1, sv_lexer_next(&p->lx) != TOK_RPAREN)) {
      n->left = parse_assign(p);
      if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
    }
    expect(p, TOK_RPAREN);
    return n;
  }

  l_regex: {
    p->lx.st.consumed = 1;
    ant_offset_t pattern_start = (p->lx.st.tok == TOK_DIV_ASSIGN) ? (p->lx.st.toff + 1) : p->lx.st.pos;
    if (p->lx.st.tok == TOK_DIV_ASSIGN) p->lx.st.pos = pattern_start;
    bool in_class = false;

    while (p->lx.st.pos < p->lx.clen && !regex_line_terminator_at(p->lx.code, p->lx.clen, p->lx.st.pos)) {
      char c = p->lx.code[p->lx.st.pos];
      if (c == '\\' && p->lx.st.pos + 1 < p->lx.clen && !regex_line_terminator_at(p->lx.code, p->lx.clen, p->lx.st.pos + 1)) {
        p->lx.st.pos += 2;
        continue;
      }
      
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      else if (c == '/' && !in_class) break;
      
      p->lx.st.pos++;
    }

    if (p->lx.st.pos >= p->lx.clen || p->lx.code[p->lx.st.pos] != '/') {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Invalid regular expression: missing /");
      return mk(N_EMPTY);
    }

    ant_offset_t pattern_end = p->lx.st.pos++;
    ant_offset_t flags_start = p->lx.st.pos;
    while (p->lx.st.pos < p->lx.clen && (is_ident_continue((unsigned char)p->lx.code[p->lx.st.pos]))) p->lx.st.pos++;
    
    ant_offset_t flags_end = p->lx.st.pos;
    if (!regexp_flags_valid(&p->lx.code[flags_start], flags_end - flags_start)) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Invalid regular expression flags");
      return mk(N_EMPTY);
    }
    
    const char *pattern_error = regexp_literal_syntax_error(
      &p->lx.code[pattern_start], pattern_end - pattern_start,
      &p->lx.code[flags_start], flags_end - flags_start
    );
    
    if (pattern_error) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX, "Invalid regular expression: /%.*s/%.*s: %s",
        (int)(pattern_end - pattern_start), &p->lx.code[pattern_start],
        (int)(flags_end - flags_start), &p->lx.code[flags_start], pattern_error
      );
      return mk(N_EMPTY);
    }
    
    sv_ast_t *n = mk(N_REGEXP);
    n->str = &p->lx.code[pattern_start];
    n->len = (uint32_t)(pattern_end - pattern_start);
    n->aux = &p->lx.code[flags_start];
    n->aux_len = (uint32_t)(flags_end - flags_start);
    n->src_end = (uint32_t)flags_end;
    
    p->lx.st.consumed = 1;
    return n;
  }

  l_private_name: {
    p->lx.st.consumed = 1;
    sv_lexer_next(&p->lx);
    if (!is_private_ident_like_tok(p->lx.st.tok)) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "private field name expected");
      return mk(N_EMPTY);
    }
    sv_ast_t *n = mk_private_ident_from_tok(p);
    p->lx.st.consumed = 1;
    return n;
  }
}

sv_ast_t *parse_array(sv_parser_t *p) {
  p->lx.st.consumed = 1;
  sv_ast_t *n = mk(N_ARRAY);
  while (sv_lexer_next(&p->lx) != TOK_RBRACKET && p->lx.st.tok != TOK_EOF) {
    if (p->lx.st.tok == TOK_COMMA) {
      p->lx.st.consumed = 1;
      sv_ast_list_push(p->js, &n->args, mk(N_EMPTY));
      continue;
    }
    if (p->lx.st.tok == TOK_REST) {
      p->lx.st.consumed = 1;
      sv_ast_t *spread = mk(N_SPREAD);
      spread->right = parse_assign(p);
      sv_ast_list_push(p->js, &n->args, spread);
    } else {
      sv_ast_list_push(p->js, &n->args, parse_assign(p));
    }
    if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
    else break;
  }
  expect(p, TOK_RBRACKET);
  n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  return n;
}

bool validate_accessor_params(sv_parser_t *p, sv_ast_t *fn, uint32_t flags) {
  if (!(flags & (FN_GETTER | FN_SETTER)) || !fn) return true;

  if ((flags & FN_GETTER) && fn->args.count != 0) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Getter must not have parameters");
    return false;
  }

  if (
    (flags & FN_SETTER) && (
    fn->args.count != 1 ||
    (fn->args.count == 1 && fn->args.items[0] && fn->args.items[0]->type == N_REST))
  ) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Setter must have exactly one non-rest parameter");
    return false;
  }

  return true;
}

sv_ast_t *parse_object(sv_parser_t *p) {
  p->lx.st.consumed = 1;
  sv_ast_t *n = mk(N_OBJECT);
  bool proto_set = false;
  while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
    sv_ast_t *prop = mk(N_PROPERTY);

    if (p->lx.st.tok == TOK_REST) {
      p->lx.st.consumed = 1;
      sv_ast_t *spread = mk(N_SPREAD);
      spread->right = parse_assign(p);
      sv_ast_list_push(p->js, &n->args, spread);
      if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
      continue;
    }

    if (p->lx.st.tok == TOK_LBRACKET) {
      p->lx.st.consumed = 1;
      prop->left = parse_assign(p);
      expect(p, TOK_RBRACKET);
      prop->flags |= FN_COMPUTED;
    }
    else if (p->lx.st.tok == TOK_MUL) {
      prop->flags |= FN_GENERATOR;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx);

      if (p->lx.st.tok == TOK_LBRACKET) {
        p->lx.st.consumed = 1;
        prop->left = parse_assign(p);
        expect(p, TOK_RBRACKET);
        prop->flags |= FN_COMPUTED;
      } else if (p->lx.st.tok == TOK_NUMBER) {
        p->lx.st.consumed = 1;
        prop->left = mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
      } else if (p->lx.st.tok == TOK_STRING) {
        prop->left = mk(N_STRING);
        sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
        prop->left->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
        p->lx.st.consumed = 1;
      } else {
        prop->left = mk_ident_from_tok(p);
        p->lx.st.consumed = 1;
      }

      prop->right = parse_func(p, false);
      prop->right->flags |= FN_GENERATOR | FN_METHOD;
      prop->right->src_off = prop->src_off;
      sv_ast_list_push(p->js, &n->args, prop);
      if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
      continue;
    }
    else if (
      (p->lx.st.tlen == 3 && memcmp(tok_str(p), "get", 3) == 0) ||
      (p->lx.st.tlen == 3 && memcmp(tok_str(p), "set", 3) == 0)) {
      uint8_t gs = tok_str(p)[0];
      uint8_t la = sv_lexer_lookahead(&p->lx);
      if (la != TOK_COLON && la != TOK_LPAREN && la != TOK_COMMA &&
          la != TOK_RBRACE) {
        p->lx.st.consumed = 1;
        prop->flags |= (gs == 'g') ? FN_GETTER : FN_SETTER;
        sv_lexer_next(&p->lx);

        if (p->lx.st.tok == TOK_LBRACKET) {
          p->lx.st.consumed = 1;
          prop->left = parse_assign(p);
          expect(p, TOK_RBRACKET);
          prop->flags |= FN_COMPUTED;
        } else if (p->lx.st.tok == TOK_NUMBER) {
          p->lx.st.consumed = 1;
          prop->left = mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
        } else if (p->lx.st.tok == TOK_STRING) {
          prop->left = mk(N_STRING);
          sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
          prop->left->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
          p->lx.st.consumed = 1;
        } else {
          prop->left = mk_ident_from_tok(p);
          p->lx.st.consumed = 1;
        }

        prop->right = parse_func(p, false);
        if (!validate_accessor_params(p, prop->right, prop->flags)) return n;
        prop->right->flags |= FN_METHOD;
        prop->right->src_off = prop->src_off;
        sv_ast_list_push(p->js, &n->args, prop);
        if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
        continue;
      }
      prop->left = mk_ident_from_tok(p);
      p->lx.st.consumed = 1;
    }
    else if (p->lx.st.tok == TOK_ASYNC) {
      uint8_t la = sv_lexer_lookahead(&p->lx);
      if (la != TOK_COLON && la != TOK_LPAREN && la != TOK_COMMA && la != TOK_RBRACE) {
        p->lx.st.consumed = 1;
        prop->flags |= FN_ASYNC;
        sv_lexer_next(&p->lx);

        if (p->lx.st.tok == TOK_MUL) {
          prop->flags |= FN_GENERATOR;
          p->lx.st.consumed = 1;
          sv_lexer_next(&p->lx);
        }

        if (p->lx.st.tok == TOK_LBRACKET) {
          p->lx.st.consumed = 1;
          prop->left = parse_assign(p);
          expect(p, TOK_RBRACKET);
          prop->flags |= FN_COMPUTED;
        } else if (p->lx.st.tok == TOK_NUMBER) {
          p->lx.st.consumed = 1;
          prop->left = mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
        } else if (p->lx.st.tok == TOK_STRING) {
          prop->left = mk(N_STRING);
          sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
          prop->left->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
          p->lx.st.consumed = 1;
        } else {
          prop->left = mk_ident_from_tok(p);
          p->lx.st.consumed = 1;
        }

        prop->right = parse_func(p, true);
        prop->right->flags |= FN_ASYNC | FN_METHOD;
        if (prop->flags & FN_GENERATOR)
          prop->right->flags |= FN_GENERATOR;
        prop->right->src_off = prop->src_off;
        sv_ast_list_push(p->js, &n->args, prop);
        if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
        continue;
      }
      prop->left = mk_ident_from_tok(p);
      p->lx.st.consumed = 1;
    }
    else if (p->lx.st.tok == TOK_NUMBER) {
      p->lx.st.consumed = 1;
      prop->left = mk_num(p, Ant_Value_AsNumberUnchecked(p->lx.st.tval));
    }
    else if (p->lx.st.tok == TOK_STRING) {
      prop->left = mk(N_STRING);
      sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
      prop->left->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
      p->lx.st.consumed = 1;
    }
    else {
      prop->left = mk_ident_from_tok(p);
      p->lx.st.consumed = 1;
    }

    if (sv_lexer_next(&p->lx) == TOK_COLON) {
      p->lx.st.consumed = 1;
      prop->flags |= FN_COLON;
      if (!(prop->flags & FN_COMPUTED) && sv_ast_is_proto_key(prop->left)) {
        if (proto_set) n->flags |= FN_PATTERN_ONLY;
        proto_set = true;
      }
      prop->right = parse_assign(p);
    } else if (p->lx.st.tok == TOK_LPAREN) {
      prop->right = parse_func(p, false);
      prop->right->flags |= FN_METHOD;
      prop->right->src_off = prop->src_off;
    } else {
      prop->right = mk_ident(p, prop->left->str, prop->left->len);
      if (sv_lexer_next(&p->lx) == TOK_ASSIGN) {
        p->lx.st.consumed = 1;
        n->flags |= FN_PATTERN_ONLY;
        sv_ast_t *def = mk(N_ASSIGN);
        def->op = TOK_ASSIGN;
        def->left = prop->right;
        def->right = parse_assign(p);
        prop->right = def;
      }
    }

    sv_ast_list_push(p->js, &n->args, prop);
    if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
    else break;
  }
  expect(p, TOK_RBRACE);
  n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  return n;
}
