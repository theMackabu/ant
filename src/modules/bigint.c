#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "internal.h"
#include "errors.h"
#include "gc/roots.h"
#include "utils.h"
#include "silver/lexer.h"

static constexpr size_t BIGINT_LIMB_SIZE = 32;
static constexpr size_t BIGINT_DOUBLE_PRECISION = DBL_MANT_DIG;

static constexpr uint64_t BIGINT_BASE = UINT64_C(0x100000000);
static constexpr uint32_t BIGINT_DEC_GROUP_BASE = UINT32_C(1000000000);

typedef struct {
  uint8_t sign;
  uint8_t pad[3];
  uint32_t limb_count;
  uint32_t limbs[];
} bigint_payload_t;

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

static inline const bigint_payload_t *bigint_payload(ant_value_t v) {
  return (const bigint_payload_t *)vptr(v);
}

static inline bool limbs_is_zero(const uint32_t *limbs, size_t count) {
  return count == 1 && limbs[0] == 0;
}

bool bigint_is_negative(ant_t *js, ant_value_t v) {
  return bigint_payload(v)->sign == 1;
}

static const uint32_t *bigint_limbs(ant_t *js, ant_value_t v, size_t *count) {
  const bigint_payload_t *payload = bigint_payload(v);
  size_t limb_count = payload->limb_count;
  if (limb_count == 0) limb_count = 1;
  if (count) *count = limb_count;
  return payload->limbs;
}

static ant_value_t bigint_alloc_payload(
  ant_t *js,
  size_t capacity,
  bigint_payload_t **payload_out
) {
  if (capacity == 0) capacity = 1;
  if (capacity > UINT32_MAX) return js_mkerr(js, "oom");

  size_t limbs_bytes;
  if (!checked_mul_size(capacity, sizeof(uint32_t), &limbs_bytes))
    return js_mkerr(js, "oom");

  size_t payload_size;
  if (!checked_add_size(offsetof(bigint_payload_t, limbs), limbs_bytes, &payload_size))
    return js_mkerr(js, "oom");

  bigint_payload_t *payload = (bigint_payload_t *)js_type_alloc(
    js, ANT_ALLOC_BIGINT, 
    payload_size, _Alignof(bigint_payload_t)
  );
  
  if (!payload) return js_mkerr(js, "oom");

  payload->sign = 0;
  payload->pad[0] = 0;
  payload->pad[1] = 0;
  payload->pad[2] = 0;
  payload->limb_count = (uint32_t)capacity;
  *payload_out = payload;
  
  return mkref(kTypeBigInt, payload);
}

static ant_value_t bigint_alloc_binary_payload(
  ant_t *js,
  ant_value_t a,
  ant_value_t b,
  size_t capacity,
  bigint_payload_t **payload_out
) {
  GC_ROOT_SAVE(root_mark, js);
  if (!gc_push_root(js, &a) || !gc_push_root(js, &b)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "oom");
  }

  ant_value_t out = bigint_alloc_payload(js, capacity, payload_out);
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t bigint_alloc_unary_payload(
  ant_t *js,
  ant_value_t value,
  size_t capacity,
  bigint_payload_t **payload_out
) {
  GC_ROOT_SAVE(root_mark, js);
  if (!gc_push_root(js, &value)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "oom");
  }

  ant_value_t out = bigint_alloc_payload(js, capacity, payload_out);
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t bigint_finish_payload(
  ant_value_t value,
  bigint_payload_t *payload,
  size_t count,
  bool negative
) {
  bigint_normalize_limbs(payload->limbs, &count);
  if (count == 1 && payload->limbs[0] == 0) negative = false;
  payload->sign = negative ? 1 : 0;
  payload->limb_count = (uint32_t)count;
  return value;
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
  return (count - 1) * BIGINT_LIMB_SIZE +
    (BIGINT_LIMB_SIZE - (size_t)lead);
}

static uint64_t bigint_extract_u64(const uint32_t *limbs, size_t count, size_t bit_offset) {
  size_t word = bit_offset / BIGINT_LIMB_SIZE;
  unsigned shift = (unsigned)(bit_offset % BIGINT_LIMB_SIZE);
  uint64_t value = (uint64_t)limbs[word] >> shift;

  if (word + 1 < count)
    value |= (uint64_t)limbs[word + 1] << (BIGINT_LIMB_SIZE - shift);
  if (shift != 0 && word + 2 < count)
    value |= (uint64_t)limbs[word + 2] << (64u - shift);
  return value;
}

