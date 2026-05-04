#include "silver/ast.h"
#include "silver/lexer.h"
#include "silver/directives.h"

#include "escape.h"
#include "debug.h"
#include "errors.h"
#include "internal.h"
#include "tokens.h"

#include <runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sv_ast_t *sv_ast_new(sv_node_type_t type) {
  sv_ast_t *n = parse_arena_bump(sizeof(sv_ast_t));
  if (!n) return NULL;
  memset(n, 0, sizeof(sv_ast_t));
  n->type = type;
  return n;
}

void sv_ast_list_push(sv_ast_list_t *list, sv_ast_t *node) {
  if (list->count >= list->cap) {
    int new_cap = list->cap ? list->cap * 2 : 4;
    sv_ast_t **new_items = parse_arena_bump((size_t)new_cap * sizeof(sv_ast_t *));
    if (!new_items) return;
    if (list->items)
      memcpy(new_items, list->items, (size_t)list->count * sizeof(sv_ast_t *));
    list->items = new_items;
    list->cap = new_cap;
  }
  list->items[list->count++] = node;
}

bool sv_ast_can_be_expression_statement(const sv_ast_t *node) {
  if (!node) return false;
  if ((node->type == N_FUNC || node->type == N_CLASS) && (node->flags & FN_PAREN)) return true;

  static const uint8_t expr_stmt_nodes[N__COUNT] = {
    [N_NUMBER] = 1, [N_STRING] = 1, [N_BIGINT] = 1, [N_BOOL] = 1,
    [N_NULL] = 1, [N_UNDEF] = 1, [N_THIS] = 1, [N_GLOBAL_THIS] = 1,
    [N_TEMPLATE] = 1, [N_REGEXP] = 1, [N_IDENT] = 1, [N_BINARY] = 1,
    [N_UNARY] = 1, [N_UPDATE] = 1, [N_ASSIGN] = 1, [N_TERNARY] = 1,
    [N_CALL] = 1, [N_NEW] = 1, [N_MEMBER] = 1, [N_OPTIONAL] = 1,
    [N_ARRAY] = 1, [N_OBJECT] = 1, [N_PROPERTY] = 1, [N_SPREAD] = 1,
    [N_SEQUENCE] = 1, [N_ARROW] = 1, [N_YIELD] = 1, [N_AWAIT] = 1,
    [N_TYPEOF] = 1, [N_DELETE] = 1, [N_VOID] = 1, [N_TAGGED_TEMPLATE] = 1,
    [N_IMPORT] = 1,
  };

  if (node->type == N_FUNC)
    return !(node->str && !(node->flags & FN_ARROW));
  if ((unsigned)node->type >= N__COUNT) return false;
  return expr_stmt_nodes[node->type] != 0;
}

typedef struct {
  ant_t *js;
  sv_lexer_t lx;
  bool no_in;
} sv_parser_t;

#define P            sv_parser_t *p
#define JS           p->js
#define TOK          p->lx.st.tok
#define TOFF         p->lx.st.toff
#define TLEN         p->lx.st.tlen
#define TVAL         p->lx.st.tval
#define POS          p->lx.st.pos
#define CONSUMED     p->lx.st.consumed
#define HAD_NEWLINE  p->lx.st.had_newline
#define CODE         p->lx.code
#define CLEN         p->lx.clen
#define CONSUME()    ((void)(CONSUMED = 1))
#define NEXT()       sv_lexer_next(&p->lx)
#define LA()         sv_lexer_lookahead(&p->lx)

static bool lookahead_crosses_line_terminator(P) {
  sv_lexer_state_t saved;
  sv_lexer_save_state(&p->lx, &saved);
  CONSUMED = 1;
  (void)NEXT();
  bool had_newline = HAD_NEWLINE;
  sv_lexer_restore_state(&p->lx, &saved);
  return had_newline;
}

static sv_ast_t *parse_stmt(P);
static sv_ast_t *parse_expr(P);
static sv_ast_t *parse_paren_expr(P);
static sv_ast_t *parse_assign(P);
static sv_ast_t *parse_ternary(P);
static sv_ast_t *parse_binary(P, int min_prec);
static sv_ast_t *parse_unary(P);
static sv_ast_t *parse_postfix(P);
static sv_ast_t *parse_call(P);
static sv_ast_t *parse_primary(P);
static sv_ast_t *parse_block(P, bool directive_ctx);
static sv_ast_t *parse_func(P);
static sv_ast_t *parse_class(P);
static sv_ast_t *parse_object(P);
static sv_ast_t *parse_array(P);
static sv_ast_t *parse_import_stmt(P);
static sv_ast_t *parse_export_stmt(P);
static sv_ast_t *parse_arrow_body(P);
static sv_ast_t *parse_binding_pattern(P);

#define SV_SYNC_ERR() ((void)sv_lexer_set_error_site(&p->lx))
#define SV_MKERR(...) (SV_SYNC_ERR(), js_mkerr(__VA_ARGS__))
#define SV_MKERR_TYPED(...) (SV_SYNC_ERR(), js_mkerr_typed(__VA_ARGS__))

static inline const char *tok_str(P) { 
  return &CODE[TOFF]; 
}

static inline bool eat(P, uint8_t tok) {
  NEXT();
  if (TOK == tok) { CONSUME(); return true; }
  return false;
}

static inline void expect(P, uint8_t tok) {
  NEXT();
  if (TOK == tok) CONSUME();
}

static inline sv_ast_t *mk_plain(sv_node_type_t type) {
  return sv_ast_new(type);
}

#define mk(type) ({ \
  sv_ast_t *_n = mk_plain(type); \
  _n->src_off = (uint32_t)TOFF; _n; \
})

static inline sv_ast_t *mk_num(double val) {
  sv_ast_t *n = mk_plain(N_NUMBER);
  n->num = val;
  return n;
}

static inline sv_ast_t *mk_ident(const char *s, uint32_t len) {
  sv_ast_t *n = mk_plain(N_IDENT);
  n->str = s;
  n->len = len;
  return n;
}

static inline void sv_ast_set_string(sv_ast_t *n, sv_lex_string_t s) {
  if (!n || !s.ok) return;
  n->str = s.str;
  n->len = s.len;
}

typedef struct {
  const char *str;
  uint32_t len;
  bool ok;
  bool valid_cooked;
} sv_tpl_cooked_t;

static inline const char *decode_ident_into_arena(const char *src, uint32_t len, uint32_t *out_len) {
  if (!src || len == 0 || !memchr(src, '\\', len)) {
    if (out_len) *out_len = len;
    return src;
  }

  char *dst = parse_arena_bump((size_t)len + 1);
  if (!dst) {
    if (out_len) *out_len = len;
    return src;
  }

  size_t di = 0; size_t i = 0;
  const uint8_t *in = (const uint8_t *)src;
  uint8_t *out = (uint8_t *)dst;
  
  while (i < len) {
    if (in[i] == '\\' && i + 1 < len && in[i + 1] == 'u') {
      size_t adv = decode_escape(in, i, len, out, &di, 0);
      if (adv > 0) { i += 2 + adv; continue; }
    }
    out[di++] = in[i++];
  }
  
  out[di] = '\0';
  if (out_len) *out_len = (uint32_t)di;
  return dst;
}

static inline bool is_contextual_ident_tok(uint8_t tok) {
  return 
    tok == TOK_AS    || 
    tok == TOK_FROM  || 
    tok == TOK_OF    ||
    tok == TOK_ASYNC ||
    tok == TOK_USING;
}

static inline const char *tok_ident_str(P, uint32_t *out_len) {
  return decode_ident_into_arena(tok_str(p), (uint32_t)TLEN, out_len);
}

static inline bool is_ident_like_tok(uint8_t tok) {
  return tok == TOK_IDENTIFIER || tok == TOK_DEFAULT || is_contextual_ident_tok(tok);
}

static inline bool is_private_ident_like_tok(uint8_t tok) {
  return tok >= TOK_IDENTIFIER && tok < TOK_IDENT_LIKE_END;
}

static inline bool sv_strict_forbidden_binding_ident(const char *s, uint32_t len) {
  return is_eval_or_arguments_name(s, len) || is_strict_reserved_name(s, len);
}

static inline bool sv_is_strict_restricted_assign_target(P, sv_ast_t *n) {
  return p->lx.strict && n && n->type == N_IDENT && is_eval_or_arguments_name(n->str, n->len);
}

static inline bool is_lexical_decl_stmt(sv_ast_t *n) {
  return n && n->type == N_VAR && (
    n->var_kind == SV_VAR_LET ||
    n->var_kind == SV_VAR_CONST ||
    n->var_kind == SV_VAR_USING ||
    n->var_kind == SV_VAR_AWAIT_USING
  );
}

static inline bool var_decl_has_initializer(sv_ast_t *n) {
  if (!n || n->type != N_VAR) return false;
  for (int i = 0; i < n->args.count; i++) {
    sv_ast_t *decl = n->args.items[i];
    if (decl && decl->type == N_VARDECL && decl->right) return true;
  }
  return false;
}

