#ifndef REGEX_SCAN_H
#define REGEX_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool js_regex_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline bool js_regex_is_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}

static inline bool js_regex_is_alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool js_regex_is_ident_char(unsigned char c) {
  return js_regex_is_alpha(c) || js_regex_is_digit(c) || c == '_' || c == '$';
}

static inline bool js_regex_word_eq(const char *word, size_t len, const char *lit, size_t lit_len) {
  return len == lit_len && memcmp(word, lit, lit_len) == 0;
}

static inline size_t js_regex_skip_ws_back(const char *code, size_t i) {
  while (i > 0 && js_regex_is_space((unsigned char)code[i - 1])) i--;
  return i;
}

static inline bool js_regex_word_allows_start(const char *word, size_t len) {
  return 
    js_regex_word_eq(word, len, "return", 6) ||
    js_regex_word_eq(word, len, "throw", 5) ||
    js_regex_word_eq(word, len, "case", 4) ||
    js_regex_word_eq(word, len, "delete", 6) ||
    js_regex_word_eq(word, len, "void", 4) ||
    js_regex_word_eq(word, len, "new", 3) ||
    js_regex_word_eq(word, len, "typeof", 6) ||
    js_regex_word_eq(word, len, "instanceof", 10) ||
    js_regex_word_eq(word, len, "in", 2) ||
    js_regex_word_eq(word, len, "of", 2) ||
    js_regex_word_eq(word, len, "yield", 5) ||
    js_regex_word_eq(word, len, "await", 5);
}

static inline bool js_regex_prev_forbids_start(unsigned char prev) {
  return 
    js_regex_is_digit(prev) ||
    prev == ')' || prev == ']' || prev == '}' ||
    prev == '"' || prev == '\'' || prev == '`' || prev == '.';
}

static inline bool js_regex_prev_allows_start(unsigned char prev) {
switch (prev) {
  case '(':
  case '[':
  case '{':
  case ',':
  case ';':
  case ':':
  case '=':
  case '!':
  case '?':
  case '+':
  case '-':
  case '*':
  case '%':
  case '&':
  case '|':
  case '^':
  case '~':
  case '<':
  case '>': return true;
  default:  return false;
}}

static inline bool js_regex_can_start(const char *code, size_t start) {
  if (start == 0) return true;

  size_t i = js_regex_skip_ws_back(code, start);
  if (i == 0) return true;

  unsigned char prev = (unsigned char)code[i - 1];

  if (js_regex_is_ident_char(prev)) {
    size_t end = i;
    while (i > 0 && js_regex_is_ident_char((unsigned char)code[i - 1])) i--;
    return js_regex_word_allows_start(code + i, end - i);
  }

  if (js_regex_prev_forbids_start(prev)) return false;
  return js_regex_prev_allows_start(prev);
}

static inline bool js_scan_regex_literal(
  const char *code, size_t len,
  size_t start, size_t *out_end
) {
  if (start >= len || code[start] != '/') return false;
  if (start + 1 >= len || code[start + 1] == '/' || code[start + 1] == '*') return false;
  if (!js_regex_can_start(code, start)) return false;

  size_t i = start + 1;
  bool in_class = false;

  for (; i < len; i++) {
    unsigned char ch = (unsigned char)code[i];
    if (ch == '\n' || ch == '\r') return false;
    
    if (ch == '\\') {
      if (i + 1 < len) i++;
      continue;
    }
    
    if (in_class) {
      if (ch == ']') in_class = false;
      continue;
    }
    
    if (ch == '[') {
      in_class = true;
      continue;
    }
    
    if (ch != '/') continue;
    
    i++;
    while (i < len && js_regex_is_alpha((unsigned char)code[i])) i++;
    
    if (out_end) *out_end = i;
    return true;
  }

  return false;
}

#endif
