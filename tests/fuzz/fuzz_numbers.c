// Number <-> string conversion (src/numbers.cc).
//
// Input: 8 bytes of double bits, a digits byte, then a string.
// Properties:
//   - ant_number_to_shortest output parses back to the same double, and
//     toPrecision(17) / toExponential(16) round-trip exactly;
//   - every formatter behaves like snprintf: the result length is the same
//     for any buffer size and a short buffer holds a NUL-terminated prefix;
//   - parsing arbitrary bytes stays within the input, decimal and Number()
//     agree whenever decimal accepts, and parseFloat accepts what both do;
//   - whitespace trimming never steps outside the string.
#include "fuzz.h"
#include "numbers.h"

#include <math.h>

typedef size_t (*format_fn)(double, int, char *, size_t);

static size_t shortest(double v, int digits, char *buf, size_t len) {
  (void)digits;
  return ant_number_to_shortest(v, buf, len);
}

static bool same_double(double a, double b) {
  if (isnan(a)) return isnan(b);
  return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool parse_all(const char *s, size_t len, ant_number_parse_mode_t mode, double *out) {
  size_t processed = 0;
  bool ok = ant_number_parse(s, len, mode, out, &processed);
  FUZZ_CHECK(processed <= len);
  return ok && processed == len;
}

// Formats into an exact-size heap buffer of every length up to the full
// result, so ASan sees any write past the end.
static size_t check_format(format_fn fn, double v, int digits, char *full, size_t full_cap) {
  size_t n = fn(v, digits, full, full_cap);
  if (n == 0) return 0;
  FUZZ_CHECK(n < full_cap && full[n] == '\0' && strlen(full) == n);
  FUZZ_CHECK(fn(v, digits, NULL, 0) == n);

  for (size_t cap = 1; cap <= n + 1; cap++) {
    char *buf = malloc(cap);
    if (!buf) abort();
    FUZZ_CHECK(fn(v, digits, buf, cap) == n);
    size_t kept = n < cap ? n : cap - 1;
    FUZZ_CHECK(buf[kept] == '\0' && memcmp(buf, full, kept) == 0);
    free(buf);
  }
  return n;
}

static void check_double(double v, uint8_t digits_byte) {
  char out[256];

  size_t n = check_format(shortest, v, 0, out, sizeof(out));
  FUZZ_CHECK(n > 0);
  double back = 0;
  FUZZ_CHECK(parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
  // Number.prototype.toString prints -0 as "0"
  FUZZ_CHECK(v == 0 ? back == 0 : same_double(v, back));

  if (isfinite(v)) {
    n = check_format(ant_number_to_precision, v, 17, out, sizeof(out));
    FUZZ_CHECK(n > 0 && parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
    FUZZ_CHECK(v == 0 ? back == 0 : same_double(v, back));

    n = check_format(ant_number_to_exponential, v, 16, out, sizeof(out));
    FUZZ_CHECK(n > 0 && parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
    FUZZ_CHECK(v == 0 ? back == 0 : same_double(v, back));
  }

  // toFixed takes 0..100, toPrecision 1..100, toExponential 0..100
  int digits = digits_byte % 101;
  n = check_format(ant_number_to_fixed, v, digits, out, sizeof(out));
  if (n) FUZZ_CHECK(parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
  n = check_format(ant_number_to_precision, v, digits ? digits : 1, out, sizeof(out));
  if (n) FUZZ_CHECK(parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
  n = check_format(ant_number_to_exponential, v, digits, out, sizeof(out));
  if (n) FUZZ_CHECK(parse_all(out, n, ANT_NUMBER_PARSE_JS_NUMBER, &back));
}

static void check_string(const char *s, size_t len) {
  double dec = 0, num = 0, flt = 0;
  bool dec_ok = parse_all(s, len, ANT_NUMBER_PARSE_DECIMAL, &dec);
  bool num_ok = parse_all(s, len, ANT_NUMBER_PARSE_JS_NUMBER, &num);

  size_t processed = 0;
  bool flt_ok = ant_number_parse(s, len, ANT_NUMBER_PARSE_FLOAT_PREFIX, &flt, &processed);
  FUZZ_CHECK(processed <= len);
  FUZZ_CHECK(flt_ok == (processed > 0));

  if (dec_ok) {
    FUZZ_CHECK(num_ok && same_double(dec, num));
    FUZZ_CHECK(flt_ok && processed == len && same_double(dec, flt));
  }

  size_t pre = ant_js_trim_prefix_len(s, len);
  size_t suf = ant_js_trim_suffix_len(s, len);
  FUZZ_CHECK(pre <= len && suf <= len);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint64_t bits = fuzz_u64(&in);
  uint8_t digits = fuzz_u8(&in);

  double v;
  memcpy(&v, &bits, sizeof(v));
  check_double(v, digits);

  char *s = fuzz_dup(in.data, in.size);
  check_string(s, in.size);
  free(s);
  return 0;
}
