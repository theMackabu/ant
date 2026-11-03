#include <string.h>

#include "parser_internal.h"

static sv_ast_t *skip_import_stmt(sv_parser_t *p) {
  while (sv_lexer_next(&p->lx) != TOK_SEMICOLON && p->lx.st.tok != TOK_EOF) p->lx.st.consumed = 1;
  if (p->lx.st.tok == TOK_SEMICOLON) p->lx.st.consumed = 1;
  return mk(N_EMPTY);
}

static void parse_import_attributes(sv_parser_t *p, sv_ast_t *decl) {
  if (sv_lexer_next(&p->lx) != TOK_WITH) return;
  p->lx.st.consumed = 1;
  expect(p, TOK_LBRACE);

  bool has_type = false;
  while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
    const char *key;
    uint32_t key_len;

    if (p->lx.st.tok == TOK_STRING) {
      sv_lex_string_t s = sv_lexer_str_literal(&p->lx);
      key = s.str;
      key_len = s.len;
    } else if (is_ident_like_tok(p->lx.st.tok)) {
      key = tok_ident_str(p, &key_len);
    } else {
      sv_parse_unexpected_token(p);
      return;
    }
    p->lx.st.consumed = 1;
    expect(p, TOK_COLON);

    if (sv_lexer_next(&p->lx) != TOK_STRING) {
      sv_parse_unexpected_token(p);
      return;
    }
    sv_lex_string_t value = sv_lexer_str_literal(&p->lx);
    p->lx.st.consumed = 1;

    bool is_type = key_len == 4 && memcmp(key, "type", 4) == 0;
    if (is_type && has_type) {
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Import attribute has duplicate key 'type'");
      return;
    }
    
    if (!is_type) {
      SV_MKERR_TYPED(
        p->js, JS_ERR_SYNTAX, "Import attribute \"%.*s\" with value \"%.*s\" is not supported",
        (int)key_len, key, (int)value.len, value.str
      );
      return;
    }

    has_type = true;
    decl->aux = value.str;
    decl->aux_len = value.len;

    if (sv_lexer_next(&p->lx) != TOK_COMMA) break;
    p->lx.st.consumed = 1;
  }
  expect(p, TOK_RBRACE);
}

enum {
  IMPORT_BIND_DEFAULT   = 1 << 0,
  IMPORT_BIND_NAMESPACE = 1 << 1,
};

static inline void import_decl_add_binding(
  sv_parser_t *p, sv_ast_t *decl,
  const char *import_name, uint32_t import_len,
  const char *local_name, uint32_t local_len,
  uint8_t flags
) {
  sv_ast_t *spec = mk_plain(N_IMPORT_SPEC);
  spec->flags = flags;
  if (import_name)
    spec->left = mk_ident(p, import_name, import_len);
  spec->right = mk_ident(p, local_name, local_len);
  sv_ast_list_push(p->js, &decl->args, spec);
}