static inline sv_ast_t *mk_ident_from_tok(P) {
  uint32_t len = 0;
  const char *name = tok_ident_str(p, &len);
  sv_ast_t *n = mk_ident(name, len);
  n->src_off = (uint32_t)TOFF;
  n->src_end = (uint32_t)(TOFF + TLEN);
  return n;
}

static inline sv_ast_t *mk_private_ident_from_tok(P) {
  sv_ast_t *n = mk(N_IDENT);
  n->str = &CODE[TOFF - 1];
  n->len = (uint32_t)(TLEN + 1);
  n->src_off = (uint32_t)(TOFF > 0 ? TOFF - 1 : TOFF);
  n->src_end = (uint32_t)(TOFF + TLEN);
  return n;
}

static inline void sv_strict_check_binding_ident(P, const char *s, uint32_t len) {
  if (!p->lx.strict || !s || len == 0) return;
  if (sv_strict_forbidden_binding_ident(s, len)) SV_MKERR_TYPED(
    JS, JS_ERR_SYNTAX, "Invalid binding identifier '%.*s' in strict mode", 
    (int)len, s
  );
}

static inline void sv_parse_unexpected_token(P) {
  size_t tok_len = 0;

  if (TOFF < CLEN && TLEN > 0) {
    ant_offset_t rem = CLEN - TOFF;
    tok_len = (size_t)TLEN;
    if ((ant_offset_t)tok_len > rem) tok_len = (size_t)rem;
  }

  if (tok_len == 0) {
    SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Unexpected token 'EOF'");
    return;
  }

  SV_MKERR_TYPED(
    JS, JS_ERR_SYNTAX,
    "Unexpected token '%.*s'",
    (int)tok_len, &CODE[TOFF]
  );
}

static inline sv_ast_t *parse_dot_property_name(P) {
  if (!is_private_ident_like_tok(TOK)) {
    sv_parse_unexpected_token(p);
    return NULL;
  }

  sv_ast_t *name = mk_ident_from_tok(p);
  CONSUME();
  return name;
}

static sv_ast_t *parse_arrow_body(P) {
  if (NEXT() == TOK_LBRACE) return parse_block(p, true);
  return parse_assign(p);
}

static inline uint32_t node_src_end(P, sv_ast_t *node) {
  if (node && node->src_end > node->src_off) return node->src_end;
  return (uint32_t)(TOFF + TLEN);
}

static void sv_parse_stmt_list(P, sv_ast_list_t *out, bool stop_at_rbrace, bool directive_ctx) {
  bool strict_mode = p->lx.strict;
  bool saved_lexer_strict = p->lx.strict;
  bool in_directive_prologue = directive_ctx;
  p->lx.strict = strict_mode;

  for (;;) {
    NEXT();
    if (TOK == TOK_EOF) break;
    if (stop_at_rbrace && TOK == TOK_RBRACE) break;

    if (TOK == TOK_ERR) {
      sv_parse_unexpected_token(p);
      break;
    }

    sv_ast_t *stmt = parse_stmt(p);
    if (stmt) sv_ast_list_push(out, stmt);
    if (JS->thrown_exists) break;
    if (!in_directive_prologue) continue;
    if (!stmt || stmt->type == N_EMPTY) continue;
    
    if (!sv_ast_can_be_expression_statement(stmt)) {
      in_directive_prologue = false;
      continue;
    }
    
    if (sv_ast_is_use_strict(JS, stmt)) {
      strict_mode = true;
      p->lx.strict = true;
      continue;
    }
    in_directive_prologue = false;
  }
  p->lx.strict = saved_lexer_strict;
}

static sv_ast_t *parse_binding_pattern(P) {
  NEXT();
  if (TOK == TOK_LBRACKET) return parse_array(p);
  if (TOK == TOK_LBRACE) return parse_object(p);
  if (is_ident_like_tok(TOK)) {
    sv_ast_t *id = mk_ident_from_tok(p);
    sv_strict_check_binding_ident(p, id->str, id->len);
    CONSUME();
    return id;
  }
  CONSUME();
  return mk(N_EMPTY);
}

static void push_arrow_params_from_expr(sv_ast_t *fn, sv_ast_t *expr) {
  if (!fn || !expr) return;
  if (expr->type == N_SEQUENCE) {
    push_arrow_params_from_expr(fn, expr->left);
    push_arrow_params_from_expr(fn, expr->right);
    return;
  }
  if (expr->type == N_ASSIGN && expr->op == TOK_ASSIGN) {
    sv_ast_t *def = mk_plain(N_ASSIGN_PAT);
    def->left = expr->left;
    def->right = expr->right;
    def->src_off = expr->src_off;
    sv_ast_list_push(&fn->args, def);
    return;
  }
  if (expr->type == N_SPREAD) {
    sv_ast_t *rest = mk_plain(N_REST);
    rest->right = expr->right;
    rest->src_off = expr->src_off;
    sv_ast_list_push(&fn->args, rest);
    return;
  }
  sv_ast_list_push(&fn->args, expr);
}

static sv_tpl_cooked_t decode_template_segment(P, const uint8_t *in, size_t start, size_t end) {
  sv_tpl_cooked_t outv = { .str = NULL, .len = 0, .ok = true, .valid_cooked = true };
  size_t raw_len = (end > start) ? (end - start) : 0;
  if (raw_len == 0) {
    outv.str = "";
    return outv;
  }

  uint8_t *out = parse_arena_bump(raw_len);
  if (!out) {
    (void)SV_MKERR(JS, "oom");
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
    uint8_t c = in[i + 1];
    if (c >= '1' && c <= '9') { outv.valid_cooked = false; return outv; }
    if (c == '0' && i + 2 < end && in[i + 2] >= '0' && in[i + 2] <= '9') {
      outv.valid_cooked = false; return outv;
    }
    if (c == 'x' && !(i + 3 < end && is_xdigit(in[i + 2]) && is_xdigit(in[i + 3]))) {
      outv.valid_cooked = false; return outv;
    }
    if (c == 'u') {
      if (i + 2 < end && in[i + 2] == '{') {
        uint32_t cp = 0; size_t j = i + 3;
        while (j < end && is_xdigit(in[j])) { cp = (cp << 4) | unhex(in[j]); j++; }
        if (!(j < end && in[j] == '}' && j > i + 3 && cp <= 0x10FFFF)) {
          outv.valid_cooked = false; return outv;
        }
      } else if (
        !(i + 5 < end && is_xdigit(in[i + 2]) 
        && is_xdigit(in[i + 3]) 
        && is_xdigit(in[i + 4]) 
        && is_xdigit(in[i + 5]))) { outv.valid_cooked = false; return outv; }
    }
    i += 1 + decode_escape(in, i, end, out, &out_len, '`');
  }

  outv.str = (const char *)out;
  outv.len = (uint32_t)out_len;
  return outv;
}

static sv_ast_t *try_parse_async_arrow(P) {
  uint8_t la = LA();
  uint32_t async_off = (uint32_t)TOFF;

  if (la == TOK_LPAREN) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    NEXT(); CONSUME();
    if (NEXT() == TOK_RPAREN) {
      CONSUME();
      if (LA() == TOK_ARROW) {
        NEXT(); CONSUME();
        sv_ast_t *fn = mk(N_FUNC);
        fn->flags = FN_ARROW | FN_ASYNC;
        fn->body = parse_arrow_body(p);
        fn->src_off = async_off;
        fn->src_end = node_src_end(p, fn->body);
        return fn;
      }
    }
    sv_lexer_restore_state(&p->lx, &saved);
    NEXT(); CONSUME();
    sv_ast_t *expr = parse_paren_expr(p);
    expect(p, TOK_RPAREN);
    if (LA() == TOK_ARROW) {
      NEXT(); CONSUME();
      sv_ast_t *fn = mk(N_FUNC);
      fn->flags = FN_ARROW | FN_ASYNC;
      push_arrow_params_from_expr(fn, expr);
      fn->body = parse_arrow_body(p);
      fn->src_off = async_off;
      fn->src_end = node_src_end(p, fn->body);
      return fn;
    }
    sv_lexer_restore_state(&p->lx, &saved);
    return NULL;
  }

  if (la == TOK_IDENTIFIER) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    NEXT(); CONSUME();
    sv_ast_t *id = mk_ident_from_tok(p);
    if (LA() == TOK_ARROW) {
      NEXT(); CONSUME();
      sv_ast_t *fn = mk(N_FUNC);
      fn->flags = FN_ARROW | FN_ASYNC;
      sv_ast_list_push(&fn->args, id);
      fn->body = parse_arrow_body(p);
      fn->src_off = async_off;
      fn->src_end = node_src_end(p, fn->body);
      return fn;
    }
    sv_lexer_restore_state(&p->lx, &saved);
    return NULL;
  }

  return NULL;
}

