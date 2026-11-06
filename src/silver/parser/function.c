#include "parser_internal.h"
#include "shapes.h"

sv_ast_t *parse_arrow_body(sv_parser_t *p, bool is_async) {
  bool saved_async = p->in_async;
  p->in_async = is_async;
  bool saved_no_in = p->no_in;
  if (sv_lexer_next(&p->lx) == TOK_LBRACE) p->no_in = false;
  sv_ast_t *body = p->lx.st.tok == TOK_LBRACE ? parse_block(p, true) : parse_assign(p);
  p->no_in = saved_no_in;
  p->in_async = saved_async;
  return body;
}

void push_arrow_params_from_expr(sv_parser_t *p, sv_ast_t *fn, sv_ast_t *expr) {
  if (!fn || !expr) return;
  if (expr->type == N_SEQUENCE) {
    push_arrow_params_from_expr(p, fn, expr->left);
    push_arrow_params_from_expr(p, fn, expr->right);
    return;
  }
  if (expr->type == N_ASSIGN && expr->op == TOK_ASSIGN) {
    sv_ast_t *def = mk_plain(N_ASSIGN_PAT);
    def->left = expr->left;
    def->right = expr->right;
    def->src_off = expr->src_off;
    sv_ast_list_push(p->js, &fn->args, def);
    return;
  }
  if (expr->type == N_SPREAD) {
    sv_ast_t *rest = mk_plain(N_REST);
    rest->right = expr->right;
    rest->src_off = expr->src_off;
    sv_ast_list_push(p->js, &fn->args, rest);
    return;
  }
  sv_ast_list_push(p->js, &fn->args, expr);
}

sv_ast_t *try_parse_async_arrow(sv_parser_t *p) {
  uint8_t la = sv_lexer_lookahead(&p->lx);
  uint32_t async_off = (uint32_t)p->lx.st.toff;

  if (la == TOK_LPAREN) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
    if (sv_lexer_next(&p->lx) == TOK_RPAREN) {
      p->lx.st.consumed = 1;
      if (sv_lexer_lookahead(&p->lx) == TOK_ARROW) {
        sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
        sv_ast_t *fn = mk(N_FUNC);
        fn->flags = FN_ARROW | FN_ASYNC;
        fn->body = parse_arrow_body(p, true);
        fn->src_off = async_off;
        fn->src_end = node_src_end(p, fn->body);
        return finish_arrow(p, fn);
      }
      sv_lexer_restore_state(&p->lx, &saved);
      return NULL;
    }
    sv_lexer_restore_state(&p->lx, &saved);
    sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
    bool saved_no_in = p->no_in;
    p->no_in = false;
    sv_ast_t *expr = parse_paren_list(p, true);
    p->no_in = saved_no_in;
    expect(p, TOK_RPAREN);
    if (sv_lexer_lookahead(&p->lx) == TOK_ARROW) {
      sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
      sv_ast_t *fn = mk(N_FUNC);
      fn->flags = FN_ARROW | FN_ASYNC;
      push_arrow_params_from_expr(p, fn, expr);
      fn->body = parse_arrow_body(p, true);
      fn->src_off = async_off;
      fn->src_end = node_src_end(p, fn->body);
      return finish_arrow(p, fn);
    }
    sv_lexer_restore_state(&p->lx, &saved);
    return NULL;
  }

  if (la == TOK_IDENTIFIER) {
    sv_lexer_state_t saved;
    sv_lexer_save_state(&p->lx, &saved);
    sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
    sv_ast_t *id = mk_ident_from_tok(p);
    if (sv_lexer_lookahead(&p->lx) == TOK_ARROW) {
      sv_lexer_next(&p->lx); p->lx.st.consumed = 1;
      sv_ast_t *fn = mk(N_FUNC);
      fn->flags = FN_ARROW | FN_ASYNC;
      sv_ast_list_push(p->js, &fn->args, id);
      fn->body = parse_arrow_body(p, true);
      fn->src_off = async_off;
      fn->src_end = node_src_end(p, fn->body);
      return finish_arrow(p, fn);
    }
    sv_lexer_restore_state(&p->lx, &saved);
    return NULL;
  }

  return NULL;
}

void parse_formal_params(sv_parser_t *p, sv_ast_t *fn, uint8_t close) {
  while (sv_lexer_next(&p->lx) != close && p->lx.st.tok != TOK_EOF) {
    if (p->lx.st.tok == TOK_REST) {
      p->lx.st.consumed = 1;
      sv_ast_t *rest = mk(N_REST);
      rest->right = parse_binding_pattern(p);
      sv_ast_list_push(p->js, &fn->args, rest);
      break;
    }
    sv_ast_t *param = parse_binding_pattern(p);
    if (sv_lexer_next(&p->lx) == TOK_ASSIGN) {
      p->lx.st.consumed = 1;
      sv_ast_t *def = mk(N_ASSIGN_PAT);
      def->left = param;
      def->right = parse_assign(p);
      param = def;
    }
    sv_ast_list_push(p->js, &fn->args, param);
    if (sv_lexer_next(&p->lx) == TOK_COMMA) {
      p->lx.st.consumed = 1;
      if (sv_lexer_next(&p->lx) == close) break;
    } else break;
  }
}

enum {
  SCOPE_ARGUMENTS  = 1 << 0,
  SCOPE_NEW_TARGET = 1 << 1,
  SCOPE_EVAL       = 1 << 2,
  SCOPE_YIELD      = 1 << 3,
};

typedef struct {
  const sv_ast_t *names[ANT_INOBJ_LARGE_SLOTS + 1];
  uint32_t count;
} this_props_t;

