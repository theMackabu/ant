#include "regex_internal.h"

static inline bool is_pcre2_passthrough_escape(char c) {
switch (c) {
  case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
  case 'b': case 'B': case 'n': case 'r': case 't': case 'f':
  case '1': case '2': case '3': case '4': case '5':
  case '6': case '7': case '8': case '9':
  case '.': case '*': case '+': case '?':
  case '(': case ')': case '[': case ']':
  case '{': case '}': case '|': case '^':
  case '$': case '\\': case '/': case '-': return true;
  default: return false;
}}

static inline bool is_class_shorthand(char c) {
  return c == 'w' || c == 'W' || c == 'd' || c == 'D' || c == 's' || c == 'S';
}

static const char regexp_space_ranges[] =
  "\\x{9}-\\x{d}\\x{20}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}"
  "\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}\\x{feff}";

static const char regexp_non_space_ranges[] =
  "\\x{0}-\\x{8}\\x{e}-\\x{1f}\\x{21}-\\x{9f}\\x{a1}-\\x{167f}"
  "\\x{1681}-\\x{1fff}\\x{200b}-\\x{2027}\\x{202a}-\\x{202e}"
  "\\x{2030}-\\x{205e}\\x{2060}-\\x{2fff}\\x{3001}-\\x{fefe}"
  "\\x{ff00}-\\x{10ffff}";

static const char regexp_folded_word_ranges[] = "\\w\\x{17f}\\x{212a}";

static const char regexp_folded_non_word_ranges[] =
  "\\x{0}-\\x{2f}\\x{3a}-\\x{40}\\x{5b}-\\x{5e}\\x{60}"
  "\\x{7b}-\\x{17e}\\x{180}-\\x{2129}\\x{212b}-\\x{10ffff}";

static const char regexp_folded_boundary[] =
  "(?:(?<=[\\w\\x{17f}\\x{212a}])(?![\\w\\x{17f}\\x{212a}])"
  "|(?<![\\w\\x{17f}\\x{212a}])(?=[\\w\\x{17f}\\x{212a}]))";

static const char regexp_folded_non_boundary[] =
  "(?:(?<=[\\w\\x{17f}\\x{212a}])(?=[\\w\\x{17f}\\x{212a}])"
  "|(?<![\\w\\x{17f}\\x{212a}])(?![\\w\\x{17f}\\x{212a}]))";

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool plain_line_anchors;
  bool line_anchors;
} regexp_pattern_builder_t;

static const struct {
  uint32_t cp;
  const char *utf8;
  const char *pcre2;
  uint8_t partners;
} regexp_legacy_caseless_fixes[] = {
  { 0xDF, "\xC3\x9F", "(?-i:\xC3\x9F)", 1 << 1 },
  { 0x1E9E, "\xE1\xBA\x9E", "(?-i:\xE1\xBA\x9E)", 1 << 0 },
  { 0x2126, "\xE2\x84\xA6", "(?-i:\xE2\x84\xA6)", 1 << 4 | 1 << 5 },
  { 0x212B, "\xE2\x84\xAB", "(?-i:\xE2\x84\xAB)", 1 << 6 | 1 << 7 },
  { 0x3C9, "\xCF\x89", "(?-i:[\xCF\x89\xCE\xA9])", 1 << 2 },
  { 0x3A9, "\xCE\xA9", "(?-i:[\xCF\x89\xCE\xA9])", 1 << 2 },
  { 0xE5, "\xC3\xA5", "(?-i:[\xC3\xA5\xC3\x85])", 1 << 3 },
  { 0xC5, "\xC3\x85", "(?-i:[\xC3\xA5\xC3\x85])", 1 << 3 },
};

static constexpr size_t REGEXP_LEGACY_CASELESS_FIX_COUNT =
  sizeof(regexp_legacy_caseless_fixes) / sizeof(regexp_legacy_caseless_fixes[0]);

static int regexp_legacy_caseless_index(uint32_t cp) {
  if (cp < 0xC5) return -1;
  for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++)
    if (regexp_legacy_caseless_fixes[i].cp == cp) return (int)i;
  return -1;
}

static bool regexp_hex_value(const char *src, size_t src_len, size_t at, size_t digits, uint32_t *out) {
  if (at + digits > src_len) return false;
  uint32_t value = 0;
  for (size_t k = 0; k < digits; k++) {
    char c = src[at + k];
    if (!ant_ascii_is_xdigit(c)) return false;
    value = value * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  }
  *out = value;
  return true;
}

