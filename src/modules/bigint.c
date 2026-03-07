#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "internal.h"
#include "errors.h"
#include "arena.h"
#include "runtime.h"

static inline bool is_decimal_digit(char c) {
  return c >= '0' && c <= '9';
}

bool bigint_is_negative(ant_t *js, ant_value_t v) {
  ant_offset_t ofs = (ant_offset_t)vdata(v);
  return js->mem[ofs + sizeof(ant_offset_t)] == 1;
}

size_t bigint_digits_len(ant_t *js, ant_value_t v) {
  ant_offset_t ofs = (ant_offset_t)vdata(v);
  ant_offset_t header = loadoff(js, ofs);
  return (size_t)((header >> 4) - 2);
}

static const char *bigint_digits(ant_t *js, ant_value_t v, size_t *len) {
  ant_offset_t ofs = (ant_offset_t)vdata(v);
  size_t total = bigint_digits_len(js, v);
  if (len) *len = total;
  return (const char *)&js->mem[ofs + sizeof(ant_offset_t) + 1];
}

static ant_value_t bigint_from_u64(ant_t *js, uint64_t value) {
  char buf[32];
  size_t len = uint_to_str(buf, sizeof(buf), value);
  return js_mkbigint(js, buf, len, false);
}

static bool bigint_parse_abs_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  size_t len = 0;
  const char *digits = bigint_digits(js, value, &len);
  uint64_t acc = 0;

  for (size_t i = 0; i < len; i++) {
    char c = digits[i];
    if (!is_decimal_digit(c)) return false;
    uint64_t digit = (uint64_t)(c - '0');
    if (acc > UINT64_MAX / 10 || (acc == UINT64_MAX / 10 && digit > (UINT64_MAX % 10))) {
      return false;
    }
    acc = acc * 10 + digit;
  }

  *out = acc;
  return true;
}

static bool bigint_parse_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  if (bigint_is_negative(js, value)) return false;
  return bigint_parse_abs_u64(js, value, out);
}

ant_value_t js_mkbigint(ant_t *js, const char *digits, size_t len, bool negative) {
  size_t total = len + 2;
  ant_offset_t ofs = js_alloc(js, total + sizeof(ant_offset_t));
  if (ofs == (ant_offset_t)~0) return js_mkerr(js, "oom");

  ant_offset_t header = (ant_offset_t)(total << 4);
  memcpy(&js->mem[ofs], &header, sizeof(header));
  js->mem[ofs + sizeof(header)] = negative ? 1 : 0;
  if (digits) memcpy(&js->mem[ofs + sizeof(header) + 1], digits, len);
  js->mem[ofs + sizeof(header) + 1 + len] = 0;
  return mkval(T_BIGINT, ofs);
}

static int bigint_cmp_abs(const char *a, size_t alen, const char *b, size_t blen) {
  while (alen > 1 && a[0] == '0') { a++; alen--; }
  while (blen > 1 && b[0] == '0') { b++; blen--; }
  if (alen != blen) return alen > blen ? 1 : -1;
  for (size_t i = 0; i < alen; i++) {
    if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
  }
  return 0;
}

static char *bigint_add_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  size_t maxlen = (alen > blen ? alen : blen) + 1;
  char *result = (char *)malloc(maxlen + 1);
  if (!result) return NULL;

  int carry = 0;
  size_t ri = 0;
  for (size_t i = 0; i < maxlen; i++) {
    int da = (i < alen) ? (a[alen - 1 - i] - '0') : 0;
    int db = (i < blen) ? (b[blen - 1 - i] - '0') : 0;
    int sum = da + db + carry;
    carry = sum / 10;
    result[ri++] = (char)('0' + (sum % 10));
  }

  while (ri > 1 && result[ri - 1] == '0') ri--;
  for (size_t i = 0; i < ri / 2; i++) {
    char tmp = result[i];
    result[i] = result[ri - 1 - i];
    result[ri - 1 - i] = tmp;
  }

  result[ri] = 0;
  *rlen = ri;
  return result;
}

