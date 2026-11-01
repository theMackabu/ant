#include "regex_internal.h"
#include <limits.h>

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

static size_t v_close_bracket(const char *src, size_t src_len, size_t open) {
  int depth = 0;
  for (size_t i = open; i < src_len; i++) {
    if (src[i] == '\\' && i + 1 < src_len) { i++; continue; }
    if (src[i] == '[') depth++;
    else if (src[i] == ']') { if (--depth == 0) return i; }
  }
  return src_len;
}

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool plain_line_anchors;
  bool line_anchors;
} regexp_pattern_builder_t;

static inline void regexp_pattern_builder_puts(
  regexp_pattern_builder_t *builder,
  const char *value,
  size_t length
) {
  size_t offset = builder->length;
  size_t available = offset < builder->capacity
    ? builder->capacity - offset
    : 0;
  size_t copy_length = length < available ? length : available;
  if (copy_length) memcpy(builder->data + offset, value, copy_length);
  builder->length += length;
}

static inline void regexp_pattern_builder_put_any_char(
  regexp_pattern_builder_t *builder
) {
  static const char any_char[] = "[\\s\\S]";
  regexp_pattern_builder_puts(builder, any_char, sizeof(any_char) - 1);
}

static void js_to_pcre2_pattern_append(
  const char *src,
  size_t src_len,
  regexp_pattern_builder_t *builder,
  bool v_flag,
  uint8_t flags,
  int initial_charclass_depth
);

static void v_translate_part_append(
  const char *part,
  size_t length,
  regexp_pattern_builder_t *builder,
  uint8_t flags
) {
  if (length && part[0] == '[') {
    js_to_pcre2_pattern_append(part, length, builder, false, flags, 0);
    return;
  }

  if (length == 1 && part[0] == '^') {
    regexp_pattern_builder_put_any_char(builder);
    return;
  }

  if (length >= 2 && part[0] == '^' && part[1] == ']') {
    regexp_pattern_builder_put_any_char(builder);
    js_to_pcre2_pattern_append(part + 2, length - 2, builder, false, flags, 0);
    regexp_pattern_builder_puts(builder, "]", 1);
    return;
  }

  regexp_pattern_builder_puts(builder, "[", 1);
  js_to_pcre2_pattern_append(part, length, builder, false, flags, 1);
  regexp_pattern_builder_puts(builder, "]", 1);
}

