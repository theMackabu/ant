#include <string.h>

#include "theme.h"
#include "highlight/emit.h"

static inline void ob_putc(hl_outbuf *o, char c) {
  if (o->pos + 1 < o->size) o->buf[o->pos++] = c;
  else o->overflow = true;
}

static inline void ob_write(hl_outbuf *o, const char *s, size_t n) {
  if (o->pos + n < o->size) {
    memcpy(o->buf + o->pos, s, n);
    o->pos += n;
  } else o->overflow = true;
}

static inline void ob_puts(hl_outbuf *o, const char *s) {
  ob_write(o, s, strlen(s));
}

static inline void ob_put_escaped(hl_outbuf *o, char c) {
  switch (c) {
    case '<': ob_write(o, "<<", 2); break;
    case '>': ob_write(o, ">>", 2); break;
    case '%': ob_write(o, "%%", 2); break;
    default:  ob_putc(o, c);        break;
  }
}

void hl_outbuf_write_escaped(hl_outbuf *o, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) ob_put_escaped(o, s[i]);
}

static inline void ob_write_with_class(hl_outbuf *o, hl_token_class cls, const char *s, size_t n) {
  if (n == 0) return;

  const char *var = hl_theme_color(cls);
  if (var) {
    ob_putc(o, '<');
    ob_puts(o, var);
    ob_putc(o, '>');
    hl_outbuf_write_escaped(o, s, n);
    ob_write(o, "</>", 3);
  } else hl_outbuf_write_escaped(o, s, n);
}

static bool span_is_template_string(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '`') return true;
    if (s[i] == '$' && i + 1 < n && s[i + 1] == '{') return true;
  }
  return false;
}

static void emit_string_literal(hl_outbuf *o, const char *s, size_t n, hl_token_class body_cls) {
  if (n == 0) return;

  size_t i = 0;
  size_t seg_start = 0;

  while (i < n) {
    unsigned char ch = (unsigned char)s[i];

    if (ch == '\\') {
      ob_write_with_class(o, body_cls, s + seg_start, i - seg_start);
      size_t esc_len = (i + 1 < n) ? 2 : 1;
      ob_write_with_class(o, HL_STRING_ESCAPE, s + i, esc_len);
      i += esc_len;
      seg_start = i;
      continue;
    }

    if (ch == '"' || ch == '\'' || ch == '`') {
      ob_write_with_class(o, body_cls, s + seg_start, i - seg_start);
      ob_write_with_class(o, HL_STRING_DELIMITER, s + i, 1);
      i++;
      seg_start = i;
      continue;
    }

    if (ch == '$' && i + 1 < n && s[i + 1] == '{') {
      ob_write_with_class(o, body_cls, s + seg_start, i - seg_start);
      ob_write_with_class(o, HL_BRACKET, s + i, 1);
      ob_write_with_class(o, HL_BRACKET, s + i + 1, 1);
      i += 2;
      seg_start = i;
      continue;
    }

    i++;
  }

  ob_write_with_class(o, body_cls, s + seg_start, n - seg_start);
}

static void emit_regex_literal(hl_outbuf *o, const char *s, size_t n) {
  if (n == 0) return;

  ob_write_with_class(o, HL_REGEX_DELIMITER, s, 1);

  size_t i = 1;
  size_t seg_start = i;
  bool in_class = false;

  while (i < n) {
    unsigned char ch = (unsigned char)s[i];

    if (!in_class && ch == '/') {
      ob_write_with_class(o, HL_REGEX, s + seg_start, i - seg_start);
      ob_write_with_class(o, HL_REGEX_DELIMITER, s + i, 1);
      i++;
      ob_write_with_class(o, HL_REGEX_DELIMITER, s + i, n - i);
      return;
    }

    if (ch == '\\') {
      ob_write_with_class(o, in_class ? HL_REGEX_CDATA : HL_REGEX, s + seg_start, i - seg_start);
      size_t esc_len = (i + 1 < n) ? 2 : 1;
      ob_write_with_class(o, HL_REGEX_ESCAPE, s + i, esc_len);
      i += esc_len;
      seg_start = i;
      continue;
    }

    if (!in_class && ch == '[') {
      ob_write_with_class(o, HL_REGEX, s + seg_start, i - seg_start);
      in_class = true;
      seg_start = i;
      i++;
      continue;
    }

    if (in_class && ch == ']') {
      i++;
      ob_write_with_class(o, HL_REGEX_CDATA, s + seg_start, i - seg_start);
      in_class = false;
      seg_start = i;
      continue;
    }

    i++;
  }

  ob_write_with_class(o, in_class ? HL_REGEX_CDATA : HL_REGEX, s + seg_start, n - seg_start);
}

static void emit_number_literal(hl_outbuf *o, const char *s, size_t n) {
  if (n >= 2 && s[0] == '0' &&
      (s[1] == 'x' || s[1] == 'X' ||
       s[1] == 'b' || s[1] == 'B' ||
       s[1] == 'o' || s[1] == 'O')) {
    ob_write_with_class(o, HL_NUMBER_PREFIX, s, 2);
    ob_write_with_class(o, HL_NUMBER, s + 2, n - 2);
    return;
  }

  ob_write_with_class(o, HL_NUMBER, s, n);
}

void hl_outbuf_init(hl_outbuf *o, char *buf, size_t size) {
  o->buf = buf;
  o->size = size;
  o->pos = 0;
  o->overflow = false;
}

void hl_outbuf_emit_span(hl_outbuf *o, hl_token_class cls, const char *s, size_t n) {
  if (cls == HL_STRING) {
    hl_token_class body_cls = span_is_template_string(s, n) ? HL_STRING_TEMPLATE : HL_STRING;
    emit_string_literal(o, s, n, body_cls);
    return;
  }

  if (cls == HL_REGEX) {
    emit_regex_literal(o, s, n);
    return;
  }

  if (cls == HL_NUMBER) {
    emit_number_literal(o, s, n);
    return;
  }

  ob_write_with_class(o, cls, s, n);
}
