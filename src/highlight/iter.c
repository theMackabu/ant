#include <string.h>
#include <stdbool.h>

#include "tokens.h"
#include "highlight.h"
#include "highlight/regex.h"
#include "silver/lexer.h"

typedef struct { const char *op; int len; hl_token_class cls; } op_entry_t;

static const op_entry_t operators[] = {
  { "===", 3, HL_OPERATOR },
  { "!==", 3, HL_OPERATOR },
  { "...", 3, HL_OPERATOR },
  { "=>",  2, HL_OPERATOR },
  { "==",  2, HL_OPERATOR },
  { "!=",  2, HL_OPERATOR },
  { "<=",  2, HL_OPERATOR },
  { ">=",  2, HL_OPERATOR },
  { "&&",  2, HL_OPERATOR },
  { "||",  2, HL_OPERATOR },
  { "??",  2, HL_OPERATOR },
  { "?.",  2, HL_OPTIONAL_CHAIN },
};

#define OP_COUNT (sizeof(operators) / sizeof(operators[0]))
#define K(s, t) if (len == sizeof(s)-1 && !memcmp(word, s, sizeof(s)-1)) return t

static hl_token_class lookup_extra_keyword(const char *word, size_t len) {
  switch (word[0]) {
  case 'a':
    K("abstract", HL_TYPE);
    K("async", HL_KEYWORD_ITALIC);
    break;
  case 'b': K("boolean", HL_TYPE_BOOLEAN); break;
  case 'd': K("declare", HL_TYPE); break;
  case 'e':
    K("enum", HL_TYPE);
    K("export", HL_KEYWORD_ITALIC);
    break;
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
  l_bool:      return HL_BOOLEAN;
  l_null:      return HL_LITERAL_NULL;
}

void hl_iter_init(hl_iter *it, const char *input, size_t input_len, const highlight_state *state) {
  it->input = input;
  it->input_len = input_len;
  it->pos = 0;
  it->state = state ? *state : HL_STATE_INIT;
  it->ctx = HL_CTX_NONE;
}

static hl_context keyword_sets_context(const char *word, size_t len) {
  if (len == 8 && memcmp(word, "function", 8) == 0) return HL_CTX_AFTER_FUNCTION;
  if (len == 5 && memcmp(word, "class", 5) == 0)    return HL_CTX_AFTER_CLASS;
  if (len == 7 && memcmp(word, "extends", 7) == 0)  return HL_CTX_AFTER_EXTENDS;
  return HL_CTX_NONE;
}

static size_t skip_inline_ws_forward(const char *input, size_t input_len, size_t i) {
  while (i < input_len && (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\r')) i++;
  return i;
}

static size_t skip_inline_ws_backward(const char *input, size_t i) {
  while (i > 0 && (input[i - 1] == ' ' || input[i - 1] == '\t' || input[i - 1] == '\n' || input[i - 1] == '\r')) i--;
  return i;
}

static bool read_prev_word(const char *input, size_t end, size_t *word_start, size_t *word_len) {
  size_t i = skip_inline_ws_backward(input, end);
  if (i == 0 || !is_ident_continue((unsigned char)input[i - 1])) return false;

  size_t wend = i;
  while (i > 0 && is_ident_continue((unsigned char)input[i - 1])) i--;

  *word_start = i;
  *word_len = wend - i;
  return true;
}

static bool is_arrow_after(const char *input, size_t input_len, size_t pos) {
  size_t i = skip_inline_ws_forward(input, input_len, pos);
  return (i + 1 < input_len && input[i] == '=' && input[i + 1] == '>');
}

static bool find_matching_close_paren(const char *input, size_t input_len, size_t open_paren, size_t *close_paren) {
  size_t depth = 0;
  for (size_t i = open_paren + 1; i < input_len; i++) {
    unsigned char ch = (unsigned char)input[i];
    if (ch == '(') {
      depth++;
      continue;
    }
    if (ch == ')') {
      if (depth == 0) {
        *close_paren = i;
        return true;
      }
      depth--;
    }
  }
  return false;
}

static bool has_declaration_keyword_before(const char *input, size_t ident_start) {
  size_t ws, wstart, wlen;
  ws = skip_inline_ws_backward(input, ident_start);
  if (ws == 0) return false;

  size_t wend = ws;
  size_t i = ws;
  while (i > 0 && is_ident_continue((unsigned char)input[i - 1])) i--;
  wstart = i;
  wlen = wend - i;

  return (wlen == 5 && memcmp(input + wstart, "const", 5) == 0) ||
         (wlen == 3 && memcmp(input + wstart, "let", 3) == 0) ||
         (wlen == 3 && memcmp(input + wstart, "var", 3) == 0);
}

static bool is_assigned_arrow_function(const char *input, size_t input_len, size_t ident_start, size_t ident_end) {
  if (!has_declaration_keyword_before(input, ident_start)) return false;

  size_t i = skip_inline_ws_forward(input, input_len, ident_end);
  if (i >= input_len || input[i] != '=') return false;
  if (i + 1 < input_len && input[i + 1] == '>') return false;
  i++;

  i = skip_inline_ws_forward(input, input_len, i);
  if (i >= input_len) return false;

  if (is_arrow_after(input, input_len, i)) return false;

  if (is_ident_begin((unsigned char)input[i])) {
    size_t j = i + 1;
    while (j < input_len && is_ident_continue((unsigned char)input[j])) j++;
    if (is_arrow_after(input, input_len, j)) return true;
  }

  if (input[i] == '(') {
    size_t close = 0;
    if (find_matching_close_paren(input, input_len, i, &close))
      if (is_arrow_after(input, input_len, close + 1)) return true;
  }

  if (is_ident_begin((unsigned char)input[i]) || input[i] == '(') {
    size_t j = i;
    if (input[j] == '(') {
      size_t close = 0;
      if (find_matching_close_paren(input, input_len, j, &close)) j = close + 1;
      else return false;
    } else {
      j++;
      while (j < input_len && is_ident_continue((unsigned char)input[j])) j++;
    }

    j = skip_inline_ws_forward(input, input_len, j);
    if (j < input_len && input[j] == ':') {
      j++;
      size_t depth = 0;
      while (j < input_len) {
        unsigned char ch = (unsigned char)input[j];
        if (ch == '(' || ch == '<') { depth++; j++; continue; }
        if (ch == ')' || ch == '>') {
          if (depth > 0) depth--;
          j++;
          continue;
        }
        if (depth == 0 && (ch == '=' || ch == ',')) break;
        j++;
      }
      if (is_arrow_after(input, input_len, j)) return true;
    }
  }

  if (input[i] == 'a') {
    if (i + 5 <= input_len && memcmp(input + i, "async", 5) == 0 &&
        (i + 5 >= input_len || !is_ident_continue((unsigned char)input[i + 5]))) {
      size_t j = skip_inline_ws_forward(input, input_len, i + 5);
      if (j < input_len && input[j] == '(') {
        size_t close = 0;
        if (find_matching_close_paren(input, input_len, j, &close))
          if (is_arrow_after(input, input_len, close + 1)) return true;
      }
      if (j < input_len && is_ident_begin((unsigned char)input[j])) {
        size_t k = j + 1;
        while (k < input_len && is_ident_continue((unsigned char)input[k])) k++;
        if (is_arrow_after(input, input_len, k)) return true;
      }
    }
  }

  if (input[i] == 'f') {
    if (i + 8 <= input_len && memcmp(input + i, "function", 8) == 0 &&
        (i + 8 >= input_len || !is_ident_continue((unsigned char)input[i + 8])))
      return true;
  }

  return false;
}

static bool has_function_keyword_before_paren(const char *input, size_t open_paren) {
  size_t word_start = 0;
  size_t word_len = 0;

  if (!read_prev_word(input, open_paren, &word_start, &word_len)) return false;
  if (word_len == 8 && memcmp(input + word_start, "function", 8) == 0) return true;

  if (!read_prev_word(input, word_start, &word_start, &word_len)) return false;
  return (word_len == 8 && memcmp(input + word_start, "function", 8) == 0);
}

static bool is_control_paren_prefix(const char *input, size_t open_paren) {
  size_t word_start = 0;
  size_t word_len = 0;
  if (!read_prev_word(input, open_paren, &word_start, &word_len)) return false;

#define C(s) (word_len == sizeof(s) - 1 && memcmp(input + word_start, s, sizeof(s) - 1) == 0)
  return C("if") || C("for") || C("while") || C("switch") || C("catch") || C("with");
#undef C
}

static bool is_likely_function_param_paren(
  const char *input, size_t input_len,
  size_t open_paren, size_t close_paren
) {
  if (is_arrow_after(input, input_len, close_paren + 1)) return true;
  if (has_function_keyword_before_paren(input, open_paren)) return true;

  size_t after = skip_inline_ws_forward(input, input_len, close_paren + 1);
  if (after < input_len && input[after] == '{' && !is_control_paren_prefix(input, open_paren))
    return true;

  return false;
}

static bool find_enclosing_open_paren(const char *input, size_t pos, size_t *open_paren) {
  size_t depth = 0;
  size_t i = pos;

  while (i > 0) {
    i--;
    unsigned char ch = (unsigned char)input[i];
    if (ch == ')') {
      depth++;
      continue;
    }
    if (ch == '(') {
      if (depth == 0) {
        *open_paren = i;
        return true;
      }
      depth--;
    }
  }
  return false;
}

static bool is_function_argument_identifier(const char *input, size_t input_len, size_t start, size_t end) {
  if (is_arrow_after(input, input_len, end)) {
    size_t left = skip_inline_ws_backward(input, start);
    if (left > 0 && input[left - 1] == '.') return false;
    return true;
  }

  size_t prev = skip_inline_ws_backward(input, start);
  if (prev == 0) return false;
  unsigned char prev_ch = (unsigned char)input[prev - 1];
  if (!(prev_ch == '(' || prev_ch == ',' || prev_ch == '{' || prev_ch == '[' || prev_ch == ':'))
    return false;

  size_t open_paren = 0;
  if (!find_enclosing_open_paren(input, start, &open_paren)) return false;

  size_t close_paren = 0;
  if (!find_matching_close_paren(input, input_len, open_paren, &close_paren)) return false;
  return is_likely_function_param_paren(input, input_len, open_paren, close_paren);
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
      *out = (hl_span){ i, 1, HL_BRACKET };
      it->pos = i + 1;
      return true;
    }
  }
  if (it->state.mode == HL_STATE_TEMPLATE_EXPR && c == '{') {
    it->state.template_depth++;
    *out = (hl_span){ i, 1, HL_BRACKET };
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

  if (c == '/') {
    size_t regex_end = 0;
    if (js_scan_regex_literal(input, input_len, i, &regex_end)) {
      it->ctx = HL_CTX_NONE;
      *out = (hl_span){ i, regex_end - i, HL_REGEX };
      it->pos = regex_end;
      return true;
    }
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

  if (c == '#' && i + 1 < input_len && is_ident_begin((unsigned char)input[i + 1])) {
    size_t start = i;
    i += 2;
    while (i < input_len && is_ident_continue((unsigned char)input[i])) i++;
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ start, i - start, HL_PROPERTY };
    it->pos = i;
    return true;
  }

  if (is_ident_begin(c)) {
    size_t start = i;
    i++;
    while (i < input_len && is_ident_continue(input[i])) i++;
    size_t word_len = i - start;
    const char *word = input + start;

    bool is_member_access = (start > 0 && input[start - 1] == '.' &&
                             (start < 2 || input[start - 2] != '.'));
    bool is_method = false;
    if (is_member_access) {
      size_t peek = i;
      while (peek < input_len && input[peek] == ' ') peek++;
      if (peek < input_len && input[peek] == '(') is_method = true;
    }
    size_t after_word = i;
    while (after_word < input_len && input[after_word] == ' ') after_word++;
    bool is_call = (after_word < input_len && input[after_word] == '(');

    hl_token_class cls = HL_NONE;
    bool is_console = (word_len == 7 && memcmp(word, "console", 7) == 0);

    if (is_console) {
      cls = HL_PROPERTY;
    } else if (is_function_argument_identifier(input, input_len, start, i)) {
      cls = HL_ARGUMENT;
    } else if (is_method) {
      cls = HL_FUNCTION;
    } else if (is_member_access) {
      cls = HL_PROPERTY;
    } else if (is_assigned_arrow_function(input, input_len, start, i)) {
      cls = HL_FUNCTION_NAME;
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

      if (cls == HL_NONE && is_call) {
        cls = HL_FUNCTION;
      }

      hl_context next_ctx = keyword_sets_context(word, word_len);
      if (next_ctx != HL_CTX_NONE) it->ctx = next_ctx;
    }

    *out = (hl_span){ start, word_len, cls };
    it->pos = i;
    return true;
  }

  if (c == '<' || c == '>' || c == '=' ||
      c == '+' || c == '-' || c == '*' || c == '/' ||
      c == '%' || c == '&' || c == '|' || c == '^' ||
      c == '~' || c == '!' || c == '?') {
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ i, 1, HL_OPERATOR };
    it->pos = i + 1;
    return true;
  }

  if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
    it->ctx = HL_CTX_NONE;
    *out = (hl_span){ i, 1, HL_BRACKET };
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
