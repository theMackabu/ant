#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "internal.h"
#include "errors.h"
#include "runtime.h"
#include "utils.h"
#include "silver/lexer.h"

#define BIGINT_BASE ((uint64_t)0x100000000ULL)
#define BIGINT_DEC_GROUP_BASE 1000000000U

typedef struct {
  uint8_t sign;
  uint8_t pad[3];
  uint32_t limb_count;
  uint32_t limbs[];
} bigint_payload_t;

static inline bool is_decimal_digit(char c) {
  return c >= '0' && c <= '9';
}

static bool checked_add_size(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b) return false;
  *out = a + b;
  return true;
}

static bool checked_mul_size(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) return false;
  *out = a * b;
  return true;
}

static uint32_t *limb_alloc(size_t count) {
  if (count == 0) count = 1;
  return (uint32_t *)calloc(count, sizeof(uint32_t));
}

static uint32_t *limb_dup(const uint32_t *limbs, size_t count) {
  if (count == 0) count = 1;
  uint32_t *copy = limb_alloc(count);
  if (!copy) return NULL;
  if (limbs && count > 0) memcpy(copy, limbs, count * sizeof(uint32_t));
  return copy;
}

static bool grow_u32_buffer(uint32_t **buf, size_t *cap) {
  if (!buf || !*buf || !cap || *cap == 0 || *cap > SIZE_MAX / 2) return false;
  size_t new_cap = *cap * 2;
  uint32_t *grown = (uint32_t *)realloc(*buf, new_cap * sizeof(uint32_t));
  if (!grown) return false;
  memset(grown + *cap, 0, (new_cap - *cap) * sizeof(uint32_t));
  *buf = grown;
  *cap = new_cap;
  return true;
}

static bool append_carry_limbs(uint32_t **limbs, size_t *count, size_t *cap, uint64_t carry) {
  while (carry != 0) {
    if (*count == *cap && !grow_u32_buffer(limbs, cap)) return false;
    (*limbs)[(*count)++] = (uint32_t)carry;
    carry >>= 32;
  }
  return true;
}

static void bigint_normalize_limbs(uint32_t *limbs, size_t *count) {
  while (*count > 1 && limbs[*count - 1] == 0) (*count)--;
}

static inline const bigint_payload_t *bigint_payload(ant_t *js, ant_value_t v) {
  (void)js;
  return (const bigint_payload_t *)(uintptr_t)vdata(v);
}

static inline bool limbs_is_zero(const uint32_t *limbs, size_t count) {
  return count == 1 && limbs[0] == 0;
}

bool bigint_is_negative(ant_t *js, ant_value_t v) {
  return bigint_payload(js, v)->sign == 1;
}

static const uint32_t *bigint_limbs(ant_t *js, ant_value_t v, size_t *count) {
  const bigint_payload_t *payload = bigint_payload(js, v);
  size_t limb_count = payload->limb_count;
  if (limb_count == 0) limb_count = 1;
  if (count) *count = limb_count;
  return payload->limbs;
}

static ant_value_t js_mkbigint_limbs(ant_t *js, const uint32_t *limbs, size_t count, bool negative) {
  uint32_t zero = 0;

  if (!limbs || count == 0) {
    limbs = &zero;
    count = 1;
  }

  while (count > 1 && limbs[count - 1] == 0) count--;
  if (count == 1 && limbs[0] == 0) negative = false;

  if (count > UINT32_MAX) return js_mkerr(js, "oom");

  size_t limbs_bytes;
  if (!checked_mul_size(count, sizeof(uint32_t), &limbs_bytes)) return js_mkerr(js, "oom");

  size_t payload_size;
  if (!checked_add_size(offsetof(bigint_payload_t, limbs), limbs_bytes, &payload_size)) {
    return js_mkerr(js, "oom");
  }

  bigint_payload_t *payload = (bigint_payload_t *)js_type_alloc(
    js, ANT_ALLOC_BIGINT, payload_size, _Alignof(bigint_payload_t)
  );
  if (!payload) return js_mkerr(js, "oom");

  payload->sign = negative ? 1 : 0;
  payload->pad[0] = 0;
  payload->pad[1] = 0;
  payload->pad[2] = 0;
  payload->limb_count = (uint32_t)count;
  memcpy(payload->limbs, limbs, limbs_bytes);

  return mkval(T_BIGINT, (uintptr_t)payload);
}

static ant_value_t bigint_from_u64(ant_t *js, uint64_t value) {
  uint32_t limbs[2] = {
    (uint32_t)(value & 0xffffffffu),
    (uint32_t)(value >> 32)
  };

  size_t count = limbs[1] == 0 ? 1 : 2;
  return js_mkbigint_limbs(js, limbs, count, false);
}

static bool bigint_parse_abs_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  if (count > 2) return false;

  uint64_t acc = limbs[0];
  if (count == 2) acc |= ((uint64_t)limbs[1] << 32);

  *out = acc;
  return true;
}

static bool bigint_parse_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  if (bigint_is_negative(js, value)) return false;
  return bigint_parse_abs_u64(js, value, out);
}