sv_ast_t *parse_import_stmt(sv_parser_t *p) {
  static const char default_name[] = "default";
  sv_ast_t *decl = mk(N_IMPORT_DECL);

  if (sv_lexer_next(&p->lx) == TOK_STRING) {
    sv_ast_t *spec = mk(N_STRING);
    sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
    spec->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    p->lx.st.consumed = 1;
    decl->right = spec;
    parse_import_attributes(p, decl);
    consume_semicolon(p);
    return decl;
  }

  bool saw_clause = false;

  if (is_ident_like_tok(sv_lexer_next(&p->lx))) {
    saw_clause = true;
    uint32_t local_len = 0;
    const char *local_name = tok_ident_str(p, &local_len);
    import_decl_add_binding(
      p, decl,
      default_name, (uint32_t)(sizeof(default_name) - 1),
      local_name, local_len,
      IMPORT_BIND_DEFAULT
    );
    p->lx.st.consumed = 1;
    if (sv_lexer_next(&p->lx) == TOK_COMMA) p->lx.st.consumed = 1;
    else goto parse_from;
  }

  if (sv_lexer_next(&p->lx) == TOK_MUL) {
    saw_clause = true;
    p->lx.st.consumed = 1;
    expect(p, TOK_AS);
    sv_lexer_next(&p->lx);
    if (!is_ident_like_tok(p->lx.st.tok)) return skip_import_stmt(p);
    uint32_t ns_len = 0;
    const char *ns_name = tok_ident_str(p, &ns_len);
    import_decl_add_binding(
      p, decl,
      NULL, 0,
      ns_name, ns_len,
      IMPORT_BIND_NAMESPACE
    );
    p->lx.st.consumed = 1;
  } else if (sv_lexer_next(&p->lx) == TOK_LBRACE) {
    saw_clause = true;
    p->lx.st.consumed = 1;
    while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
      const char *import_name;
      uint32_t import_len;

      if (p->lx.st.tok == TOK_STRING) {
        sv_lex_string_t s = sv_lexer_str_literal(&p->lx);
        import_name = s.str;
        import_len = s.len;
        p->lx.st.consumed = 1;
      } else if (!(p->lx.st.tok >= TOK_IDENTIFIER && p->lx.st.tok < TOK_IDENT_LIKE_END)) {
        p->lx.st.consumed = 1;
        continue;
      } else {
        import_name = tok_ident_str(p, &import_len);
        p->lx.st.consumed = 1;
      }

      const char *local_name = import_name;
      uint32_t local_len = import_len;

      if (sv_lexer_next(&p->lx) == TOK_AS) {
        p->lx.st.consumed = 1;
        sv_lexer_next(&p->lx);
        if (!is_ident_like_tok(p->lx.st.tok)) return skip_import_stmt(p);
        local_name = tok_ident_str(p, &local_len);
        p->lx.st.consumed = 1;
      }

      import_decl_add_binding(p, decl, import_name, import_len, local_name, local_len, 0);

      if (sv_lexer_next(&p->lx) == TOK_COMMA) {
        p->lx.st.consumed = 1;
        if (sv_lexer_next(&p->lx) == TOK_RBRACE) break;
      }
    }
    expect(p, TOK_RBRACE);
  }

parse_from:
  if (!saw_clause) return skip_import_stmt(p);
  expect(p, TOK_FROM);
  if (sv_lexer_next(&p->lx) != TOK_STRING) return skip_import_stmt(p);

  sv_ast_t *spec = mk(N_STRING);
  sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
  spec->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  p->lx.st.consumed = 1;
  decl->right = spec;
  parse_import_attributes(p, decl);

  consume_semicolon(p);
  return decl;
}

static sv_ast_t *parse_export_name(sv_parser_t *p) {
  sv_lexer_next(&p->lx);
  if (p->lx.st.tok == TOK_STRING) {
    sv_lex_string_t s = sv_lexer_str_literal(&p->lx);
    sv_ast_t *name = mk(N_IDENT);
    name->str = s.str;
    name->len = s.len;
    p->lx.st.consumed = 1;
    return name;
  }

  if (!(p->lx.st.tok >= TOK_IDENTIFIER && p->lx.st.tok < TOK_IDENT_LIKE_END)) {
    sv_parse_unexpected_token(p);
    return NULL;
  }
  sv_ast_t *name = mk_ident_from_tok(p);
  p->lx.st.consumed = 1;
  return name;
}

