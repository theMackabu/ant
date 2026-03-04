#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <crprintf.h>

#include "tokens.h"
#include "highlight.h"
#include "silver/lexer.h"

typedef struct { const char *op; int len; hl_token_class cls; } op_entry_t;

static const op_entry_t operators[] = {
  { "===", 3, HL_OPERATOR_CMP },
  { "!==", 3, HL_OPERATOR_CMP },
  { "...", 3, HL_OPERATOR },
  { "=>",  2, HL_OPERATOR },
  { "==",  2, HL_OPERATOR_CMP },
  { "!=",  2, HL_OPERATOR_CMP },
  { "<=",  2, HL_OPERATOR_CMP },
  { ">=",  2, HL_OPERATOR_CMP },
  { "&&",  2, HL_OPERATOR },
  { "||",  2, HL_OPERATOR },
  { "??",  2, HL_OPERATOR },
  { "?.",  2, HL_OPTIONAL_CHAIN },
};

#define OP_COUNT (sizeof(operators) / sizeof(operators[0]))
#define K(s, t) if (len == sizeof(s)-1 && !memcmp(word, s, sizeof(s)-1)) return t

static hl_token_class lookup_extra_keyword(const char *word, size_t len) {
  switch (word[0]) {
  case 'a': K("abstract", HL_TYPE); break;
  case 'b': K("boolean", HL_TYPE_BOOLEAN); break;
  case 'd': K("declare", HL_TYPE); break;
  case 'e': K("enum", HL_TYPE); break;
  case 'g': K("global", HL_KEYWORD_ITALIC); break;
  case 'i':
    K("interface", HL_TYPE);
    K("implements", HL_TYPE);
    break;
  case 'n':
    K("namespace", HL_TYPE);
    K("never", HL_TYPE);
    break;
  case 'o': K("object", HL_TYPE); break;
  case 'p':
    K("package", HL_KEYWORD);
    K("private", HL_KEYWORD);
    K("protected", HL_KEYWORD);
    K("public", HL_KEYWORD);
    break;
  case 'r': K("readonly", HL_TYPE); break;
  case 's':
    K("string", HL_TYPE_STRING);
    K("symbol", HL_TYPE_STRING);
    break;
  case 't': K("type", HL_TYPE); break;
  case 'u': K("unknown", HL_TYPE); break;
  } return HL_NONE;
}

#undef K

static hl_token_class tok_to_class(uint8_t tok) {
  static const void *dispatch[] = {
    [TOK_ASYNC]       = &&l_kw_italic,
    [TOK_EXPORT]      = &&l_kw_italic,
    [TOK_THIS]        = &&l_kw_italic,
    [TOK_GLOBAL_THIS] = &&l_kw_italic,
    [TOK_WINDOW]      = &&l_kw_italic,
    [TOK_DELETE]      = &&l_kw_delete,
    [TOK_TYPEOF]      = &&l_type,
    [TOK_INSTANCEOF]  = &&l_type,
    [TOK_OF]          = &&l_type,
    [TOK_IN]          = &&l_type,
    [TOK_AS]          = &&l_type,
    [TOK_TRUE]        = &&l_bool,
    [TOK_FALSE]       = &&l_bool,
    [TOK_NULL]        = &&l_null,
    [TOK_UNDEF]       = &&l_null,
  };

  if (tok <= TOK_IDENTIFIER || tok >= TOK_IDENT_LIKE_END) return HL_NONE;
  if (tok < sizeof(dispatch) / sizeof(*dispatch) && dispatch[tok]) goto *dispatch[tok];
  
  return HL_KEYWORD;

  l_kw_italic: return HL_KEYWORD_ITALIC;
  l_kw_delete: return HL_KEYWORD_DELETE;
  l_type:      return HL_TYPE;
  l_bool:      return HL_TYPE_BOOLEAN;
  l_null:      return HL_LITERAL_NULL;
}