static int bigint_cmp_abs_limbs(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen) {
  while (alen > 1 && a[alen - 1] == 0) alen--;
  while (blen > 1 && b[blen - 1] == 0) blen--;

  if (alen != blen) return alen > blen ? 1 : -1;

  for (size_t i = alen; i-- > 0;) {
    if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
  }

  return 0;
}

static uint32_t *bigint_add_abs_limbs(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen, size_t *rlen) {
  size_t maxlen = alen > blen ? alen : blen;

  uint32_t *result = limb_alloc(maxlen + 1);
  if (!result) return NULL;

  uint64_t carry = 0;
  for (size_t i = 0; i < maxlen; i++) {
    uint64_t da = i < alen ? a[i] : 0;
    uint64_t db = i < blen ? b[i] : 0;
    uint64_t sum = da + db + carry;
    result[i] = (uint32_t)sum;
    carry = sum >> 32;
  }

  result[maxlen] = (uint32_t)carry;
  *rlen = maxlen + (carry ? 1 : 0);
  if (*rlen == 0) *rlen = 1;
  bigint_normalize_limbs(result, rlen);
  return result;
}

static uint32_t *bigint_add_u32(const uint32_t *a, size_t alen, uint32_t value, size_t *rlen) {
  uint32_t *result = limb_dup(a, alen);
  if (!result) return NULL;

  uint64_t carry = value;
  size_t i = 0;

  while (carry != 0 && i < alen) {
    uint64_t sum = (uint64_t)result[i] + carry;
    result[i] = (uint32_t)sum;
    carry = sum >> 32;
    i++;
  }

  if (carry == 0) {
    *rlen = alen;
    bigint_normalize_limbs(result, rlen);
    return result;
  }

  uint32_t *grown = (uint32_t *)realloc(result, (alen + 1) * sizeof(uint32_t));
  if (!grown) {
    free(result);
    return NULL;
  }

  grown[alen] = (uint32_t)carry;
  *rlen = alen + 1;
  return grown;
}

static uint32_t *bigint_sub_abs_limbs(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen, size_t *rlen) {
  uint32_t *result = limb_alloc(alen);
  if (!result) return NULL;

  uint64_t borrow = 0;

  for (size_t i = 0; i < alen; i++) {
    uint64_t da = a[i];
    uint64_t db = i < blen ? b[i] : 0;
    uint64_t subtrahend = db + borrow;

    if (da < subtrahend) {
      result[i] = (uint32_t)(BIGINT_BASE + da - subtrahend);
      borrow = 1;
    } else {
      result[i] = (uint32_t)(da - subtrahend);
      borrow = 0;
    }
  }

  *rlen = alen;
  bigint_normalize_limbs(result, rlen);
  return result;
}

static uint32_t *bigint_mul_abs_limbs(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen, size_t *rlen) {
  uint32_t *result = limb_alloc(alen + blen + 1);
  if (!result) return NULL;

  for (size_t i = 0; i < alen; i++) {
    uint64_t carry = 0;

    for (size_t j = 0; j < blen; j++) {
      uint64_t prod = (uint64_t)a[i] * (uint64_t)b[j];
      uint64_t lo = (uint64_t)result[i + j] + (prod & 0xffffffffu) + (carry & 0xffffffffu);
      result[i + j] = (uint32_t)lo;
      carry = (prod >> 32) + (carry >> 32) + (lo >> 32);
    }

    size_t k = i + blen;
    while (carry != 0) {
      uint64_t cur = (uint64_t)result[k] + (carry & 0xffffffffu);
      result[k] = (uint32_t)cur;
      carry = (carry >> 32) + (cur >> 32);
      k++;
    }
  }

  *rlen = alen + blen + 1;
  bigint_normalize_limbs(result, rlen);
  return result;
}

static uint32_t *bigint_shift_left_abs(const uint32_t *limbs, size_t count, uint64_t shift, size_t *rlen) {
  size_t limb_shift = (size_t)(shift >> 5);
  unsigned bit_shift = (unsigned)(shift & 31u);

  size_t out_count = count + limb_shift + 1;
  uint32_t *out = limb_alloc(out_count);
  if (!out) return NULL;

  if (bit_shift == 0) {
    memcpy(out + limb_shift, limbs, count * sizeof(uint32_t));
    *rlen = count + limb_shift;
    bigint_normalize_limbs(out, rlen);
    return out;
  }

  uint32_t carry = 0;
  for (size_t i = 0; i < count; i++) {
    uint64_t cur = ((uint64_t)limbs[i] << bit_shift) | carry;
    out[i + limb_shift] = (uint32_t)cur;
    carry = (uint32_t)(cur >> 32);
  }

  out[count + limb_shift] = carry;
  *rlen = out_count;
  bigint_normalize_limbs(out, rlen);
  return out;
}

