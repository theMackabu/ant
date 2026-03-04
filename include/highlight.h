#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
  HL_STATE_NORMAL,
  HL_STATE_STRING_SINGLE,
  HL_STATE_STRING_DOUBLE,
  HL_STATE_TEMPLATE,
  HL_STATE_TEMPLATE_EXPR,
  HL_STATE_BLOCK_COMMENT,
} hl_mode_t;

typedef struct {
  hl_mode_t mode;
  int template_depth;
} highlight_state;

#define HL_STATE_INIT ((highlight_state){ .mode = HL_STATE_NORMAL, .template_depth = 0 })

typedef enum {
  HL_NONE,
  HL_KEYWORD,
  HL_KEYWORD_ITALIC,
  HL_KEYWORD_DELETE,
  HL_KEYWORD_EXTENDS,
  HL_TYPE,
  HL_TYPE_STRING,
  HL_TYPE_BOOLEAN,
  HL_LITERAL_NULL,
  HL_STRING,
  HL_BOOLEAN,
  HL_NUMBER,
  HL_COMMENT,
  HL_FUNCTION_NAME,
  HL_CLASS_NAME,
  HL_PARENT_CLASS,
  HL_FUNCTION,
  HL_PROPERTY,
  HL_OPERATOR,
  HL_OPERATOR_CMP,
  HL_OPTIONAL_CHAIN,
  HL_SEMICOLON,
} hl_token_class;

typedef struct {
  size_t          off;
  size_t          len;
  hl_token_class  cls;
} hl_span;

typedef enum {
  HL_CTX_NONE,
  HL_CTX_AFTER_FUNCTION,
  HL_CTX_AFTER_CLASS,
  HL_CTX_AFTER_EXTENDS,
} hl_context;

typedef struct {
  const char      *input;
  size_t           input_len;
  size_t           pos;
  highlight_state  state;
  hl_context       ctx;
} hl_iter;

static inline highlight_state hl_iter_state(const hl_iter *it) { return it->state; }

void hl_iter_init(hl_iter *it, const char *input, size_t input_len, const highlight_state *state);
bool hl_iter_next(hl_iter *it, hl_span *out);

int ant_highlight(
  const char *input, size_t input_len,
  char *out, size_t out_size
);

int ant_highlight_stateful(
  const char *input, size_t input_len,
  char *out, size_t out_size,
  highlight_state *state
);

int highlight_js_line_clipped(
  const char *line, size_t line_len,
  size_t max_cols,
  char *out, size_t out_size,
  highlight_state *state
);

#endif