void hl_iter_init(hl_iter *it, const char *input, size_t input_len, const highlight_state *state) {
  it->input = input;
  it->input_len = input_len;
  it->pos = 0;
  it->state = state ? *state : (highlight_state){ .mode = HL_STATE_NORMAL, .template_depth = 0 };
  it->ctx = HL_CTX_NONE;
}

static hl_context keyword_sets_context(const char *word, size_t len) {
  if (len == 8 && memcmp(word, "function", 8) == 0) return HL_CTX_AFTER_FUNCTION;
  if (len == 5 && memcmp(word, "class", 5) == 0)    return HL_CTX_AFTER_CLASS;
  if (len == 7 && memcmp(word, "extends", 7) == 0)  return HL_CTX_AFTER_EXTENDS;
  return HL_CTX_NONE;
}

bool hl_iter_next(hl_iter *it, hl_span *out) {
  const char *input = it->input;
  size_t input_len = it->input_len;
  size_t i = it->pos;

  if (i >= input_len) return false;
  unsigned char c = (unsigned char)input[i];

  if (it->state.mode == HL_STATE_BLOCK_COMMENT) {
    size_t start = i;
    while (i < input_len) {
      if (input[i] == '*' && i + 1 < input_len && input[i + 1] == '/') {
        i += 2;
        it->state.mode = HL_STATE_NORMAL;
        break;
      }
      i++;
    }
    *out = (hl_span){ start, i - start, HL_COMMENT };
    it->pos = i;
    return true;
  }

  if (it->state.mode == HL_STATE_STRING_SINGLE || it->state.mode == HL_STATE_STRING_DOUBLE) {
    char quote = (it->state.mode == HL_STATE_STRING_SINGLE) ? '\'' : '"';
    size_t start = i;
    while (i < input_len) {
      if (input[i] == '\\' && i + 1 < input_len) { i += 2; continue; }
      if (input[i] == quote) {
        i++;
        it->state.mode = (it->state.template_depth > 0) ? HL_STATE_TEMPLATE_EXPR : HL_STATE_NORMAL;
        break;
      }
      i++;
    }
    *out = (hl_span){ start, i - start, HL_STRING };
    it->pos = i;
    return true;
  }

  if (it->state.mode == HL_STATE_TEMPLATE) {
    size_t start = i;
    while (i < input_len) {
      if (input[i] == '\\' && i + 1 < input_len) { i += 2; continue; }
      if (input[i] == '$' && i + 1 < input_len && input[i + 1] == '{') {
        i += 2;
        it->state.mode = HL_STATE_TEMPLATE_EXPR;
        it->state.template_depth++;
        break;
      }
      if (input[i] == '`') {
        i++;
        it->state.mode = (it->state.template_depth > 0) ? HL_STATE_TEMPLATE_EXPR : HL_STATE_NORMAL;
        break;
      }
      i++;
    }
    *out = (hl_span){ start, i - start, HL_STRING };
    it->pos = i;
    return true;
  }

  if (it->state.mode == HL_STATE_TEMPLATE_EXPR && c == '}') {
    it->state.template_depth--;
    if (it->state.template_depth <= 0) {
      it->state.mode = HL_STATE_TEMPLATE;
      it->state.template_depth = 0;
      *out = (hl_span){ i, 1, HL_NONE };
      it->pos = i + 1;
      return true;
    }
  }
  if (it->state.mode == HL_STATE_TEMPLATE_EXPR && c == '{') {
    it->state.template_depth++;
    *out = (hl_span){ i, 1, HL_NONE };
    it->pos = i + 1;
    return true;
  }

  if (c == '/' && i + 1 < input_len && input[i + 1] == '/') {
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ i, input_len - i, HL_COMMENT };
    it->pos = input_len;
    return true;
  }

  if (c == '/' && i + 1 < input_len && input[i + 1] == '*') {
    it->ctx = HL_CTX_NONE;
    size_t start = i;
    i += 2;
    while (i + 1 < input_len && !(input[i] == '*' && input[i + 1] == '/')) i++;
    if (i + 1 < input_len) {
      i += 2;
    } else {
      i = input_len;
      it->state.mode = HL_STATE_BLOCK_COMMENT;
    }
    *out = (hl_span){ start, i - start, HL_COMMENT };
    it->pos = i;
    return true;
  }

  if (c == '\'' || c == '"') {
    it->ctx = HL_CTX_NONE;
    size_t start = i;
    it->state.mode = (c == '\'') ? HL_STATE_STRING_SINGLE : HL_STATE_STRING_DOUBLE;
    i++;
    while (i < input_len) {
      if (input[i] == '\\' && i + 1 < input_len) { i += 2; continue; }
      if ((unsigned char)input[i] == c) {
        i++;
        it->state.mode = (it->state.template_depth > 0) ? HL_STATE_TEMPLATE_EXPR : HL_STATE_NORMAL;
        break;
      }
      i++;
    }
    *out = (hl_span){ start, i - start, HL_STRING };
    it->pos = i;
    return true;
  }

  if (c == '`') {
    it->ctx = HL_CTX_NONE;
    it->state.mode = HL_STATE_TEMPLATE;
    *out = (hl_span){ i, 1, HL_STRING };
    it->pos = i + 1;
    return true;
  }

  if (c == ';') {
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ i, 1, HL_SEMICOLON };
    it->pos = i + 1;
    return true;
  }

  if (IS_DIGIT(c) || (c == '.' && i + 1 < input_len && IS_DIGIT(input[i + 1]))) {
    it->ctx = HL_CTX_NONE;
    size_t start = i;
    if (c == '0' && i + 1 < input_len) {
      unsigned char next = (unsigned char)input[i + 1];
      if (next == 'x' || next == 'X') {
        i += 2;
        while (i < input_len && (IS_XDIGIT(input[i]) || input[i] == '_')) i++;
        goto num_done;
      } else if (next == 'b' || next == 'B') {
        i += 2;
        while (i < input_len && (input[i] == '0' || input[i] == '1' || input[i] == '_')) i++;
        goto num_done;
      } else if (next == 'o' || next == 'O') {
        i += 2;
        while (i < input_len && (IS_OCTAL(input[i]) || input[i] == '_')) i++;
        goto num_done;
      }
    }
    while (i < input_len && (IS_DIGIT(input[i]) || input[i] == '_')) i++;
    if (i < input_len && input[i] == '.') {
      i++;
      while (i < input_len && (IS_DIGIT(input[i]) || input[i] == '_')) i++;
    }
    if (i < input_len && (input[i] == 'e' || input[i] == 'E')) {
      i++;
      if (i < input_len && (input[i] == '+' || input[i] == '-')) i++;
      while (i < input_len && (IS_DIGIT(input[i]) || input[i] == '_')) i++;
    }
    num_done:
    if (i < input_len && input[i] == 'n') i++;
    *out = (hl_span){ start, i - start, HL_NUMBER };
    it->pos = i;
    return true;
  }

  for (int k = 0; k < (int)OP_COUNT; k++) {
    int oplen = operators[k].len;
    if (i + (size_t)oplen <= input_len &&
        memcmp(input + i, operators[k].op, (size_t)oplen) == 0) {
      it->ctx = HL_CTX_NONE;
      *out = (hl_span){ i, (size_t)oplen, operators[k].cls };
      it->pos = i + (size_t)oplen;
      return true;
    }
  }

  if (is_ident_begin(c)) {
    size_t start = i;
    i++;
    while (i < input_len && is_ident_continue(input[i])) i++;
    size_t word_len = i - start;
    const char *word = input + start;

    bool is_method = false;
    if (start > 0 && input[start - 1] == '.') {
      size_t peek = i;
      while (peek < input_len && input[peek] == ' ') peek++;
      if (peek < input_len && input[peek] == '(') is_method = true;
    }

    hl_token_class cls = HL_NONE;

    if (is_method) {
      cls = HL_FUNCTION;
    } else if (start > 0 && input[start - 1] == '.') {
      cls = HL_PROPERTY;
    } else if (it->ctx == HL_CTX_AFTER_FUNCTION) {
      cls = HL_FUNCTION_NAME;
      it->ctx = HL_CTX_NONE;
    } else if (it->ctx == HL_CTX_AFTER_CLASS) {
      cls = HL_CLASS_NAME;
      it->ctx = HL_CTX_NONE;
    } else if (it->ctx == HL_CTX_AFTER_EXTENDS) {
      cls = HL_PARENT_CLASS;
      it->ctx = HL_CTX_NONE;
    } else {
      cls = lookup_extra_keyword(word, word_len);

      if (cls == HL_NONE) {
        if ((word_len == 3 && memcmp(word, "NaN", 3) == 0) ||
            (word_len == 8 && memcmp(word, "Infinity", 8) == 0)) {
          cls = HL_NUMBER;
        }
        else if (word_len == 7 && memcmp(word, "extends", 7) == 0) {
          cls = HL_KEYWORD_EXTENDS;
        } else {
          cls = tok_to_class(sv_parsekeyword(word, word_len));
        }
      }

      if (cls == HL_NONE) {
        size_t peek = i;
        while (peek < input_len && input[peek] == ' ') peek++;
        if (peek < input_len && input[peek] == ':' &&
            (peek + 1 >= input_len || input[peek + 1] != ':'))
          cls = HL_PROPERTY;
      }

      if (cls == HL_NONE && word[0] >= 'A' && word[0] <= 'Z') {
        cls = HL_TYPE;
      }

      hl_context next_ctx = keyword_sets_context(word, word_len);
      if (next_ctx != HL_CTX_NONE) it->ctx = next_ctx;
    }

    *out = (hl_span){ start, word_len, cls };
    it->pos = i;
    return true;
  }

  if (c == '<' || c == '>' || c == '=') {
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ i, 1, HL_OPERATOR_CMP };
    it->pos = i + 1;
    return true;
  }

  if (c == ' ' || c == '\t') {
    size_t start = i;
    while (i < input_len && (input[i] == ' ' || input[i] == '\t')) i++;
    *out = (hl_span){ start, i - start, HL_NONE };
    it->pos = i;
    return true;
  }

  it->ctx = HL_CTX_NONE;
  *out = (hl_span){ i, 1, HL_NONE };
  it->pos = i + 1;
  return true;
}