static bool bigint_test_bit(const uint32_t *limbs, size_t count, size_t bit) {
  size_t word = bit / BIGINT_LIMB_SIZE;
  if (word >= count) return false;
  return ((limbs[word] >> (bit % BIGINT_LIMB_SIZE)) & 1u) != 0;
}

static bool bigint_any_bits_below(const uint32_t *limbs, size_t count, size_t bit_count) {
  size_t full_words = bit_count / BIGINT_LIMB_SIZE;
  for (size_t i = 0; i < full_words && i < count; i++) {
    if (limbs[i] != 0) return true;
  }

  unsigned remaining = (unsigned)(bit_count % BIGINT_LIMB_SIZE);
  if (remaining == 0 || full_words >= count) return false;
  
  uint32_t mask = (UINT32_C(1) << remaining) - 1u;
  return (limbs[full_words] & mask) != 0;
}

static bool bigint_should_round_up(
  const uint32_t *limbs, size_t count,
  size_t discarded_bits, uint64_t significand
) {
  size_t halfway_bit = discarded_bits - 1;
  if (!bigint_test_bit(limbs, count, halfway_bit)) return false;

  bool above_halfway = bigint_any_bits_below(limbs, count, halfway_bit);
  bool odd_significand = (significand & 1u) != 0;
  
  return above_halfway || odd_significand;
}

double bigint_to_double(ant_t *js, ant_value_t v) {
  size_t count;
  const uint32_t *limbs = bigint_limbs(js, v, &count);
  
  size_t bit_length = bigint_abs_bitlen_limbs(limbs, count);
  if (bit_length == 0) return 0.0;

  size_t discarded_bits = 
    bit_length > BIGINT_DOUBLE_PRECISION
    ? bit_length - BIGINT_DOUBLE_PRECISION : 0;
    
  uint64_t significand = bigint_extract_u64(limbs, count, discarded_bits);
  if (discarded_bits != 0 && bigint_should_round_up(limbs, count, discarded_bits, significand)) {
    significand++;
    if (significand == (UINT64_C(1) << BIGINT_DOUBLE_PRECISION)) {
      significand >>= 1;
      bit_length++;
    }
  }

  double result;
  if (bit_length <= BIGINT_DOUBLE_PRECISION) result = (double)significand;
  else if (bit_length > DBL_MAX_EXP) result = HUGE_VAL; else result = ldexp(
    (double)significand,
    (int)(bit_length - BIGINT_DOUBLE_PRECISION)
  );
  
  return bigint_is_negative(js, v) ? -result : result;
}

static ant_value_t js_mkbigint_limbs(ant_t *js, const uint32_t *limbs, size_t count, bool negative) {
  uint32_t zero = 0;

  if (!limbs || count == 0) {
    limbs = &zero;
    count = 1;
  }

  while (count > 1 && limbs[count - 1] == 0) count--;

  bigint_payload_t *payload = NULL;
  ant_value_t out = bigint_alloc_payload(js, count, &payload);
  if (is_err(out)) return out;

  memcpy(payload->limbs, limbs, count * sizeof(uint32_t));
  return bigint_finish_payload(out, payload, count, negative);
}

ant_value_t bigint_from_uint64(ant_t *js, uint64_t value) {
  uint32_t limbs[2] = {
    (uint32_t)(value & 0xffffffffu),
    (uint32_t)(value >> 32)
  };

  size_t count = limbs[1] == 0 ? 1 : 2;
  return js_mkbigint_limbs(js, limbs, count, false);
}

ant_value_t bigint_from_int64(ant_t *js, int64_t value) {
  uint64_t magnitude = value < 0
    ? (uint64_t)(-(value + 1)) + 1
    : (uint64_t)value;
  
  uint32_t limbs[2] = {
    (uint32_t)(magnitude & 0xffffffffu),
    (uint32_t)(magnitude >> 32)
  };

  size_t count = limbs[1] == 0 ? 1 : 2;
  return js_mkbigint_limbs(js, limbs, count, value < 0);
}