static uint32_t *bigint_shift_right_abs(
  const uint32_t *limbs,
  size_t count,
  uint64_t shift,
  bool *truncated,
  size_t *rlen
) {
  size_t limb_shift = (size_t)(shift >> 5);
  unsigned bit_shift = (unsigned)(shift & 31u);

  bool lost = false;

  if (limb_shift >= count) {
    for (size_t i = 0; i < count; i++) {
      if (limbs[i] != 0) { lost = true; break; }
    }

    uint32_t *zero = limb_alloc(1);
    if (!zero) return NULL;
    zero[0] = 0;
    if (truncated) *truncated = lost;
    *rlen = 1;
    return zero;
  }

  for (size_t i = 0; i < limb_shift; i++) {
    if (limbs[i] != 0) { lost = true; break; }
  }

  size_t out_count = count - limb_shift;
  uint32_t *out = limb_alloc(out_count);
  if (!out) return NULL;

  if (bit_shift == 0) {
    memcpy(out, limbs + limb_shift, out_count * sizeof(uint32_t));
  } else {
    uint32_t carry = 0;
    uint32_t mask = (1u << bit_shift) - 1u;

    for (size_t src = count; src-- > limb_shift;) {
      uint32_t cur = limbs[src];
      size_t dst = src - limb_shift;
      out[dst] = (cur >> bit_shift) | (carry << (32u - bit_shift));
      carry = cur & mask;
    }

    if ((limbs[limb_shift] & mask) != 0) lost = true;
  }

  *rlen = out_count;
  bigint_normalize_limbs(out, rlen);
  if (truncated) *truncated = lost;
  return out;
}

static uint32_t bigint_div_small_inplace(uint32_t *limbs, size_t count, uint32_t divisor) {
  uint64_t rem = 0;

  for (size_t i = count; i-- > 0;) {
    uint64_t cur = (rem << 32) | limbs[i];
    limbs[i] = (uint32_t)(cur / divisor);
    rem = cur % divisor;
  }

  return (uint32_t)rem;
}

static size_t bigint_abs_bitlen_limbs(const uint32_t *limbs, size_t count) {
  while (count > 1 && limbs[count - 1] == 0) count--;
  if (count == 1 && limbs[0] == 0) return 0;

  uint32_t top = limbs[count - 1];
#if defined(__GNUC__) || defined(__clang__)
  unsigned lead = (unsigned)__builtin_clz(top);
#else
  unsigned lead = 0;
  while ((top & 0x80000000u) == 0) {
    top <<= 1;
    lead++;
  }
#endif
  return (count - 1) * 32u + (32u - (size_t)lead);
}

static uint32_t *bigint_to_twos_complement_limbs(
  const uint32_t *limbs,
  size_t count,
  bool negative,
  size_t width
) {
  if (width == 0) width = 1;

  uint32_t *out = limb_alloc(width);
  if (!out) return NULL;

  size_t copy_count = count < width ? count : width;
  if (copy_count > 0) memcpy(out, limbs, copy_count * sizeof(uint32_t));

  if (!negative) return out;

  for (size_t i = 0; i < width; i++) out[i] = ~out[i];

  uint64_t carry = 1;
  for (size_t i = 0; i < width && carry != 0; i++) {
    uint64_t cur = (uint64_t)out[i] + carry;
    out[i] = (uint32_t)cur;
    carry = cur >> 32;
  }

  return out;
}

static uint32_t *bigint_from_twos_complement_limbs(
  const uint32_t *twos,
  size_t width,
  bool *negative_out,
  size_t *count_out
) {
  if (width == 0) width = 1;

  bool negative = (twos[width - 1] & 0x80000000u) != 0;
  uint32_t *mag = limb_dup(twos, width);
  if (!mag) return NULL;

  if (negative) {
    for (size_t i = 0; i < width; i++) mag[i] = ~mag[i];
    uint64_t carry = 1;
    for (size_t i = 0; i < width && carry != 0; i++) {
      uint64_t cur = (uint64_t)mag[i] + carry;
      mag[i] = (uint32_t)cur;
      carry = cur >> 32;
    }
  }

  size_t mcount = width;
  bigint_normalize_limbs(mag, &mcount);
  if (mcount == 1 && mag[0] == 0) negative = false;

  if (negative_out) *negative_out = negative;
  if (count_out) *count_out = mcount;
  return mag;
}

typedef enum {
  BIGINT_BAND = 0,
  BIGINT_BOR,
  BIGINT_BXOR
} bigint_bitop_t;

static ant_value_t bigint_bitwise_binary(ant_t *js, ant_value_t a, ant_value_t b, bigint_bitop_t op) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  size_t abit = bigint_abs_bitlen_limbs(ad, alen);
  size_t bbit = bigint_abs_bitlen_limbs(bd, blen);
  size_t width_bits = (abit > bbit ? abit : bbit) + 1;
  size_t width = (width_bits + 31u) / 32u;
  if (width == 0) width = 1;

  uint32_t *at = bigint_to_twos_complement_limbs(ad, alen, aneg, width);
  uint32_t *bt = bigint_to_twos_complement_limbs(bd, blen, bneg, width);
  if (!at || !bt) {
    free(at);
    free(bt);
    return js_mkerr(js, "oom");
  }

  for (size_t i = 0; i < width; i++) {
    switch (op) {
      case BIGINT_BAND: at[i] &= bt[i]; break;
      case BIGINT_BOR:  at[i] |= bt[i]; break;
      case BIGINT_BXOR: at[i] ^= bt[i]; break;
    }
  }

  bool negative = false;
  size_t mcount = 0;
  uint32_t *mag = bigint_from_twos_complement_limbs(at, width, &negative, &mcount);
  free(at);
  free(bt);
  if (!mag) return js_mkerr(js, "oom");

  ant_value_t out = js_mkbigint_limbs(js, mag, mcount, negative);
  free(mag);
  return out;
}