typedef struct {
  char  *buf;
  size_t size;
  size_t pos;
  bool   overflow;
} outbuf_t;

static inline void ob_putc(outbuf_t *o, char c) {
  if (o->pos + 1 < o->size) o->buf[o->pos++] = c;
  else o->overflow = true;
}

static inline void ob_write(outbuf_t *o, const char *s, size_t n) {
  if (o->pos + n < o->size) {
    memcpy(o->buf + o->pos, s, n);
    o->pos += n;
  } else o->overflow = true;
}

static inline void ob_puts(outbuf_t *o, const char *s) {
  ob_write(o, s, strlen(s));
}

static inline void ob_put_escaped(outbuf_t *o, char c) {
switch (c) {
  case '<': ob_write(o, "<<", 2); break;
  case '>': ob_write(o, ">>", 2); break;
  case '%': ob_write(o, "%%", 2); break;
  default:  ob_putc(o, c);        break;
}}

static inline void ob_write_escaped(outbuf_t *o, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) ob_put_escaped(o, s[i]);
}

static const char *class_to_crvar(hl_token_class cls) {
switch (cls) {
  case HL_KEYWORD:         return "blue";
  case HL_KEYWORD_ITALIC:  return "italic+blue";
  case HL_KEYWORD_DELETE:  return "red";
  case HL_KEYWORD_EXTENDS: return "italic+cyan";
  case HL_TYPE:            return "cyan";
  case HL_TYPE_STRING:     return "green";
  case HL_TYPE_BOOLEAN:    return "magenta";
  case HL_LITERAL_NULL:    return "gray";
  case HL_STRING:          return "green";
  case HL_NUMBER:          return "yellow";
  case HL_COMMENT:         return "dim";
  case HL_FUNCTION_NAME:   return "green";
  case HL_CLASS_NAME:      return "bold+yellow";
  case HL_PARENT_CLASS:    return "bold+cyan";
  case HL_FUNCTION:        return "green";
  case HL_PROPERTY:        return "bright_magenta";
  case HL_OPERATOR:        return "bright_magenta";
  case HL_OPERATOR_CMP:    return "gray";
  case HL_OPTIONAL_CHAIN:  return "gray";
  case HL_SEMICOLON:       return "dim";
  default:                 return NULL;
}}