static bool regexp_legacy_atom(const char *src, size_t src_len, size_t at, bool in_class, uint32_t *cp, size_t *next) {
  unsigned char c = (unsigned char)src[at];
  if (c != '\\') {
    utf8proc_int32_t decoded = c;
    utf8proc_ssize_t n = c < 0x80 ? 1 : utf8_next((const utf8proc_uint8_t *)src + at, (utf8proc_ssize_t)(src_len - at), &decoded);
    *cp = decoded < 0 ? c : (uint32_t)decoded;
    *next = at + (n > 0 ? (size_t)n : 1);
    return true;
  }
  if (at + 1 >= src_len) return false;
  unsigned char n = (unsigned char)src[at + 1];
  if (n >= 0x80) return regexp_legacy_atom(src, src_len, at + 1, in_class, cp, next);
  if (n == 'u' && regexp_hex_value(src, src_len, at + 2, 4, cp)) { *next = at + 6; return true; }
  if (n == 'x' && regexp_hex_value(src, src_len, at + 2, 2, cp)) { *next = at + 4; return true; }
  if (n == 'b' && !in_class) return false;
  const char *controls = strchr("tnvfrb", n);
  if (n && controls) {
    static const uint8_t control_values[] = { '\t', '\n', '\v', '\f', '\r', '\b' };
    *cp = control_values[controls - "tnvfrb"];
  } else if ((n | 0x20) >= 'a' && (n | 0x20) <= 'z') return false;
  else if (n >= '0' && n <= '9') return false;
  else *cp = n;
  *next = at + 2;
  return true;
}

static uint32_t regexp_legacy_canonicalize(uint32_t cp) {
  if (cp > 0xFFFF) return cp;
  utf8proc_int32_t upper[3];
  if (utf8_full_case_map(NULL, 0, 0, 0, (utf8proc_int32_t)cp, true, upper) != 1) return cp;
  if (upper[0] < 0 || upper[0] > 0xFFFF || (cp >= 128 && upper[0] < 128)) return cp;
  return (uint32_t)upper[0];
}

int regexp_legacy_backref_callout(pcre2_callout_block *block, void *data) {
  const compiled_regex_cache_entry_t *compiled = data;
  uint32_t group = block->callout_number;
  if (block->callout_string) {
    group = 0;
    for (uint32_t i = 0; compiled && i < compiled->namecount && !group; i++) {
      const regexp_group_name_t *entry = &compiled->group_names[i];
      if (
        entry->length == block->callout_string_length &&
        memcmp(entry->name, block->callout_string, entry->length) == 0 &&
        entry->group < block->capture_top && block->offset_vector[2 * entry->group] != PCRE2_UNSET
      ) group = entry->group;
    }
  }
  if (!group || group >= block->capture_top) return 0;

  PCRE2_SIZE at = block->offset_vector[2 * group], end = block->offset_vector[2 * group + 1];
  if (at == PCRE2_UNSET) return 0;
  const utf8proc_uint8_t *subject = block->subject;
  PCRE2_SIZE pos = block->current_position, len = block->subject_length;
  while (at < end) {
    if (pos >= len) return 1;
    utf8proc_int32_t a, b;
    utf8proc_ssize_t na = utf8_next(subject + at, (utf8proc_ssize_t)(end - at), &a);
    utf8proc_ssize_t nb = utf8_next(subject + pos, (utf8proc_ssize_t)(len - pos), &b);
    if (na <= 0 || nb <= 0 || a < 0 || b < 0) {
      size_t width = (size_t)utf8_char_len_at((const char *)subject, (size_t)end, (size_t)at);
      if (pos + width > len || memcmp(subject + at, subject + pos, width) != 0) return 1;
      at += width;
      pos += width;
      continue;
    }
    if (a != b && regexp_legacy_canonicalize((uint32_t)a) != regexp_legacy_canonicalize((uint32_t)b)) return 1;
    at += (PCRE2_SIZE)na;
    pos += (PCRE2_SIZE)nb;
  }
  return 0;
}

int regexp_callout_found(pcre2_callout_enumerate_block *block, void *data) {
  (void)block;
  *(bool *)data = true;
  return 1;
}

static uint8_t regexp_legacy_fixes_in_range(uint32_t lo, uint32_t hi) {
  uint8_t present = 0;
  for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++) {
    uint32_t cp = regexp_legacy_caseless_fixes[i].cp;
    if (cp >= lo && cp <= hi) present |= (uint8_t)(1u << i);
  }
  return present;
}