ant_value_t bigint_bitand(ant_t *js, ant_value_t a, ant_value_t b) {
  return bigint_bitwise_binary(js, a, b, BIGINT_BAND);
}

ant_value_t bigint_bitor(ant_t *js, ant_value_t a, ant_value_t b) {
  return bigint_bitwise_binary(js, a, b, BIGINT_BOR);
}

ant_value_t bigint_bitxor(ant_t *js, ant_value_t a, ant_value_t b) {
  return bigint_bitwise_binary(js, a, b, BIGINT_BXOR);
}

ant_value_t bigint_bitnot(ant_t *js, ant_value_t value) {
  bool neg = bigint_is_negative(js, value);
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  size_t bits = bigint_abs_bitlen_limbs(limbs, count);
  size_t width_bits = bits + 1;
  size_t width = (width_bits + 31u) / 32u;
  if (width == 0) width = 1;

  uint32_t *twos = bigint_to_twos_complement_limbs(limbs, count, neg, width);
  if (!twos) return js_mkerr(js, "oom");
  for (size_t i = 0; i < width; i++) twos[i] = ~twos[i];

  bool out_neg = false;
  size_t out_count = 0;
  
  uint32_t *mag = bigint_from_twos_complement_limbs(twos, width, &out_neg, &out_count);
  free(twos); if (!mag) return js_mkerr(js, "oom");

  ant_value_t out = js_mkbigint_limbs(js, mag, out_count, out_neg);
  free(mag);
  
  return out;
}

static inline unsigned clz32_nonzero(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (unsigned)__builtin_clz(v);
#else
  unsigned n = 0;
  while ((v & 0x80000000u) == 0) {
    v <<= 1;
    n++;
  }
  return n;
#endif
}

static bool bigint_divmod_abs_limbs(
  const uint32_t *num,
  size_t num_count,
  const uint32_t *den,
  size_t den_count,
  uint32_t **q_out,
  size_t *q_count_out,
  uint32_t **r_out,
  size_t *r_count_out
) {
  if (den_count == 1 && den[0] == 0) return false;

  int cmp = bigint_cmp_abs_limbs(num, num_count, den, den_count);
  if (cmp < 0) {
    if (q_out) {
      uint32_t *q = limb_alloc(1);
      if (!q) return false;
      q[0] = 0;
      *q_out = q;
      if (q_count_out) *q_count_out = 1;
    }

    if (r_out) {
      uint32_t *r = limb_dup(num, num_count);
      if (!r) {
        if (q_out && *q_out) {
          free(*q_out);
          *q_out = NULL;
        }
        return false;
      }
      size_t rcount = num_count;
      bigint_normalize_limbs(r, &rcount);
      *r_out = r;
      if (r_count_out) *r_count_out = rcount;
    }

    return true;
  }

  if (den_count == 1) {
    uint32_t divisor = den[0];
    uint32_t *q = limb_dup(num, num_count);
    if (!q) return false;

    uint64_t rem = 0;
    for (size_t i = num_count; i-- > 0;) {
      uint64_t cur = (rem << 32) | q[i];
      q[i] = (uint32_t)(cur / divisor);
      rem = cur % divisor;
    }

    size_t qcount = num_count;
    bigint_normalize_limbs(q, &qcount);

    if (q_out) {
      *q_out = q;
      if (q_count_out) *q_count_out = qcount;
    } else free(q);

    if (r_out) {
      uint32_t *r = limb_alloc(1);
      if (!r) {
        if (q_out && *q_out) {
          free(*q_out);
          *q_out = NULL;
        }
        return false;
      }
      r[0] = (uint32_t)rem;
      *r_out = r;
      if (r_count_out) *r_count_out = 1;
    }

    return true;
  }

  size_t m = num_count - den_count;
  uint32_t *vn = limb_alloc(den_count);
  uint32_t *un = limb_alloc(num_count + 1);
  uint32_t *q = limb_alloc(m + 1);

  if (!vn || !un || !q) {
    free(vn);
    free(un);
    free(q);
    return false;
  }

  unsigned shift = clz32_nonzero(den[den_count - 1]);

  if (shift == 0) {
    memcpy(vn, den, den_count * sizeof(uint32_t));
    memcpy(un, num, num_count * sizeof(uint32_t));
    un[num_count] = 0;
  } else {
    uint32_t carry = 0;
    for (size_t i = 0; i < den_count; i++) {
      uint64_t cur = ((uint64_t)den[i] << shift) | carry;
      vn[i] = (uint32_t)cur;
      carry = (uint32_t)(cur >> 32);
    }

    carry = 0;
    for (size_t i = 0; i < num_count; i++) {
      uint64_t cur = ((uint64_t)num[i] << shift) | carry;
      un[i] = (uint32_t)cur;
      carry = (uint32_t)(cur >> 32);
    }
    un[num_count] = carry;
  }

  for (size_t j = m + 1; j-- > 0;) {
    uint64_t numerator = ((uint64_t)un[j + den_count] << 32) | un[j + den_count - 1];
    uint64_t qhat = numerator / vn[den_count - 1];
    uint64_t rhat = numerator % vn[den_count - 1];

    if (qhat >= BIGINT_BASE) {
      qhat = BIGINT_BASE - 1;
      rhat = numerator - qhat * vn[den_count - 1];
    }

    if (den_count > 1) {
      while (qhat * (uint64_t)vn[den_count - 2] > ((rhat << 32) | un[j + den_count - 2])) {
        qhat--;
        rhat += vn[den_count - 1];
        if (rhat >= BIGINT_BASE) break;
      }
    }

    uint64_t k = 0;
    for (size_t i = 0; i < den_count; i++) {
      uint64_t p = qhat * (uint64_t)vn[i] + k;
      k = p >> 32;
      uint32_t plow = (uint32_t)p;

      if (un[j + i] < plow) {
        un[j + i] = (uint32_t)((uint64_t)un[j + i] + BIGINT_BASE - plow);
        k += 1;
      } else {
        un[j + i] -= plow;
      }
    }

    bool borrow = un[j + den_count] < k;
    un[j + den_count] = (uint32_t)(un[j + den_count] - k);

    if (borrow) {
      qhat--;
      uint64_t carry = 0;
      for (size_t i = 0; i < den_count; i++) {
        uint64_t sum = (uint64_t)un[j + i] + vn[i] + carry;
        un[j + i] = (uint32_t)sum;
        carry = sum >> 32;
      }
      un[j + den_count] = (uint32_t)((uint64_t)un[j + den_count] + carry);
    }

    q[j] = (uint32_t)qhat;
  }

  size_t qcount = m + 1;
  bigint_normalize_limbs(q, &qcount);

  if (q_out) {
    *q_out = q;
    if (q_count_out) *q_count_out = qcount;
  } else free(q);

  if (r_out) {
    uint32_t *r = limb_alloc(den_count);
    if (!r) {
      free(vn);
      free(un);
      if (q_out && *q_out) {
        free(*q_out);
        *q_out = NULL;
      }
      return false;
    }

    if (shift == 0) {
      memcpy(r, un, den_count * sizeof(uint32_t));
    } else {
      uint32_t carry = 0;
      for (size_t i = den_count; i-- > 0;) {
        uint32_t cur = un[i];
        r[i] = (cur >> shift) | (carry << (32u - shift));
        carry = cur & ((1u << shift) - 1u);
      }
    }

    size_t rcount = den_count;
    bigint_normalize_limbs(r, &rcount);
    *r_out = r;
    if (r_count_out) *r_count_out = rcount;
  }

  free(vn);
  free(un);
  return true;
}

