#include "parser_internal.h"

bool lookahead_crosses_line_terminator(sv_parser_t *p) {
  sv_lexer_state_t saved;
  sv_lexer_save_state(&p->lx, &saved);
  p->lx.st.consumed = 1;
  (void)sv_lexer_next(&p->lx);
  bool had_newline = p->lx.st.had_newline;
  sv_lexer_restore_state(&p->lx, &saved);
  return had_newline;
}

void sv_parse_stop(sv_parser_t *p) {
  p->lx.st.tok = TOK_EOF;
  p->lx.st.consumed = 0;
  p->lx.st.toff = p->lx.st.pos = p->lx.clen;
  p->lx.st.tlen = 0;
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

sv_ast_t *sv_parse_function_body(ant_t *js, const char *code, ant_offset_t clen, bool strict) {
  return parse_program(js, code, clen, SV_PARSE_SCRIPT, strict, NULL, true);
}

sv_ast_t *sv_parse_function_parts(ant_t *js, const sv_function_parts_t *parts) {
  sv_parser_t parser = {
    .js = js, .goal = SV_PARSE_SCRIPT,
    .in_async = parts->is_async, .function_depth = 1,
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
  fn->body = mk_plain(N_BLOCK);
  fn->body->src_off = (uint32_t)parts->body_off;
  fn->body->src_end = (uint32_t)parts->body_end;
  sv_parse_stmt_list(p, &fn->body->args, false, true, false);
  if (Ant_Exception_Pending(js)) return NULL;

  finish_func(fn);
  return fn;
}