sv_ast_t *parse_export_stmt(sv_parser_t *p) {
  sv_ast_t *decl = mk(N_EXPORT);
  sv_lexer_next(&p->lx);

  if (p->lx.st.tok == TOK_DEFAULT) {
    p->lx.st.consumed = 1;
    decl->flags |= EX_DEFAULT;

    if (sv_lexer_next(&p->lx) == TOK_ASYNC && sv_lexer_lookahead(&p->lx) == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
      uint32_t async_off = (uint32_t)p->lx.st.toff;
      p->lx.st.consumed = 1;
      sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
      decl->left = parse_func(p, true);
      decl->left->flags |= FN_ASYNC;
      decl->left->src_off = async_off;
      if (sv_lexer_next(&p->lx) == TOK_SEMICOLON) p->lx.st.consumed = 1;
      return decl;
    }
    if (p->lx.st.tok == TOK_FUNC) {
      p->lx.st.consumed = 1;
      decl->left = parse_func(p, false);
      if (sv_lexer_next(&p->lx) == TOK_SEMICOLON) p->lx.st.consumed = 1;
      return decl;
    }
    if (p->lx.st.tok == TOK_CLASS) {
      uint32_t class_off = (uint32_t)p->lx.st.toff;
      p->lx.st.consumed = 1;
      decl->left = parse_class(p);
      decl->left->src_off = class_off;
      decl->left->flags |= FN_CLASS_DECL;
      if (sv_lexer_next(&p->lx) == TOK_SEMICOLON) p->lx.st.consumed = 1;
      return decl;
    }

    decl->left = parse_assign(p);
    consume_semicolon(p);
    return decl;
  }

  if (p->lx.st.tok == TOK_ASYNC && sv_lexer_lookahead(&p->lx) == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
    decl->flags |= EX_DECL;
    uint32_t async_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
    decl->left = parse_func(p, true);
    decl->left->flags |= FN_ASYNC;
    decl->left->src_off = async_off;
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "exported function declarations require a name");
    return decl;
  }

  if (p->lx.st.tok == TOK_FUNC) {
    decl->flags |= EX_DECL;
    p->lx.st.consumed = 1;
    decl->left = parse_func(p, false);
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "exported function declarations require a name");
    return decl;
  }
  if (p->lx.st.tok == TOK_CLASS) {
    decl->flags |= EX_DECL;
    uint32_t class_off = (uint32_t)p->lx.st.toff;
    p->lx.st.consumed = 1;
    decl->left = parse_class(p);
    decl->left->src_off = class_off;
    decl->left->flags |= FN_CLASS_DECL;
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "exported class declarations require a name");
    return decl;
  }

  if (p->lx.st.tok == TOK_VAR || p->lx.st.tok == TOK_LET || p->lx.st.tok == TOK_CONST) {
    decl->flags |= EX_DECL;
    sv_var_kind_t kind = (
      p->lx.st.tok == TOK_VAR) ? SV_VAR_VAR :
      (p->lx.st.tok == TOK_LET) ? SV_VAR_LET : SV_VAR_CONST;
    p->lx.st.consumed = 1;
    decl->left = parse_var_decl(p, kind, false);
    consume_semicolon(p);
    return decl;
  }

  if (p->lx.st.tok == TOK_LBRACE) {
    decl->flags |= EX_NAMED;
    p->lx.st.consumed = 1;
    while (sv_lexer_next(&p->lx) != TOK_RBRACE && p->lx.st.tok != TOK_EOF) {
      sv_ast_t *local_name = parse_export_name(p);
      if (!local_name) return decl;

      sv_ast_t *export_name = local_name;
      if (sv_lexer_next(&p->lx) == TOK_AS) {
        p->lx.st.consumed = 1;
        export_name = parse_export_name(p);
        if (!export_name) return decl;
      }

      sv_ast_t *spec = mk(N_IMPORT_SPEC);
      spec->left = local_name;
      spec->right = export_name;
      sv_ast_list_push(p->js, &decl->args, spec);

      if (sv_lexer_next(&p->lx) == TOK_COMMA) {
        p->lx.st.consumed = 1;
        if (sv_lexer_next(&p->lx) == TOK_RBRACE) break;
      } else {
        break;
      }
    }
    expect(p, TOK_RBRACE);

    if (sv_lexer_next(&p->lx) == TOK_FROM) {
      p->lx.st.consumed = 1;
      if (sv_lexer_next(&p->lx) != TOK_STRING) {
        sv_parse_unexpected_token(p);
        return decl;
      }
      sv_ast_t *spec = mk(N_STRING);
      sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
      spec->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
      decl->right = spec;
      decl->flags |= EX_FROM;
      p->lx.st.consumed = 1;
      parse_import_attributes(p, decl);
    }

    consume_semicolon(p);
    return decl;
  }

  if (p->lx.st.tok == TOK_MUL) {
    decl->flags |= EX_STAR;
    p->lx.st.consumed = 1;

    if (sv_lexer_next(&p->lx) == TOK_AS) {
      decl->flags |= EX_NAMESPACE;
      p->lx.st.consumed = 1;

      sv_ast_t *name = parse_export_name(p);
      if (!name) return decl;

      sv_ast_t *spec = mk(N_IMPORT_SPEC);
      spec->right = name;
      sv_ast_list_push(p->js, &decl->args, spec);
    }

    expect(p, TOK_FROM);
    if (sv_lexer_next(&p->lx) != TOK_STRING) {
      sv_parse_unexpected_token(p);
      return decl;
    }
    sv_ast_t *spec = mk(N_STRING);
    sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
    spec->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    decl->right = spec;
    decl->flags |= EX_FROM;
    p->lx.st.consumed = 1;
    parse_import_attributes(p, decl);

    consume_semicolon(p);
    return decl;
  }

  sv_parse_unexpected_token(p);
  return decl;
}