static sv_ast_t *parse_primary(P) {
  NEXT();

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
    [TOK_GLOBAL_THIS] = &&l_globalthis,
    [TOK_WINDOW]      = &&l_globalthis,
    [TOK_HASH]        = &&l_private_name,
  };

  if (TOK < TOK_MAX && dispatch[TOK])
    goto *dispatch[TOK];

  sv_parse_unexpected_token(p);
  return mk(N_EMPTY);

  l_number: {
    CONSUME();
    return mk_num(tod(TVAL));
  }

  l_string: {
    sv_ast_t *n = mk(N_STRING);
    sv_ast_set_string(n, sv_lexer_str_literal(&p->lx));
    CONSUME();
    return n;
  }

  l_bigint: {
    CONSUME();
    sv_ast_t *n = mk(N_BIGINT);
    n->str = tok_str(p);
    n->len = (uint32_t)TLEN;
    return n;
  }

  l_true:  { CONSUME(); sv_ast_t *n = mk(N_BOOL); n->num = 1; return n; }
  l_false: { CONSUME(); sv_ast_t *n = mk(N_BOOL); n->num = 0; return n; }
  l_null:  { CONSUME(); return mk(N_NULL); }
  l_undef: { CONSUME(); return mk(N_UNDEF); }
  l_this:  { CONSUME(); return mk(N_THIS); }
  l_globalthis: { CONSUME(); return mk(N_GLOBAL_THIS); }

  l_ident: {
    CONSUME();
    return mk_ident_from_tok(p);
  }

  l_paren: {
    uint32_t paren_off = (uint32_t)TOFF;
    CONSUME();
    if (NEXT() == TOK_RPAREN) {
      CONSUME();
      sv_ast_t *n = mk(N_UNDEF);
      n->flags |= FN_PAREN;
      n->src_off = paren_off;
      return n;
    }
    sv_ast_t *expr = parse_paren_expr(p);
    expect(p, TOK_RPAREN);
    expr->flags |= FN_PAREN;
    if (expr->type != N_FUNC && expr->type != N_CLASS) expr->src_off = paren_off;
    return expr;
  }

  l_array:  return parse_array(p);
  l_object: return parse_object(p);
  l_func:   { CONSUME(); return parse_func(p); }

  l_class: {
    uint32_t class_off = (uint32_t)TOFF;
    CONSUME();
    sv_ast_t *cls = parse_class(p);
    cls->src_off = class_off;
    return cls;
  }

  l_async: {
    uint32_t async_off = (uint32_t)TOFF;
    CONSUME();
    bool has_line_term = lookahead_crosses_line_terminator(p);
    if (!has_line_term && LA() == TOK_FUNC) {
      NEXT(); CONSUME();
      sv_ast_t *fn = parse_func(p);
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
    CONSUME();
    sv_ast_t *n = mk(N_TEMPLATE);
    const uint8_t *in = (const uint8_t *)&CODE[TOFF];
    size_t tpl_len = TLEN;
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
      s->aux = (const char *)&in[seg_start];
      s->aux_len = (uint32_t)(i - seg_start);
      sv_tpl_cooked_t cooked = decode_template_segment(p, in, seg_start, i);
      if (cooked.ok && cooked.valid_cooked) {
        s->str = cooked.str;
        s->len = cooked.len;
      } else if (cooked.ok) s->flags |= FN_INVALID_COOKED;
      
      sv_ast_list_push(&n->args, s);
      if (i >= tpl_len - 1 || in[i] != '$') break;

      i += 2;
      size_t expr_start = i;
      size_t expr_max_len = (
        tpl_len > 0 && expr_start < tpl_len - 1)
        ? (tpl_len - 1 - expr_start) : 0;

      sv_lexer_checkpoint_t cp;
      sv_lexer_push_source(&p->lx, &cp, (const char *)&in[expr_start], (ant_offset_t)expr_max_len);

      sv_ast_t *expr = parse_expr(p);
      sv_ast_list_push(&n->args, expr);

      if (NEXT() != TOK_RBRACE) {
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Unterminated template expression");
        sv_lexer_pop_source(&p->lx, &cp);
        return n;
      }
      CONSUME();

      size_t consumed_expr = (size_t)POS;
      sv_lexer_pop_source(&p->lx, &cp);

      if (consumed_expr == 0) {
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Unterminated template expression");
        return n;
      } i = expr_start + consumed_expr;
    }
    return n;
  }

  l_new: {
    CONSUME();

    if (NEXT() == TOK_DOT) {
      CONSUME();
      if (NEXT() == TOK_IDENTIFIER && TLEN == 6 && memcmp(tok_str(p), "target", 6) == 0) {
        CONSUME();
        return mk(N_NEW_TARGET);
      }
      sv_parse_unexpected_token(p);
      return mk(N_EMPTY);
    }

    sv_ast_t *n = mk(N_NEW);
    sv_ast_t *callee = parse_primary(p);
    
    for (;;) {
      uint8_t la = NEXT();
      if (la == TOK_DOT) {
        CONSUME();
        NEXT();
        sv_ast_t *mem = mk(N_MEMBER);
        mem->left = callee;
        mem->right = parse_dot_property_name(p);
        if (!mem->right) return mk(N_EMPTY);
        callee = mem;
      } else if (la == TOK_LBRACKET) {
        CONSUME();
        sv_ast_t *mem = mk(N_MEMBER);
        mem->left = callee;
        mem->right = parse_expr(p);
        mem->flags = 1;
        expect(p, TOK_RBRACKET);
        callee = mem;
      } else break;
    }
    n->left = callee;

    if (NEXT() == TOK_LPAREN) {
      CONSUME();
      while (NEXT() != TOK_RPAREN && TOK != TOK_EOF) {
        if (TOK == TOK_REST) {
          CONSUME();
          sv_ast_t *spread = mk(N_SPREAD);
          spread->right = parse_assign(p);
          sv_ast_list_push(&n->args, spread);
        } else sv_ast_list_push(&n->args, parse_assign(p));
        if (NEXT() == TOK_COMMA) CONSUME();
        else break;
      } expect(p, TOK_RPAREN);
    }
    return n;
  }

  l_typeof: {
    CONSUME();
    sv_ast_t *n = mk(N_TYPEOF);
    n->right = parse_unary(p);
    return n;
  }

  l_void: {
    CONSUME();
    sv_ast_t *n = mk(N_VOID);
    n->right = parse_unary(p);
    return n;
  }

  l_delete: {
    CONSUME();
    sv_ast_t *n = mk(N_DELETE);
    n->right = parse_unary(p);
    if (p->lx.strict && n->right && n->right->type == N_IDENT) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "cannot delete bindings in strict mode");
      return mk(N_EMPTY);
    }
    return n;
  }

  l_yield: {
    CONSUME();
    sv_ast_t *n = mk(N_YIELD);
    if (NEXT() == TOK_MUL) {
      CONSUME();
      n->flags = 1;
    }
    if (TOK != TOK_SEMICOLON && TOK != TOK_RBRACE &&
        TOK != TOK_RPAREN && TOK != TOK_RBRACKET &&
        TOK != TOK_EOF && TOK != TOK_COMMA)
      n->right = parse_assign(p);
    return n;
  }

  l_await: {
    CONSUME();
    sv_ast_t *n = mk(N_AWAIT);
    n->right = parse_unary(p);
    return n;
  }

  l_super: {
    CONSUME();
    return mk_ident("super", 5);
  }

  l_spread: {
    CONSUME();
    sv_ast_t *n = mk(N_SPREAD);
    n->right = parse_assign(p);
    return n;
  }

  l_import_expr: {
    CONSUME();
    if (NEXT() != TOK_LPAREN) return mk_ident("import", 6);
    CONSUME();
    sv_ast_t *n = mk(N_IMPORT);
    n->right = parse_expr(p);
    expect(p, TOK_RPAREN);
    return n;
  }

  l_regex: {
    CONSUME();
    ant_offset_t pattern_start = (TOK == TOK_DIV_ASSIGN) ? (TOFF + 1) : POS;
    if (TOK == TOK_DIV_ASSIGN) POS = pattern_start;
    bool in_class = false;

    while (POS < CLEN) {
      char c = CODE[POS];
      if (c == '\\' && POS + 1 < CLEN) {
        POS += 2;
        continue;
      }
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      else if (c == '/' && !in_class) break;
      POS++;
    }

    ant_offset_t pattern_end = POS;
    if (POS < CLEN) POS++;

    ant_offset_t flags_start = POS;
    while (POS < CLEN) {
      char c = CODE[POS];
      if (
        c == 'd' || c == 'g' || c == 'i' || c == 'm' ||
        c == 's' || c == 'u' || c == 'v' || c == 'y') POS++;
      else break;
    }
    
    ant_offset_t flags_end = POS;
    sv_ast_t *n = mk(N_REGEXP);
    
    n->str = &CODE[pattern_start];
    n->len = (uint32_t)(pattern_end - pattern_start);
    n->aux = &CODE[flags_start];
    n->aux_len = (uint32_t)(flags_end - flags_start);
    CONSUMED = 1;
    return n;
  }

  l_private_name: {
    CONSUME();
    NEXT();
    if (!is_private_ident_like_tok(TOK)) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "private field name expected");
      return mk(N_EMPTY);
    }
    sv_ast_t *n = mk_private_ident_from_tok(p);
    CONSUME();
    return n;
  }
}