static ant_value_t bigint_from_string_digits(
  ant_t *js,
  const char *digits,
  size_t len,
  bool negative,
  bool allow_separators
) {
  if (!digits || len == 0) {
    uint32_t zero = 0;
    return js_mkbigint_limbs(js, &zero, 1, false);
  }

  uint32_t base = 10;
  size_t start = 0;

  if (len >= 2 && digits[0] == '0') {
    char p = (char)(digits[1] | 0x20);
    if (p == 'x') {
      base = 16;
      start = 2;
    } else if (p == 'o') {
      base = 8;
      start = 2;
    } else if (p == 'b') {
      base = 2;
      start = 2;
    }
  }

  if (start >= len) return js_mkerr(js, "Cannot convert string to BigInt");

  size_t cap = len / 8 + 2;
  if (cap < 4) cap = 4;
  uint32_t *limbs = limb_alloc(cap);
  if (!limbs) return js_mkerr(js, "oom");

  size_t count = 1;
  bool has_digit = false;
  bool prev_sep = false;

  for (size_t i = start; i < len; i++) {
    char ch = digits[i];

    if (ch == '_') {
      if (!allow_separators || !has_digit || prev_sep) {
        free(limbs);
        return js_mkerr(js, "Cannot convert string to BigInt");
      }
      prev_sep = true;
      continue;
    }

    int digit = hex_digit(ch);
    if (digit < 0 || (uint32_t)digit >= base) {
      free(limbs);
      return js_mkerr(js, "Cannot convert string to BigInt");
    }

    has_digit = true;
    prev_sep = false;

    uint64_t carry = (uint64_t)digit;

    for (size_t j = 0; j < count; j++) {
      uint64_t cur = (uint64_t)limbs[j] * base + carry;
      limbs[j] = (uint32_t)cur;
      carry = cur >> 32;
    }

    if (carry != 0 && !append_carry_limbs(&limbs, &count, &cap, carry)) {
      free(limbs);
      return js_mkerr(js, "oom");
    }
  }

  if (!has_digit || prev_sep) {
    free(limbs);
    return js_mkerr(js, "Cannot convert string to BigInt");
  }

  ant_value_t result = js_mkbigint_limbs(js, limbs, count, negative);
  free(limbs);
  return result;
}

