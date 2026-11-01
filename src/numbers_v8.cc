// Copyright 2011 the V8 project authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Number.prototype.toString(radix) and parseInt, adapted from V8's
// src/numbers/conversions.cc (DoubleToRadixStringView, StringToIntHelper,
// NumberParseIntHelper, InternalStringToIntDouble) for UTF-8 input.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "numbers.h"
#include "double-conversion/strtod.h"

namespace {

inline double JunkStringValue() {
  return std::numeric_limits<double>::quiet_NaN();
}

inline double SignedZero(bool negative) {
  return negative ? -0.0 : 0.0;
}

inline bool isDigit(int x, int radix) {
  return (x >= '0' && x <= '9' && x < '0' + radix) ||
         (radix > 10 && x >= 'a' && x < 'a' + radix - 10) ||
         (radix > 10 && x >= 'A' && x < 'A' + radix - 10);
}

inline double NextDouble(double value) {
  return std::nextafter(value, std::numeric_limits<double>::infinity());
}

inline bool ExponentIsPositive(double value) {
  return value >= 9007199254740992.0;
}

template <int radix_log_2>
double InternalStringToIntDouble(const char* current, const char* end) {
  while (*current == '0') {
    ++current;
    if (current == end) return 0.0;
  }

  int64_t number = 0;
  int exponent = 0;
  constexpr int radix = (1 << radix_log_2);

  constexpr int lim_0 = '0' + (radix < 10 ? radix : 10);
  constexpr int lim_a = 'a' + (radix - 10);
  constexpr int lim_A = 'A' + (radix - 10);

  do {
    int digit;
    if (*current >= '0' && *current < lim_0) {
      digit = static_cast<char>(*current) - '0';
    } else if (*current >= 'a' && *current < lim_a) {
      digit = static_cast<char>(*current) - 'a' + 10;
    } else if (*current >= 'A' && *current < lim_A) {
      digit = static_cast<char>(*current) - 'A' + 10;
    } else {
      break;
    }

    number = number * radix + digit;
    int overflow = static_cast<int>(number >> 53);
    if (overflow != 0) {
      int overflow_bits_count = 1;
      while (overflow > 1) {
        overflow_bits_count++;
        overflow >>= 1;
      }

      int dropped_bits_mask = ((1 << overflow_bits_count) - 1);
      int dropped_bits = static_cast<int>(number) & dropped_bits_mask;
      number >>= overflow_bits_count;
      exponent = overflow_bits_count;

      constexpr int kMaxExponent = std::numeric_limits<double>::max_exponent;
      bool zero_tail = true;
      while (true) {
        ++current;
        if (current == end || !isDigit(*current, radix)) break;
        zero_tail = zero_tail && *current == '0';
        if (exponent <= kMaxExponent) exponent += radix_log_2;
      }

      int middle_value = (1 << (overflow_bits_count - 1));
      if (dropped_bits > middle_value) {
        number++;
      } else if (dropped_bits == middle_value) {
        if ((number & 1) != 0 || !zero_tail) {
          number++;
        }
      }

      if ((number & (static_cast<int64_t>(1) << 53)) != 0) {
        exponent++;
        number >>= 1;
      }
      break;
    }
    ++current;
  } while (current != end);

  if (exponent == 0) return static_cast<double>(number);
  return std::ldexp(static_cast<double>(number), exponent);
}

double HandlePowerOfTwoCase(const char* current, const char* end, int radix) {
  switch (radix) {
    case 2: return InternalStringToIntDouble<1>(current, end);
    case 4: return InternalStringToIntDouble<2>(current, end);
    case 8: return InternalStringToIntDouble<3>(current, end);
    case 16: return InternalStringToIntDouble<4>(current, end);
    default: return InternalStringToIntDouble<5>(current, end);
  }
}

double HandleBaseTenCase(const char* current, const char* end) {
  constexpr size_t kMaxSignificantDigits = 309;
  constexpr size_t kBufferSize = kMaxSignificantDigits + 2;
  constexpr size_t kMaxExactDigits = 15;
  char buffer[kBufferSize];
  size_t buffer_pos = 0;
  uint64_t exact = 0;
  while (*current >= '0' && *current <= '9') {
    if (buffer_pos < kMaxExactDigits) exact = exact * 10 + static_cast<uint64_t>(*current - '0');
    if (buffer_pos <= kMaxSignificantDigits) {
      buffer[buffer_pos++] = static_cast<char>(*current);
    }
    ++current;
    if (current == end) break;
  }

  if (buffer_pos <= kMaxExactDigits) return static_cast<double>(exact);
  buffer[buffer_pos] = '\0';
  return double_conversion::Strtod(
    double_conversion::Vector<const char>(buffer, static_cast<int>(buffer_pos)), 0);
}

double HandleGenericCase(const char* current, const char* end, int radix) {
  int lim_0 = '0' + (radix < 10 ? radix : 10);
  int lim_a = 'a' + (radix - 10);
  int lim_A = 'A' + (radix - 10);

  double result = 0;
  bool done = false;
  do {
    uint32_t part = 0, multiplier = 1;
    while (true) {
      uint32_t d;
      if (*current >= '0' && *current < lim_0) {
        d = *current - '0';
      } else if (*current >= 'a' && *current < lim_a) {
        d = *current - 'a' + 10;
      } else if (*current >= 'A' && *current < lim_A) {
        d = *current - 'A' + 10;
      } else {
        done = true;
        break;
      }

      const uint32_t kMaximumMultiplier = 0xFFFFFFFFu / 36;
      uint32_t m = multiplier * static_cast<uint32_t>(radix);
      if (m > kMaximumMultiplier) break;
      part = part * radix + d;
      multiplier = m;

      ++current;
      if (current == end) {
        done = true;
        break;
      }
    }
    result = result * multiplier + part;
  } while (!done);

  return result;
}

}  // namespace

