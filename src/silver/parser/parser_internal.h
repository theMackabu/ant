#ifndef ANT_SILVER_PARSER_PARSER_INTERNAL_H
#define ANT_SILVER_PARSER_PARSER_INTERNAL_H

#include "escape.h"
#include "tokens.h"
#include "silver/ast.h"
#include "silver/lexer.h"
#include "silver/call.h"
#include "silver/directives.h"

typedef struct {
  ant_t *js;
  sv_lexer_t lx;
  sv_parse_goal_t goal;
  bool no_in;
  bool module_item;
  bool in_async;
  bool wants_module;
  bool tagged_template;
  uint32_t function_depth;
} sv_parser_t;

#define SV_RAISE(mkerr) ({                                  \
  if (!Ant_Exception_Pending(p->js)) {                      \
    js_error_site_t sv_site_ = sv_lexer_error_site(&p->lx); \
    (void)(mkerr);                                          \
  }                                                         \
  sv_parse_stop(p);                                         \
})

#define SV_MKERR(...) SV_RAISE(js_mkerr_at(&sv_site_, __VA_ARGS__))
#define SV_MKERR_TYPED(...) SV_RAISE(js_mkerr_typed_at(&sv_site_, __VA_ARGS__))

#define mk_plain(type) sv_ast_new(p->js, type)

#define mk(type) ({                          \
  sv_ast_t *_n = mk_plain(type);             \
  _n->src_off = (uint32_t)p->lx.st.toff; _n; \
})

typedef struct {
  const char *str;
  uint32_t len;
  const char *error;
  bool ok;
  bool valid_cooked;
} sv_tpl_cooked_t;

void finish_func(sv_ast_t *fn);
void sv_parse_stop(sv_parser_t *p);
void consume_semicolon(sv_parser_t *p);

bool sv_parse_stack_overflow(sv_parser_t *p);
bool arrow_param_is_name(const sv_ast_t *node);
bool lookahead_crosses_line_terminator(sv_parser_t *p);
bool using_declaration_here(sv_parser_t *p, bool for_head);
bool validate_accessor_params(sv_parser_t *p, sv_ast_t *fn, uint32_t flags);

sv_ast_t *parse_object(sv_parser_t *p);
sv_ast_t *parse_member_suffix(sv_parser_t *p, sv_ast_t *left, uint8_t la);
sv_ast_t *parse_arrow_body(sv_parser_t *p, bool is_async);
sv_ast_t *parse_binding_pattern(sv_parser_t *p);
sv_ast_t *try_parse_async_arrow(sv_parser_t *p);
sv_ast_t *parse_primary(sv_parser_t *p);
sv_ast_t *parse_array(sv_parser_t *p);
sv_ast_t *finish_arrow(sv_parser_t *p, sv_ast_t *fn);
sv_ast_t *parse_func(sv_parser_t *p, bool is_async);
sv_ast_t *parse_class(sv_parser_t *p);
sv_ast_t *parse_block(sv_parser_t *p, bool directive_ctx);
sv_ast_t *parse_var_decl(sv_parser_t *p, sv_var_kind_t kind, bool allow_uninit_const);
sv_ast_t *parse_import_stmt(sv_parser_t *p);
sv_ast_t *parse_export_stmt(sv_parser_t *p);
sv_ast_t *parse_unary(sv_parser_t *p);
sv_ast_t *parse_assign(sv_parser_t *p);
sv_ast_t *parse_expr(sv_parser_t *p);
sv_ast_t *parse_paren_list(sv_parser_t *p, bool trailing_comma);
sv_ast_t *parse_paren_expr(sv_parser_t *p);

void parse_formal_params(sv_parser_t *p, sv_ast_t *fn, uint8_t close);
void push_arrow_params_from_expr(sv_parser_t *p, sv_ast_t *fn, sv_ast_t *expr);
void sv_parse_stmt_list(sv_parser_t *p, sv_ast_list_t *out, bool stop_at_rbrace, bool directive_ctx, bool module_items);

unsigned scope_facts_eval_yield(const sv_ast_list_t *list, const sv_ast_t *body);
const char *bigint_key_decimal(ant_t *js, const char *lit, uint32_t len, uint32_t *out_len);

static inline bool regex_line_terminator_at(const char *code, ant_offset_t len, ant_offset_t pos) {
  unsigned char c = (unsigned char)code[pos];
  if (c == '\n' || c == '\r') return true;
  return c == 0xE2 && pos + 2 < len &&
    (unsigned char)code[pos + 1] == 0x80 &&
    ((unsigned char)code[pos + 2] == 0xA8 || (unsigned char)code[pos + 2] == 0xA9);
}

static inline const char *tok_str(sv_parser_t *p) { 
  return &p->lx.code[p->lx.st.toff]; 
}

static inline sv_ast_t *mk_num(sv_parser_t *p, double val) {
  sv_ast_t *n = mk(N_NUMBER);
  n->num = val;
  n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  return n;
}

