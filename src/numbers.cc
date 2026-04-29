#include "numbers.h"

#include <cstring>
#include <limits>

#include "double-conversion/utils.h"
#include "double-conversion/double-to-string.h"
#include "double-conversion/string-to-double.h"

using double_conversion::StringBuilder;
using double_conversion::DoubleToStringConverter;
using double_conversion::StringToDoubleConverter;

struct JsTrimToken {
  const char *bytes;
  size_t len;
};

static constexpr size_t kShortestBufferSize =
  DoubleToStringConverter::kMaxCharsEcmaScriptShortest + 1;
  
static constexpr size_t kFixedBufferSize =
  1 + DoubleToStringConverter::kMaxFixedDigitsBeforePoint + 1 +
  DoubleToStringConverter::kMaxFixedDigitsAfterPoint + 1;
  
static constexpr size_t kPrecisionBufferSize =
  DoubleToStringConverter::kMaxPrecisionDigits + 7 + 1;
  
static constexpr size_t kExponentialBufferSize =
  DoubleToStringConverter::kMaxExponentialDigits + 8 + 1;

#define JS_TRIM_TOKEN(bytes) { bytes, sizeof(bytes) - 1 }
static constexpr JsTrimToken kJsStringTrimTokens[] = {
  JS_TRIM_TOKEN("\t"),
  JS_TRIM_TOKEN("\n"),
  JS_TRIM_TOKEN("\v"),
  JS_TRIM_TOKEN("\f"),
  JS_TRIM_TOKEN("\r"),
  JS_TRIM_TOKEN(" "),
  JS_TRIM_TOKEN("\xc2""\xa0"),
  JS_TRIM_TOKEN("\xe1""\x9a""\x80"),
  JS_TRIM_TOKEN("\xe2""\x80""\x80"),
  JS_TRIM_TOKEN("\xe2""\x80""\x81"),
  JS_TRIM_TOKEN("\xe2""\x80""\x82"),
  JS_TRIM_TOKEN("\xe2""\x80""\x83"),
  JS_TRIM_TOKEN("\xe2""\x80""\x84"),
  JS_TRIM_TOKEN("\xe2""\x80""\x85"),
  JS_TRIM_TOKEN("\xe2""\x80""\x86"),
  JS_TRIM_TOKEN("\xe2""\x80""\x87"),
  JS_TRIM_TOKEN("\xe2""\x80""\x88"),
  JS_TRIM_TOKEN("\xe2""\x80""\x89"),
  JS_TRIM_TOKEN("\xe2""\x80""\x8a"),
  JS_TRIM_TOKEN("\xe2""\x80""\xa8"),
  JS_TRIM_TOKEN("\xe2""\x80""\xa9"),
  JS_TRIM_TOKEN("\xe2""\x80""\xaf"),
  JS_TRIM_TOKEN("\xe2""\x81""\x9f"),
  JS_TRIM_TOKEN("\xe3""\x80""\x80"),
  JS_TRIM_TOKEN("\xef""\xbb""\xbf"),
};
#undef JS_TRIM_TOKEN

static size_t js_string_trim_prefix_len(const char *str, size_t len) {
  for (const JsTrimToken &token : kJsStringTrimTokens) 
    if (len >= token.len && std::memcmp(str, token.bytes, token.len) == 0) return token.len;
  return 0;
}

static size_t js_string_trim_suffix_len(const char *str, size_t len) {
  for (const JsTrimToken &token : kJsStringTrimTokens) 
    if (len >= token.len && std::memcmp(str + len - token.len, token.bytes, token.len) == 0) return token.len;
  return 0;
}

static void trim_js_string_whitespace(const char **str, size_t *len, bool trim_trailing, size_t *leading) {
  size_t lead = 0;
  
  while (*len > 0) {
    size_t n = js_string_trim_prefix_len(*str, *len);
    if (n == 0) break;
    *str += n; *len -= n; lead += n;
  }

  while (trim_trailing && *len > 0) {
    size_t n = js_string_trim_suffix_len(*str, *len);
    if (n == 0) break;
    *len -= n;
  }

  if (leading) *leading = lead;
}

static bool ant_starts_with_nondecimal_prefix(const char *str, size_t len) {
  return
    len >= 2 && str[0] == '0' && 
    ((str[1] | 0x20) == 'x' || 
    (str[1] | 0x20)  == 'b' || (str[1] | 0x20) == 'o');
}

static bool ant_parse_radix_integer(const char *str, size_t len, int radix, double *out) {
  if (!str || len == 0 || !out) return false;
  double value = 0.0;
  
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)str[i];
    int digit = -1;
    if (ch >= '0' && ch <= '9') digit = ch - '0';
    else if (ch >= 'a' && ch <= 'z') digit = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'Z') digit = ch - 'A' + 10;
    if (digit < 0 || digit >= radix) return false;
    value = value * (double)radix + (double)digit;
  }

  *out = value;
  return true;
}