static uint8_t regexp_legacy_class_extras(const char *src, size_t src_len, size_t open, size_t *close) {
  uint8_t present = 0;
  size_t i = open + 1;
  if (i < src_len && src[i] == '^') i++;
  while (i < src_len && src[i] != ']') {
    uint32_t lo, hi;
    size_t after;
    if (!regexp_legacy_atom(src, src_len, i, true, &lo, &after)) {
      i += i + 1 < src_len ? 2 : 1;
      continue;
    }
    hi = lo;
    if (after + 1 < src_len && src[after] == '-' && src[after + 1] != ']') {
      size_t range_end;
      if (regexp_legacy_atom(src, src_len, after + 1, true, &hi, &range_end) && hi >= lo) after = range_end;
      else hi = lo;
    }
    present |= regexp_legacy_fixes_in_range(lo, hi);
    i = after;
  }
  *close = i;
  if (i >= src_len) return 0;
  uint8_t partners = 0;
  for (size_t fix = 0; fix < REGEXP_LEGACY_CASELESS_FIX_COUNT; fix++)
    if (present & (1u << fix)) partners |= regexp_legacy_caseless_fixes[fix].partners;
  return (uint8_t)(partners & ~present);
}

static size_t regexp_group_name_end(const char *src, size_t src_len, size_t open) {
  size_t end = open;
  while (end < src_len && src[end] != '>') end++;
  return end;
}

static size_t regexp_class_end(const char *src, size_t src_len, size_t open, bool v_flag) {
  int depth = 0;
  for (size_t i = open; i < src_len; i++) {
    if (src[i] == '\\') i++;
    else if (src[i] == '[' && (v_flag || depth == 0)) depth++;
    else if (src[i] == ']' && --depth == 0) return i;
  }
  return src_len;
}

static bool regexp_next_group_name(
  const char *src, size_t src_len, bool v_flag,
  size_t *at, size_t *start, size_t *end
) {
  for (size_t i = *at; i + 3 < src_len; i++) {
    if (src[i] == '\\') { i++; continue; }
    if (src[i] == '[') { i = regexp_class_end(src, src_len, i, v_flag); continue; }
    if (src[i] != '(' || src[i + 1] != '?' || src[i + 2] != '<' || src[i + 3] == '=' || src[i + 3] == '!') continue;
    *start = i + 3;
    *end = regexp_group_name_end(src, src_len, i + 3);
    *at = *end;
    return *end < src_len;
  }
  return false;
}

static uint32_t regexp_name_next_cp(const char *src, size_t end, size_t *at) {
  size_t i = *at;
  uint32_t cp, low;
  if (src[i] == '\\' && i + 2 < end && src[i + 1] == 'u') {
    if (src[i + 2] == '{') {
      for (cp = 0, i += 3; i < end && src[i] != '}'; i++)
        cp = cp * 16 + (uint32_t)(src[i] <= '9' ? src[i] - '0' : (src[i] | 0x20) - 'a' + 10);
      *at = i + 1;
      return cp;
    }
    if (regexp_hex_value(src, end, i + 2, 4, &cp)) {
      i += 6;
      if (
        cp >= 0xD800 && cp <= 0xDBFF && i + 1 < end && src[i] == '\\' && src[i + 1] == 'u' &&
        regexp_hex_value(src, end, i + 2, 4, &low) && low >= 0xDC00 && low <= 0xDFFF
      ) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        i += 6;
      }
      *at = i;
      return cp;
    }
  }
  utf8proc_int32_t decoded;
  *at = i + (size_t)utf8_next((const utf8proc_uint8_t *)src + i, (utf8proc_ssize_t)(end - i), &decoded);
  return decoded < 0 ? (unsigned char)src[i] : (uint32_t)decoded;
}

static regexp_name_span_t regexp_name_span(const char *src, size_t start, size_t end) {
  regexp_name_span_t span = { .start = start, .end = end, .hash = 0xcbf29ce484222325ull };
  span.escaped = memchr(src + start, '\\', end - start) != NULL;
  for (size_t at = start; at < end; ) {
    char utf8[4];
    const char *bytes = src + at;
    int width = 1;
    if (span.escaped) {
      width = utf8_encode(regexp_name_next_cp(src, end, &at), utf8);
      bytes = utf8;
    } else at++;
    for (int i = 0; i < width; i++) span.hash = (span.hash ^ (unsigned char)bytes[i]) * 0x100000001b3ull;
  }
  return span;
}