static sv_ast_t *parse_array(P) {
  CONSUME();
  sv_ast_t *n = mk(N_ARRAY);
  while (NEXT() != TOK_RBRACKET && TOK != TOK_EOF) {
    if (TOK == TOK_COMMA) {
      CONSUME();
      sv_ast_list_push(&n->args, mk(N_EMPTY));
      continue;
    }
    if (TOK == TOK_REST) {
      CONSUME();
      sv_ast_t *spread = mk(N_SPREAD);
      spread->right = parse_assign(p);
      sv_ast_list_push(&n->args, spread);
    } else {
      sv_ast_list_push(&n->args, parse_assign(p));
    }
    if (NEXT() == TOK_COMMA) CONSUME();
    else break;
  }
  expect(p, TOK_RBRACKET);
  return n;
}

static bool validate_accessor_params(P, sv_ast_t *fn, uint16_t flags) {
  if (!(flags & (FN_GETTER | FN_SETTER)) || !fn) return true;

  if ((flags & FN_GETTER) && fn->args.count != 0) {
    SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Getter must not have parameters");
    return false;
  }

  if (
    (flags & FN_SETTER) && (
    fn->args.count != 1 ||
    (fn->args.count == 1 && fn->args.items[0] && fn->args.items[0]->type == N_REST))
  ) {
    SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Setter must have exactly one non-rest parameter");
    return false;
  }

  return true;
}

static sv_ast_t *parse_object(P) {
  CONSUME();
  sv_ast_t *n = mk(N_OBJECT);
  bool proto_set = false;
  while (NEXT() != TOK_RBRACE && TOK != TOK_EOF) {
    sv_ast_t *prop = mk(N_PROPERTY);

    if (TOK == TOK_REST) {
      CONSUME();
      sv_ast_t *spread = mk(N_SPREAD);
      spread->right = parse_assign(p);
      sv_ast_list_push(&n->args, spread);
      if (NEXT() == TOK_COMMA) CONSUME();
      continue;
    }

    if (TOK == TOK_LBRACKET) {
      CONSUME();
      prop->left = parse_assign(p);
      expect(p, TOK_RBRACKET);
      prop->flags |= FN_COMPUTED;
    }
    else if (TOK == TOK_MUL) {
      prop->flags |= FN_GENERATOR;
      CONSUME();
      NEXT();

      if (TOK == TOK_LBRACKET) {
        CONSUME();
        prop->left = parse_assign(p);
        expect(p, TOK_RBRACKET);
        prop->flags |= FN_COMPUTED;
      } else if (TOK == TOK_NUMBER) {
        CONSUME();
        prop->left = mk_num(tod(TVAL));
      } else if (TOK == TOK_STRING) {
        prop->left = mk(N_STRING);
        sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
        CONSUME();
      } else {
        prop->left = mk_ident_from_tok(p);
        CONSUME();
      }

      prop->right = parse_func(p);
      prop->right->flags |= FN_GENERATOR | FN_METHOD;
      prop->right->src_off = prop->src_off;
      sv_ast_list_push(&n->args, prop);
      if (NEXT() == TOK_COMMA) CONSUME();
      continue;
    }
    else if (
      (TLEN == 3 && memcmp(tok_str(p), "get", 3) == 0) ||
      (TLEN == 3 && memcmp(tok_str(p), "set", 3) == 0)) {
      uint8_t gs = tok_str(p)[0];
      uint8_t la = LA();
      if (la != TOK_COLON && la != TOK_LPAREN && la != TOK_COMMA &&
          la != TOK_RBRACE) {
        CONSUME();
        prop->flags |= (gs == 'g') ? FN_GETTER : FN_SETTER;
        NEXT();

        if (TOK == TOK_LBRACKET) {
          CONSUME();
          prop->left = parse_assign(p);
          expect(p, TOK_RBRACKET);
          prop->flags |= FN_COMPUTED;
        } else if (TOK == TOK_NUMBER) {
          CONSUME();
          prop->left = mk_num(tod(TVAL));
        } else if (TOK == TOK_STRING) {
          prop->left = mk(N_STRING);
          sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
          CONSUME();
        } else {
          prop->left = mk_ident_from_tok(p);
          CONSUME();
        }

        prop->right = parse_func(p);
        if (!validate_accessor_params(p, prop->right, prop->flags)) return n;
        prop->right->flags |= FN_METHOD;
        prop->right->src_off = prop->src_off;
        sv_ast_list_push(&n->args, prop);
        if (NEXT() == TOK_COMMA) CONSUME();
        continue;
      }
      prop->left = mk_ident_from_tok(p);
      CONSUME();
    }
    else if (TOK == TOK_ASYNC) {
      uint8_t la = LA();
      if (la != TOK_COLON && la != TOK_LPAREN && la != TOK_COMMA && la != TOK_RBRACE) {
        CONSUME();
        prop->flags |= FN_ASYNC;
        NEXT();

        if (TOK == TOK_MUL) {
          prop->flags |= FN_GENERATOR;
          CONSUME();
          NEXT();
        }

        if (TOK == TOK_LBRACKET) {
          CONSUME();
          prop->left = parse_assign(p);
          expect(p, TOK_RBRACKET);
          prop->flags |= FN_COMPUTED;
        } else if (TOK == TOK_NUMBER) {
          CONSUME();
          prop->left = mk_num(tod(TVAL));
        } else if (TOK == TOK_STRING) {
          prop->left = mk(N_STRING);
          sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
          CONSUME();
        } else {
          prop->left = mk_ident_from_tok(p);
          CONSUME();
        }

        prop->right = parse_func(p);
        prop->right->flags |= FN_ASYNC | FN_METHOD;
        if (prop->flags & FN_GENERATOR)
          prop->right->flags |= FN_GENERATOR;
        prop->right->src_off = prop->src_off;
        sv_ast_list_push(&n->args, prop);
        if (NEXT() == TOK_COMMA) CONSUME();
        continue;
      }
      prop->left = mk_ident_from_tok(p);
      CONSUME();
    }
    else if (TOK == TOK_NUMBER) {
      CONSUME();
      prop->left = mk_num(tod(TVAL));
    }
    else if (TOK == TOK_STRING) {
      prop->left = mk(N_STRING);
      sv_ast_set_string(prop->left, sv_lexer_str_literal(&p->lx));
      CONSUME();
    }
    else {
      prop->left = mk_ident_from_tok(p);
      CONSUME();
    }

    if (NEXT() == TOK_COLON) {
      CONSUME();
      prop->flags |= FN_COLON;
      if (prop->left && prop->left->type == N_IDENT &&
          prop->left->len == 9 && memcmp(prop->left->str, "__proto__", 9) == 0) {
        if (proto_set) { SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Duplicate __proto__ fields are not allowed in object literals"); return n; }
        proto_set = true;
      }
      prop->right = parse_assign(p);
    } else if (TOK == TOK_LPAREN) {
      prop->right = parse_func(p);
      prop->right->flags |= FN_METHOD;
      prop->right->src_off = prop->src_off;
    } else {
      prop->right = mk_ident(prop->left->str, prop->left->len);
      if (NEXT() == TOK_ASSIGN) {
        CONSUME();
        sv_ast_t *def = mk(N_ASSIGN);
        def->op = TOK_ASSIGN;
        def->left = prop->right;
        def->right = parse_assign(p);
        prop->right = def;
      }
    }

    sv_ast_list_push(&n->args, prop);
    if (NEXT() == TOK_COMMA) CONSUME();
    else break;
  }
  expect(p, TOK_RBRACE);
  return n;
}

static sv_ast_t *parse_call(P) {
  sv_ast_t *n = parse_primary(p);

  for (;;) {
    uint8_t la = NEXT();
    if (la == TOK_LPAREN) {
      CONSUME();
      sv_ast_t *call = mk(N_CALL);
      call->left = n;
      while (NEXT() != TOK_RPAREN && TOK != TOK_EOF) {
        if (TOK == TOK_REST) {
          CONSUME();
          sv_ast_t *spread = mk(N_SPREAD);
          spread->right = parse_assign(p);
          sv_ast_list_push(&call->args, spread);
        } else {
          sv_ast_list_push(&call->args, parse_assign(p));
        }
        if (NEXT() == TOK_COMMA) CONSUME();
        else break;
      }
      expect(p, TOK_RPAREN);
      n = call;
    } else if (la == TOK_DOT) {
      CONSUME();
      NEXT();
      sv_ast_t *mem = mk(N_MEMBER);
      mem->left = n;
      if (TOK == TOK_HASH) {
        CONSUME();
        NEXT();
        if (!is_private_ident_like_tok(TOK)) {
          SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "private field name expected");
          return mk(N_EMPTY);
        }
        mem->right = mk_private_ident_from_tok(p);
        CONSUME();
      } else {
        mem->right = parse_dot_property_name(p);
        if (!mem->right) return mk(N_EMPTY);
      }
      n = mem;
    } else if (la == TOK_LBRACKET) {
      CONSUME();
      sv_ast_t *mem = mk(N_MEMBER);
      mem->left = n;
      mem->right = parse_expr(p);
      mem->flags = 1;
      expect(p, TOK_RBRACKET);
      n = mem;
    } else if (la == TOK_OPTIONAL_CHAIN) {
      CONSUME();
      sv_ast_t *opt = mk(N_OPTIONAL);
      opt->left = n;
      if (NEXT() == TOK_LBRACKET) {
        CONSUME();
        opt->right = parse_expr(p);
        opt->flags = 1;
        expect(p, TOK_RBRACKET);
      } else if (TOK == TOK_LPAREN) {
        sv_ast_t *call = mk(N_CALL);
        call->left = opt;
        CONSUME();
        while (NEXT() != TOK_RPAREN && TOK != TOK_EOF) {
          sv_ast_list_push(&call->args, parse_assign(p));
          if (NEXT() == TOK_COMMA) CONSUME();
          else break;
        }
        expect(p, TOK_RPAREN);
        n = call;
        continue;
      } else if (TOK == TOK_HASH) {
        CONSUME();
        NEXT();
        if (!is_private_ident_like_tok(TOK)) {
          SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "private field name expected");
          return mk(N_EMPTY);
        }
        opt->right = mk_private_ident_from_tok(p);
        CONSUME();
      } else {
        opt->right = parse_dot_property_name(p);
        if (!opt->right) return mk(N_EMPTY);
      }
      n = opt;
    } else if (la == TOK_TEMPLATE) {
      sv_ast_t *tagged = mk(N_TAGGED_TEMPLATE);
      tagged->left = n;
      tagged->right = parse_primary(p);
      n = tagged;
    } else break;
  }
  return n;
}