static char *bigint_sub_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  char *result = (char *)malloc(alen + 1);
  if (!result) return NULL;

  int borrow = 0;
  size_t ri = 0;
  for (size_t i = 0; i < alen; i++) {
    int da = a[alen - 1 - i] - '0';
    int db = (i < blen) ? (b[blen - 1 - i] - '0') : 0;
    int diff = da - db - borrow;
    if (diff < 0) {
      diff += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    result[ri++] = (char)('0' + diff);
  }

  while (ri > 1 && result[ri - 1] == '0') ri--;
  for (size_t i = 0; i < ri / 2; i++) {
    char tmp = result[i];
    result[i] = result[ri - 1 - i];
    result[ri - 1 - i] = tmp;
  }

  result[ri] = 0;
  *rlen = ri;
  return result;
}

static char *bigint_mul_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  size_t reslen = alen + blen;
  int *temp = (int *)calloc(reslen, sizeof(int));
  if (!temp) return NULL;

  for (size_t i = 0; i < alen; i++) {
    for (size_t j = 0; j < blen; j++) {
      temp[i + j] += (a[alen - 1 - i] - '0') * (b[blen - 1 - j] - '0');
    }
  }

  for (size_t i = 0; i < reslen - 1; i++) {
    temp[i + 1] += temp[i] / 10;
    temp[i] %= 10;
  }

  size_t start = reslen - 1;
  while (start > 0 && temp[start] == 0) start--;

  char *result = (char *)malloc(start + 2);
  if (!result) {
    free(temp);
    return NULL;
  }

  for (size_t i = 0; i <= start; i++) result[i] = (char)('0' + temp[start - i]);
  result[start + 1] = 0;
  *rlen = start + 1;

  free(temp);
  return result;
}

static char *bigint_div_abs(
  const char *a,
  size_t alen,
  const char *b,
  size_t blen,
  size_t *rlen,
  char **rem,
  size_t *remlen
) {
  if (blen == 1 && b[0] == '0') return NULL;

  if (bigint_cmp_abs(a, alen, b, blen) < 0) {
    char *result = (char *)malloc(2);
    result[0] = '0';
    result[1] = 0;
    *rlen = 1;
    if (rem) {
      *rem = (char *)malloc(alen + 1);
      memcpy(*rem, a, alen);
      (*rem)[alen] = 0;
      *remlen = alen;
    }
    return result;
  }

  char *current = (char *)calloc(alen + 1, 1);
  char *result = (char *)calloc(alen + 1, 1);
  if (!current || !result) {
    free(current);
    free(result);
    return NULL;
  }

  size_t curlen = 0, reslen = 0;
  for (size_t i = 0; i < alen; i++) {
    if (curlen == 1 && current[0] == '0') curlen = 0;
    current[curlen++] = a[i];
    current[curlen] = 0;

    int count = 0;
    while (bigint_cmp_abs(current, curlen, b, blen) >= 0) {
      size_t sublen;
      char *sub = bigint_sub_abs(current, curlen, b, blen, &sublen);
      if (!sub) break;
      memcpy(current, sub, sublen + 1);
      curlen = sublen;
      free(sub);
      count++;
    }

    result[reslen++] = (char)('0' + count);
  }

  size_t start = 0;
  while (start < reslen - 1 && result[start] == '0') start++;
  memmove(result, result + start, reslen - start + 1);

  *rlen = reslen - start;
  if (rem) {
    *rem = current;
    *remlen = curlen;
  } else {
    free(current);
  }

  return result;
}