int ant_highlight_stateful(
  const char *input, size_t input_len,
  char *out, size_t out_size,
  highlight_state *state
) {
  outbuf_t o = { .buf = out, .size = out_size, .pos = 0, .overflow = false };

  hl_iter it;
  hl_iter_init(&it, input, input_len, state);

  hl_span span;
  while (hl_iter_next(&it, &span) && !o.overflow) {
    const char *var = class_to_crvar(span.cls);
    if (var) {
      ob_putc(&o, '<');
      ob_puts(&o, var);
      ob_putc(&o, '>');
      ob_write_escaped(&o, input + span.off, span.len);
      ob_write(&o, "</>", 3);
    } else {
      ob_write_escaped(&o, input + span.off, span.len);
    }
  }

  *state = hl_iter_state(&it);

  if (o.overflow) {
    o.pos = 0;
    o.overflow = false;
    ob_write_escaped(&o, input, input_len);
    if (o.overflow) {
      size_t safe = out_size > 1 ? out_size - 1 : 0;
      if (safe > input_len) safe = input_len;
      memcpy(out, input, safe);
      out[safe] = '\0';
      return (int)safe;
    }
  }

  o.buf[o.pos] = '\0';
  return (int)o.pos;
}

int ant_highlighl(const char *input, size_t input_len, char *out, size_t out_size) {
  highlight_state state = { .mode = HL_STATE_NORMAL, .template_depth = 0 };
  return ant_highlight_stateful(input, input_len, out, out_size, &state);
}