static sv_ast_t *parse_postfix(P) {
  sv_ast_t *n = parse_call(p);
  uint8_t la = NEXT();
  if ((la == TOK_POSTINC || la == TOK_POSTDEC) && !HAD_NEWLINE) {
    if (sv_is_strict_restricted_assign_target(p, n)) {
      SV_MKERR_TYPED(
        JS, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    CONSUME();
    sv_ast_t *u = mk(N_UPDATE);
    u->op = la;
    u->right = n;
    return u;
  }
  return n;
}

static sv_ast_t *parse_unary(P) {
  uint8_t la = NEXT();
  if (la == TOK_NOT || la == TOK_TILDA ||
      la == TOK_UPLUS || la == TOK_UMINUS ||
      la == TOK_PLUS || la == TOK_MINUS) {
    CONSUME();
    sv_ast_t *n = mk(N_UNARY);
    n->op = (la == TOK_PLUS) ? TOK_UPLUS :
            (la == TOK_MINUS) ? TOK_UMINUS : la;
    n->right = parse_unary(p);
    if (NEXT() == TOK_EXP) {
      SV_MKERR_TYPED(
        JS, JS_ERR_SYNTAX,
       "Unary operator used immediately before exponentiation expression. "
       "Parenthesis must be used to disambiguate operator precedence");
      return mk(N_EMPTY);
    }
    return n;
  }
  if (la == TOK_POSTINC || la == TOK_POSTDEC) {
    CONSUME();
    sv_ast_t *target = parse_unary(p);
    if (sv_is_strict_restricted_assign_target(p, target)) {
      SV_MKERR_TYPED(
        JS, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    sv_ast_t *n = mk(N_UPDATE);
    n->op = la;
    n->right = target;
    n->flags = 1;
    return n;
  }
  if (la == TOK_THROW) {
    CONSUME();
    sv_ast_t *n = mk(N_THROW);
    n->right = parse_assign(p);
    return n;
  }
  return parse_postfix(p);
}

static sv_ast_t *parse_binary(P, int min_prec) {
  sv_ast_t *left = parse_unary(p);

  for (;;) {
    uint8_t op = NEXT();
    if (op >= TOK_MAX) break;
    if (op == TOK_IN && p->no_in) break;
    int prec = prec_table[op];
    if (prec == 0 || prec < min_prec) break;
    CONSUME();
    int next_prec = (op == TOK_EXP) ? prec : prec + 1;
    sv_ast_t *right = parse_binary(p, next_prec);
    sv_ast_t *bin = mk(N_BINARY);
    bin->op = op;
    bin->left = left;
    bin->right = right;
    left = bin;
  }
  return left;
}

static sv_ast_t *parse_ternary(P) {
  sv_ast_t *cond = parse_binary(p, 1);
  if (NEXT() == TOK_Q) {
    CONSUME();
    sv_ast_t *n = mk(N_TERNARY);
    n->cond = cond;
    n->left = parse_assign(p);
    expect(p, TOK_COLON);
    n->right = parse_assign(p);
    return n;
  }
  return cond;
}

static bool is_assign_op(uint8_t tok) {
  return tok >= TOK_ASSIGN && tok <= TOK_NULLISH_ASSIGN;
}

static sv_ast_t *parse_assign(P) {
  sv_ast_t *left = parse_ternary(p);
  uint8_t op = NEXT();
  if (op == TOK_ARROW) {
    if (HAD_NEWLINE) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Line terminator not allowed before arrow");
      return mk(N_EMPTY);
    }
    CONSUME();
    sv_ast_t *fn = mk(N_FUNC);
    fn->flags = FN_ARROW;
    fn->src_off = left->src_off;
    if (left->type == N_IDENT) {
      sv_ast_list_push(&fn->args, left);
    } else if (left->flags & FN_PAREN) {
      if (left->type != N_UNDEF)
        push_arrow_params_from_expr(fn, left);
    } else {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Malformed arrow function parameter list");
      return mk(N_EMPTY);
    }
    fn->body = parse_arrow_body(p);
    fn->src_end = node_src_end(p, fn->body);
    return fn;
  }
  if (is_assign_op(op)) {
    if (left->type == N_NEW_TARGET) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Invalid left-hand side in assignment");
      return mk(N_EMPTY);
    }
    if ((left->flags & FN_PAREN) && op == TOK_ASSIGN &&
        (left->type == N_OBJECT || left->type == N_OBJECT_PAT ||
         left->type == N_ARRAY || left->type == N_ARRAY_PAT)) {
      SV_MKERR_TYPED(
        JS, JS_ERR_SYNTAX,
        "Invalid destructuring assignment target");
      return mk(N_EMPTY);
    }
    if (sv_is_strict_restricted_assign_target(p, left)) {
      SV_MKERR_TYPED(
        JS, JS_ERR_SYNTAX,
        "cannot modify eval or arguments in strict mode");
      return mk(N_EMPTY);
    }
    CONSUME();
    sv_ast_t *n = mk(N_ASSIGN);
    n->op = op;
    n->left = left;
    n->right = parse_assign(p);
    return n;
  }
  return left;
}

static sv_ast_t *parse_expr(P) {
  sv_ast_t *left = parse_assign(p);
  while (NEXT() == TOK_COMMA) {
    CONSUME();
    sv_ast_t *n = mk(N_SEQUENCE);
    n->left = left;
    n->right = parse_assign(p);
    left = n;
  }
  return left;
}

static sv_ast_t *parse_paren_expr(P) {
  sv_ast_t *left = parse_assign(p);
  while (NEXT() == TOK_COMMA) {
    CONSUME();
    if (NEXT() == TOK_RPAREN && LA() == TOK_ARROW) break;

    sv_ast_t *n = mk(N_SEQUENCE);
    n->left = left;
    n->right = parse_assign(p);
    left = n;
  }
  return left;
}

bool ast_references_arguments(const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_IDENT && node->len == 9 && memcmp(node->str, "arguments", 9) == 0)
    return true;
    
  if (node->type == N_FUNC && !(node->flags & FN_ARROW)) {
    for (int i = 0; i < node->args.count; i++)
      if (ast_references_arguments(node->args.items[i])) return true;
    return false;
  }
  
  if (ast_references_arguments(node->left))            return true;
  if (ast_references_arguments(node->right))           return true;
  if (ast_references_arguments(node->body))            return true;
  if (ast_references_arguments(node->catch_body))      return true;
  if (ast_references_arguments(node->finally_body))    return true;
  for (int i = 0; i < node->args.count; i++)
    if (ast_references_arguments(node->args.items[i])) return true;
    
  return false;
}

static bool ast_references_new_target(const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_NEW_TARGET) return true;
  if (node->type == N_FUNC && !(node->flags & FN_ARROW)) return false;

  if (ast_references_new_target(node->left))         return true;
  if (ast_references_new_target(node->right))        return true;
  if (ast_references_new_target(node->cond))         return true;
  if (ast_references_new_target(node->body))         return true;
  if (ast_references_new_target(node->catch_body))   return true;
  if (ast_references_new_target(node->finally_body)) return true;
  if (ast_references_new_target(node->catch_param))  return true;
  if (ast_references_new_target(node->init))         return true;
  if (ast_references_new_target(node->update))       return true;
  
  for (int i = 0; i < node->args.count; i++)
    if (ast_references_new_target(node->args.items[i])) return true;
    
  return false;
}