static void this_props_note(this_props_t *props, const sv_ast_t *node) {
  const sv_ast_t *target = node->left;
  if (
    !target || target->type != N_MEMBER || (target->flags & 1) || !target->left || target->left->type != N_THIS ||
    !target->right || !target->right->len || is_private_name_node(target->right)
  ) return;
  const sv_ast_t *key = target->right;
  for (uint32_t i = 0; i < props->count; i++)
    if (props->names[i]->len == key->len && memcmp(props->names[i]->str, key->str, key->len) == 0) return;
  props->names[props->count++] = key;
}

static unsigned ast_scope_facts(const sv_ast_t *node, unsigned want, bool in_arrow, this_props_t *props) {
  if (!node) return 0;
  if (props && props->count > ANT_INOBJ_LARGE_SLOTS) props = NULL;
  if (!want && !props) return 0;
  unsigned found = 0;

  switch (node->type) {
    case N_IDENT:
      return ast_is_ident(node, "arguments", 9) ? SCOPE_ARGUMENTS & want : 0;
    case N_NEW_TARGET:
      return SCOPE_NEW_TARGET & want;
    case N_YIELD:
      found |= SCOPE_YIELD;
      break;
    case N_CALL:
      if (ast_is_ident(node->left, "eval", 4)) found |= SCOPE_EVAL;
      if (in_arrow && ast_is_ident(node->left, "super", 5)) found |= SCOPE_NEW_TARGET;
      break;
    case N_ASSIGN:
      if (props) this_props_note(props, node);
      break;
    case N_CLASS:
      props = NULL;
      break;
    case N_FUNC:
      if (!(node->flags & FN_ARROW)) return 0;
      in_arrow = true;
      want &= ~SCOPE_YIELD;
      break;
    case N_ARROW:
      want &= ~SCOPE_YIELD;
      break;
    default:
      break;
  }

  found &= want;
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child) {
    if (found == want && !props) break;
    found |= ast_scope_facts(child, want & ~found, in_arrow, props);
  }
  
  return found;
}

static constexpr unsigned SCOPE_ALL = SCOPE_ARGUMENTS | SCOPE_NEW_TARGET | SCOPE_EVAL | SCOPE_YIELD;
#define SCOPE_FACTS_FLAGS(facts) \
  (FN_SCOPE_FACTS | ((facts) & SCOPE_EVAL ? FN_DIRECT_EVAL : 0) | ((facts) & SCOPE_YIELD ? FN_OWN_YIELD : 0))

void finish_func(sv_ast_t *fn) {
  if (fn->flags & FN_ARROW) return;
  this_props_t props = {.count = 0};
  bool counts = !(fn->flags & (FN_GENERATOR | FN_ASYNC));
  
  unsigned body = ast_scope_facts(fn->body, SCOPE_ALL, false, counts ? &props : NULL);
  if (counts) fn->this_props = (uint8_t)props.count;
  
  unsigned params = 0;
  for (int i = 0; i < fn->args.count && params != SCOPE_ALL; i++)
    params |= ast_scope_facts(fn->args.items[i], SCOPE_ALL & ~params, false, NULL);
  unsigned facts = body | params;

  if (body & SCOPE_ARGUMENTS) fn->flags |= FN_USES_ARGS;
  bool length_only = (facts & SCOPE_ARGUMENTS) && ast_arguments_length_only(fn->body, false, false);
  
  for (int i = 0; length_only && i < fn->args.count; i++)
    length_only = ast_arguments_length_only(fn->args.items[i], false, false);
  
  if (length_only) fn->flags |= FN_ARGS_LENGTH_ONLY;
  if (facts & (SCOPE_NEW_TARGET | SCOPE_EVAL)) fn->flags |= FN_USES_NEW_TARGET;

  fn->flags |= SCOPE_FACTS_FLAGS(facts);
}

unsigned scope_facts_eval_yield(const sv_ast_list_t *list, const sv_ast_t *body) {
  const unsigned want = SCOPE_EVAL | SCOPE_YIELD;
  unsigned found = body ? ast_scope_facts(body, want, false, NULL) : 0;
  for (int i = 0; list && i < list->count && found != want; i++)
    found |= ast_scope_facts(list->items[i], want & ~found, false, NULL);
  return SCOPE_FACTS_FLAGS(found);
}

sv_ast_t *finish_arrow(sv_parser_t *p, sv_ast_t *fn) {
  fn->flags |= scope_facts_eval_yield(&fn->args, fn->body);
  return fn;
}

sv_ast_t *parse_func(sv_parser_t *p, bool is_async) {
  sv_ast_t *fn = mk(N_FUNC);
  if (is_async) fn->flags |= FN_ASYNC;

  if (sv_lexer_next(&p->lx) == TOK_MUL) {
    p->lx.st.consumed = 1;
    fn->flags |= FN_GENERATOR;
  }

  sv_lexer_next(&p->lx);
  if (binding_ident_here(p)) {
    fn->str = tok_ident_str(p, &fn->len);
    sv_strict_check_binding_ident(p, fn->str, fn->len);
    p->lx.st.consumed = 1;
  }

  bool saved_async = p->in_async;
  bool saved_no_in = p->no_in;
  p->in_async = is_async;
  p->no_in = false;
  
  expect(p, TOK_LPAREN);
  parse_formal_params(p, fn, TOK_RPAREN);
  expect(p, TOK_RPAREN);
  
  fn->body = parse_block(p, true);
  p->no_in = saved_no_in;
  p->in_async = saved_async;

  fn->src_end = (uint32_t)(p->lx.st.toff + p->lx.st.tlen);
  finish_func(fn);
  
  return fn;
}