void regexp_name_spans_init(regexp_name_spans_t *spans, const char *src, size_t src_len, bool v_flag) {
  *spans = (regexp_name_spans_t){ .capacity = 16 };
  spans->items = spans->inline_items;
  size_t at = 0, start, end;
  while (regexp_next_group_name(src, src_len, v_flag, &at, &start, &end)) {
    if (spans->count == spans->capacity) {
      uint32_t capacity = spans->capacity * 2;
      regexp_name_span_t *items = spans->items == spans->inline_items
        ? malloc(sizeof(*items) * capacity)
        : realloc(spans->items, sizeof(*items) * capacity);
      if (!items) return;
      if (spans->items == spans->inline_items) memcpy(items, spans->inline_items, sizeof(spans->inline_items));
      spans->items = items;
      spans->capacity = capacity;
    }
    spans->items[spans->count++] = regexp_name_span(src, start, end);
  }
}

void regexp_name_spans_free(regexp_name_spans_t *spans) {
  if (spans->items != spans->inline_items) free(spans->items);
}

static bool regexp_name_spans_equal(const char *src, const regexp_name_span_t *a, const regexp_name_span_t *b) {
  size_t a_at = a->start, b_at = b->start;
  if (a->hash != b->hash) return false;
  if (!a->escaped && !b->escaped)
    return a->end - a_at == b->end - b_at && memcmp(src + a_at, src + b_at, a->end - a_at) == 0;
  while (a_at < a->end && b_at < b->end)
    if (regexp_name_next_cp(src, a->end, &a_at) != regexp_name_next_cp(src, b->end, &b_at)) return false;
  return a_at == a->end && b_at == b->end;
}

static int regexp_name_spans_ordinal(const char *src, const regexp_name_spans_t *spans, size_t start, size_t end) {
  regexp_name_span_t name = regexp_name_span(src, start, end);
  for (uint32_t i = 0; i < spans->count; i++)
    if (regexp_name_spans_equal(src, &spans->items[i], &name)) return (int)i;
  return -1;
}

static size_t regexp_name_ordinal_digits(char *out, uint32_t ordinal) {
  char digits[10];
  size_t count = 0;
  do digits[count++] = (char)('0' + ordinal % 10); while ((ordinal /= 10));
  for (size_t i = 0; i < count; i++) out[i] = digits[count - 1 - i];
  return count;
}

size_t regexp_name_span_decode(const char *src, const regexp_name_span_t *span, char *out) {
  if (!span->escaped) {
    memcpy(out, src + span->start, span->end - span->start);
    return span->end - span->start;
  }
  size_t length = 0;
  for (size_t at = span->start; at < span->end; )
    length += (size_t)utf8_encode(regexp_name_next_cp(src, span->end, &at), out + length);
  return length;
}