ant_value_t bigint_from_integral_double(ant_t *js, double value) {
  #define DOUBLE_LIMB_CAP 33
  
  uint64_t bits, mantissa;
  uint32_t limbs[DOUBLE_LIMB_CAP] = {0};
  uint32_t exp_bits;
  
  int exp, shift;
  bool negative;

  if (!isfinite(value) || floor(value) != value)
    return js_mkerr(js, "Cannot convert non-integral double to BigInt");

  negative = signbit(value);
  if (value == 0) return bigint_from_uint64(js, 0);

  memcpy(&bits, &value, sizeof(bits));
  exp_bits = (uint32_t)((bits >> 52) & 0x7ffu);
  if (exp_bits == 0) return bigint_from_uint64(js, 0);

  mantissa = (bits & 0x000fffffffffffffull) | (1ull << 52);
  exp = (int)exp_bits - 1023;

  if (exp < 52) {
    uint64_t magnitude = mantissa >> (52 - exp);
    return js_mkbigint_limbs(
      js, (uint32_t[]){
        (uint32_t)(magnitude & 0xffffffffu),
        (uint32_t)(magnitude >> 32)
      },
      magnitude > UINT32_MAX ? 2 : 1,
      negative
    );
  }

  shift = exp - 52;
  size_t word_shift = (size_t)shift / 32;
  unsigned bit_shift = (unsigned)shift % 32;
  
  uint32_t parts[2] = {
    (uint32_t)(mantissa & 0xffffffffu),
    (uint32_t)(mantissa >> 32)
  };

  for (size_t i = 0; i < 2; i++) {
    if (parts[i] == 0) continue;
    size_t dst = word_shift + i;
    if (dst >= DOUBLE_LIMB_CAP) return js_mkerr(js, "BigInt is too large");
    limbs[dst] |= parts[i] << bit_shift;
    if (bit_shift != 0) {
      if (dst + 1 >= DOUBLE_LIMB_CAP) return js_mkerr(js, "BigInt is too large");
      limbs[dst + 1] |= parts[i] >> (32u - bit_shift);
    }
  }

  size_t count = DOUBLE_LIMB_CAP;
  bigint_normalize_limbs(limbs, &count);
  
  return js_mkbigint_limbs(js, limbs, count, negative);
}

static uint64_t bigint_low_u64(ant_t *js, ant_value_t value) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);
  uint64_t out = count > 0 ? (uint64_t)limbs[0] : 0;
  if (count > 1) out |= ((uint64_t)limbs[1] << 32);
  return out;
}

bool bigint_to_uint64_wrapping(ant_t *js, ant_value_t value, uint64_t *out) {
  if (!out || vtype(value) != kTypeBigInt) return false;
  uint64_t low = bigint_low_u64(js, value);
  *out = bigint_is_negative(js, value) ? (uint64_t)(0 - low) : low;
  return true;
}