static size_t u32_dec_len(uint32_t v) {
  if (v >= 1000000000u) return 10;
  if (v >= 100000000u) return 9;
  if (v >= 10000000u) return 8;
  if (v >= 1000000u) return 7;
  if (v >= 100000u) return 6;
  if (v >= 10000u) return 5;
  if (v >= 1000u) return 4;
  if (v >= 100u) return 3;
  if (v >= 10u) return 2;
  return 1;
}

static char *bigint_abs_to_decimal_string(const uint32_t *limbs, size_t count, size_t *out_len) {
  if (limbs_is_zero(limbs, count)) {
    char *z = (char *)malloc(2);
    if (!z) return NULL;
    z[0] = '0';
    z[1] = '\0';
    if (out_len) *out_len = 1;
    return z;
  }

  uint32_t *tmp = limb_dup(limbs, count);
  if (!tmp) return NULL;

  size_t tmp_count = count;
  size_t groups_cap = count * 2 + 1;
  uint32_t *groups = (uint32_t *)malloc(groups_cap * sizeof(uint32_t));
  if (!groups) {
    free(tmp);
    return NULL;
  }

  size_t groups_len = 0;

  while (!(tmp_count == 1 && tmp[0] == 0)) {
    if (groups_len == groups_cap && !grow_u32_buffer(&groups, &groups_cap)) {
      free(tmp);
      free(groups);
      return NULL;
    }

    uint32_t rem = bigint_div_small_inplace(tmp, tmp_count, BIGINT_DEC_GROUP_BASE);
    groups[groups_len++] = rem;
    bigint_normalize_limbs(tmp, &tmp_count);
  }

  free(tmp);

  size_t len = u32_dec_len(groups[groups_len - 1]) + (groups_len - 1) * 9;
  char *out = (char *)malloc(len + 1);
  if (!out) {
    free(groups);
    return NULL;
  }

  size_t pos = 0;
  pos += (size_t)snprintf(out + pos, len + 1 - pos, "%u", groups[groups_len - 1]);

  for (size_t i = groups_len - 1; i-- > 0;) {
    pos += (size_t)snprintf(out + pos, len + 1 - pos, "%09u", groups[i]);
  }

  out[len] = '\0';
  free(groups);

  if (out_len) *out_len = len;
  return out;
}

static char *bigint_abs_to_radix_string(const uint32_t *limbs, size_t count, uint32_t radix, size_t *out_len) {
  static const char digit_map[] = "0123456789abcdefghijklmnopqrstuvwxyz";

  if (limbs_is_zero(limbs, count)) {
    char *z = (char *)malloc(2);
    if (!z) return NULL;
    z[0] = '0';
    z[1] = '\0';
    if (out_len) *out_len = 1;
    return z;
  }

  uint32_t *tmp = limb_dup(limbs, count);
  if (!tmp) return NULL;

  size_t tmp_count = count;
  size_t out_cap = count * 32 + 2;
  char *out = (char *)malloc(out_cap);
  if (!out) {
    free(tmp);
    return NULL;
  }

  size_t out_pos = 0;

  while (!(tmp_count == 1 && tmp[0] == 0)) {
    if (out_pos + 1 >= out_cap) {
      size_t new_cap = out_cap * 2;
      char *new_out = (char *)realloc(out, new_cap);
      if (!new_out) {
        free(tmp);
        free(out);
        return NULL;
      }
      out = new_out;
      out_cap = new_cap;
    }

    uint32_t rem = bigint_div_small_inplace(tmp, tmp_count, radix);
    out[out_pos++] = digit_map[rem];
    bigint_normalize_limbs(tmp, &tmp_count);
  }

  for (size_t i = 0; i < out_pos / 2; i++) {
    char t = out[i];
    out[i] = out[out_pos - 1 - i];
    out[out_pos - 1 - i] = t;
  }

  out[out_pos] = '\0';
  free(tmp);

  if (out_len) *out_len = out_pos;
  return out;
}

ant_value_t js_mkbigint(ant_t *js, const char *digits, size_t len, bool negative) {
  return bigint_from_string_digits(js, digits, len, negative, true);
}

ant_value_t bigint_add(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  uint32_t *result = NULL;
  size_t rlen = 0;
  bool rneg = false;

  if (aneg == bneg) {
    result = bigint_add_abs_limbs(ad, alen, bd, blen, &rlen);
    rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs_limbs(ad, alen, bd, blen);
    if (cmp >= 0) {
      result = bigint_sub_abs_limbs(ad, alen, bd, blen, &rlen);
      rneg = aneg;
    } else {
      result = bigint_sub_abs_limbs(bd, blen, ad, alen, &rlen);
      rneg = bneg;
    }
  }

  if (!result) return js_mkerr(js, "oom");

  ant_value_t out = js_mkbigint_limbs(js, result, rlen, rneg);
  free(result);
  return out;
}