static sv_ast_t *parse_func(P) {
  sv_ast_t *fn = mk(N_FUNC);

  if (NEXT() == TOK_MUL) {
    CONSUME();
    fn->flags |= FN_GENERATOR;
  }

  if (is_ident_like_tok(NEXT())) {
    fn->str = tok_ident_str(p, &fn->len);
    sv_strict_check_binding_ident(p, fn->str, fn->len);
    CONSUME();
  }

  expect(p, TOK_LPAREN);
  while (NEXT() != TOK_RPAREN && TOK != TOK_EOF) {
    if (TOK == TOK_REST) {
      CONSUME();
      sv_ast_t *rest = mk(N_REST);
      rest->right = parse_binding_pattern(p);
      sv_ast_list_push(&fn->args, rest);
      break;
    }
    sv_ast_t *param = parse_binding_pattern(p);
    if (NEXT() == TOK_ASSIGN) {
      CONSUME();
      sv_ast_t *def = mk(N_ASSIGN_PAT);
      def->left = param;
      def->right = parse_assign(p);
      param = def;
    }
    sv_ast_list_push(&fn->args, param);
    if (NEXT() == TOK_COMMA) {
      CONSUME();
      if (NEXT() == TOK_RPAREN) break;
    } else break;
  }
  expect(p, TOK_RPAREN);

  fn->body = parse_block(p, true);
  fn->src_end = (uint32_t)(TOFF + TLEN);
  if (!(fn->flags & FN_ARROW) && ast_references_arguments(fn->body))
    fn->flags |= FN_USES_ARGS;
  if (!(fn->flags & FN_ARROW) && ast_references_new_target(fn->body))
    fn->flags |= FN_USES_NEW_TARGET;
  return fn;
}

static sv_ast_t *parse_class(P) {
  sv_ast_t *cls = mk(N_CLASS);

  if (is_ident_like_tok(NEXT()) &&
      !(TLEN == 7 && memcmp(tok_str(p), "extends", 7) == 0)) {
    cls->str = tok_ident_str(p, &cls->len);
    CONSUME();
  }

  if (NEXT() == TOK_IDENTIFIER && TLEN == 7 && memcmp(tok_str(p), "extends", 7) == 0) {
    CONSUME();
    cls->left = parse_assign(p);
  }

  expect(p, TOK_LBRACE);
  while (NEXT() != TOK_RBRACE && TOK != TOK_EOF) {
    if (TOK == TOK_SEMICOLON) { CONSUME(); continue; }

    uint8_t flags = 0;

    if (
      TOK == TOK_STATIC &&
      LA() != TOK_LPAREN &&
      LA() != TOK_ASSIGN &&
      LA() != TOK_SEMICOLON &&
      LA() != TOK_RBRACE
    ) {
      flags |= FN_STATIC;
      CONSUME();
      NEXT();

      if (TOK == TOK_LBRACE) {
        bool saved_strict = p->lx.strict;
        p->lx.strict = true;
        sv_ast_t *block = parse_block(p, false);
        p->lx.strict = saved_strict;
        block->type = N_STATIC_BLOCK;
        block->flags = FN_STATIC | FN_CLASS_BODY;
        sv_ast_list_push(&cls->args, block);
        continue;
      }
    }

    sv_ast_t *method = mk(N_METHOD);
    uint32_t method_src_off = (uint32_t)TOFF;

    if (TOK == TOK_ASYNC && LA() != TOK_LPAREN) {
      flags |= FN_ASYNC;
      CONSUME();
      NEXT();
    }

    if (TOK == TOK_MUL) {
      flags |= FN_GENERATOR;
      CONSUME();
      NEXT();
    }

    if ((TLEN == 3 && memcmp(tok_str(p), "get", 3) == 0) ||
        (TLEN == 3 && memcmp(tok_str(p), "set", 3) == 0)) {
      uint8_t la = LA();
      if (la != TOK_LPAREN && la != TOK_ASSIGN && la != TOK_SEMICOLON && la != TOK_RBRACE) {
        flags |= (tok_str(p)[0] == 'g') ? FN_GETTER : FN_SETTER;
        CONSUME();
        NEXT();
      }
    }

    if (TOK == TOK_LBRACKET) {
      CONSUME();
      method->left = parse_assign(p);
      expect(p, TOK_RBRACKET);
      flags |= FN_COMPUTED;
    } else if (TOK == TOK_HASH) {
      CONSUME();
      NEXT();
      if (!is_private_ident_like_tok(TOK)) {
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "private field name expected");
        return mk(N_EMPTY);
      }
      method->left = mk_private_ident_from_tok(p);
      CONSUME();
    } else {
      method->left = mk_ident_from_tok(p);
      CONSUME();
    }

    method->flags = flags;

    if (!(flags & FN_STATIC) && (flags & FN_GENERATOR) &&
        method->left && method->left->type == N_IDENT &&
        method->left->len == 11 &&
        memcmp(method->left->str, "constructor", 11) == 0) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Class constructor may not be a generator");
      return cls;
    }

    if (NEXT() == TOK_LPAREN) {
      bool saved_strict = p->lx.strict;
      p->lx.strict = true;
      method->right = parse_func(p);
      p->lx.strict = saved_strict;
      if (!validate_accessor_params(p, method->right, method->flags)) return cls;
      method->right->flags |= (flags & (FN_ASYNC | FN_GENERATOR)) | FN_METHOD | FN_CLASS_BODY;
      method->right->src_off = method_src_off;
    } else if (TOK == TOK_ASSIGN) {
      CONSUME();
      method->right = parse_assign(p);
      if (NEXT() == TOK_SEMICOLON) CONSUME();
    } else {
      method->right = mk(N_UNDEF);
      if (TOK == TOK_SEMICOLON) CONSUME();
    }

    sv_ast_list_push(&cls->args, method);
  }
  expect(p, TOK_RBRACE);
  cls->src_end = (uint32_t)(TOFF + TLEN);
  return cls;
}

static sv_ast_t *parse_block(P, bool directive_ctx) {
  expect(p, TOK_LBRACE);
  sv_ast_t *block = mk(N_BLOCK);
  sv_parse_stmt_list(p, &block->args, true, directive_ctx);
  if (JS->thrown_exists) return block;
  expect(p, TOK_RBRACE);
  return block;
}

static sv_ast_t *parse_var_decl(P, sv_var_kind_t kind, bool allow_uninit_const) {
  sv_ast_t *var = mk(N_VAR);
  var->var_kind = kind;

  do {
    NEXT();
    sv_ast_t *decl = mk(N_VARDECL);

    if (TOK == TOK_LBRACKET) {
      decl->left = parse_array(p);
    } else if (TOK == TOK_LBRACE) {
      decl->left = parse_object(p);
    } else if (TOK == TOK_ERR) {
      sv_parse_unexpected_token(p);
      return var;
    } else {
      decl->left = mk_ident_from_tok(p);
      sv_strict_check_binding_ident(p, decl->left->str, decl->left->len);
      CONSUME();
    }
    if (NEXT() == TOK_ASSIGN) {
      CONSUME();
      decl->right = parse_assign(p);
    } else if ((kind == SV_VAR_CONST || kind == SV_VAR_USING || kind == SV_VAR_AWAIT_USING) && !allow_uninit_const) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Missing initializer in const declaration");
    }
    sv_ast_list_push(&var->args, decl);
  } while (NEXT() == TOK_COMMA && (CONSUME(), 1));

  return var;
}

static sv_ast_t *skip_import_stmt(P) {
  while (NEXT() != TOK_SEMICOLON && TOK != TOK_EOF) CONSUME();
  if (TOK == TOK_SEMICOLON) CONSUME();
  return mk(N_EMPTY);
}

enum {
  IMPORT_BIND_DEFAULT   = 1 << 0,
  IMPORT_BIND_NAMESPACE = 1 << 1,
};

static inline void import_decl_add_binding(
  sv_ast_t *decl,
  const char *import_name, uint32_t import_len,
  const char *local_name, uint32_t local_len,
  uint8_t flags
) {
  sv_ast_t *spec = mk_plain(N_IMPORT_SPEC);
  spec->flags = flags;
  if (import_name)
    spec->left = mk_ident(import_name, import_len);
  spec->right = mk_ident(local_name, local_len);
  sv_ast_list_push(&decl->args, spec);
}

static sv_ast_t *parse_import_stmt(P) {
  static const char default_name[] = "default";
  sv_ast_t *decl = mk(N_IMPORT_DECL);

  if (NEXT() == TOK_STRING) {
    sv_ast_t *spec = mk(N_STRING);
    sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
    CONSUME();
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    decl->right = spec;
    return decl;
  }

  bool saw_clause = false;

  if (is_ident_like_tok(NEXT())) {
    saw_clause = true;
    uint32_t local_len = 0;
    const char *local_name = tok_ident_str(p, &local_len);
    import_decl_add_binding(
      decl,
      default_name, (uint32_t)(sizeof(default_name) - 1),
      local_name, local_len,
      IMPORT_BIND_DEFAULT
    );
    CONSUME();
    if (NEXT() == TOK_COMMA) CONSUME();
    else goto parse_from;
  }

  if (NEXT() == TOK_MUL) {
    saw_clause = true;
    CONSUME();
    expect(p, TOK_AS);
    NEXT();
    if (!is_ident_like_tok(TOK)) return skip_import_stmt(p);
    uint32_t ns_len = 0;
    const char *ns_name = tok_ident_str(p, &ns_len);
    import_decl_add_binding(
      decl,
      NULL, 0,
      ns_name, ns_len,
      IMPORT_BIND_NAMESPACE
    );
    CONSUME();
  } else if (NEXT() == TOK_LBRACE) {
    saw_clause = true;
    CONSUME();
    while (NEXT() != TOK_RBRACE && TOK != TOK_EOF) {
      const char *import_name;
      uint32_t import_len;

      if (TOK == TOK_STRING) {
        sv_lex_string_t s = sv_lexer_str_literal(&p->lx);
        import_name = s.str;
        import_len = s.len;
        CONSUME();
      } else if (!(TOK >= TOK_IDENTIFIER && TOK < TOK_IDENT_LIKE_END)) {
        CONSUME();
        continue;
      } else {
        import_name = tok_ident_str(p, &import_len);
        CONSUME();
      }

      const char *local_name = import_name;
      uint32_t local_len = import_len;

      if (NEXT() == TOK_AS) {
        CONSUME();
        NEXT();
        if (!is_ident_like_tok(TOK)) return skip_import_stmt(p);
        local_name = tok_ident_str(p, &local_len);
        CONSUME();
      }

      import_decl_add_binding(decl, import_name, import_len, local_name, local_len, 0);

      if (NEXT() == TOK_COMMA) {
        CONSUME();
        if (NEXT() == TOK_RBRACE) break;
      }
    }
    expect(p, TOK_RBRACE);
  }

parse_from:
  if (!saw_clause) return skip_import_stmt(p);
  expect(p, TOK_FROM);
  if (NEXT() != TOK_STRING) return skip_import_stmt(p);

  sv_ast_t *spec = mk(N_STRING);
  sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
  CONSUME();
  decl->right = spec;

  if (NEXT() == TOK_SEMICOLON) CONSUME();
  return decl;
}

