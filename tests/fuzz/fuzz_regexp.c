// RegExp syntax check (src/modules/regex/syntax*.c) and JS -> PCRE2 pattern
// translation (js_to_pcre2_pattern in src/modules/regex/translate.c).
//
// Input: a flags byte, then the pattern source.
// Properties:
//   - translation behaves like snprintf: the same length for any buffer
//     size, and a short buffer holds a NUL-terminated prefix;
//   - a pattern the JS syntax check accepts compiles in PCRE2, unless PCRE2
//     hits one of its own limits (see engine_limit), since `new RegExp`
//     would otherwise fail with no SyntaxError to explain it.
#include "fuzz.h"
#include "modules/regex.h"
#include "utf8.h"

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>

enum {
#define REGEXP_FLAG(id, name, ch) FLAG_INDEX_##id,
#include "modules/regex_flags.h"
};

enum {
#define REGEXP_FLAG(id, name, ch) FLAG_##id = 1 << FLAG_INDEX_##id,
#include "modules/regex_flags.h"
};

// Limits of PCRE2 itself rather than mistakes in the translation.
static bool engine_limit(int errcode) {
  switch (errcode) {
    case PCRE2_ERROR_QUANTIFIER_TOO_BIG:
    case PCRE2_ERROR_PATTERN_TOO_LARGE:
    case PCRE2_ERROR_PATTERN_TOO_COMPLICATED:
    case PCRE2_ERROR_PARENTHESES_NEST_TOO_DEEP:
    case PCRE2_ERROR_LOOKBEHIND_NOT_FIXED_LENGTH:
    case PCRE2_ERROR_LOOKBEHIND_TOO_LONG:
    case PCRE2_ERROR_TOO_MANY_CAPTURES:
    case PCRE2_ERROR_TOO_MANY_NAMED_SUBPATTERNS:
    case PCRE2_ERROR_SUBPATTERN_NAME_TOO_LONG:
    case PCRE2_ERROR_HEAP_FAILED:
    // UTF mode has no lone surrogates for \uD800-\uDFFF to match
    case PCRE2_ERROR_UNICODE_DISALLOWED_CODE_POINT:
      return true;
    default:
      return false;
  }
}

// Group names with $ or escapes are valid in JS but not in PCRE2 (known gap).
static bool has_pcre2_unsafe_group_name(const char *s, size_t len) {
  for (size_t i = 0; i + 3 < len; i++) {
    if (s[i] == '\\') { i++; continue; }
    if (s[i] != '(' || s[i + 1] != '?' || s[i + 2] != '<' || s[i + 3] == '=' || s[i + 3] == '!') continue;
    for (size_t k = i + 3; k < len && s[k] != '>'; k++)
      if (s[k] == '$' || s[k] == '\\') return true;
  }
  return false;
}

static bool has_pair(const char *s, size_t len, char c) {
  for (size_t i = 0; i + 1 < len; i++) if (s[i] == c && s[i + 1] == c) return true;
  return false;
}

// Mirrors regexp_compile_code in src/modules/regex/compile.c.
static pcre2_code *compile(const char *pattern, size_t len, uint8_t flags, int *errcode, PCRE2_SIZE *erroffset) {
  uint32_t options = PCRE2_UTF | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES | PCRE2_DOLLAR_ENDONLY;
  if (flags & FLAG_IGNORE_CASE) options |= PCRE2_CASELESS;
  if (flags & FLAG_MULTILINE) options |= PCRE2_MULTILINE | PCRE2_ALT_CIRCUMFLEX;
  if (flags & FLAG_DOTALL) options |= PCRE2_DOTALL;

  pcre2_compile_context *ctx = pcre2_compile_context_create(NULL);
  if (!ctx) abort();
  pcre2_set_newline(ctx, PCRE2_NEWLINE_LF);
  if ((flags & FLAG_IGNORE_CASE) && !(flags & (FLAG_UNICODE | FLAG_UNICODE_SET)))
    pcre2_set_compile_extra_options(ctx, PCRE2_EXTRA_CASELESS_RESTRICT);
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, len, options, errcode, erroffset, ctx);
  pcre2_compile_context_free(ctx);
  return re;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint8_t control = fuzz_u8(&in);
  uint8_t flags = 0;
  if (control & 1) flags |= FLAG_IGNORE_CASE;
  if (control & 2) flags |= FLAG_MULTILINE;
  if (control & 4) flags |= FLAG_DOTALL;
  if (control & 8) flags |= FLAG_GLOBAL;
  // 'u' and 'v' together are a SyntaxError before the pattern is looked at
  if ((control & 0x30) == 0x10) flags |= FLAG_UNICODE;
  if ((control & 0x30) == 0x20) flags |= FLAG_UNICODE_SET;

  char *src = fuzz_dup(in.data, in.size);
  size_t len = in.size;

  size_t required = js_to_pcre2_pattern(src, len, NULL, 0, flags);
  char *full = malloc(required + 1);
  if (!full) abort();
  FUZZ_CHECK(js_to_pcre2_pattern(src, len, full, required + 1, flags) == required);
  FUZZ_CHECK(full[required] == '\0');

  size_t step = required > 64 ? required / 16 : 1;
  for (size_t cap = 1; cap <= required; cap += step) {
    char *buf = malloc(cap);
    if (!buf) abort();
    FUZZ_CHECK(js_to_pcre2_pattern(src, len, buf, cap, flags) == required);
    FUZZ_CHECK(buf[cap - 1] == '\0' && memcmp(buf, full, cap - 1) == 0);
    free(buf);
  }

  bool unicode_sets = flags & FLAG_UNICODE_SET;
  bool unicode = unicode_sets || (flags & FLAG_UNICODE);
  const char *syntax = regexp_syntax_error(src, len, unicode, unicode_sets);

  // JS strings are WTF-8, and PCRE2 has no lone surrogates (engine_limit).
  // Known gaps, see docs/exec-plans/active/fuzz-and-property-testing.md:
  // v-flag set operations whose operand is a class, and group names PCRE2
  // cannot spell.
  bool set_ops = unicode_sets && (has_pair(src, len, '-') || has_pair(src, len, '&'));
  bool known_gap = set_ops || has_pcre2_unsafe_group_name(src, len);
  if (!syntax && !known_gap && utf8_validate_bytes(src, len)) {
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code *re = compile(full, required, flags, &errcode, &erroffset);
    if (!re && !engine_limit(errcode)) {
      PCRE2_UCHAR message[256];
      pcre2_get_error_message(errcode, message, sizeof(message));
      fprintf(stderr, "accepted /%.*s/ (flags 0x%02x) -> %s\nPCRE2 error %d at %zu: %s\n",
        (int)len, src, flags, full, errcode, (size_t)erroffset, (const char *)message);
      FUZZ_FAIL("syntax check accepted a pattern PCRE2 rejects");
    }
    pcre2_code_free(re);
  }

  free(full);
  free(src);
  return 0;
}