ant_value_t bigint_sub(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  uint32_t *result = NULL;
  size_t rlen = 0;
  bool rneg = false;

  if (aneg != bneg) {
    result = bigint_add_abs_limbs(ad, alen, bd, blen, &rlen);
    rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs_limbs(ad, alen, bd, blen);
    if (cmp >= 0) {
      result = bigint_sub_abs_limbs(ad, alen, bd, blen, &rlen);
      rneg = aneg;
    } else {
      result = bigint_sub_abs_limbs(bd, blen, ad, alen, &rlen);
      rneg = !aneg;
    }
  }

  if (!result) return js_mkerr(js, "oom");

  ant_value_t out = js_mkbigint_limbs(js, result, rlen, rneg);
  free(result);
  return out;
}

ant_value_t bigint_mul(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  uint32_t *result = bigint_mul_abs_limbs(ad, alen, bd, blen, &alen);
  if (!result) return js_mkerr(js, "oom");

  bool rneg = (aneg != bneg) && !(alen == 1 && result[0] == 0);
  ant_value_t out = js_mkbigint_limbs(js, result, alen, rneg);
  free(result);
  return out;
}

ant_value_t bigint_div(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  if (blen == 1 && bd[0] == 0) return js_mkerr(js, "Division by zero");

  uint32_t *quot = NULL;
  size_t qlen = 0;

  if (!bigint_divmod_abs_limbs(ad, alen, bd, blen, &quot, &qlen, NULL, NULL)) {
    return js_mkerr(js, "oom");
  }

  bool qneg = (aneg != bneg) && !(qlen == 1 && quot[0] == 0);
  ant_value_t out = js_mkbigint_limbs(js, quot, qlen, qneg);
  free(quot);
  return out;
}

ant_value_t bigint_mod(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  if (blen == 1 && bd[0] == 0) return js_mkerr(js, "Division by zero");

  uint32_t *rem = NULL;
  size_t rlen = 0;

  if (!bigint_divmod_abs_limbs(ad, alen, bd, blen, NULL, NULL, &rem, &rlen)) {
    return js_mkerr(js, "oom");
  }

  bool rneg = aneg && !(rlen == 1 && rem[0] == 0);
  ant_value_t out = js_mkbigint_limbs(js, rem, rlen, rneg);
  free(rem);
  return out;
}

ant_value_t bigint_neg(ant_t *js, ant_value_t a) {
  size_t len = 0;
  const uint32_t *limbs = bigint_limbs(js, a, &len);
  bool neg = bigint_is_negative(js, a);

  if (limbs_is_zero(limbs, len)) return js_mkbigint_limbs(js, limbs, len, false);
  return js_mkbigint_limbs(js, limbs, len, !neg);
}

static inline bool bigint_is_odd(ant_t *js, ant_value_t v) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, v, &count);
  (void)count;
  return (limbs[0] & 1u) != 0;
}

static inline ant_value_t bigint_pow2(ant_t *js, uint64_t bits) {
  uint64_t limb_index_u64 = bits >> 5;
  if (limb_index_u64 > SIZE_MAX - 1) return js_mkerr(js, "oom");

  size_t count = (size_t)limb_index_u64 + 1;
  uint32_t *limbs = limb_alloc(count);
  if (!limbs) return js_mkerr(js, "oom");

  limbs[(size_t)limb_index_u64] = 1u << (bits & 31u);
  ant_value_t out = js_mkbigint_limbs(js, limbs, count, false);
  free(limbs);
  return out;
}

ant_value_t bigint_shift_left(ant_t *js, ant_value_t value, uint64_t shift) {
  if (shift == 0) return value;

  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  if (limbs_is_zero(limbs, count)) return js_mkbigint(js, "0", 1, false);

  uint32_t *result = NULL;
  size_t rlen = 0;
  result = bigint_shift_left_abs(limbs, count, shift, &rlen);
  if (!result) return js_mkerr(js, "oom");

  ant_value_t out = js_mkbigint_limbs(js, result, rlen, bigint_is_negative(js, value));
  free(result);
  return out;
}

ant_value_t bigint_shift_right(ant_t *js, ant_value_t value, uint64_t shift) {
  if (shift == 0) return value;

  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  if (limbs_is_zero(limbs, count)) return js_mkbigint(js, "0", 1, false);

  bool neg = bigint_is_negative(js, value);
  bool truncated = false;

  size_t qlen = 0;
  uint32_t *q = bigint_shift_right_abs(limbs, count, shift, &truncated, &qlen);
  if (!q) return js_mkerr(js, "oom");

  if (!neg) {
    ant_value_t out = js_mkbigint_limbs(js, q, qlen, false);
    free(q);
    return out;
  }

  if (truncated) {
    size_t new_len = 0;
    uint32_t *adj = bigint_add_u32(q, qlen, 1, &new_len);
    free(q);
    if (!adj) return js_mkerr(js, "oom");
    q = adj;
    qlen = new_len;
  }

  ant_value_t out = js_mkbigint_limbs(js, q, qlen, !limbs_is_zero(q, qlen));
  free(q);
  return out;
}

ant_value_t bigint_shift_right_logical(ant_t *js, ant_value_t value, uint64_t shift) {
  (void)value;
  (void)shift;
  return js_mkerr_typed(js, JS_ERR_TYPE, "BigInts have no unsigned right shift, use >> instead");
}