ant_value_t bigint_add(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  char *result;
  size_t rlen;
  bool rneg;

  if (aneg == bneg) {
    result = bigint_add_abs(ad, alen, bd, blen, &rlen);
    rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs(ad, alen, bd, blen);
    if (cmp >= 0) {
      result = bigint_sub_abs(ad, alen, bd, blen, &rlen);
      rneg = aneg;
    } else {
      result = bigint_sub_abs(bd, blen, ad, alen, &rlen);
      rneg = bneg;
    }
  }

  if (!result) return js_mkerr(js, "oom");
  if (rlen == 1 && result[0] == '0') rneg = false;

  ant_value_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

ant_value_t bigint_sub(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  char *result;
  size_t rlen;
  bool rneg;

  if (aneg != bneg) {
    result = bigint_add_abs(ad, alen, bd, blen, &rlen);
    rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs(ad, alen, bd, blen);
    if (cmp >= 0) {
      result = bigint_sub_abs(ad, alen, bd, blen, &rlen);
      rneg = aneg;
    } else {
      result = bigint_sub_abs(bd, blen, ad, alen, &rlen);
      rneg = !aneg;
    }
  }

  if (!result) return js_mkerr(js, "oom");
  if (rlen == 1 && result[0] == '0') rneg = false;

  ant_value_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

ant_value_t bigint_mul(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  size_t rlen;
  char *result = bigint_mul_abs(ad, alen, bd, blen, &rlen);
  if (!result) return js_mkerr(js, "oom");

  bool rneg = (aneg != bneg) && !(rlen == 1 && result[0] == '0');
  ant_value_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

ant_value_t bigint_div(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  if (blen == 1 && bd[0] == '0') return js_mkerr(js, "Division by zero");

  size_t rlen;
  char *result = bigint_div_abs(ad, alen, bd, blen, &rlen, NULL, NULL);
  if (!result) return js_mkerr(js, "oom");

  bool rneg = (aneg != bneg) && !(rlen == 1 && result[0] == '0');
  ant_value_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

ant_value_t bigint_mod(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  if (blen == 1 && bd[0] == '0') return js_mkerr(js, "Division by zero");

  size_t rlen, remlen;
  char *rem;
  char *result = bigint_div_abs(ad, alen, bd, blen, &rlen, &rem, &remlen);
  if (!result) return js_mkerr(js, "oom");
  free(result);

  bool rneg = aneg && !(remlen == 1 && rem[0] == '0');
  ant_value_t r = js_mkbigint(js, rem, remlen, rneg);
  free(rem);
  return r;
}

ant_value_t bigint_neg(ant_t *js, ant_value_t a) {
  size_t len;
  const char *digits = bigint_digits(js, a, &len);
  bool neg = bigint_is_negative(js, a);
  if (len == 1 && digits[0] == '0') return js_mkbigint(js, digits, len, false);
  return js_mkbigint(js, digits, len, !neg);
}

ant_value_t bigint_exp(ant_t *js, ant_value_t base, ant_value_t exp) {
  if (bigint_is_negative(js, exp)) return js_mkerr(js, "Exponent must be positive");

  size_t explen;
  const char *expd = bigint_digits(js, exp, &explen);
  if (explen == 1 && expd[0] == '0') return js_mkbigint(js, "1", 1, false);

  ant_value_t result = js_mkbigint(js, "1", 1, false);
  ant_value_t b = base;
  ant_value_t e = exp;
  ant_value_t two = js_mkbigint(js, "2", 1, false);

  while (true) {
    size_t elen;
    const char *ed = bigint_digits(js, e, &elen);
    if (elen == 1 && ed[0] == '0') break;

    int last_digit = ed[elen - 1] - '0';
    if (last_digit % 2 == 1) {
      result = bigint_mul(js, result, b);
      if (is_err(result)) return result;
    }

    b = bigint_mul(js, b, b);
    if (is_err(b)) return b;

    e = bigint_div(js, e, two);
    if (is_err(e)) return e;
  }

  return result;
}

static inline ant_value_t bigint_pow2(ant_t *js, uint64_t bits) {
  ant_value_t two = js_mkbigint(js, "2", 1, false);
  if (is_err(two)) return two;

  ant_value_t exp = bigint_from_u64(js, bits);
  if (is_err(exp)) return exp;

  return bigint_exp(js, two, exp);
}

ant_value_t bigint_shift_left(ant_t *js, ant_value_t value, uint64_t shift) {
  if (shift == 0) return value;
  if (shift > 18446744073709551615ULL) return js_mkerr(js, "Shift count too large");

  size_t digits_len;
  const char *digits = bigint_digits(js, value, &digits_len);
  if (digits_len == 1 && digits[0] == '0') return js_mkbigint(js, "0", 1, false);

  uint64_t u64 = 0;
  if (!bigint_is_negative(js, value) && shift < 64 && bigint_parse_u64(js, value, &u64)) {
    if (u64 <= (UINT64_MAX >> shift)) return bigint_from_u64(js, u64 << shift);
  }

  ant_value_t pow = bigint_pow2(js, shift);
  if (is_err(pow)) return pow;
  return bigint_mul(js, value, pow);
}

ant_value_t bigint_shift_right(ant_t *js, ant_value_t value, uint64_t shift) {
  if (shift == 0) return value;
  if (shift > 18446744073709551615ULL) return js_mkerr(js, "Shift count too large");

  size_t digits_len;
  const char *digits = bigint_digits(js, value, &digits_len);
  if (digits_len == 1 && digits[0] == '0') return js_mkbigint(js, "0", 1, false);

  uint64_t u64 = 0;
  if (!bigint_is_negative(js, value) && bigint_parse_u64(js, value, &u64)) {
    if (shift >= 64) return js_mkbigint(js, "0", 1, false);
    return bigint_from_u64(js, u64 >> shift);
  }

  if (bigint_parse_abs_u64(js, value, &u64)) {
    if (shift >= 64) {
      return js_mkbigint(js, bigint_is_negative(js, value) ? "1" : "0", 1, bigint_is_negative(js, value));
    }

    uint64_t shifted = u64 >> shift;
    if (bigint_is_negative(js, value)) {
      if ((u64 & ((1ULL << shift) - 1)) != 0) shifted += 1;
      ant_value_t pos = bigint_from_u64(js, shifted);
      if (is_err(pos)) return pos;
      return bigint_neg(js, pos);
    }

    return bigint_from_u64(js, shifted);
  }

  ant_value_t pow = bigint_pow2(js, shift);
  if (is_err(pow)) return pow;
  return bigint_div(js, value, pow);
}

ant_value_t bigint_shift_right_logical(ant_t *js, ant_value_t value, uint64_t shift) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "BigInts have no unsigned right shift, use >> instead");
}

size_t bigint_compare(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);
  
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen);
  const char *bd = bigint_digits(js, b, &blen);

  if (aneg && !bneg) return -1;
  if (!aneg && bneg) return 1;

  int cmp = bigint_cmp_abs(ad, alen, bd, blen);
  return aneg ? -cmp : cmp;
}

