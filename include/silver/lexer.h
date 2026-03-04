#ifndef SILVER_LEXER_H
#define SILVER_LEXER_H

#include <stdbool.h>
#include "types.h"

typedef struct {
  jsoff_t pos;
  jsoff_t toff;
  jsoff_t tlen;
  jsval_t tval;
  uint8_t tok;
  uint8_t consumed;
  bool had_newline;
} sv_lexer_state_t;

typedef struct {
  ant_t *js;
  const char *code;
  jsoff_t clen;
  bool strict;
  sv_lexer_state_t st;
} sv_lexer_t;

typedef struct {
  const char *str;
  uint32_t len;
  bool ok;
} sv_lex_string_t;

typedef struct {
  const char *code;
  jsoff_t clen;
  bool strict;
  sv_lexer_state_t st;
} sv_lexer_checkpoint_t;

sv_lex_string_t sv_lexer_str_literal(sv_lexer_t *lx);

void sv_lexer_init(sv_lexer_t *lx, ant_t *js, const char *code, jsoff_t clen, bool strict);
void sv_lexer_set_error_site(sv_lexer_t *lx);

void sv_lexer_save_state(const sv_lexer_t *lx, sv_lexer_state_t *st);
void sv_lexer_restore_state(sv_lexer_t *lx, const sv_lexer_state_t *st);

void sv_lexer_push_source(sv_lexer_t *lx, sv_lexer_checkpoint_t *cp, const char *code, jsoff_t clen);
void sv_lexer_pop_source(sv_lexer_t *lx, const sv_lexer_checkpoint_t *cp);

uint8_t sv_lexer_next(sv_lexer_t *lx);
uint8_t sv_lexer_lookahead(sv_lexer_t *lx);
uint8_t sv_parsekeyword(const char *buf, size_t len);

bool is_space(int c);
bool is_digit(int c);

bool is_eval_or_arguments_name(const char *buf, size_t len);
bool is_strict_reserved_name(const char *buf, size_t len);

#endif