int bigint_compare(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  if (aneg && !bneg) return -1;
  if (!aneg && bneg) return 1;

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  int cmp = bigint_cmp_abs_limbs(ad, alen, bd, blen);
  return aneg ? -cmp : cmp;
}

bool bigint_is_zero(ant_t *js, ant_value_t v) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, v, &count);
  return limbs_is_zero(limbs, count);
}

size_t bigint_digits_len(ant_t *js, ant_value_t v) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, v, &count);

  size_t len = 0;
  char *digits = bigint_abs_to_decimal_string(limbs, count, &len);
  if (!digits) return 0;

  free(digits);
  return len;
}

ant_value_t bigint_exp(ant_t *js, ant_value_t base, ant_value_t exp) {
  if (bigint_is_negative(js, exp)) return js_mkerr(js, "Exponent must be positive");
  if (bigint_is_zero(js, exp)) return js_mkbigint(js, "1", 1, false);

  ant_value_t result = js_mkbigint(js, "1", 1, false);
  ant_value_t b = base;
  ant_value_t e = exp;

  while (!bigint_is_zero(js, e)) {
    if (bigint_is_odd(js, e)) {
      result = bigint_mul(js, result, b);
      if (is_err(result)) return result;
    }

    b = bigint_mul(js, b, b);
    if (is_err(b)) return b;

    e = bigint_shift_right(js, e, 1);
    if (is_err(e)) return e;
  }

  return result;
}

size_t strbigint(ant_t *js, ant_value_t value, char *buf, size_t len) {
  bool neg = bigint_is_negative(js, value);

  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  size_t dlen = 0;
  char *digits = bigint_abs_to_decimal_string(limbs, count, &dlen);
  if (!digits) return 0;

  size_t total = dlen + (neg ? 1 : 0);
  if (len == 0) {
    free(digits);
    return total;
  }

  size_t n = 0;

  if (neg && n < len - 1) buf[n] = '-';
  if (neg) n++;

  size_t avail = n < len ? len - n - 1 : 0;
  size_t copy_len = dlen < avail ? dlen : avail;
  if (copy_len > 0) memcpy(buf + n, digits, copy_len);

  size_t term = n + copy_len;
  if (term >= len) term = len - 1;
  buf[term] = '\0';

  free(digits);
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

    int need = snprintf(NULL, 0, "%.0f", d);
    if (need < 0) return js_mkerr(js, "Cannot convert to BigInt");

    char *buf = (char *)malloc((size_t)need + 1);
    if (!buf) return js_mkerr(js, "oom");

    snprintf(buf, (size_t)need + 1, "%.0f", d);
    ant_value_t out = js_mkbigint(js, buf, (size_t)need, neg);
    free(buf);
    return out;
  }

  if (vtype(arg) == T_STR) {
    ant_offset_t slen;
    ant_offset_t off = vstr(js, arg, &slen);
    const char *str = (const char *)(uintptr_t)(off);

    size_t start = 0;
    size_t end = slen;
    
    while (start < end && is_space((unsigned char)str[start])) start++;
    while (end > start && is_space((unsigned char)str[end - 1])) end--;
    if (start >= end) return js_mkbigint(js, "0", 1, false);

    bool neg = false;
    if (str[start] == '-') {
      neg = true;
      start++;
    } else if (str[start] == '+') start++;

    if (start >= end) return js_mkerr(js, "Cannot convert string to BigInt");
    return bigint_from_string_digits(js, str + start, end - start, neg, false);
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
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, val, &count);

  size_t dlen = 0;
  char *digits = NULL;

  if (radix == 10) digits = bigint_abs_to_decimal_string(limbs, count, &dlen);
  else digits = bigint_abs_to_radix_string(limbs, count, (uint32_t)radix, &dlen);

  if (!digits) return js_mkerr(js, "oom");

  if (!neg) {
    ant_value_t out = js_mkstr(js, digits, dlen);
    free(digits);
    return out;
  }

  char *full = (char *)malloc(dlen + 2);
  if (!full) {
    free(digits);
    return js_mkerr(js, "oom");
  }

  full[0] = '-';
  memcpy(full + 1, digits, dlen);
  full[dlen + 1] = '\0';

  ant_value_t out = js_mkstr(js, full, dlen + 1);
  free(digits);
  free(full);
  return out;
}

void init_bigint_module(void) {
  ant_t *js = rt->js;

  ant_value_t glob = js_glob(js);
  ant_value_t object_proto = js->object;
  ant_value_t function_proto = js_get_slot(glob, SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  ant_value_t bigint_proto = js_mkobj(js);
  js_set_proto_init(bigint_proto, object_proto);
  js_setprop(js, bigint_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_bigint_toString));

  ant_value_t bigint_ctor_obj = mkobj(js, 0);
  js_set_proto_init(bigint_ctor_obj, function_proto);
  js_set_slot(bigint_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_BigInt));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  js_setprop_nonconfigurable(js, bigint_ctor_obj, "prototype", 9, bigint_proto);
  js_setprop(js, bigint_ctor_obj, ANT_STRING("name"), ANT_STRING("BigInt"));
  js_setprop(js, glob, js_mkstr(js, "BigInt", 6), js_obj_to_func(bigint_ctor_obj));
}