bool bigint_to_int64_wrapping(ant_t *js, ant_value_t value, int64_t *out) {
  if (!out) return false;
  uint64_t bits = 0;
  if (!bigint_to_uint64_wrapping(js, value, &bits)) return false;
  *out = (int64_t)bits;
  return true;
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

static size_t bigint_add_abs_limbs(
  const uint32_t *a,
  size_t alen,
  const uint32_t *b,
  size_t blen,
  uint32_t *result
) {
  size_t maxlen = alen > blen ? alen : blen;

  uint64_t carry = 0;
  for (size_t i = 0; i < maxlen; i++) {
    uint64_t da = i < alen ? a[i] : 0;
    uint64_t db = i < blen ? b[i] : 0;
    uint64_t sum = da + db + carry;
    result[i] = (uint32_t)sum;
    carry = sum >> 32;
  }

  result[maxlen] = (uint32_t)carry;
  return maxlen + (carry ? 1 : 0);
}

static size_t bigint_add_u32_inplace(
  uint32_t *result,
  size_t count,
  size_t capacity,
  uint32_t value
) {
  uint64_t carry = value;
  size_t i = 0;

  while (carry != 0 && i < count) {
    uint64_t sum = (uint64_t)result[i] + carry;
    result[i] = (uint32_t)sum;
    carry = sum >> 32;
    i++;
  }

  if (carry != 0 && count < capacity) result[count++] = (uint32_t)carry;
  bigint_normalize_limbs(result, &count);
  return count;
}

static size_t bigint_sub_abs_limbs(
  const uint32_t *a,
  size_t alen,
  const uint32_t *b,
  size_t blen,
  uint32_t *result
) {
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

  return alen;
}

static size_t bigint_mul_abs_limbs(
  const uint32_t *a,
  size_t alen,
  const uint32_t *b,
  size_t blen,
  uint32_t *result
) {
  if (alen == 1 && blen == 1) {
    uint64_t product = (uint64_t)a[0] * (uint64_t)b[0];
    result[0] = (uint32_t)product;
    result[1] = (uint32_t)(product >> 32);
    return result[1] == 0 ? 1 : 2;
  }

  size_t capacity = alen + blen + 1;
  memset(result, 0, capacity * sizeof(uint32_t));

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

  return capacity;
}

static size_t bigint_shift_left_abs(
  const uint32_t *limbs,
  size_t count,
  uint64_t shift,
  uint32_t *out
) {
  size_t limb_shift = (size_t)(shift >> 5);
  unsigned bit_shift = (unsigned)(shift & 31u);

  size_t out_count = count + limb_shift + 1;
  memset(out, 0, out_count * sizeof(uint32_t));

  if (bit_shift == 0) {
    memcpy(out + limb_shift, limbs, count * sizeof(uint32_t));
    out_count = count + limb_shift;
    bigint_normalize_limbs(out, &out_count);
    return out_count;
  }

  uint32_t carry = 0;
  for (size_t i = 0; i < count; i++) {
    uint64_t cur = ((uint64_t)limbs[i] << bit_shift) | carry;
    out[i + limb_shift] = (uint32_t)cur;
    carry = (uint32_t)(cur >> 32);
  }

  out[count + limb_shift] = carry;
  bigint_normalize_limbs(out, &out_count);
  return out_count;
}

static size_t bigint_shift_right_abs(
  const uint32_t *limbs,
  size_t count,
  uint64_t shift,
  uint32_t *out,
  bool *truncated
) {
  size_t limb_shift = (size_t)(shift >> 5);
  unsigned bit_shift = (unsigned)(shift & 31u);

  bool lost = false;

  if (limb_shift >= count) {
    for (size_t i = 0; i < count; i++) {
      if (limbs[i] != 0) { lost = true; break; }
    }

    out[0] = 0;
    if (truncated) *truncated = lost;
    return 1;
  }

  for (size_t i = 0; i < limb_shift; i++) {
    if (limbs[i] != 0) { lost = true; break; }
  }

  size_t out_count = count - limb_shift;

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

  bigint_normalize_limbs(out, &out_count);
  if (truncated) *truncated = lost;
  return out_count;
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

static uint32_t bigint_twos_complement_limb(
  const uint32_t *limbs,
  size_t count,
  bool negative,
  size_t index,
  uint64_t *carry
) {
  uint32_t limb = index < count ? limbs[index] : 0;
  if (!negative) return limb;
  uint64_t cur = (uint64_t)(~limb) + *carry;
  *carry = cur >> 32;
  return (uint32_t)cur;
}

static size_t bigint_twos_complement_to_magnitude(
  uint32_t *limbs,
  size_t width,
  bool *negative_out
) {
  if (width == 0) width = 1;

  bool negative = (limbs[width - 1] & 0x80000000u) != 0;

  if (negative) {
    uint64_t carry = 1;
    for (size_t i = 0; i < width; i++) {
      uint64_t cur = (uint64_t)(~limbs[i]) + carry;
      limbs[i] = (uint32_t)cur;
      carry = cur >> 32;
    }
  }

  bigint_normalize_limbs(limbs, &width);
  if (width == 1 && limbs[0] == 0) negative = false;

  if (negative_out) *negative_out = negative;
  return width;
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

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (width > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_binary_payload(js, a, b, width, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    ad = bigint_limbs(js, a, &alen);
    bd = bigint_limbs(js, b, &blen);
  }

  uint64_t acarry = 1;
  uint64_t bcarry = 1;
  
  for (size_t i = 0; i < width; i++) {
    uint32_t at = bigint_twos_complement_limb(ad, alen, aneg, i, &acarry);
    uint32_t bt = bigint_twos_complement_limb(bd, blen, bneg, i, &bcarry);
    switch (op) {
      case BIGINT_BAND: result[i] = at & bt; break;
      case BIGINT_BOR:  result[i] = at | bt; break;
      case BIGINT_BXOR: result[i] = at ^ bt; break;
    }
  }

  bool negative = false;
  size_t count = bigint_twos_complement_to_magnitude(result, width, &negative);
  return payload
    ? bigint_finish_payload(out, payload, count, negative)
    : js_mkbigint_limbs(js, result, count, negative);
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

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (width > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_unary_payload(js, value, width, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    limbs = bigint_limbs(js, value, &count);
  }

  uint64_t carry = 1;
  for (size_t i = 0; i < width; i++)
    result[i] = ~bigint_twos_complement_limb(limbs, count, neg, i, &carry);

  bool out_neg = false;
  size_t out_count = bigint_twos_complement_to_magnitude(result, width, &out_neg);
  return payload
    ? bigint_finish_payload(out, payload, out_count, out_neg)
    : js_mkbigint_limbs(js, result, out_count, out_neg);
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

  size_t capacity = 0;
  size_t rlen = 0;
  bool rneg = false;
  int cmp = 0;

  if (aneg == bneg) {
    capacity = (alen > blen ? alen : blen) + 1;
    rneg = aneg;
  } else {
    cmp = bigint_cmp_abs_limbs(ad, alen, bd, blen);
    if (cmp >= 0) {
      capacity = alen;
      rneg = aneg;
    } else {
      capacity = blen;
      rneg = bneg;
    }
  }

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (capacity > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_binary_payload(js, a, b, capacity, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    ad = bigint_limbs(js, a, &alen);
    bd = bigint_limbs(js, b, &blen);
  }

  if (aneg == bneg)
    rlen = bigint_add_abs_limbs(ad, alen, bd, blen, result);
  else if (cmp >= 0)
    rlen = bigint_sub_abs_limbs(ad, alen, bd, blen, result);
  else
    rlen = bigint_sub_abs_limbs(bd, blen, ad, alen, result);

  return payload
    ? bigint_finish_payload(out, payload, rlen, rneg)
    : js_mkbigint_limbs(js, result, rlen, rneg);
}

ant_value_t bigint_sub(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  size_t capacity = 0;
  size_t rlen = 0;
  bool rneg = false;
  int cmp = 0;

  if (aneg != bneg) {
    capacity = (alen > blen ? alen : blen) + 1;
    rneg = aneg;
  } else {
    cmp = bigint_cmp_abs_limbs(ad, alen, bd, blen);
    if (cmp >= 0) {
      capacity = alen;
      rneg = aneg;
    } else {
      capacity = blen;
      rneg = !aneg;
    }
  }

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (capacity > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_binary_payload(js, a, b, capacity, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    ad = bigint_limbs(js, a, &alen);
    bd = bigint_limbs(js, b, &blen);
  }

  if (aneg != bneg)
    rlen = bigint_add_abs_limbs(ad, alen, bd, blen, result);
  else if (cmp >= 0)
    rlen = bigint_sub_abs_limbs(ad, alen, bd, blen, result);
  else
    rlen = bigint_sub_abs_limbs(bd, blen, ad, alen, result);

  return payload
    ? bigint_finish_payload(out, payload, rlen, rneg)
    : js_mkbigint_limbs(js, result, rlen, rneg);
}

ant_value_t bigint_mul(ant_t *js, ant_value_t a, ant_value_t b) {
  bool aneg = bigint_is_negative(js, a);
  bool bneg = bigint_is_negative(js, b);

  size_t alen = 0, blen = 0;
  const uint32_t *ad = bigint_limbs(js, a, &alen);
  const uint32_t *bd = bigint_limbs(js, b, &blen);

  size_t capacity = alen + blen + 1;
  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (capacity > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_binary_payload(js, a, b, capacity, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    ad = bigint_limbs(js, a, &alen);
    bd = bigint_limbs(js, b, &blen);
  }

  size_t rlen = bigint_mul_abs_limbs(ad, alen, bd, blen, result);
  return payload
    ? bigint_finish_payload(out, payload, rlen, aneg != bneg)
    : js_mkbigint_limbs(js, result, rlen, aneg != bneg);
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

  bigint_payload_t *payload = NULL;
  ant_value_t out = bigint_alloc_unary_payload(js, a, len, &payload);
  if (is_err(out)) return out;

  limbs = bigint_limbs(js, a, &len);
  memcpy(payload->limbs, limbs, len * sizeof(uint32_t));
  return bigint_finish_payload(out, payload, len, !neg);
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

  if (limbs_is_zero(limbs, count)) return value;

  uint64_t limb_shift_u64 = shift >> 5;
  if (limb_shift_u64 > SIZE_MAX - count - 1u) return js_mkerr(js, "oom");
  size_t capacity = count + (size_t)limb_shift_u64 + 1u;
  bool negative = bigint_is_negative(js, value);

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (capacity > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_unary_payload(js, value, capacity, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    limbs = bigint_limbs(js, value, &count);
  }

  size_t rlen = bigint_shift_left_abs(limbs, count, shift, result);
  return payload
    ? bigint_finish_payload(out, payload, rlen, negative)
    : js_mkbigint_limbs(js, result, rlen, negative);
}

ant_value_t bigint_shift_right(ant_t *js, ant_value_t value, uint64_t shift) {
  if (shift == 0) return value;

  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(js, value, &count);

  if (limbs_is_zero(limbs, count)) return value;

  bool neg = bigint_is_negative(js, value);
  bool truncated = false;

  uint64_t limb_shift_u64 = shift >> 5;
  size_t capacity = limb_shift_u64 >= count
    ? 1u
    : count - (size_t)limb_shift_u64 + (neg ? 1u : 0u);

  uint32_t stack_limbs[BIGINT_LIMB_SIZE];
  bigint_payload_t *payload = NULL;
  ant_value_t out = js_mkundef();
  uint32_t *result = stack_limbs;

  if (capacity > BIGINT_LIMB_SIZE) {
    out = bigint_alloc_unary_payload(js, value, capacity, &payload);
    if (is_err(out)) return out;
    result = payload->limbs;
    limbs = bigint_limbs(js, value, &count);
  }

  size_t qlen = bigint_shift_right_abs(limbs, count, shift, result, &truncated);
  if (neg && truncated)
    qlen = bigint_add_u32_inplace(result, qlen, capacity, 1);

  bool negative = neg && !limbs_is_zero(result, qlen);
  return payload
    ? bigint_finish_payload(out, payload, qlen, negative)
    : js_mkbigint_limbs(js, result, qlen, negative);
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

ant_value_t bigint_from_value(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == kTypeBigInt) return arg;

  if (vtype(arg) == kTypeNumber) {
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

  if (vtype(arg) == kTypeString) {
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

  if (vtype(arg) == kTypeBool) return js_mkbigint(js, vdata(arg) ? "1" : "0", 1, false);

  return js_mkerr(js, "Cannot convert to BigInt");
}

static ant_value_t builtin_BigInt(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) != kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt is not a constructor");
  if (nargs < 1) return js_mkbigint(js, "0", 1, false);

  return bigint_from_value(js, args[0]);
}

static ant_value_t bigint_to_u64(ant_t *js, ant_value_t value, uint64_t *out) {
  if (!bigint_parse_u64(js, value, out)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }
  return js_mkundef();
}

ant_value_t bigint_asint_bits(ant_t *js, ant_value_t arg, uint64_t *bits_out) {
  if (vtype(arg) == kTypeBigInt) return bigint_to_u64(js, arg, bits_out);

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

  if (vtype(args[1]) != kTypeBigInt) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
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

  if (vtype(args[1]) != kTypeBigInt) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
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
  if (vtype(val) != kTypeBigInt) {
    val = unwrap_primitive(js, val);
    if (vtype(val) != kTypeBigInt) return js_mkerr(js, "toString called on non-BigInt");
  }

  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
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

static ant_value_t builtin_bigint_valueOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t val = js->this_val;
  if (vtype(val) != kTypeBigInt) {
    val = unwrap_primitive(js, val);
    if (vtype(val) != kTypeBigInt) return js_mkerr(js, "valueOf called on non-BigInt");
  }
  return val;
}

void init_bigint_module(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js->sym.function_proto;

  ant_value_t bigint_proto = js_mkobj(js);
  js->sym.bigint_proto = bigint_proto;
  js_set_proto_init(bigint_proto, object_proto);
  defmethod(js, bigint_proto, "toString", 8, js_mkfun(builtin_bigint_toString));
  defmethod(js, bigint_proto, "valueOf", 7, js_mkfun(builtin_bigint_valueOf));

  ant_value_t bigint_ctor_obj = mkobj(js, 0);
  js_set_proto_init(bigint_ctor_obj, function_proto);
  js_set_slot(bigint_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_BigInt));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  js_setprop_nonconfigurable(js, bigint_ctor_obj, "prototype", 9, bigint_proto);
  js_setprop(js, bigint_ctor_obj, ANT_STRING("name"), ANT_STRING("BigInt"));
  js_set_global_builtin(js, "BigInt", js_obj_to_func(js, bigint_ctor_obj));
}
