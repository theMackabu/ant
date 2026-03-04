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

bool is_ident_begin(int c);
bool is_ident_continue(int c);

bool is_eval_or_arguments_name(const char *buf, size_t len);
bool is_strict_reserved_name(const char *buf, size_t len);

#define CHAR_DIGIT  0x01
#define CHAR_XDIGIT 0x02
#define CHAR_ALPHA  0x04
#define CHAR_IDENT  0x08
#define CHAR_IDENT1 0x10
#define CHAR_WS     0x20
#define CHAR_OCTAL  0x40

extern const uint8_t char_type[256];

#define IS_DIGIT(c)  (char_type[(uint8_t)(c)] & CHAR_DIGIT)
#define IS_XDIGIT(c) (char_type[(uint8_t)(c)] & CHAR_XDIGIT)
#define IS_IDENT(c)  (char_type[(uint8_t)(c)] & CHAR_IDENT)
#define IS_IDENT1(c) (char_type[(uint8_t)(c)] & CHAR_IDENT1)
#define IS_OCTAL(c)  (char_type[(uint8_t)(c)] & CHAR_OCTAL)

#endif