extern "C" double ant_number_parse_int(const char* current, size_t length, int radix) {
  const char* end = current + length;

  while (current < end && (static_cast<unsigned char>(*current) <= ' ' || static_cast<unsigned char>(*current) >= 0x80)) {
    size_t n = ant_js_trim_prefix_len(current, static_cast<size_t>(end - current));
    if (n == 0) break;
    current += n;
  }
  if (current == end) return JunkStringValue();

  bool negative = false;
  if (*current == '+') {
    ++current;
    if (current == end) return JunkStringValue();
  } else if (*current == '-') {
    ++current;
    if (current == end) return JunkStringValue();
    negative = true;
  }

  bool leading_zero = false;
  if (radix == 0) {
    radix = 10;
    if (*current == '0') {
      ++current;
      if (current == end) return SignedZero(negative);
      if (*current == 'x' || *current == 'X') {
        radix = 16;
        ++current;
        if (current == end) return JunkStringValue();
      } else {
        leading_zero = true;
      }
    }
  } else if (radix == 16) {
    if (*current == '0') {
      ++current;
      if (current == end) return SignedZero(negative);
      if (*current == 'x' || *current == 'X') {
        ++current;
        if (current == end) return JunkStringValue();
      } else {
        leading_zero = true;
      }
    }
  }

  while (*current == '0') {
    leading_zero = true;
    ++current;
    if (current == end) return SignedZero(negative);
  }

  if (leading_zero && !isDigit(*current, radix)) return SignedZero(negative);
  if (!leading_zero && !isDigit(*current, radix)) return JunkStringValue();

  double result;
  if (radix == 10) result = HandleBaseTenCase(current, end);
  else if ((radix & (radix - 1)) == 0) result = HandlePowerOfTwoCase(current, end, radix);
  else result = HandleGenericCase(current, end, radix);

  return negative ? -result : result;
}

extern "C" size_t ant_number_to_radix(double value, int radix, char* buffer, size_t size, const char** out_start) {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";

  size_t integer_cursor = size / 2;
  size_t fraction_cursor = integer_cursor;

  bool negative = value < 0;
  if (negative) value = -value;

  double integer = std::floor(value);
  double fraction = value - integer;
  double delta = 0.5 * (NextDouble(value) - value);
  delta = std::max(NextDouble(0.0), delta);
  if (fraction >= delta) {
    buffer[fraction_cursor++] = '.';
    do {
      fraction *= radix;
      delta *= radix;
      int digit = static_cast<int>(fraction);
      buffer[fraction_cursor++] = chars[digit];
      fraction -= digit;
      if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
        if (fraction + delta > 1) {
          while (true) {
            fraction_cursor--;
            if (fraction_cursor == size / 2) {
              integer += 1;
              break;
            }
            char c = buffer[fraction_cursor];
            digit = c > '9' ? (c - 'a' + 10) : (c - '0');
            if (digit + 1 < radix) {
              buffer[fraction_cursor++] = chars[digit + 1];
              break;
            }
          }
          break;
        }
      }
    } while (fraction >= delta);
  }

  while (ExponentIsPositive(integer / radix)) {
    integer /= radix;
    buffer[--integer_cursor] = '0';
  }
  do {
    double remainder = std::fmod(integer, radix);
    buffer[--integer_cursor] = chars[static_cast<int>(remainder)];
    integer = (integer - remainder) / radix;
  } while (integer > 0);

  if (negative) buffer[--integer_cursor] = '-';
  *out_start = buffer + integer_cursor;
  return fraction_cursor - integer_cursor;
}