static sv_ast_t *parse_export_name(P) {
  NEXT();
  if (TOK == TOK_STRING) {
    sv_lex_string_t s = sv_lexer_str_literal(&p->lx);
    sv_ast_t *name = mk(N_IDENT);
    name->str = s.str;
    name->len = s.len;
    CONSUME();
    return name;
  }

  if (!(TOK >= TOK_IDENTIFIER && TOK < TOK_IDENT_LIKE_END)) {
    sv_parse_unexpected_token(p);
    return NULL;
  }
  sv_ast_t *name = mk_ident_from_tok(p);
  CONSUME();
  return name;
}

static sv_ast_t *parse_export_stmt(P) {
  sv_ast_t *decl = mk(N_EXPORT);
  NEXT();

  if (TOK == TOK_DEFAULT) {
    CONSUME();
    decl->flags |= EX_DEFAULT;

    if (NEXT() == TOK_ASYNC && LA() == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
      uint32_t async_off = (uint32_t)TOFF;
      CONSUME();
      NEXT(); CONSUME();
      decl->left = parse_func(p);
      decl->left->flags |= FN_ASYNC;
      decl->left->src_off = async_off;
      if (NEXT() == TOK_SEMICOLON) CONSUME();
      return decl;
    }
    if (TOK == TOK_FUNC) {
      CONSUME();
      decl->left = parse_func(p);
      if (NEXT() == TOK_SEMICOLON) CONSUME();
      return decl;
    }
    if (TOK == TOK_CLASS) {
      uint32_t class_off = (uint32_t)TOFF;
      CONSUME();
      decl->left = parse_class(p);
      decl->left->src_off = class_off;
      if (NEXT() == TOK_SEMICOLON) CONSUME();
      return decl;
    }

    decl->left = parse_assign(p);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return decl;
  }

  if (TOK == TOK_ASYNC && LA() == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
    decl->flags |= EX_DECL;
    uint32_t async_off = (uint32_t)TOFF;
    CONSUME();
    NEXT(); CONSUME();
    decl->left = parse_func(p);
    decl->left->flags |= FN_ASYNC;
    decl->left->src_off = async_off;
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "exported function declarations require a name");
    return decl;
  }

  if (TOK == TOK_FUNC) {
    decl->flags |= EX_DECL;
    CONSUME();
    decl->left = parse_func(p);
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "exported function declarations require a name");
    return decl;
  }
  if (TOK == TOK_CLASS) {
    decl->flags |= EX_DECL;
    uint32_t class_off = (uint32_t)TOFF;
    CONSUME();
    decl->left = parse_class(p);
    decl->left->src_off = class_off;
    if (!decl->left->str || decl->left->len == 0)
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "exported class declarations require a name");
    return decl;
  }

  if (TOK == TOK_VAR || TOK == TOK_LET || TOK == TOK_CONST) {
    decl->flags |= EX_DECL;
    sv_var_kind_t kind = (
      TOK == TOK_VAR) ? SV_VAR_VAR :
      (TOK == TOK_LET) ? SV_VAR_LET : SV_VAR_CONST;
    CONSUME();
    decl->left = parse_var_decl(p, kind, false);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return decl;
  }

  if (TOK == TOK_LBRACE) {
    decl->flags |= EX_NAMED;
    CONSUME();
    while (NEXT() != TOK_RBRACE && TOK != TOK_EOF) {
      sv_ast_t *local_name = parse_export_name(p);
      if (!local_name) return decl;

      sv_ast_t *export_name = local_name;
      if (NEXT() == TOK_AS) {
        CONSUME();
        export_name = parse_export_name(p);
        if (!export_name) return decl;
      }

      sv_ast_t *spec = mk(N_IMPORT_SPEC);
      spec->left = local_name;
      spec->right = export_name;
      sv_ast_list_push(&decl->args, spec);

      if (NEXT() == TOK_COMMA) {
        CONSUME();
        if (NEXT() == TOK_RBRACE) break;
      } else {
        break;
      }
    }
    expect(p, TOK_RBRACE);

    if (NEXT() == TOK_FROM) {
      CONSUME();
      if (NEXT() != TOK_STRING) {
        sv_parse_unexpected_token(p);
        return decl;
      }
      sv_ast_t *spec = mk(N_STRING);
      sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
      decl->right = spec;
      decl->flags |= EX_FROM;
      CONSUME();
    }

    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return decl;
  }

  if (TOK == TOK_MUL) {
    decl->flags |= EX_STAR;
    CONSUME();

    if (NEXT() == TOK_AS) {
      decl->flags |= EX_NAMESPACE;
      CONSUME();

      sv_ast_t *name = parse_export_name(p);
      if (!name) return decl;

      sv_ast_t *spec = mk(N_IMPORT_SPEC);
      spec->right = name;
      sv_ast_list_push(&decl->args, spec);
    }

    expect(p, TOK_FROM);
    if (NEXT() != TOK_STRING) {
      sv_parse_unexpected_token(p);
      return decl;
    }
    sv_ast_t *spec = mk(N_STRING);
    sv_ast_set_string(spec, sv_lexer_str_literal(&p->lx));
    decl->right = spec;
    decl->flags |= EX_FROM;
    CONSUME();

    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return decl;
  }

  sv_parse_unexpected_token(p);
  return decl;
}

