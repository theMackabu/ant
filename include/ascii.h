#pragma once

#include <stdbool.h>

static inline bool ant_ascii_is_digit(int c) {
  return c >= '0' && c <= '9';
}

static inline bool ant_ascii_is_upper(int c) {
  return c >= 'A' && c <= 'Z';
}

static inline bool ant_ascii_is_lower(int c) {
  return c >= 'a' && c <= 'z';
}

static inline bool ant_ascii_is_field_char(int c) {
  return c == '\t' || (c >= 0x20 && c != 0x7f);
}

static inline bool ant_ascii_is_alpha(int c) {
  return ant_ascii_is_upper(c) || ant_ascii_is_lower(c);
}

static inline bool ant_ascii_is_alnum(int c) {
  return ant_ascii_is_alpha(c) || ant_ascii_is_digit(c);
}

static inline bool ant_ascii_is_xdigit(int c) {
  return ant_ascii_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool ant_ascii_is_space(int c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

static inline int ant_ascii_to_lower(int c) {
  return ant_ascii_is_upper(c) ? c + ('a' - 'A') : c;
}

static inline int ant_ascii_to_upper(int c) {
  return ant_ascii_is_lower(c) ? c - ('a' - 'A') : c;
}

static inline int ant_ascii_hex_value(int c) {
  if (ant_ascii_is_digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static inline bool ant_ascii_is_tchar(int c) {
  if (ant_ascii_is_alnum(c)) return true;
  
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~': return true;
    default: return false;
  }
}