static int v_set_op(const char *src, size_t start, size_t end, size_t *op_pos) {
  int depth = 0;
  for (size_t i = start; i < end; ) {
    if (src[i] == '\\' && i + 1 < end) {
      char n = src[i + 1];
      if ((n == 'p' || n == 'P') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      if ((n == 'u' || n == 'x') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      i += 2; continue;
    }
    if (src[i] == '[') { depth++; i++; continue; }
    if (src[i] == ']') { if (depth > 0) { depth--; i++; continue; } break; }
    if (!depth && i + 1 < end) {
      if (src[i] == '&' && src[i+1] == '&') { *op_pos = i; return 1; }
      if (src[i] == '-' && src[i+1] == '-') { *op_pos = i; return 2; }
    }
    i++;
  }
  return 0;
}

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

static bool regexp_source_has_group_names(const char *src, size_t src_len) {
  for (size_t i = 0; i + 3 < src_len; i++) {
    if (src[i] == '\\') { i++; continue; }
    if (src[i] == '(' && src[i + 1] == '?' && src[i + 2] == '<' && src[i + 3] != '=' && src[i + 3] != '!') return true;
  }
  return false;
}

static bool regexp_is_brace_quantifier(const char *src, size_t src_len, size_t at) {
  size_t i = at + 1;
  size_t digits = 0;
  while (i < src_len && src[i] >= '0' && src[i] <= '9') { i++; digits++; }
  if (digits == 0 || i >= src_len) return false;
  if (src[i] == '}') return true;
  if (src[i++] != ',') return false;
  while (i < src_len && src[i] >= '0' && src[i] <= '9') i++;
  return i < src_len && src[i] == '}';
}

// Capturing groups in a non-v pattern, for telling backreferences from
// Annex B legacy octal escapes.
static unsigned regexp_count_capture_groups(const char *src, size_t src_len) {
  unsigned count = 0;
  bool in_class = false;
  for (size_t i = 0; i < src_len; i++) {
    char c = src[i];
    if (c == '\\') { i++; continue; }
    if (in_class) { if (c == ']') in_class = false; continue; }
    if (c == '[') { in_class = true; continue; }
    if (c != '(') continue;
    if (i + 1 >= src_len || src[i + 1] != '?') count++;
    else if (i + 3 < src_len && src[i + 2] == '<' && src[i + 3] != '=' && src[i + 3] != '!') count++;
  }
  return count;
}

static void js_to_pcre2_pattern_append(
  const char *src,
  size_t src_len,
  regexp_pattern_builder_t *builder,
  bool v_flag,
  uint8_t flags,
  int initial_charclass_depth
) {
  bool unicode_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && (flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  bool legacy_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && !(flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  bool unicode = (flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) != 0;
  unsigned capture_groups = UINT_MAX;
  size_t legacy_class_close = 0;
  int charclass_depth = initial_charclass_depth;
  char *output = builder->data;
  size_t output_capacity = builder->capacity;
  size_t output_length = builder->length;

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
      for (; si < name_end; si++) OUT(src[si]);
      if (si < src_len) OUT(src[si]);
      continue;
    }

    // classes only nest with v; elsewhere [ is literal, and escaped so PCRE2
    // does not read [:name:] as a POSIX class
    if (src[si] == '[' && charclass_depth > 0 && !(flags & REGEXP_FLAG_UNICODE_SET)) {
      OUT('\\'); OUT('[');
      continue;
    }

    if (src[si] == '[') {
      if (si + 2 < src_len && src[si + 1] == '^' && src[si + 2] == ']') {
        OUT('['); OUT('\\'); OUT('s'); OUT('\\'); OUT('S'); OUT(']');
        si += 2;
        continue;
      }

      if (charclass_depth == 0 && si + 1 < src_len && src[si + 1] == ']') {
        OUTS("[^\\s\\S]");
        si++;
        continue;
      }

      if (v_flag && charclass_depth == 0) {
        size_t close = v_close_bracket(src, src_len, si);
        size_t op_pos;
        int op_type = v_set_op(src, si + 1, close, &op_pos);
        if (op_type && close < src_len) {
          const char *left = &src[si + 1];
          size_t left_len = op_pos - si - 1;
          const char *right = &src[op_pos + 2];
          size_t right_len = close - op_pos - 2;
          const char *lookahead = op_type == 1 ? left : right;
          size_t lookahead_len = op_type == 1 ? left_len : right_len;
          const char *match = op_type == 1 ? right : left;
          size_t match_len = op_type == 1 ? right_len : left_len;
          OUT('('); OUT('?'); OUT(op_type == 1 ? '=' : '!');
          builder->length = output_length;
          v_translate_part_append(lookahead, lookahead_len, builder, flags);
          output_length = builder->length;
          OUT(')');
          builder->length = output_length;
          v_translate_part_append(match, match_len, builder, flags);
          output_length = builder->length;
          si = close;
          continue;
        }
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
      // PCRE2 reads [: [. [= as POSIX class syntax, JS as literal characters
      if (si + 1 < src_len && (src[si + 1] == ':' || src[si + 1] == '.' || src[si + 1] == '=')) {
        OUT('\\'); OUT(src[si + 1]);
        si++;
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

    if (charclass_depth > 0 && src[si] == '-' && si > 0 && src[si - 1] != '[' &&
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

    // a { that does not open {n}, {n,} or {n,m} is literal in JS, while
    // PCRE2 also reads {,m} as a quantifier
    if (charclass_depth == 0 && src[si] == '{' && !regexp_is_brace_quantifier(src, src_len, si)) {
      OUT('\\'); OUT('{');
      continue;
    }

    if (src[si] != '\\' || si + 1 >= src_len) {
      OUT(src[si]);
      continue;
    }

    char next = src[si + 1];

    // \B is an identity escape in a class; PCRE2 rejects it there
    if (next == 'B' && charclass_depth > 0) {
      OUT('B');
      si++;
      continue;
    }

    if (next == 'v') {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('b'); OUT('}');
      si++;
      continue;
    }

    // \u{...} is a code point only with u or v; otherwise u{n} quantifies u
    if (unicode && next == 'u' && si + 2 < src_len && src[si + 2] == '{') {
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
      // PCRE2's UTF mode has no surrogates, so a \uHHHH\uHHHH pair becomes
      // its code point; in a non-unicode class each unit stays separate
      uint32_t hi = 0, lo = 0;
      size_t after = 0;
      if (
        (unicode || charclass_depth == 0) &&
        regexp_hex_value(src, src_len, si + 2, 4, &hi) && hi >= 0xD800 && hi <= 0xDBFF &&
        si + 11 < src_len && src[si + 6] == '\\' && src[si + 7] == 'u' &&
        regexp_hex_value(src, src_len, si + 8, 4, &lo) && lo >= 0xDC00 && lo <= 0xDFFF
      ) after = si + 11;
      if (after) {
        char escaped[12];
        int escaped_len = snprintf(escaped, sizeof(escaped), "\\x{%x}", 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
        for (int k = 0; k < escaped_len; k++) OUT(escaped[k]);
        si = after;
        continue;
      }
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

    // \0 starts an octal escape anywhere, and \1-\7 do inside a class;
    // outside one, the capture-group count below tells a backreference
    // (\10 with ten groups) from a legacy octal escape
    if (next >= '0' && next <= '7' && (next == '0' || charclass_depth > 0)) {
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

    // without u or v, \p is an identity escape and {...} is literal
    if (unicode && (next == 'p' || next == 'P') && si + 2 < src_len && src[si + 2] == '{') {
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

    if (
      next == 'k' && charclass_depth == 0 && si + 2 < src_len && src[si + 2] == '<' &&
      regexp_source_has_group_names(src, src_len)
    ) {
      size_t name_end = regexp_group_name_end(src, src_len, si + 3);
      if (name_end < src_len) {
        if (legacy_ignore_case) {
          OUTS("(?C\"");
          for (size_t k = si + 3; k < name_end; k++) OUT(src[k]);
          OUTS("\")");
        }
        for (; si <= name_end; si++) OUT(src[si]);
        si = name_end;
        continue;
      }
    }

    // Annex B: \N past the last group is a legacy octal escape, and \8 or
    // \9 the digit itself; PCRE2 reads \1-\9 as backreferences regardless
    if (!unicode && charclass_depth == 0 && next >= '1' && next <= '9') {
      if (capture_groups == UINT_MAX) capture_groups = regexp_count_capture_groups(src, src_len);
      unsigned value = 0;
      for (size_t k = si + 1; k < src_len && src[k] >= '0' && src[k] <= '9' && value <= capture_groups; k++)
        value = value * 10 + (unsigned)(src[k] - '0');
      if (value > capture_groups) {
        if (next >= '8') {
          si++;
          OUT(next);
          continue;
        }
        unsigned octal = (unsigned)(next - '0');
        size_t used = 1;
        while (
          used < 3 && si + 1 + used < src_len &&
          src[si + 1 + used] >= '0' && src[si + 1 + used] <= '7' &&
          octal * 8 + (unsigned)(src[si + 1 + used] - '0') <= 0377
        ) octal = octal * 8 + (unsigned)(src[si + 1 + used++] - '0');
        char escaped[10];
        int escaped_len = snprintf(escaped, sizeof(escaped), "\\x{%x}", octal);
        for (int k = 0; k < escaped_len; k++) OUT(escaped[k]);
        si += used;
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

    // a backreference by now; PCRE2 reads \10 and up as octal unless that
    // many groups come before it, so multi-digit ones (forward references
    // included) are spelled \g{n}
    if (charclass_depth == 0 && next >= '1' && next <= '9' && si + 2 < src_len && src[si + 2] >= '0' && src[si + 2] <= '9') {
      unsigned group = 0;
      size_t used = 0;
      while (si + 1 + used < src_len && src[si + 1 + used] >= '0' && src[si + 1 + used] <= '9' && group <= 65535)
        group = group * 10 + (unsigned)(src[si + 1 + used++] - '0');
      char ref[16];
      int ref_len = snprintf(ref, sizeof(ref), "\\g{%u}", group);
      for (int k = 0; k < ref_len; k++) OUT(ref[k]);
      si += used;
      continue;
    }

    if (is_pcre2_passthrough_escape(next)) {
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    si++;
    // an identity escape of punctuation keeps its backslash, which PCRE2
    // also reads as literal (and so never sees [: as POSIX syntax)
    if ((unsigned char)next < 0x80 && !ant_ascii_is_alnum(next)) OUT('\\');
    OUT(next);
    next_char:;
  }

#undef OUTS
#undef OUT
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
  js_to_pcre2_pattern_append(src, src_len, &builder, (flags & REGEXP_FLAG_UNICODE_SET) != 0, flags, 0);
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
  const char *src,
  size_t src_len,
  char *stack_buf,
  size_t stack_size,
  uint8_t flags
) {
  return regexp_translate_cstr(src, src_len, stack_buf, stack_size, flags, false, NULL);
}