static sv_ast_t *parse_stmt(P) {
  NEXT();

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

  if (TOK == TOK_USING) {
    CONSUME();
    sv_ast_t *n = parse_var_decl(p, SV_VAR_USING, false);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }
  
  if (TOK == TOK_AWAIT) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    CONSUME();
    NEXT();
    if (TOK == TOK_USING) {
      CONSUME();
      sv_ast_t *n = parse_var_decl(p, SV_VAR_AWAIT_USING, false);
      if (NEXT() == TOK_SEMICOLON) CONSUME();
      return n;
    }
    sv_lexer_restore_state(&p->lx, &saved);
  }

  if (TOK < TOK_MAX && dispatch[TOK])
    goto *dispatch[TOK];
  goto l_expr_stmt;

  l_semi:  { CONSUME(); return mk(N_EMPTY); }
  l_block: return parse_block(p, false);

  l_var: {
    CONSUME();
    sv_ast_t *n = parse_var_decl(p, SV_VAR_VAR, false);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }
  l_let: {
    CONSUME();
    sv_ast_t *n = parse_var_decl(p, SV_VAR_LET, false);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }
  l_const: {
    CONSUME();
    sv_ast_t *n = parse_var_decl(p, SV_VAR_CONST, false);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_if: {
    CONSUME();
    sv_ast_t *n = mk(N_IF);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->left = parse_stmt(p);
    if (is_lexical_decl_stmt(n->left)) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      return n;
    }
    if (NEXT() == TOK_ELSE) {
      CONSUME();
      n->right = parse_stmt(p);
      if (is_lexical_decl_stmt(n->right)) {
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
        return n;
      }
    }
    return n;
  }

  l_while: {
    CONSUME();
    sv_ast_t *n = mk(N_WHILE);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_stmt(p);
    if (is_lexical_decl_stmt(n->body))
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
    return n;
  }

  l_do: {
    CONSUME();
    sv_ast_t *n = mk(N_DO_WHILE);
    n->body = parse_stmt(p);
    if (is_lexical_decl_stmt(n->body))
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
    expect(p, TOK_WHILE);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_for: {
    CONSUME();
    bool is_for_await = false;
    if (NEXT() == TOK_AWAIT) {
      CONSUME();
      is_for_await = true;
    }
    expect(p, TOK_LPAREN);
    sv_ast_t *init_node = NULL;

    NEXT();
    if (TOK == TOK_AWAIT) {
      sv_lexer_state_t saved;
      sv_lexer_save_state(&p->lx, &saved);
      CONSUME();
      NEXT();
      if (TOK == TOK_USING) {
        CONSUME();
        p->no_in = true;
        init_node = parse_var_decl(p, SV_VAR_AWAIT_USING, true);
        p->no_in = false;
      } else sv_lexer_restore_state(&p->lx, &saved);
    }
    
    if (!init_node && TOK == TOK_USING) {
      CONSUME();
      p->no_in = true;
      init_node = parse_var_decl(p, SV_VAR_USING, true);
      p->no_in = false;
    } else if (!init_node && (TOK == TOK_VAR || TOK == TOK_LET || TOK == TOK_CONST)) {
      sv_var_kind_t kind = (
        TOK == TOK_VAR) ? SV_VAR_VAR :
        (TOK == TOK_LET) ? SV_VAR_LET : SV_VAR_CONST;
      CONSUME();
      p->no_in = true;
      init_node = parse_var_decl(p, kind, true);
      p->no_in = false;
    } else if (!init_node && TOK != TOK_SEMICOLON) {
      p->no_in = true;
      init_node = parse_expr(p);
      p->no_in = false;
    }

    uint8_t la = NEXT();
    if (la == TOK_IN) {
      CONSUME();
      sv_ast_t *n = mk(N_FOR_IN);
      n->left = init_node;
      n->right = parse_expr(p);
      expect(p, TOK_RPAREN);
      n->body = parse_stmt(p);
      if (p->lx.strict && init_node && init_node->type == N_VAR && var_decl_has_initializer(init_node))
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "for-in loop variable declaration may not have an initializer in strict mode");
      if (is_lexical_decl_stmt(n->body))
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      return n;
    }
    if (la == TOK_OF || (la == TOK_IDENTIFIER && TLEN == 2 &&
        memcmp(tok_str(p), "of", 2) == 0)) {
      CONSUME();
      sv_ast_t *n = mk(is_for_await ? N_FOR_AWAIT_OF : N_FOR_OF);
      n->left = init_node;
      n->right = parse_assign(p);
      expect(p, TOK_RPAREN);
      n->body = parse_stmt(p);
      if (is_lexical_decl_stmt(n->body))
        SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
      return n;
    }

    if (init_node && init_node->type == N_VAR && (
        init_node->var_kind == SV_VAR_CONST ||
        init_node->var_kind == SV_VAR_USING ||
        init_node->var_kind == SV_VAR_AWAIT_USING)) {
      for (int i = 0; i < init_node->args.count; i++) {
        sv_ast_t *decl = init_node->args.items[i];
        if (decl && decl->type == N_VARDECL && !decl->right) {
          SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Missing initializer in const declaration");
          return mk(N_EMPTY);
        }
      }
    }

    sv_ast_t *n = mk(N_FOR);
    n->init = init_node;
    expect(p, TOK_SEMICOLON);
    if (NEXT() != TOK_SEMICOLON)
      n->cond = parse_expr(p);
    expect(p, TOK_SEMICOLON);
    if (NEXT() != TOK_RPAREN)
      n->update = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_stmt(p);
    if (is_lexical_decl_stmt(n->body))
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "Lexical declaration cannot appear in single-statement context");
    return n;
  }

  l_return: {
    CONSUME();
    sv_ast_t *n = mk(N_RETURN);
    if (NEXT() != TOK_SEMICOLON && TOK != TOK_RBRACE && TOK != TOK_EOF &&
        !HAD_NEWLINE)
      n->right = parse_expr(p);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_throw: {
    CONSUME();
    sv_ast_t *n = mk(N_THROW);
    n->right = parse_expr(p);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_break: {
    CONSUME();
    sv_ast_t *n = mk(N_BREAK);
    if ((NEXT() == TOK_IDENTIFIER || is_contextual_ident_tok(TOK)) && !HAD_NEWLINE) {
      n->str = tok_ident_str(p, &n->len);
      CONSUME();
    }
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_continue: {
    CONSUME();
    sv_ast_t *n = mk(N_CONTINUE);
    if ((NEXT() == TOK_IDENTIFIER || is_contextual_ident_tok(TOK)) && !HAD_NEWLINE) {
      n->str = tok_ident_str(p, &n->len);
      CONSUME();
    }
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return n;
  }

  l_try: {
    CONSUME();
    sv_ast_t *n = mk(N_TRY);
    n->body = parse_block(p, false);
    if (NEXT() == TOK_CATCH) {
      CONSUME();
      if (NEXT() == TOK_LPAREN) {
        CONSUME();
        n->catch_param = parse_binding_pattern(p);
        if (n->catch_param->type == N_IDENT)
          sv_strict_check_binding_ident(p, n->catch_param->str, n->catch_param->len);
        expect(p, TOK_RPAREN);
      }
      n->catch_body = parse_block(p, false);
    }
    if (NEXT() == TOK_FINALLY) {
      CONSUME();
      n->finally_body = parse_block(p, false);
    }
    return n;
  }

  l_switch: {
    CONSUME();
    sv_ast_t *n = mk(N_SWITCH);
    expect(p, TOK_LPAREN);
    n->cond = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);
    while (NEXT() != TOK_RBRACE && TOK != TOK_EOF) {
      sv_ast_t *c = mk(N_CASE);
      if (TOK == TOK_CASE) {
        CONSUME();
        c->left = parse_expr(p);
      } else if (TOK == TOK_DEFAULT) {
        CONSUME();
      }
      expect(p, TOK_COLON);
      while (NEXT() != TOK_CASE && TOK != TOK_DEFAULT &&
             TOK != TOK_RBRACE && TOK != TOK_EOF)
        sv_ast_list_push(&c->args, parse_stmt(p));
      sv_ast_list_push(&n->args, c);
    }
    expect(p, TOK_RBRACE);
    return n;
  }

  l_debugger: { CONSUME(); if (NEXT() == TOK_SEMICOLON) CONSUME(); return mk(N_DEBUGGER); }

  l_with: {
    if (p->lx.strict) {
      SV_MKERR_TYPED(JS, JS_ERR_SYNTAX, "with statement not allowed in strict mode");
      return mk(N_EMPTY);
    }
    CONSUME();
    sv_ast_t *n = mk(N_WITH);
    expect(p, TOK_LPAREN);
    n->left = parse_expr(p);
    expect(p, TOK_RPAREN);
    n->body = parse_stmt(p);
    return n;
  }

  l_func: {
    CONSUME();
    return parse_func(p);
  }

  l_class: {
    uint32_t class_off = (uint32_t)TOFF;
    CONSUME();
    sv_ast_t *cls = parse_class(p);
    cls->src_off = class_off;
    return cls;
  }

  l_async: {
    uint8_t la = LA();
    uint32_t async_off = (uint32_t)TOFF;
    if (la == TOK_FUNC && !lookahead_crosses_line_terminator(p)) {
      CONSUME();
      NEXT(); CONSUME();
      sv_ast_t *fn = parse_func(p);
      fn->flags |= FN_ASYNC;
      fn->src_off = async_off;
      return fn;
    }
    goto l_expr_stmt;
  }

  l_import: {
    uint8_t la = LA();
    if (la == TOK_LPAREN || la == TOK_DOT)
      goto l_expr_stmt;
    CONSUME();
    return parse_import_stmt(p);
  }

  l_export: {
    CONSUME();
    return parse_export_stmt(p);
  }

  l_expr_stmt: {
    if (TOK == TOK_IDENTIFIER || is_contextual_ident_tok(TOK)) {
      uint8_t la = LA();
      if (la == TOK_COLON) {
        sv_ast_t *label = mk(N_LABEL);
        label->str = tok_ident_str(p, &label->len);
        CONSUME();
        NEXT(); CONSUME();
        label->body = parse_stmt(p);
        return label;
      }
    }

    sv_ast_t *expr = parse_expr(p);
    if (NEXT() == TOK_SEMICOLON) CONSUME();
    return expr;
  }
}

sv_ast_t *sv_parse(ant_t *js, const char *code, ant_offset_t clen, bool strict){
  if (sv_parse_trace_unlikely) {
    fprintf(stderr, "[parse] start len=%u strict=%d\n", (unsigned)clen, strict ? 1 : 0);
  }

  sv_parser_t parser = { .js = js };
  sv_parser_t *p = &parser;
  sv_lexer_init(&p->lx, js, code, clen, strict);

  sv_ast_t *program = mk(N_PROGRAM);
  sv_parse_stmt_list(p, &program->args, false, true);
  if (sv_parse_trace_unlikely) fprintf(
    stderr, "[parse] after-stmt-list thrown=%d strict=%d body=%d\n",
    js->thrown_exists ? 1 : 0,
    p->lx.strict ? 1 : 0,
    program ? program->args.count : -1
  );

  if (js->thrown_exists) {
    if (sv_parse_trace_unlikely) fprintf(stderr, "[parse] return null\n");
    return NULL;
  }
  
  if (p->lx.strict) program->flags |= FN_PARSE_STRICT;
  if (sv_parse_trace_unlikely) fprintf(stderr,
    "[parse] return program strict=%d body=%d\n",
    p->lx.strict ? 1 : 0, program->args.count
  );

  return program;
}