bool bigint_is_zero(ant_t *js, ant_value_t v) {
  size_t len;
  const char *digits = bigint_digits(js, v, &len);
  return len == 1 && digits[0] == '0';
}

size_t strbigint(ant_t *js, ant_value_t value, char *buf, size_t len) {
  bool neg = bigint_is_negative(js, value);
  size_t dlen;
  const char *digits = bigint_digits(js, value, &dlen);
  size_t total = dlen + (neg ? 1 : 0);

  if (len == 0) return total;

  size_t n = 0;
  if (neg && n < len - 1) buf[n] = '-';
  if (neg) n++;

  size_t avail = n < len ? len - n - 1 : 0;
  size_t copy_len = dlen < avail ? dlen : avail;
  if (copy_len > 0) memcpy(buf + n, digits, copy_len);

  size_t term = n + copy_len;
  if (term >= len) term = len - 1;
  buf[term] = '\0';
  return total;
}

static ant_value_t builtin_BigInt(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) != T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt is not a constructor");
  if (nargs < 1) return js_mkbigint(js, "0", 1, false);

  ant_value_t arg = args[0];
  if (vtype(arg) == T_BIGINT) return arg;

  if (vtype(arg) == T_NUM) {
    double d = tod(arg);
    if (!isfinite(d)) return js_mkerr(js, "Cannot convert Infinity or NaN to BigInt");
    if (d != trunc(d)) return js_mkerr(js, "Cannot convert non-integer to BigInt");

    bool neg = d < 0;
    if (neg) d = -d;

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", d);
    return js_mkbigint(js, buf, strlen(buf), neg);
  }

  if (vtype(arg) == T_STR) {
    ant_offset_t slen;
    ant_offset_t off = vstr(js, arg, &slen);
    const char *str = (const char *)&js->mem[off];

    bool neg = false;
    size_t i = 0;
    if (slen > 0 && str[0] == '-') {
      neg = true;
      i++;
    } else if (slen > 0 && str[0] == '+') {
      i++;
    }

    while (i < slen && str[i] == '0') i++;
    if (i >= slen) return js_mkbigint(js, "0", 1, false);

    for (size_t j = i; j < slen; j++) {
      if (!is_decimal_digit(str[j])) return js_mkerr(js, "Cannot convert string to BigInt");
    }

    return js_mkbigint(js, str + i, slen - i, neg);
  }

  if (vtype(arg) == T_BOOL) return js_mkbigint(js, vdata(arg) ? "1" : "0", 1, false);

  return js_mkerr(js, "Cannot convert to BigInt");
}