static void js_to_pcre2_pattern_append(
  const char *src,
  size_t src_len,
  regexp_pattern_builder_t *builder,
  bool v_flag,
  uint8_t flags
) {
  bool unicode_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && (flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  bool legacy_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && !(flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  size_t legacy_class_close = 0;
  int charclass_depth = 0;
  char *output = builder->data;
  size_t output_capacity = builder->capacity;
  size_t output_length = builder->length;
  regexp_name_spans_t names;
  bool names_ready = false;

#define OUT(ch) do { \
  size_t output_offset = output_length++; \
  if (output_offset < output_capacity) output[output_offset] = (ch); \
} while (0)

#define OUTS(str) do { \
  for (const char *output_str = (str); *output_str; output_str++) OUT(*output_str); \
} while (0)

  for (size_t si = 0; si < src_len; si++) {
    if (
      charclass_depth == 0 && src[si] == '(' && si + 3 < src_len && src[si + 1] == '?' &&
      src[si + 2] == '<' && src[si + 3] != '=' && src[si + 3] != '!'
    ) {
      size_t name_end = regexp_group_name_end(src, src_len, si + 3);
      if (!names_ready) { regexp_name_spans_init(&names, src, src_len, v_flag); names_ready = true; }
      int ordinal = name_end < src_len ? regexp_name_spans_ordinal(src, &names, si + 3, name_end) : -1;
      if (ordinal >= 0) {
        char name[16];
        size_t name_len = regexp_name_ordinal_digits(name, (uint32_t)ordinal);
        OUTS("(?<_");
        for (size_t k = 0; k < name_len; k++) OUT(name[k]);
        OUT('>');
        si = name_end;
        continue;
      }
    }

    if (src[si] == '[') {
      if (charclass_depth > 0 && !v_flag) {
        OUT('\\'); OUT('[');
        continue;
      }

      if (si + 2 < src_len && src[si + 1] == '^' && src[si + 2] == ']') {
        OUTS("[\\s\\S]");
        si += 2;
        continue;
      }

      if (si + 1 < src_len && src[si + 1] == ']') {
        OUTS("[^\\s\\S]");
        si++;
        continue;
      }

      if (legacy_ignore_case && charclass_depth == 0) {
        size_t close;
        uint8_t extras = regexp_legacy_class_extras(src, src_len, si, &close);
        if (extras) {
          bool negated = si + 1 < src_len && src[si + 1] == '^';
          OUTS(negated ? "(?:(?-i:[" : "(?:(?!(?-i:[");
          for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++)
            if (extras & (1u << i)) OUTS(regexp_legacy_caseless_fixes[i].utf8);
          OUTS(negated ? "])|" : "]))");
          legacy_class_close = close + 1;
        }
      }
      charclass_depth++;
      OUT('[');
      if (si + 1 < src_len && (src[si + 1] == ':' || src[si + 1] == '.' || src[si + 1] == '=')) {
        OUT('\\'); OUT(src[++si]);
      }
      continue;
    }
    if (src[si] == ']' && charclass_depth > 0) {
      charclass_depth--;
      OUT(']');
      if (legacy_class_close == si + 1) {
        OUT(')');
        legacy_class_close = 0;
      }
      continue;
    }

    if (!v_flag && charclass_depth > 0 && src[si] == '-' && si > 0 && src[si - 1] != '[' &&
        si + 1 < src_len && src[si + 1] != ']') {
      bool prev_is_shorthand = (si >= 2 && src[si - 2] == '\\' && is_class_shorthand(src[si - 1]));
      bool next_is_shorthand = (si + 2 < src_len && src[si + 1] == '\\' && is_class_shorthand(src[si + 2]));
      if (prev_is_shorthand || next_is_shorthand) {
        OUT('\\'); OUT('-');
        continue;
      }
      OUT('-');
      continue;
    }

    if (legacy_ignore_case && charclass_depth == 0 && ((unsigned char)src[si] >= 0xC3 || src[si] == '\\')) {
      uint32_t cp;
      size_t after;
      int fix = regexp_legacy_atom(src, src_len, si, false, &cp, &after) ? regexp_legacy_caseless_index(cp) : -1;
      if (fix >= 0) {
        OUTS(regexp_legacy_caseless_fixes[fix].pcre2);
        si = after - 1;
        continue;
      }
    }

    if (charclass_depth == 0 && src[si] == '.' && !(flags & REGEXP_FLAG_DOTALL)) {
      OUTS("[^\\n\\r\\x{2028}\\x{2029}]");
      continue;
    }

    if (charclass_depth == 0 && (src[si] == '^' || src[si] == '$') && (flags & REGEXP_FLAG_MULTILINE)) {
      if (src[si] == '^') builder->line_anchors = true;
      if (builder->plain_line_anchors) OUT(src[si]);
      else OUTS(src[si] == '^' ? "(?:^|(?<=[\\r\\x{2028}\\x{2029}]))" : "(?:$|(?=[\\r\\x{2028}\\x{2029}]))");
      continue;
    }

    if (src[si] != '\\' || si + 1 >= src_len) {
      OUT(src[si]);
      continue;
    }

    char next = src[si + 1];

    if (next == 'v') {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('b'); OUT('}');
      si++;
      continue;
    }

    if (next == 'u' && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}' && ant_ascii_is_xdigit(src[brace_end])) brace_end++;
      if (brace_end < src_len && src[brace_end] == '}' && brace_end > brace_start) {
        OUT('\\'); OUT('x'); OUT('{');
        for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
        OUT('}');
        si = brace_end;
        continue;
      }
    }

    if (next == 'u' && si + 5 < src_len &&
        ant_ascii_is_xdigit(src[si+2]) && ant_ascii_is_xdigit(src[si+3]) &&
        ant_ascii_is_xdigit(src[si+4]) && ant_ascii_is_xdigit(src[si+5])) {
      OUT('\\'); OUT('x'); OUT('{');
      OUT(src[si+2]); OUT(src[si+3]); OUT(src[si+4]); OUT(src[si+5]);
      OUT('}');
      si += 5;
      continue;
    }

    if (next == 'u') {
      si++;
      OUT('u');
      continue;
    }

    if (next == 'x' && si + 3 < src_len &&
        ant_ascii_is_xdigit(src[si+2]) && ant_ascii_is_xdigit(src[si+3])) {
      OUT('\\'); OUT('x'); OUT(src[si+2]); OUT(src[si+3]);
      si += 3;
      continue;
    }

    if (next == 'x') {
      si++;
      OUT('x');
      continue;
    }

    if (next == '0' && (si + 2 >= src_len || src[si+2] < '0' || src[si+2] > '9')) {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('}');
      si++;
      continue;
    }

    if (next >= '0' && next <= '7') {
      unsigned int octal = next - '0';
      size_t advance = 1;
      if (si + 2 < src_len && src[si+2] >= '0' && src[si+2] <= '7') {
        octal = octal * 8 + (src[si+2] - '0');
        advance = 2;
        if (si + 3 < src_len && src[si+3] >= '0' && src[si+3] <= '7' && octal * 8 + (src[si+3] - '0') <= 255) {
          octal = octal * 8 + (src[si+3] - '0');
          advance = 3;
        }
      }
      
      if (advance > 1 || next == '0') {
        char hex[8];
        int hlen = snprintf(hex, sizeof(hex), "\\x{%02x}", octal);
        for (int k = 0; k < hlen; k++) OUT(hex[k]);
        si += advance;
        continue;
      }
    }

    if (next == 'c' && si + 2 < src_len &&
        ((src[si+2] >= 'A' && src[si+2] <= 'Z') || (src[si+2] >= 'a' && src[si+2] <= 'z'))) {
      OUT('\\'); OUT('c'); OUT(src[si+2]);
      si += 2;
      continue;
    }

    if (next == 'c') {
      OUT('\\'); OUT('\\'); OUT('c');
      si++;
      continue;
    }

    if ((next == 'p' || next == 'P') && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}') brace_end++;
      if (brace_end < src_len && src[brace_end] == '}') {
        const char *prop = &src[brace_start];
        size_t prop_len = brace_end - brace_start;
        static const struct { const char *name; const char *code; } gc_map[] = {
          {"Letter","L"},{"Cased_Letter","LC"},{"Uppercase_Letter","Lu"},
          {"Lowercase_Letter","Ll"},{"Titlecase_Letter","Lt"},
          {"Modifier_Letter","Lm"},{"Other_Letter","Lo"},
          {"Mark","M"},{"Nonspacing_Mark","Mn"},{"Spacing_Mark","Mc"},
          {"Enclosing_Mark","Me"},
          {"Number","N"},{"Decimal_Number","Nd"},{"Letter_Number","Nl"},
          {"Other_Number","No"},
          {"Punctuation","P"},{"Connector_Punctuation","Pc"},
          {"Dash_Punctuation","Pd"},{"Open_Punctuation","Ps"},
          {"Close_Punctuation","Pe"},{"Initial_Punctuation","Pi"},
          {"Final_Punctuation","Pf"},{"Other_Punctuation","Po"},
          {"Symbol","S"},{"Math_Symbol","Sm"},{"Currency_Symbol","Sc"},
          {"Modifier_Symbol","Sk"},{"Other_Symbol","So"},
          {"Separator","Z"},{"Space_Separator","Zs"},
          {"Line_Separator","Zl"},{"Paragraph_Separator","Zp"},
          {"Other","C"},{"Control","Cc"},{"Format","Cf"},
          {"Surrogate","Cs"},{"Private_Use","Co"},{"Unassigned","Cn"},
        };
        static const struct { const char *script; const char *range; } u17_scripts[] = {
          {"Sidetic",       "\\x{10940}-\\x{1095F}"},
          {"Garay",         "\\x{10D40}-\\x{10D8F}"},
          {"Gurung_Khema",  "\\x{16100}-\\x{1613F}"},
          {"Kirat_Rai",     "\\x{16D40}-\\x{16D7F}"},
          {"Ol_Onal",       "\\x{1E5D0}-\\x{1E5FF}"},
          {"Sunuwar",       "\\x{11BC0}-\\x{11BFF}"},
          {"Tulu_Tigalari", "\\x{11380}-\\x{113FF}"},
        };
        bool has_eq = (memchr(prop, '=', prop_len) != NULL);
        bool has_colon = (memchr(prop, ':', prop_len) != NULL);
        if (!has_eq && !has_colon && next == 'p' && charclass_depth == 0) {
          static const struct { const char *name; const char *exp; } sprops[] = {
            {"Emoji_Keycap_Sequence",
             "(?:\\x{23}\\x{fe0f}\\x{20e3}|\\x{2a}\\x{fe0f}\\x{20e3}|[\\x{30}-\\x{39}]\\x{fe0f}\\x{20e3})"},
            {"RGI_Emoji",
             "(?:"
             "[\\x{1f1e6}-\\x{1f1ff}]{2}"
             "|(?:(?:\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}|(?!\\p{Emoji_Modifier})\\p{Emoji_Presentation}|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f})\\x{200d})+"
             "(?:\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}|(?!\\p{Emoji_Modifier})\\p{Emoji_Presentation}|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f})"
             "|\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}"
             "|[#*0-9]\\x{fe0f}\\x{20e3}"
             "|\\x{1f3f4}\\x{e0067}\\x{e0062}(?:\\x{e0065}\\x{e006e}\\x{e0067}|\\x{e0073}\\x{e0063}\\x{e0074}|\\x{e0077}\\x{e006c}\\x{e0073})\\x{e007f}"
             "|\\p{Emoji_Presentation}"
             "|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f}"
             ")"},
          };
          for (size_t m = 0; m < sizeof(sprops)/sizeof(sprops[0]); m++) {
            if (strlen(sprops[m].name) == prop_len && memcmp(sprops[m].name, prop, prop_len) == 0) {
              for (const char *r = sprops[m].exp; *r; r++) OUT(*r);
              si = brace_end;
              goto next_char;
            }
          }
        }
        if (has_eq || has_colon) {
          char sep = has_eq ? '=' : ':';
          const char *val = memchr(prop, sep, prop_len);
          if (val) {
            val++;
            size_t val_len = prop_len - (size_t)(val - prop);
            for (size_t m = 0; m < sizeof(u17_scripts)/sizeof(u17_scripts[0]); m++) {
              if (strlen(u17_scripts[m].script) == val_len &&
                  memcmp(u17_scripts[m].script, val, val_len) == 0) {
                const char *r = u17_scripts[m].range;
                OUT('[');
                if (next == 'P') OUT('^');
                for (; *r; r++) OUT(*r);
                OUT(']');
                si = brace_end;
                goto next_char;
              }
            }
          }
        }
        if (!has_eq && !has_colon) {
          static const struct { const char *name; const char *range; } rangeprops[] = {
            {"ASCII", "\\x{0}-\\x{7f}"},
            {"Any",   "\\x{0}-\\x{10ffff}"},
          };
          for (size_t m = 0; m < sizeof(rangeprops)/sizeof(rangeprops[0]); m++) {
            if (strlen(rangeprops[m].name) == prop_len && memcmp(rangeprops[m].name, prop, prop_len) == 0) {
              if (charclass_depth > 0) {
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
              } else {
                OUT('['); if (next == 'P') OUT('^');
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
                OUT(']');
              }
              si = brace_end;
              goto next_char;
            }
          }
        }
        const char *replacement = NULL;
        if (!has_eq && !has_colon) {
          for (size_t m = 0; m < sizeof(gc_map)/sizeof(gc_map[0]); m++) {
            if (strlen(gc_map[m].name) == prop_len &&
                memcmp(gc_map[m].name, prop, prop_len) == 0) {
              replacement = gc_map[m].code;
              break;
            }
          }
        }
        static const struct { const char *prop; const char *extra; } u17_props[] = {
          {"Emoji", "\\x{1FACD}-\\x{1FACE}\\x{1FAE9}\\x{1FAF9}"},
        };
        const char *extra_range = NULL;
        if (!has_eq && !has_colon && !replacement) {
          for (size_t m = 0; m < sizeof(u17_props)/sizeof(u17_props[0]); m++) {
            if (strlen(u17_props[m].prop) == prop_len &&
                memcmp(u17_props[m].prop, prop, prop_len) == 0) {
              extra_range = u17_props[m].extra;
              break;
            }
          }
        }
        if (extra_range && charclass_depth == 0) {
          const char *pfx = (next == 'p') ? "(?:\\p{" : "(?:\\P{";
          for (const char *r = pfx; *r; r++) OUT(*r);
          for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          OUT('}'); OUT('|'); OUT('[');
          if (next == 'P') OUT('^');
          for (const char *r = extra_range; *r; r++) OUT(*r);
          OUT(']'); OUT(')');
        } else {
          OUT('\\'); OUT(next); OUT('{');
          if (replacement) {
            for (const char *r = replacement; *r; r++) OUT(*r);
          } else {
            for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          }
          OUT('}');
        }
        si = brace_end;
        continue;
      }
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    if (next == 's' || next == 'S') {
      if (charclass_depth > 0) OUTS(next == 's' ? regexp_space_ranges : regexp_non_space_ranges);
      else {
        OUT('['); if (next == 'S') OUT('^');
        OUTS(regexp_space_ranges);
        OUT(']');
      }
      si++;
      continue;
    }

    if (unicode_ignore_case && (next == 'w' || next == 'W')) {
      if (charclass_depth > 0) OUTS(next == 'w' ? regexp_folded_word_ranges : regexp_folded_non_word_ranges);
      else {
        OUT('['); if (next == 'W') OUT('^');
        OUTS(regexp_folded_word_ranges);
        OUT(']');
      }
      si++;
      continue;
    }

    if (unicode_ignore_case && charclass_depth == 0 && (next == 'b' || next == 'B')) {
      OUTS(next == 'b' ? regexp_folded_boundary : regexp_folded_non_boundary);
      si++;
      continue;
    }

    if (next == 'k' && charclass_depth == 0 && si + 2 < src_len && src[si + 2] == '<') {
      size_t name_end = regexp_group_name_end(src, src_len, si + 3);
      if (!names_ready) { regexp_name_spans_init(&names, src, src_len, v_flag); names_ready = true; }
      int ordinal = name_end < src_len ? regexp_name_spans_ordinal(src, &names, si + 3, name_end) : -1;
      if (ordinal >= 0) {
        if (legacy_ignore_case) {
          OUTS("(?C\"");
          for (size_t k = si + 3; k < name_end; ) {
            char utf8[4];
            int width = utf8_encode(regexp_name_next_cp(src, name_end, &k), utf8);
            for (int w = 0; w < width; w++) OUT(utf8[w]);
          }
          OUTS("\")");
        }
        char name[16];
        size_t name_len = regexp_name_ordinal_digits(name, (uint32_t)ordinal);
        OUTS("\\k<_");
        for (size_t k = 0; k < name_len; k++) OUT(name[k]);
        OUT('>');
        si = name_end;
        continue;
      }
    }

    if (legacy_ignore_case && charclass_depth == 0 && next >= '1' && next <= '9') {
      unsigned group = 0;
      for (size_t k = si + 1; k < src_len && src[k] >= '0' && src[k] <= '9' && group <= 255; k++)
        group = group * 10 + (unsigned)(src[k] - '0');
      if (group <= 255) {
        char callout[12];
        int callout_len = snprintf(callout, sizeof(callout), "(?C%u)", group);
        for (int k = 0; k < callout_len; k++) OUT(callout[k]);
      }
    }

    if (is_pcre2_passthrough_escape(next) || (v_flag && charclass_depth > 0 && next > 0x20 && next < 0x7f && !ant_ascii_is_alnum(next))) {
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    si++;
    OUT(next);
    next_char:;
  }