static bool ant_parse_js_number_prefix(const char *str, size_t len, double *out) {
  if (len < 3 || str[0] != '0') return false;

  char kind = (char)(str[1] | 0x20);
  int radix = kind == 'b' ? 2 : (kind == 'o' ? 8 : 0);
  if (radix == 0) return false;

  double value = 0.0;
  if (!ant_parse_radix_integer(str + 2, len - 2, radix, &value))
    return false;

  *out = value;
  return true;
}

extern "C" bool ant_number_parse(
  const char *str, size_t len,
  ant_number_parse_mode_t mode,
  double *out, size_t *processed
) {
  if (processed) *processed = 0;
  if (!str || !out) return false;

  size_t leading = 0;
  if (mode == ANT_NUMBER_PARSE_JS_NUMBER || mode == ANT_NUMBER_PARSE_FLOAT_PREFIX) {
  trim_js_string_whitespace(&str, &len, mode == ANT_NUMBER_PARSE_JS_NUMBER, &leading);
  
  if (mode == ANT_NUMBER_PARSE_JS_NUMBER && len == 0) {
    *out = 0.0;
    if (processed) *processed = leading;
    return true;
  }}

  if (
    mode == ANT_NUMBER_PARSE_JS_NUMBER && len >= 3 &&
    (str[0] == '+' || str[0] == '-') &&
    ant_starts_with_nondecimal_prefix(str + 1, len - 1)
  ) return false;

  if (mode == ANT_NUMBER_PARSE_JS_NUMBER && ant_parse_js_number_prefix(str, len, out)) {
    if (processed) *processed = len;
    return true;
  }

  int flags = 0;
  if (mode == ANT_NUMBER_PARSE_JS_NUMBER)
    flags = 
      StringToDoubleConverter::ALLOW_HEX |
      StringToDoubleConverter::ALLOW_LEADING_SPACES |
      StringToDoubleConverter::ALLOW_TRAILING_SPACES;
  else if (mode == ANT_NUMBER_PARSE_FLOAT_PREFIX) 
    flags = 
      StringToDoubleConverter::ALLOW_LEADING_SPACES | 
      StringToDoubleConverter::ALLOW_TRAILING_JUNK;

  StringToDoubleConverter converter(
    flags, 0.0,
    std::numeric_limits<double>::quiet_NaN(),
    "Infinity", "NaN"
  );
  
  int count = 0;
  double value = converter.StringToDouble(str, (int)len, &count);
  
  if (count <= 0) return false;
  if (mode != ANT_NUMBER_PARSE_FLOAT_PREFIX && (size_t)count != len) return false;

  *out = value;
  if (processed) *processed = leading + (size_t)count;
  
  return true;
}

static inline size_t copy_truncated_number_result(char *dst, size_t dstlen, const char *src, size_t srclen) {
  if (!dst || dstlen == 0) return srclen;
  size_t n = srclen < dstlen - 1 ? srclen : dstlen - 1;
  
  if (n > 0) std::memcpy(dst, src, n);
  dst[n] = '\0';

  return srclen;
}

template <typename Format>
static size_t ant_format_number(
  char *buf,
  size_t len,
  char *scratch,
  size_t scratch_len,
  Format format
) {
  char *out = (buf && len >= scratch_len) ? buf : scratch;
  size_t out_len = (out == buf) ? len : scratch_len;

  StringBuilder builder(out, (int)out_len);
  bool ok = format(&builder);
  if (!ok) return 0;

  int pos = builder.position();
  if (pos < 0) return 0;
  builder.Finalize();

  if (out == buf) return (size_t)pos;
  return copy_truncated_number_result(buf, len, scratch, (size_t)pos);
}

extern "C" size_t ant_number_to_shortest(double value, char *buf, size_t len) {
  char scratch[kShortestBufferSize];
  return ant_format_number(buf, len, scratch, sizeof(scratch), [value](StringBuilder *builder) {
    return DoubleToStringConverter::EcmaScriptConverter().ToShortest(value, builder);
  });
}

extern "C" size_t ant_number_to_fixed(double value, int digits, char *buf, size_t len) {
  char scratch[kFixedBufferSize];
  return ant_format_number(buf, len, scratch, sizeof(scratch), [value, digits](StringBuilder *builder) {
    return DoubleToStringConverter::EcmaScriptConverter().ToFixed(value, digits, builder);
  });
}

extern "C" size_t ant_number_to_precision(double value, int precision, char *buf, size_t len) {
  char scratch[kPrecisionBufferSize];
  return ant_format_number(buf, len, scratch, sizeof(scratch), [value, precision](StringBuilder *builder) {
    return DoubleToStringConverter::EcmaScriptConverter().ToPrecision(value, precision, builder);
  });
}

extern "C" size_t ant_number_to_exponential(double value, int digits, char *buf, size_t len) {
  char scratch[kExponentialBufferSize];
  return ant_format_number(buf, len, scratch, sizeof(scratch), [value, digits](StringBuilder *builder) {
    return DoubleToStringConverter::EcmaScriptConverter().ToExponential(value, digits, builder);
  });
}