static ant_value_t bigint_to_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  if (!bigint_parse_u64(js, value, out)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }
  return js_mkundef();
}

ant_value_t bigint_asint_bits(ant_t *js, ant_value_t arg, uint64_t *bits_out) {
  if (vtype(arg) == T_BIGINT) return bigint_to_u64(js, arg, bits_out);

  double bits = js_to_number(js, arg);
  if (!isfinite(bits) || bits < 0 || bits != floor(bits)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }

  if (bits > 18446744073709551615.0) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }

  *bits_out = (uint64_t)bits;
  return js_mkundef();
}

static ant_value_t builtin_BigInt_asIntN(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "BigInt.asIntN requires 2 arguments");

  uint64_t bits = 0;
  ant_value_t err = bigint_asint_bits(js, args[0], &bits);
  if (is_err(err)) return err;

  if (vtype(args[1]) != T_BIGINT) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  if (bits == 0) return js_mkbigint(js, "0", 1, false);

  ant_value_t mod = bigint_pow2(js, bits);
  if (is_err(mod)) return mod;

  ant_value_t res = bigint_mod(js, args[1], mod);
  if (is_err(res)) return res;

  if (bigint_is_negative(js, res)) {
    ant_value_t adj = bigint_add(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }

  ant_value_t threshold = bigint_pow2(js, bits - 1);
  if (is_err(threshold)) return threshold;

  if (bigint_compare(js, res, threshold) >= 0) {
    ant_value_t adj = bigint_sub(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }

  return res;
}

static ant_value_t builtin_BigInt_asUintN(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "BigInt.asUintN requires 2 arguments");

  uint64_t bits = 0;
  ant_value_t err = bigint_asint_bits(js, args[0], &bits);
  if (is_err(err)) return err;

  if (vtype(args[1]) != T_BIGINT) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  if (bits == 0) return js_mkbigint(js, "0", 1, false);

  ant_value_t mod = bigint_pow2(js, bits);
  if (is_err(mod)) return mod;

  ant_value_t res = bigint_mod(js, args[1], mod);
  if (is_err(res)) return res;

  if (bigint_is_negative(js, res)) {
    ant_value_t adj = bigint_add(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }

  return res;
}

static ant_value_t builtin_bigint_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t val = js->this_val;
  if (vtype(val) != T_BIGINT) return js_mkerr(js, "toString called on non-BigInt");

  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) return js_mkerr(js, "radix must be between 2 and 36");
  }

  bool neg = bigint_is_negative(js, val);
  size_t dlen;
  const char *digits = bigint_digits(js, val, &dlen);

  if (radix == 10) {
    size_t buflen = dlen + 2;
    char *buf = (char *)ant_calloc(buflen);
    if (!buf) return js_mkerr(js, "oom");

    size_t n = 0;
    if (neg) buf[n++] = '-';
    memcpy(buf + n, digits, dlen);
    n += dlen;

    ant_value_t ret = js_mkstr(js, buf, n);
    free(buf);
    return ret;
  }

  const uint32_t base = 1000000000U;
  size_t result_cap = dlen * 4 + 16;
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");

  size_t rpos = result_cap - 1;
  result[rpos] = '\0';

  size_t limb_cap = (dlen + 8) / 9 + 1;
  uint32_t *limbs = (uint32_t *)ant_calloc(limb_cap * sizeof(uint32_t));
  if (!limbs) {
    free(result);
    return js_mkerr(js, "oom");
  }
  size_t limb_len = 1;

  for (size_t i = 0; i < dlen; i++) {
    uint64_t carry = (uint64_t)(digits[i] - '0');
    for (size_t j = 0; j < limb_len; j++) {
      uint64_t cur = (uint64_t)limbs[j] * 10 + carry;
      limbs[j] = (uint32_t)(cur % base);
      carry = cur / base;
    }

    if (carry != 0) {
      if (limb_len == limb_cap) {
        size_t new_cap = limb_cap * 2;
        uint32_t *new_limbs = (uint32_t *)ant_realloc(limbs, new_cap * sizeof(uint32_t));
        if (!new_limbs) {
          free(limbs);
          free(result);
          return js_mkerr(js, "oom");
        }
        limbs = new_limbs;
        limb_cap = new_cap;
      }
      limbs[limb_len++] = (uint32_t)carry;
    }
  }

  static const char digit_map[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  while (limb_len > 0 && !(limb_len == 1 && limbs[0] == 0)) {
    uint64_t remainder = 0;
    for (size_t i = limb_len; i-- > 0;) {
      uint64_t cur = (uint64_t)limbs[i] + remainder * base;
      limbs[i] = (uint32_t)(cur / (uint64_t)radix);
      remainder = cur % (uint64_t)radix;
    }

    while (limb_len > 0 && limbs[limb_len - 1] == 0) limb_len--;

    if (rpos == 0) {
      size_t new_cap = result_cap * 2;
      char *new_result = (char *)ant_calloc(new_cap);
      if (!new_result) {
        free(limbs);
        free(result);
        return js_mkerr(js, "oom");
      }

      size_t used = result_cap - rpos;
      memcpy(new_result + new_cap - used, result + rpos, used);
      free(result);

      result = new_result;
      rpos = new_cap - used;
      result_cap = new_cap;
    }

    result[--rpos] = digit_map[remainder];
  }

  free(limbs);

  if (rpos == result_cap - 1) result[--rpos] = '0';
  if (neg) result[--rpos] = '-';

  ant_value_t ret = js_mkstr(js, result + rpos, result_cap - 1 - rpos);
  free(result);
  return ret;
}

void init_bigint_module(void) {
  ant_t *js = rt->js;
  
  ant_value_t glob = js_glob(js);
  ant_value_t object_proto = js->object;
  ant_value_t function_proto = js_get_slot(js, glob, SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  ant_value_t bigint_proto = js_mkobj(js);
  js_set_proto(js, bigint_proto, object_proto);
  js_setprop(js, bigint_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_bigint_toString));

  ant_value_t bigint_ctor_obj = mkobj(js, 0);
  js_set_proto(js, bigint_ctor_obj, function_proto);
  js_set_slot(js, bigint_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_BigInt));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  js_setprop_nonconfigurable(js, bigint_ctor_obj, "prototype", 9, bigint_proto);
  js_setprop(js, bigint_ctor_obj, ANT_STRING("name"), ANT_STRING("BigInt"));
  js_setprop(js, glob, js_mkstr(js, "BigInt", 6), js_obj_to_func(bigint_ctor_obj));
}