int highlight_js_line_clipped(
  const char *line, size_t line_len,
  size_t max_cols,
  char *out, size_t out_size,
  highlight_state *state
) {
  outbuf_t o = { .buf = out, .size = out_size, .pos = 0, .overflow = false };

  hl_iter it;
  hl_iter_init(&it, line, line_len, state);

  size_t vis_cols = 0;
  hl_span span;

  while (hl_iter_next(&it, &span)) {
    if (vis_cols >= max_cols) continue;

    size_t span_remaining = max_cols - vis_cols;
    size_t emit_len = span.len < span_remaining ? span.len : span_remaining;

    if (!o.overflow) {
      const char *var = class_to_crvar(span.cls);
      if (var) {
        ob_putc(&o, '<');
        ob_puts(&o, var);
        ob_putc(&o, '>');
        ob_write_escaped(&o, line + span.off, emit_len);
        ob_write(&o, "</>", 3);
      } else {
        ob_write_escaped(&o, line + span.off, emit_len);
      }
    }

    vis_cols += span.len;
  }

  *state = hl_iter_state(&it);

  if (o.overflow) {
    o.pos = 0;
    size_t emit = line_len < max_cols ? line_len : max_cols;
    ob_write_escaped(&o, line, emit);
    if (o.overflow) {
      size_t safe = out_size > 1 ? out_size - 1 : 0;
      if (safe > emit) safe = emit;
      memcpy(out, line, safe);
      out[safe] = '\0';
      return (int)safe;
    }
  }

  o.buf[o.pos] = '\0';
  return (int)o.pos;
}
