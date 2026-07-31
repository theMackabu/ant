#ifndef ANT_HASH_H
#define ANT_HASH_H

// Ported from Node.js `src/node_hash.h` (HashBytes), which is itself based on
// rapidhash V3 with the mixing primitive from wyhash. Kept as a port rather than a
// vendored header for the same reasons Node did it: upstream rapidhash ships three
// variants behind a matrix of feature macros, this needs exactly one function, and the
// 32-bit multiply fallback below has no upstream equivalent.
//
//   Node.js     https://github.com/nodejs/node/blob/main/src/node_hash.h
//               Copyright Node.js contributors - MIT License
//   rapidhash   https://github.com/Nicoshev/rapidhash
//               Copyright (C) 2025 Nicolas De Carli - MIT License
//   wyhash      https://github.com/wangyi-fudan/wyhash
//               Wang Yi - public domain (The Unlicense)
//

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_X64) && !defined(_M_ARM64EC)
#pragma intrinsic(_umul128)
#endif
#endif

static inline uint64_t ant_hash_mix(uint64_t a, uint64_t b) {
#ifdef __SIZEOF_INT128__
  __uint128_t r = (__uint128_t)a * b;
  a = (uint64_t)r;
  b = (uint64_t)(r >> 64);
#elif defined(_MSC_VER) && (defined(_WIN64) || defined(_M_HYBRID_CHPE_ARM64))
#if defined(_M_X64)
  a = _umul128(a, b, &b);
#else
  uint64_t hi = __umulh(a, b);
  a = a * b;
  b = hi;
#endif
#else
  uint64_t ha = a >> 32, hb = b >> 32;
  uint64_t la = (uint32_t)a, lb = (uint32_t)b;
  uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
  uint64_t t = rl + (rm0 << 32);
  uint64_t lo = t + (rm1 << 32);
  uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + (t < rl) + (lo < t);
  a = lo;
  b = hi;
#endif
  return a ^ b;
}

static inline void ant_hash_mum(uint64_t *a, uint64_t *b) {
#ifdef __SIZEOF_INT128__
  __uint128_t r = (__uint128_t)(*a) * (*b);
  *a = (uint64_t)r;
  *b = (uint64_t)(r >> 64);
#elif defined(_MSC_VER) && (defined(_WIN64) || defined(_M_HYBRID_CHPE_ARM64))
#if defined(_M_X64)
  *a = _umul128(*a, *b, b);
#else
  uint64_t hi = __umulh(*a, *b);
  *a = (*a) * (*b);
  *b = hi;
#endif
#else
  uint64_t ha = *a >> 32, hb = *b >> 32;
  uint64_t la = (uint32_t)(*a), lb = (uint32_t)(*b);
  uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
  uint64_t t = rl + (rm0 << 32);
  *a = t + (rm1 << 32);
  *b = rh + (rm0 >> 32) + (rm1 >> 32) + (t < rl) + (*a < t);
#endif
}

static inline uint64_t ant_hash_read64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline uint64_t ant_hash_read32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static const uint64_t ant_hash_secret[8] = {
  0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL,
  0x4b33a62ed433d4a3ULL, 0x4d5a2da51de1aa47ULL,
  0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL,
  0x90ed1765281c388cULL, 0xaaaaaaaaaaaaaaaaULL,
};

static inline uint64_t hash_key(const char *key, size_t len) {
  const uint8_t *p = (const uint8_t *)key;

  uint64_t seed = ant_hash_mix(ant_hash_secret[2], ant_hash_secret[1]);
  uint64_t a = 0, b = 0;
  size_t i = len;

  if (len <= 16) {
    if (len >= 4) {
      seed ^= len;
      if (len >= 8) {
        a = ant_hash_read64(p);
        b = ant_hash_read64(p + len - 8);
      } else {
        a = ant_hash_read32(p);
        b = ant_hash_read32(p + len - 4);
      }
    } else if (len > 0) {
      a = ((uint64_t)p[0] << 45) | p[len - 1];
      b = p[len >> 1];
    }
  } else if (len <= 48) {
    seed = ant_hash_mix(ant_hash_read64(p) ^ ant_hash_secret[2], ant_hash_read64(p + 8) ^ seed);
    if (len > 32) seed = ant_hash_mix(ant_hash_read64(p + 16) ^ ant_hash_secret[2], ant_hash_read64(p + 24) ^ seed);
    a = ant_hash_read64(p + len - 16) ^ len;
    b = ant_hash_read64(p + len - 8);
  } else {
    uint64_t see1 = seed, see2 = seed;
    do {
      seed = ant_hash_mix(ant_hash_read64(p) ^ ant_hash_secret[0], ant_hash_read64(p + 8) ^ seed);
      see1 = ant_hash_mix(ant_hash_read64(p + 16) ^ ant_hash_secret[1], ant_hash_read64(p + 24) ^ see1);
      see2 = ant_hash_mix(ant_hash_read64(p + 32) ^ ant_hash_secret[2], ant_hash_read64(p + 40) ^ see2);
      p += 48;
      i -= 48;
    } while (i > 48);

    seed ^= see1 ^ see2;

    if (i > 16) {
      seed = ant_hash_mix(ant_hash_read64(p) ^ ant_hash_secret[2], ant_hash_read64(p + 8) ^ seed);
      if (i > 32) seed = ant_hash_mix(ant_hash_read64(p + 16) ^ ant_hash_secret[2], ant_hash_read64(p + 24) ^ seed);
    }

    a = ant_hash_read64(p + i - 16) ^ i;
    b = ant_hash_read64(p + i - 8);
  }

  a ^= ant_hash_secret[1];
  b ^= seed;
  ant_hash_mum(&a, &b);

  return ant_hash_mix(a ^ ant_hash_secret[7], b ^ ant_hash_secret[1] ^ len);
}

#endif