#undef OUTS
#undef OUT
  if (names_ready) regexp_name_spans_free(&names);
  builder->length = output_length;
}

static size_t regexp_translate(
  const char *src, size_t src_len,
  char *dst, size_t dst_size,
  uint8_t flags, bool plain_line_anchors, bool *line_anchors
) {
  regexp_pattern_builder_t builder = {
    .data = dst,
    .capacity = dst_size ? dst_size - 1 : 0,
    .plain_line_anchors = plain_line_anchors,
  };
  js_to_pcre2_pattern_append(src, src_len, &builder, (flags & REGEXP_FLAG_UNICODE_SET) != 0, flags);
  if (dst_size) {
    size_t terminator = builder.length < dst_size
      ? builder.length
      : dst_size - 1;
    dst[terminator] = '\0';
  }
  if (line_anchors) *line_anchors = builder.line_anchors;
  return builder.length;
}

size_t js_to_pcre2_pattern(
  const char *src,
  size_t src_len,
  char *dst,
  size_t dst_size,
  uint8_t flags
) {
  return regexp_translate(src, src_len, dst, dst_size, flags, false, NULL);
}

js_cstr_t regexp_translate_cstr(
  const char *src, size_t src_len,
  char *stack_buf, size_t stack_size,
  uint8_t flags, bool plain_line_anchors, bool *line_anchors
) {
  js_cstr_t result = {0};
  size_t required = regexp_translate(
    src, src_len, stack_buf, stack_size, flags, plain_line_anchors, line_anchors
  );
  if (required < stack_size) {
    result.ptr = stack_buf;
    result.len = required;
    return result;
  }
  if (required == SIZE_MAX) return result;

  char *allocated = malloc(required + 1);
  if (!allocated) return result;
  size_t translated = regexp_translate(
    src, src_len, allocated, required + 1, flags, plain_line_anchors, NULL
  );
  if (translated != required) {
    free(allocated);
    return result;
  }

  result.ptr = allocated;
  result.len = translated;
  result.needs_free = true;
  return result;
}

js_cstr_t js_to_pcre2_pattern_cstr(
  const char *src, size_t src_len, char *stack_buf, size_t stack_size, uint8_t flags
) {
  return regexp_translate_cstr(src, src_len, stack_buf, stack_size, flags, false, NULL);
}