static inline sv_ast_t *mk_ident(sv_parser_t *p, const char *s, uint32_t len) {
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

static inline const char *decode_ident_into_arena(ant_t *js, const char *src, uint32_t len, uint32_t *out_len) {
  if (!src || len == 0 || !memchr(src, '\\', len)) {
    if (out_len) *out_len = len;
    return src;
  }

  char *dst = parse_arena_bump(js, (size_t)len + 1);
  if (!dst) {
    if (out_len) *out_len = len;
    return src;
  }

  size_t di = 0; size_t i = 0;
  const uint8_t *in = (const uint8_t *)src;
  uint8_t *out = (uint8_t *)dst;
  
  while (i < len) {
    if (in[i] == '\\' && i + 1 < len && in[i + 1] == 'u') {
      size_t adv = decode_escape(in, i, len, out, &di, 0, NULL);
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

static inline const char *tok_ident_str(sv_parser_t *p, uint32_t *out_len) {
  return decode_ident_into_arena(p->js, tok_str(p), (uint32_t)p->lx.st.tlen, out_len);
}

static inline bool is_ident_like_tok(uint8_t tok) {
  return tok == TOK_IDENTIFIER || tok == TOK_DEFAULT || is_contextual_ident_tok(tok);
}

static inline bool is_binding_ident_tok(uint8_t tok) {
  return 
    tok == TOK_IDENTIFIER || is_contextual_ident_tok(tok) ||
    tok == TOK_LET || tok == TOK_STATIC || tok == TOK_YIELD || tok == TOK_AWAIT ||
    tok == TOK_UNDEF || tok == TOK_WINDOW || tok == TOK_GLOBAL_THIS;
}

static inline bool binding_ident_here(sv_parser_t *p) {
  return is_binding_ident_tok(p->lx.st.tok) && !(p->lx.st.tok == TOK_AWAIT && (p->in_async || p->goal == SV_PARSE_MODULE));
}

static inline bool is_private_ident_like_tok(uint8_t tok) {
  return tok >= TOK_IDENTIFIER && tok < TOK_IDENT_LIKE_END;
}

static inline bool is_leading_dot_number_token(sv_parser_t *p) {
  return p->lx.st.tok == TOK_NUMBER && p->lx.st.tlen > 0 && p->lx.st.toff < p->lx.clen && p->lx.code[p->lx.st.toff] == '.';
}

static inline bool sv_strict_forbidden_binding_ident(const char *s, uint32_t len) {
  return is_eval_or_arguments_name(s, len) || is_strict_reserved_name(s, len);
}

static inline bool sv_is_strict_restricted_assign_target(sv_parser_t *p, sv_ast_t *n) {
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

static inline sv_ast_t *mk_ident_from_tok(sv_parser_t *p) {
  if (p->lx.st.tok == TOK_BIGINT) {
    uint32_t key_len = 0;
    const char *key = bigint_key_decimal(p->js, tok_str(p), (uint32_t)p->lx.st.tlen, &key_len);
    sv_ast_t *n = mk(N_STRING);
    
    n->str = key ? key : tok_str(p);
    n->len = key ? key_len : (uint32_t)p->lx.st.tlen;
    n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
    
    return n;
  }
  
  uint32_t len = 0;
  const char *name = tok_ident_str(p, &len);
  sv_ast_t *n = mk_ident(p, name, len);
  
  n->src_off = (uint32_t)p->lx.st.toff;
  n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  
  return n;
}

static inline sv_ast_t *mk_private_ident_from_tok(sv_parser_t *p) {
  sv_ast_t *n = mk(N_IDENT);
  
  n->str = &p->lx.code[p->lx.st.toff - 1];
  n->len = (uint32_t)(p->lx.st.tlen + 1);
  n->src_off = (uint32_t)(p->lx.st.toff > 0 ? p->lx.st.toff - 1 : p->lx.st.toff);
  n->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  
  return n;
}

static inline void sv_strict_check_binding_ident(sv_parser_t *p, const char *s, uint32_t len) {
  if (!p->lx.strict || !s || len == 0) return;
  if (sv_strict_forbidden_binding_ident(s, len)) SV_MKERR_TYPED(
    p->js, JS_ERR_SYNTAX, "Invalid binding identifier '%.*s' in strict mode", 
    (int)len, s
  );
}

static inline void sv_parse_unexpected_token(sv_parser_t *p) {
  size_t tok_len = 0;

  if (p->lx.st.toff < p->lx.clen && p->lx.st.tlen > 0) {
    ant_offset_t rem = p->lx.clen - p->lx.st.toff;
    tok_len = (size_t)p->lx.st.tlen;
    if ((ant_offset_t)tok_len > rem) tok_len = (size_t)rem;
  }

  if (tok_len == 0) {
    SV_MKERR_TYPED(p->js, JS_ERR_SYNTAX, "Unexpected token 'EOF'");
    return;
  }

  SV_MKERR_TYPED(
    p->js, JS_ERR_SYNTAX,
    "Unexpected token '%.*s'",
    (int)tok_len, &p->lx.code[p->lx.st.toff]
  );
}

static inline void expect(sv_parser_t *p, uint8_t tok) {
  if (sv_lexer_next(&p->lx) == tok) p->lx.st.consumed = 1;
  else sv_parse_unexpected_token(p);
}

static inline sv_ast_t *parse_dot_property_name(sv_parser_t *p) {
  if (!is_private_ident_like_tok(p->lx.st.tok)) {
    sv_parse_unexpected_token(p);
    return NULL;
  }

  sv_ast_t *name = mk_ident_from_tok(p);
  p->lx.st.consumed = 1;
  return name;
}

static inline __attribute__((always_inline)) sv_ast_t *parse_with_in(sv_parser_t *p, sv_ast_t *(*parse)(sv_parser_t *p)) {
  if (__builtin_expect(!p->no_in, 1)) return parse(p);
  p->no_in = false;
  sv_ast_t *n = parse(p);
  p->no_in = true;
  return n;
}

static inline uint32_t node_src_end(sv_parser_t *p, sv_ast_t *node) {
  if (node && node->src_end > node->src_off) return node->src_end;
  return (uint32_t)(node && node->type == N_BLOCK ? p->lx.st.toff + p->lx.st.tlen : p->lx.st.toff);
}

#endif
