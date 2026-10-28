#include "utf8.h"
#include "utils.h"
#include "gc/strings.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static inline void utf16_scan_decode(
  const unsigned char *p,
  const unsigned char *end,
  size_t *slen_out,
  size_t *units_out,
  uint32_t *cp_out
) {
  unsigned char c = *p;
  if (c < 0x80) {
    if (cp_out) *cp_out = c;
    *slen_out = 1;
    *units_out = 1;
    return;
  }

  if ((c & 0xE0) == 0xC0) {
    if (cp_out && p + 1 < end) {
      *cp_out = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
      *slen_out = 2;
      *units_out = 1;
      return;
    }
    if (!cp_out) {
      *slen_out = 2;
      *units_out = 1;
      return;
    }
  } else if ((c & 0xF0) == 0xE0) {
    if (cp_out && p + 2 < end) {
      *cp_out = ((uint32_t)(c & 0x0F) << 12)
        | ((uint32_t)(p[1] & 0x3F) << 6)
        | (uint32_t)(p[2] & 0x3F);
      *slen_out = 3;
      *units_out = 1;
      return;
    }
    if (!cp_out) {
      *slen_out = 3;
      *units_out = 1;
      return;
    }
  } else if ((c & 0xF8) == 0xF0) {
    if (cp_out && p + 3 < end) {
      *cp_out = ((uint32_t)(c & 0x07) << 18)
        | ((uint32_t)(p[1] & 0x3F) << 12)
        | ((uint32_t)(p[2] & 0x3F) << 6)
        | (uint32_t)(p[3] & 0x3F);
      *slen_out = 4;
      *units_out = 2;
      return;
    }
    if (!cp_out) {
      *slen_out = 4;
      *units_out = 2;
      return;
    }
  }

  if (cp_out) *cp_out = c;
  *slen_out = 1;
  *units_out = 1;
}

static const uint8_t utf16_lead_info[32] = {
  9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
  9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 20, 9,
};

static inline size_t utf16_lead_len(unsigned char c) {
  return utf16_lead_info[c >> 3] & 7;
}

static inline size_t utf16_lead_units(unsigned char c) {
  return utf16_lead_info[c >> 3] >> 3;
}

static constexpr size_t UTF16_POS_WAYS = 8;
static constexpr size_t UTF16_INDEX_CHUNK = 64;
static constexpr size_t UTF16_INDEX_MIN_BYTES = 256;

typedef struct {
  uint32_t byte_off;
  uint32_t utf16_pos;
} utf16_index_point_t;

struct utf16_pos_slot {
  const char *str;
  size_t len;
  size_t byte;
  size_t units;
  uint64_t stamp;
  utf16_index_point_t *points;
  size_t point_count;
  bool index_wanted;
  bool truncated_tail;
};

static _Thread_local struct {
  uint64_t epoch;
  uint64_t clock;
  utf16_pos_slot_t *mru;
  utf16_pos_slot_t slots[UTF16_POS_WAYS];
} utf16_pos_cache;

static __attribute__((noinline, cold)) void utf16_pos_cache_reset(uint64_t epoch) {
  for (size_t i = 0; i < UTF16_POS_WAYS; i++) free(utf16_pos_cache.slots[i].points);
  memset(&utf16_pos_cache, 0, sizeof(utf16_pos_cache));
  utf16_pos_cache.epoch = epoch;
}

static inline void utf16_pos_cache_sync(void) {
  uint64_t epoch = gc_strings_sweep_epoch();
  if (utf16_pos_cache.epoch != epoch) utf16_pos_cache_reset(epoch);
}

void utf8_thread_cleanup(void) {
  utf16_pos_cache_reset(0);
}

static bool utf16_has_truncated_tail(const char *str, size_t len) {
  const unsigned char *s = (const unsigned char *)str;
  for (size_t back = 1; back <= 3 && back <= len; back++)
    if (utf16_lead_len(s[len - back]) > back) return true;
  return false;
}

static __attribute__((noinline)) utf16_pos_slot_t *utf16_pos_slot_lookup(const char *str, size_t len) {
  utf16_pos_slot_t *slot = utf16_pos_cache.mru;

  if (!slot || slot->str != str) {
    utf16_pos_slot_t *victim = &utf16_pos_cache.slots[0];
    slot = NULL;

    for (size_t i = 0; i < UTF16_POS_WAYS; i++) {
      utf16_pos_slot_t *candidate = &utf16_pos_cache.slots[i];
      if (candidate->str == str) {
        slot = candidate;
        break;
      }
      if (candidate->stamp < victim->stamp) victim = candidate;
    }

    if (!slot) {
      free(victim->points);
      *victim = (utf16_pos_slot_t){ .str = str, .len = len, .truncated_tail = utf16_has_truncated_tail(str, len) };
      slot = victim;
    }

    slot->stamp = ++utf16_pos_cache.clock;
    utf16_pos_cache.mru = slot;
  }

  if (slot->len < len) {
    free(slot->points);
    slot->points = NULL;
    slot->point_count = 0;
    slot->len = len;
    slot->truncated_tail = utf16_has_truncated_tail(str, len);
  }

  return slot;
}

static inline void utf16_pos_open(utf16_pos_t *pos, const char *str, size_t len) {
  utf16_pos_cache_sync();

  utf16_pos_slot_t *slot = utf16_pos_cache.mru;
  if (__builtin_expect(!slot || slot->str != str, 0))
    slot = utf16_pos_slot_lookup(str, len);

  bool resume = slot->byte <= len;
  pos->str = str;
  pos->len = len;
  pos->byte = resume ? slot->byte : 0;
  pos->units = resume ? slot->units : 0;
  pos->slot = slot;
  pos->ascii = false;
  pos->truncated_tail = slot->truncated_tail;
}

static __attribute__((noinline)) utf16_pos_slot_t *utf16_pos_relookup(utf16_pos_t *pos) {
  utf16_pos_cache_sync();
  pos->slot = utf16_pos_slot_lookup(pos->str, pos->len);
  return pos->slot;
}

static inline void utf16_pos_store(const utf16_pos_t *pos) {
  pos->slot->byte = pos->byte;
  pos->slot->units = pos->units;
}

static inline void utf16_pos_close(utf16_pos_t *pos) {
  utf16_pos_slot_t *slot = pos->slot;
  if (__builtin_expect(!slot || slot->str != pos->str, 0)) slot = utf16_pos_relookup(pos);
  slot->byte = pos->byte;
  slot->units = pos->units;
}

void utf16_pos_init_slow(utf16_pos_t *pos, const char *str, size_t len) {
  utf16_pos_open(pos, str, len);
}

void utf16_pos_save_slow(utf16_pos_t *pos) {
  utf16_pos_close(pos);
}

typedef struct {
  size_t byte;
  size_t units;
} utf16_pos_mark_t;

static inline bool utf16_pos_backward_safe(const char *str) {
  return str_is_valid_wtf8(str);
}

static __attribute__((noinline, cold))
bool utf16_index_build(utf16_pos_slot_t *slot) {
  size_t max_points = slot->len / UTF16_INDEX_CHUNK + 2;
  utf16_index_point_t *points = malloc(max_points * sizeof(*points));
  if (!points) return false;

  const unsigned char *start = (const unsigned char *)slot->str;
  const unsigned char *end = start + slot->len;
  const unsigned char *p = start;

  size_t utf16_pos = 0;
  size_t point_count = 0;
  points[point_count++] = (utf16_index_point_t){0, 0};

  while (p < end) {
    size_t slen = utf16_lead_len(*p), units = utf16_lead_units(*p);

    if (
      utf16_pos + units >
      point_count * UTF16_INDEX_CHUNK
    ) points[point_count++] = (utf16_index_point_t){
      .byte_off = (uint32_t)(p - start),
      .utf16_pos = (uint32_t)utf16_pos,
    };

    utf16_pos += units;
    p += slen;
  }

  slot->points = points;
  slot->point_count = point_count;
  return true;
}

static const utf16_pos_slot_t *utf16_pos_index(utf16_pos_slot_t *slot, const char *str) {
  if (!slot || slot->str != str) return NULL;
  if (slot->len < UTF16_INDEX_MIN_BYTES || slot->len > UINT32_MAX) return NULL;
  if (slot->points) return slot;

  if (!slot->index_wanted) {
    slot->index_wanted = true;
    return NULL;
  }

  return utf16_index_build(slot) ? slot : NULL;
}

static inline utf16_pos_mark_t utf16_pos_jump(
  utf16_pos_mark_t mark, const utf16_index_point_t *point, size_t len
) {
  if (point->utf16_pos <= mark.units || point->byte_off > len) return mark;
  return (utf16_pos_mark_t){ .byte = point->byte_off, .units = point->utf16_pos };
}

static __attribute__((noinline)) utf16_pos_mark_t utf16_pos_reposition(
  const char *str, size_t len, utf16_pos_slot_t *slot,
  size_t byte, size_t units, size_t target
) {
  const unsigned char *s = (const unsigned char *)str;

  if (target < units) {
    bool walk = units - target <= target && utf16_pos_backward_safe(str);
    if (walk && units - target > UTF16_INDEX_CHUNK && utf16_pos_index(slot, str)) walk = false;
    if (walk) {
      while (units > target && byte > 0) {
        byte--;
        while (byte > 0 && (s[byte] & 0xC0) == 0x80) byte--;
        units -= utf16_units_for_utf8_byte(s[byte]);
      }
    } else {
      byte = 0;
      units = 0;
    }
  }

  utf16_pos_mark_t mark = { .byte = byte, .units = units };
  if (target - units <= UTF16_INDEX_CHUNK) return mark;

  const ant_flat_string_t *flat = str_flat_from_bytes(str);
  if ((size_t)flat->len == len) {
    ant_offset_t known = str_flat_cached_utf16_len(flat);
    if (known != STR_UTF16_LEN_UNKNOWN && target >= (size_t)known && str_is_valid_wtf8(str))
      return (utf16_pos_mark_t){ .byte = len, .units = (size_t)known };
  }

  const utf16_pos_slot_t *index = utf16_pos_index(slot, str);
  if (!index) return mark;

  size_t point = target / UTF16_INDEX_CHUNK;
  if (point >= index->point_count) point = index->point_count - 1;
  while (point > 0 && index->points[point].utf16_pos > target) point--;

  return utf16_pos_jump(mark, &index->points[point], len);
}

static __attribute__((noinline)) utf16_pos_mark_t utf16_pos_reposition_byte(
  const char *str, size_t len, utf16_pos_slot_t *slot,
  size_t byte, size_t units, size_t target, bool *done
) {
  *done = false;

  if (target < byte) {
    bool walk = byte - target <= target && utf16_pos_backward_safe(str);
    if (walk && byte - target > UTF16_INDEX_CHUNK * 4 && utf16_pos_index(slot, str)) walk = false;
    if (walk) {
      *done = true;
      return (utf16_pos_mark_t){
        .byte = target,
        .units = units - utf16_strlen(str + target, byte - target),
      };
    }
    byte = 0;
    units = 0;
  }

  utf16_pos_mark_t mark = { .byte = byte, .units = units };
  if (target - byte <= UTF16_INDEX_CHUNK * 4) return mark;

  const utf16_pos_slot_t *index = utf16_pos_index(slot, str);
  if (!index) return mark;

  size_t lo = 0, hi = index->point_count;
  while (hi - lo > 1) {
    size_t mid = lo + (hi - lo) / 2;
    if (index->points[mid].byte_off <= target) lo = mid;
    else hi = mid;
  }

  return utf16_pos_jump(mark, &index->points[lo], len);
}

static __attribute__((noinline)) void utf16_pos_note_end(const char *str, size_t len, size_t units) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  if ((size_t)flat->len != len) return;
  if (str_flat_cached_utf16_len(flat) == STR_UTF16_LEN_UNKNOWN)
    str_flat_set_utf16_len(flat, (ant_offset_t)units);
}

static __attribute__((noinline, cold)) utf16_pos_mark_t utf16_pos_seek_truncated(
  const char *str, size_t len, size_t byte, size_t units, size_t target, bool split_truncated
) {
  const unsigned char *s = (const unsigned char *)str;

  while (byte < len && units < target) {
    size_t slen = utf16_lead_len(s[byte]), width = utf16_lead_units(s[byte]);
    if (split_truncated && byte + slen > len) slen = width = 1;
    if (units + width > target) break;
    byte += slen;
    units += width;
  }

  return (utf16_pos_mark_t){ .byte = byte < len ? byte : len, .units = units };
}

static __attribute__((always_inline)) inline size_t utf16_pos_seek_to(
  utf16_pos_t *pos, size_t target, bool split_truncated
) {
  size_t byte = pos->byte, units = pos->units, len = pos->len;

  if (target < units || target - units > UTF16_INDEX_CHUNK) {
    utf16_pos_mark_t mark = utf16_pos_reposition(pos->str, len, pos->slot, byte, units, target);
    byte = mark.byte;
    units = mark.units;
  }

  const unsigned char *s = (const unsigned char *)pos->str;

  if (__builtin_expect(pos->truncated_tail, 0)) {
    utf16_pos_mark_t mark = utf16_pos_seek_truncated(pos->str, len, byte, units, target, split_truncated);
    pos->byte = mark.byte;
    pos->units = mark.units;
    return mark.byte;
  }

  while (byte < len && units < target) {
    unsigned char c = s[byte];
    if (c < 0x80) {
      byte++;
      units++;
      continue;
    }
    size_t width = utf16_lead_units(c);
    if (units + width > target) break;
    byte += utf16_lead_len(c);
    units += width;
  }

  if (byte == len) utf16_pos_note_end(pos->str, len, units);

  pos->byte = byte;
  pos->units = units;
  return byte;
}

static __attribute__((always_inline)) inline size_t utf16_pos_units_to(utf16_pos_t *pos, size_t target) {
  size_t byte = pos->byte, units = pos->units, len = pos->len;
  if (target > len) target = len;

  if (target < byte || target - byte > UTF16_INDEX_CHUNK * 4) {
    bool done;
    utf16_pos_mark_t mark = utf16_pos_reposition_byte(pos->str, len, pos->slot, byte, units, target, &done);
    pos->byte = byte = mark.byte;
    pos->units = units = mark.units;
    if (done) return units;
  }

  const unsigned char *s = (const unsigned char *)pos->str;

  while (byte < target) {
    unsigned char c = s[byte];
    if (c < 0x80) {
      byte++;
      units++;
      continue;
    }
    size_t slen = utf16_lead_len(c), width = utf16_lead_units(c);
    if (byte + slen > target) {
      pos->byte = byte;
      pos->units = units;
      return units + width;
    }
    byte += slen;
    units += width;
  }

  if (byte == len) utf16_pos_note_end(pos->str, len, units);
  pos->byte = byte;
  pos->units = units;
  return units;
}

size_t utf16_pos_seek_slow(utf16_pos_t *pos, size_t target) {
  return utf16_pos_seek_to(pos, target, false);
}

size_t utf16_pos_units_slow(utf16_pos_t *pos, size_t byte) {
  return utf16_pos_units_to(pos, byte);
}

static bool utf8_json_quote_reserve(char **buf, size_t *cap, size_t need) {
  if (need <= *cap) return true;

  size_t next = *cap ? *cap * 2 : 64;
  while (next < need) next *= 2;

  char *tmp = realloc(*buf, next);
  if (!tmp) return false;
  *buf = tmp;
  *cap = next;
  return true;
}

static bool utf8_json_quote_append(
  char **buf, size_t *len, size_t *cap, const void *src, size_t src_len
) {
  if (!utf8_json_quote_reserve(buf, cap, *len + src_len + 1)) return false;
  memcpy(*buf + *len, src, src_len);
  *len += src_len;
  (*buf)[*len] = '\0';
  return true;
}

static bool utf8_json_quote_append_char(char **buf, size_t *len, size_t *cap, char ch) {
  return utf8_json_quote_append(buf, len, cap, &ch, 1);
}

static bool utf8_json_quote_append_u_escape(
  char **buf, size_t *len, size_t *cap, uint32_t code_unit
) {
  char escape[6] = {
    '\\', 'u',
    hex_char((int)(code_unit >> 12)),
    hex_char((int)(code_unit >> 8)),
    hex_char((int)(code_unit >> 4)),
    hex_char((int)code_unit),
  };
  return utf8_json_quote_append(buf, len, cap, escape, sizeof(escape));
}

static const char json_ascii_escape_action[128] = {
  ['"'] = '"', ['\\'] = '\\',
  ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't',
  [0x00] = 'u', [0x01] = 'u', [0x02] = 'u', [0x03] = 'u', [0x04] = 'u', [0x05] = 'u',
  [0x06] = 'u', [0x07] = 'u', [0x0b] = 'u', [0x0e] = 'u', [0x0f] = 'u', [0x10] = 'u',
  [0x11] = 'u', [0x12] = 'u', [0x13] = 'u', [0x14] = 'u', [0x15] = 'u', [0x16] = 'u',
  [0x17] = 'u', [0x18] = 'u', [0x19] = 'u', [0x1a] = 'u', [0x1b] = 'u', [0x1c] = 'u',
  [0x1d] = 'u', [0x1e] = 'u', [0x1f] = 'u',
};

static inline size_t wtf8_decode_at(const char *str, size_t byte_len, size_t i, uint32_t *out_cp) {
  unsigned char c = (unsigned char)str[i];

  if (c < 0x80) {
    *out_cp = c;
    return 1;
  }

  size_t n = c < 0xC0 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  if (n == 0 || i + n > byte_len) return 0;

  uint32_t cp = (uint32_t)(c & (0x7F >> n));
  for (size_t k = 1; k < n; k++) {
    if (((unsigned char)str[i + k] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | ((uint32_t)str[i + k] & 0x3F);
  }

  *out_cp = cp;
  return n;
}

static inline bool wtf8_is_high_surrogate(uint32_t cp) { return cp >= 0xD800 && cp <= 0xDBFF; }
static inline bool wtf8_is_low_surrogate(uint32_t cp)  { return cp >= 0xDC00 && cp <= 0xDFFF; }

char *utf8_json_quote(const char *str, size_t byte_len, size_t *out_len) {
  size_t raw_len = 0;
  size_t raw_cap = byte_len + 12;

  char *raw = malloc(raw_cap);
  if (!raw) {
    if (out_len) *out_len = 0;
    return NULL;
  }

  if (!utf8_json_quote_append_char(&raw, &raw_len, &raw_cap, '"')) goto oom;

  size_t i = 0;
  while (i < byte_len) {
    unsigned char c = (unsigned char)str[i];

    if (c < 0x80) {
      char action = json_ascii_escape_action[c];
      bool ok;

      if (action == 0) ok = utf8_json_quote_append(&raw, &raw_len, &raw_cap, &str[i], 1);
      else if (action == 'u') ok = utf8_json_quote_append_u_escape(&raw, &raw_len, &raw_cap, c);
      else {
        char two[2] = { '\\', action };
        ok = utf8_json_quote_append(&raw, &raw_len, &raw_cap, two, 2);
      }

      if (!ok) goto oom;
      i++;
      continue;
    }

    uint32_t cp;
    size_t n = wtf8_decode_at(str, byte_len, i, &cp);
    if (n == 0) {
      if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, &str[i], 1)) goto oom;
      i++;
      continue;
    }

    if (wtf8_is_high_surrogate(cp) && i + n < byte_len) {
      uint32_t cp2;
      size_t n2 = wtf8_decode_at(str, byte_len, i + n, &cp2);
      if (n2 && wtf8_is_low_surrogate(cp2)) {
        uint32_t full = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
        char utf8[4];
        int en = utf8_encode(full, utf8);
        if (en <= 0 || !utf8_json_quote_append(&raw, &raw_len, &raw_cap, utf8, (size_t)en)) goto oom;
        i += n + n2;
        continue;
      }
    }

    if (wtf8_is_high_surrogate(cp) || wtf8_is_low_surrogate(cp)) {
      if (!utf8_json_quote_append_u_escape(&raw, &raw_len, &raw_cap, cp)) goto oom;
      i += n;
      continue;
    }

    if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, &str[i], n)) goto oom;
    i += n;
  }

  if (!utf8_json_quote_append_char(&raw, &raw_len, &raw_cap, '"')) goto oom;
  if (out_len) *out_len = raw_len;
  return raw;

oom:
  free(raw);
  if (out_len) *out_len = 0;
  return NULL;
}

size_t utf8_char_len_at(const char *str, size_t byte_len, size_t pos) {
  if (pos >= byte_len) return 1;
  int seq = utf8_sequence_length((unsigned char)str[pos]);
  if (seq <= 0) return 1;
  if (pos + (size_t)seq > byte_len) return byte_len - pos;
  return (size_t)seq;
}

char *latin1_to_utf8(const uint8_t *src, size_t len, size_t *out_len) {
  if (len > SIZE_MAX / 2) return NULL;

  char *out = malloc(len == 0 ? 1 : len * 2);
  if (!out) return NULL;

  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t byte = src[i];
    if (byte < 0x80) out[o++] = (char)byte;
    else {
      out[o++] = (char)(0xc0 | (byte >> 6));
      out[o++] = (char)(0x80 | (byte & 0x3f));
    }
  }

  if (out_len) *out_len = o;
  return out;
}

uint8_t *utf8_to_latin1(const char *src, size_t len, size_t *out_len, bool *is_latin1) {
  if (out_len) *out_len = 0;
  if (is_latin1) *is_latin1 = true;

  uint8_t *out = malloc(len == 0 ? 1 : len);
  if (!out) return NULL;

  size_t i = 0, o = 0;
  while (i < len) {
    uint8_t first = (uint8_t)src[i];
    if (first < 0x80) {
      out[o++] = first;
      i++;
      continue;
    }

    if (
      (first == 0xc2 || first == 0xc3) &&
      i + 1 < len &&
      (((uint8_t)src[i + 1] & 0xc0) == 0x80)
    ) {
      out[o++] = (uint8_t)(((first & 0x1f) << 6) | ((uint8_t)src[i + 1] & 0x3f));
      i += 2;
      continue;
    }

    free(out);
    if (is_latin1) *is_latin1 = false;
    return NULL;
  }

  if (out_len) *out_len = o;
  return out;
}

size_t utf16_strlen(const char *str, size_t byte_len) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  size_t utf16_len = 0;

  while (p < end) {
    if (*p >= 0x80) {
      utf16_len += utf16_lead_units(*p);
      p += utf16_lead_len(*p);
      continue;
    }

    size_t step = 1;
    if (end - p >= 8) {
      uint64_t word;
      memcpy(&word, p, sizeof(word));
      uint64_t high = word & UINT64_C(0x8080808080808080);
      step = high ? (size_t)__builtin_ctzll(high) >> 3 : 8;
    }

    utf16_len += step;
    p += step;
  }

  return utf16_len;
}

static bool utf8_validate_impl(const char *str, size_t len, bool allow_surrogates) {
  const unsigned char *s = (const unsigned char *)str;
  const unsigned char *end = s + len;

  while (s < end) {
    unsigned char c = *s;
    if (c < 0x80) {
      s++;
      continue;
    }

    size_t cont;
    unsigned char lo = 0x80;
    unsigned char hi = 0xbf;
    
    if (c >= 0xc2 && c <= 0xdf) cont = 1;
    else if (c == 0xe0) { cont = 2; lo = 0xa0; }
    else if (c >= 0xe1 && c <= 0xec) cont = 2;
    else if (c == 0xed) { cont = 2; if (!allow_surrogates) hi = 0x9f; }
    else if (c == 0xee || c == 0xef) cont = 2;
    else if (c == 0xf0) { cont = 3; lo = 0x90; }
    else if (c >= 0xf1 && c <= 0xf3) cont = 3;
    else if (c == 0xf4) { cont = 3; hi = 0x8f; }
    else return false;

    if ((size_t)(end - s) <= cont) return false;
    if (s[1] < lo || s[1] > hi) return false;
    
    for (size_t i = 2; i <= cont; i++)
      if (s[i] < 0x80 || s[i] > 0xbf) return false;
    
    s += cont + 1;
  }
  
  return true;
}

bool wtf8_validate_bytes(const char *str, size_t len) {
  return utf8_validate_impl(str, len, true);
}

static inline bool utf8_is_cont(uint8_t b) {
  return (int8_t)b < -0x40;
}

static size_t utf8_valid_prefix_scalar(const uint8_t *s, size_t len, size_t i, bool *out_ascii) {
  const uint64_t high = UINT64_C(0x8080808080808080);
  bool ascii = true;

  for (;;) {
    while (len - i >= 32) {
      uint64_t w[4];
      memcpy(w, s + i, sizeof(w));
      if ((w[0] | w[1] | w[2] | w[3]) & high) break;
      i += 32;
    }
    
    for (;;) {
      if (len - i < 8) {
        while (i < len && s[i] < 0x80) i++;
        break;
      }
      uint64_t w;
      memcpy(&w, s + i, sizeof(w));
      uint64_t m = w & high;
      if (m) { i += (size_t)__builtin_ctzll(m) >> 3; break; }
      i += 8;
    }
    
    if (i >= len) break;
    ascii = false;
    unsigned run = 0;

    while (i < len) {
      uint8_t c = s[i];
      if (c < 0x80) {
        i++;
        if (++run == 8) break;
        continue;
      }
      
      run = 0;
      size_t avail = len - i;
      if (c >= 0xc2 && c <= 0xdf) {
        if (avail < 2 || !utf8_is_cont(s[i + 1])) goto done;
        i += 2;
      } else if (c >= 0xe0 && c <= 0xef) {
        if (avail < 3) goto done;
        uint8_t b = s[i + 1];
        uint8_t lo = c == 0xe0 ? 0xa0 : 0x80;
        uint8_t hi = c == 0xed ? 0x9f : 0xbf;
        if (b < lo || b > hi || !utf8_is_cont(s[i + 2])) goto done;
        i += 3;
      } else if (c >= 0xf0 && c <= 0xf4) {
        if (avail < 4) goto done;
        uint8_t b = s[i + 1];
        uint8_t lo = c == 0xf0 ? 0x90 : 0x80;
        uint8_t hi = c == 0xf4 ? 0x8f : 0xbf;
        if (b < lo || b > hi || !utf8_is_cont(s[i + 2]) || !utf8_is_cont(s[i + 3])) goto done;
        i += 4;
      } else goto done;
    }
    
    if (i >= len) break;
  }

done:
  if (out_ascii) *out_ascii = ascii;
  return i;
}

#if defined(__aarch64__) && defined(__ARM_NEON)
static inline uint8x16_t utf8_neon_block_errors(uint8x16_t in, uint8x16_t prev) {
  static const uint8_t byte_1_high[16] = {
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x80, 0x80, 0x80, 0x80, 0x21, 0x01, 0x15, 0x49,
  };
  static const uint8_t byte_1_low[16] = {
    0xe7, 0xa3, 0x83, 0x83, 0x8b, 0xcb, 0xcb, 0xcb,
    0xcb, 0xcb, 0xcb, 0xcb, 0xcb, 0xdb, 0xcb, 0xcb,
  };
  static const uint8_t byte_2_high[16] = {
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0xe6, 0xae, 0xba, 0xba, 0x01, 0x01, 0x01, 0x01,
  };

  uint8x16_t prev1 = vextq_u8(prev, in, 15);
  uint8x16_t special = vandq_u8(
    vandq_u8(
      vqtbl1q_u8(vld1q_u8(byte_1_high), vshrq_n_u8(prev1, 4)),
      vqtbl1q_u8(vld1q_u8(byte_1_low), vandq_u8(prev1, vdupq_n_u8(0x0f)))
    ),
    vqtbl1q_u8(vld1q_u8(byte_2_high), vshrq_n_u8(in, 4))
  );
  
  uint8x16_t third = vcgeq_u8(vextq_u8(prev, in, 14), vdupq_n_u8(0xe0));
  uint8x16_t fourth = vcgeq_u8(vextq_u8(prev, in, 13), vdupq_n_u8(0xf0));
  uint8x16_t must_cont = vandq_u8(vorrq_u8(third, fourth), vdupq_n_u8(0x80));
  
  return veorq_u8(must_cont, special);
}

static inline uint8x16_t utf8_neon_incomplete(uint8x16_t in) {
  static const uint8_t max_value[16] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0xdf, 0xbf,
  };
  return vqsubq_u8(in, vld1q_u8(max_value));
}

static inline uint8x16_t utf8_neon_chunk(
  const uint8_t *s, uint8_t *dst, uint8x16_t *prev,
  uint8x16_t *incomplete, bool *ascii
) {
  uint8x16_t blocks[4] = {
    vld1q_u8(s), vld1q_u8(s + 16),
    vld1q_u8(s + 32), vld1q_u8(s + 48),
  };
  
  if (dst) {
    vst1q_u8(dst, blocks[0]);
    vst1q_u8(dst + 16, blocks[1]);
    vst1q_u8(dst + 32, blocks[2]);
    vst1q_u8(dst + 48, blocks[3]);
  }
  
  uint8x16_t pairs = vpmaxq_u8(
    vpmaxq_u8(blocks[0], blocks[1]),
    vpmaxq_u8(blocks[2], blocks[3])
  );
  
  uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(vpmaxq_u8(pairs, pairs)), 0)
    & UINT64_C(0x8080808080808080);
  
  if (!high) {
    uint8x16_t err = *incomplete;
    *incomplete = vdupq_n_u8(0);
    *prev = blocks[3];
    return err;
  }

  *ascii = false;
  uint8x16_t err = vdupq_n_u8(0);
  
  for (int k = 0; k < 4; k++) {
    uint8x16_t in = blocks[k];
    if (!(high & (UINT64_C(0x8080) << (16 * k)))) {
      err = vorrq_u8(err, *incomplete);
      *incomplete = vdupq_n_u8(0);
    } else {
      err = vorrq_u8(err, utf8_neon_block_errors(in, *prev));
      *incomplete = utf8_neon_incomplete(in);
    }
    *prev = in;
  }
  
  return err;
}

static size_t utf8_neon_valid_blocks(const uint8_t *s, size_t len, uint8_t *dst, bool *out_ascii) {
  uint8x16_t prev = vdupq_n_u8(0);
  uint8x16_t incomplete = vdupq_n_u8(0);
  bool ascii = true;
  size_t i = 0;

  while (len - i >= 256) {
    uint8x16_t err = vorrq_u8(
      vorrq_u8(
        utf8_neon_chunk(s + i, dst ? dst + i : NULL, &prev, &incomplete, &ascii),
        utf8_neon_chunk(s + i + 64, dst ? dst + i + 64 : NULL, &prev, &incomplete, &ascii)
      ),
      vorrq_u8(
        utf8_neon_chunk(s + i + 128, dst ? dst + i + 128 : NULL, &prev, &incomplete, &ascii),
        utf8_neon_chunk(s + i + 192, dst ? dst + i + 192 : NULL, &prev, &incomplete, &ascii)
      )
    );
    if (vmaxvq_u8(err)) goto found;
    i += 256;
  }
  
  while (len - i >= 64) {
    if (vmaxvq_u8(utf8_neon_chunk(s + i, dst ? dst + i : NULL, &prev, &incomplete, &ascii))) break;
    i += 64;
  }

found:;
  size_t back = 0;
  while (back < 3 && back < i && utf8_is_cont(s[i - back - 1])) back++;
  if (back < i && s[i - back - 1] >= 0xc0) back++;
  
  *out_ascii = ascii;
  return i - back;
}
#endif

static size_t utf8_valid_prefix_impl(const uint8_t *s, size_t len, uint8_t *dst, bool *out_ascii) {
  size_t start = 0;
  bool ascii = true;
#if defined(__aarch64__) && defined(__ARM_NEON)
  if (len >= 64) start = utf8_neon_valid_blocks(s, len, dst, &ascii);
#endif
  bool tail_ascii;
  size_t end = utf8_valid_prefix_scalar(s, len, start, &tail_ascii);
  if (dst && end > start) memcpy(dst + start, s + start, end - start);
  if (out_ascii) *out_ascii = ascii && tail_ascii;
  return end;
}

size_t utf8_valid_prefix(const char *str, size_t len, bool *out_ascii) {
  return utf8_valid_prefix_impl((const uint8_t *)str, len, NULL, out_ascii);
}

size_t utf8_copy_valid_prefix(char *dst, const char *src, size_t len, bool *out_ascii) {
  return utf8_valid_prefix_impl((const uint8_t *)src, len, (uint8_t *)dst, out_ascii);
}

bool utf8_validate_bytes(const char *str, size_t len) {
  return utf8_valid_prefix(str, len, NULL) == len;
}

static size_t utf8_invalid_span(const uint8_t *s, size_t avail) {
  uint8_t c = s[0];
  uint8_t lo = 0x80, hi = 0xbf;
  size_t need;

  if (c >= 0xc2 && c <= 0xdf) need = 1;
  else if (c >= 0xe0 && c <= 0xef) {
    need = 2;
    if (c == 0xe0) lo = 0xa0;
    else if (c == 0xed) hi = 0x9f;
  } else if (c >= 0xf0 && c <= 0xf4) {
    need = 3;
    if (c == 0xf0) lo = 0x90;
    else if (c == 0xf4) hi = 0x8f;
  } else return 1;

  size_t k = 1;
  while (k <= need && k < avail && s[k] >= lo && s[k] <= hi) {
    lo = 0x80; hi = 0xbf;
    k++;
  }

  return k;
}

static inline bool wtf8_surrogate_at(const uint8_t *s, size_t avail) {
  return avail >= 3 && s[0] == 0xed && s[1] >= 0xa0 && s[1] <= 0xbf && utf8_is_cont(s[2]);
}

static char *utf8_replace_invalid_from(
  const char *str, size_t len, size_t valid,
  bool keep_surrogates, size_t *out_len
) {
  size_t tail = len - valid;
  if (tail > (SIZE_MAX - valid - 1) / 3) return NULL;

  char *out = malloc(valid + tail * 3 + 1);
  if (!out) return NULL;

  const uint8_t *s = (const uint8_t *)str;
  memcpy(out, str, valid);
  size_t i = valid, o = valid;

  while (i < len) {
    if (keep_surrogates && wtf8_surrogate_at(s + i, len - i)) {
      memcpy(out + o, str + i, 3);
      i += 3;
    } else {
      i += utf8_invalid_span(s + i, len - i);
      memcpy(out + o, "\xef\xbf\xbd", 3);
    }
    o += 3;
    
    size_t run = utf8_valid_prefix(str + i, len - i, NULL);
    memcpy(out + o, str + i, run);
    o += run;
    i += run;
  }

  out[o] = '\0';
  if (out_len) *out_len = o;
  return out;
}

size_t utf8_truncated_tail(const char *str, size_t len) {
  const uint8_t *s = (const uint8_t *)str;
  size_t conts = 0;
  
  while (conts < 3 && conts < len && utf8_is_cont(s[len - 1 - conts])) conts++;
  if (conts == len) return 0;
  
  uint8_t lead = s[len - 1 - conts];
  if (lead < 0xc2 || lead > 0xf4) return 0;
  
  size_t have = conts + 1;
  size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
  if (have >= need || utf8_invalid_span(s + len - have, have) != have) return 0;
  
  return have;
}

char *utf8_replace_invalid(const char *str, size_t len, size_t valid, size_t *out_len) {
  return utf8_replace_invalid_from(str, len, valid, false, out_len);
}

char *wtf8_replace_invalid(const char *str, size_t len, size_t *out_len) {
  size_t valid = utf8_valid_prefix(str, len, NULL);
  if (valid == len || wtf8_validate_bytes(str + valid, len - valid)) return NULL;
  return utf8_replace_invalid_from(str, len, valid, true, out_len);
}

static bool utf8_wtf8_surrogate_at(
  const uint8_t *str, size_t len,
  size_t pos, uint16_t *out
) {
  if (
    pos + 2 >= len || str[pos] != 0xed ||
    (str[pos + 1] & 0xe0) != 0xa0 ||
    (str[pos + 2] & 0xc0) != 0x80
  ) return false;

  *out = (uint16_t)(
    ((uint16_t)(str[pos] & 0x0f) << 12) |
    ((uint16_t)(str[pos + 1] & 0x3f) << 6) |
    (uint16_t)(str[pos + 2] & 0x3f)
  );
  
  return *out >= 0xd800 && *out <= 0xdfff;
}

typedef uint8_t wtf8_v16_t __attribute__((vector_size(16)));

static inline bool wtf8_v16_any(wtf8_v16_t v) {
  uint64_t w[2];
  memcpy(w, &v, sizeof(w));
  return (w[0] | w[1]) != 0;
}

static inline wtf8_v16_t wtf8_v16_load(const uint8_t *p) {
  wtf8_v16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline bool wtf8_is_surrogate_lead(const uint8_t *p) {
  return p[0] == 0xed && (p[1] & 0xe0) == 0xa0;
}

static __attribute__((noinline)) const uint8_t *wtf8_find_surrogate_lead_dense(const uint8_t *p, const uint8_t *end) {
  for (;;) {
    for (; end - p > 32; p += 32) {
      wtf8_v16_t lo = (wtf8_v16_t)(wtf8_v16_load(p) == 0xed);
      wtf8_v16_t hi = (wtf8_v16_t)(wtf8_v16_load(p + 16) == 0xed);
      if (!wtf8_v16_any(lo | hi)) break;
      lo &= (wtf8_v16_t)(wtf8_v16_load(p + 1) >= 0xa0);
      hi &= (wtf8_v16_t)(wtf8_v16_load(p + 17) >= 0xa0);
      if (!wtf8_v16_any(lo | hi)) continue;
      for (const uint8_t *q = p; q < p + 32; q++) if (wtf8_is_surrogate_lead(q)) return q;
    }
    if (end - p < 2 || (p = memchr(p, 0xed, (size_t)(end - 1 - p))) == NULL) return NULL;
    if (wtf8_is_surrogate_lead(p)) return p;
    p++;
  }
}

static inline const uint8_t *wtf8_find_surrogate_lead(const uint8_t *p, const uint8_t *end) {
  if (end - p < 2 || (p = memchr(p, 0xed, (size_t)(end - 1 - p))) == NULL) return NULL;
  if (wtf8_is_surrogate_lead(p)) return p;
  return wtf8_find_surrogate_lead_dense(p + 1, end);
}

bool wtf8_has_surrogate(const char *str, size_t len) {
  const uint8_t *src = (const uint8_t *)str;
  const uint8_t *end = src + len;
  for (const uint8_t *p = src; (p = wtf8_find_surrogate_lead(p, end)) != NULL; p++) {
    uint16_t unit;
    if (utf8_wtf8_surrogate_at(src, len, (size_t)(p - src), &unit)) return true;
  }
  return false;
}

size_t wtf8_to_utf8(const char *str, size_t len, char *dst) {
  const uint8_t *src = (const uint8_t *)str;
  const uint8_t *end = src + len;
  size_t pos = 0, written = 0;
  while (pos < len) {
    const uint8_t *hit = wtf8_find_surrogate_lead(src + pos, end);
    size_t next = hit ? (size_t)(hit - src) : len;
    memmove(dst + written, src + pos, next - pos);
    written += next - pos;
    pos = next;
    if (!hit) break;
    uint16_t first, second;
    if (!utf8_wtf8_surrogate_at(src, len, pos, &first)) {
      dst[written++] = (char)src[pos++];
      continue;
    }
    if (first <= 0xdbff && utf8_wtf8_surrogate_at(src, len, pos + 3, &second) && second >= 0xdc00) {
      uint32_t cp = UINT32_C(0x10000) + ((uint32_t)(first - 0xd800) << 10) + (uint32_t)(second - 0xdc00);
      written += (size_t)utf8_encode(cp, dst + written);
      pos += 6;
    } else {
      memcpy(dst + written, "\xef\xbf\xbd", 3);
      written += 3;
      pos += 3;
    }
  }
  return written;
}

bool wtf8_has_lone_surrogate(const char *str, size_t len) {
  const uint8_t *src = (const uint8_t *)str;
  const uint8_t *end = src + len;
  for (const uint8_t *p = src; (p = wtf8_find_surrogate_lead(p, end)) != NULL; p++) {
    size_t pos = (size_t)(p - src);
    uint16_t first, second;
    if (!utf8_wtf8_surrogate_at(src, len, pos, &first)) continue;
    if (first > 0xdbff || !utf8_wtf8_surrogate_at(src, len, pos + 3, &second) || second < 0xdc00) return true;
    p += 5;
  }
  return false;
}

static char *wtf8_to_utf8_copy(const char *str, size_t *len) {
  char *utf8 = malloc(*len ? *len : 1);
  if (utf8) *len = wtf8_to_utf8(str, *len, utf8);
  return utf8;
}

char *wtf8_to_utf8_dup(const char *str, size_t *len) {
  if (!str || !wtf8_has_surrogate(str, *len)) return NULL;
  return wtf8_to_utf8_copy(str, len);
}

char *wtf8_str_to_utf8_dup(const char *str, size_t *len) {
  if (!str) return NULL;
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  if ((flat->meta & STR_META_WELL_FORMED) || str_is_ascii(str)) return NULL;
  if (str_flat_utf_valid_state(flat) == STR_UTF_VALID) return NULL;
  if (!wtf8_has_surrogate(str, *len)) {
    flat->meta |= STR_META_WELL_FORMED;
    return NULL;
  }
  return wtf8_to_utf8_copy(str, len);
}

static size_t utf8_export_wtf8(
  const char *str, size_t str_len,
  uint8_t *dst, size_t dst_len,
  size_t *out_read_units
) {
  const uint8_t *src = (const uint8_t *)str;
  size_t pos = 0;
  size_t written = 0;
  size_t read_units = 0;

  while (pos < str_len) {
    uint8_t encoded[4];
    const uint8_t *piece = src + pos;
    size_t consumed = 1;
    size_t encoded_len = 3;
    size_t units = 1;
    uint16_t first;

    if (utf8_wtf8_surrogate_at(src, str_len, pos, &first)) {
      uint16_t second;
      if (
        first <= 0xdbff &&
        utf8_wtf8_surrogate_at(src, str_len, pos + 3, &second) &&
        second >= 0xdc00
      ) {
        uint32_t cp = UINT32_C(0x10000)
          + ((uint32_t)(first - 0xd800) << 10)
          + (uint32_t)(second - 0xdc00);
        encoded_len = (size_t)utf8_encode(cp, (char *)encoded);
        piece = encoded;
        consumed = 6;
        units = 2;
      } else {
        memcpy(encoded, "\xef\xbf\xbd", 3);
        piece = encoded;
        consumed = 3;
      }
    } else {
      utf8proc_int32_t cp;
      utf8proc_ssize_t n = utf8proc_iterate(
        (const utf8proc_uint8_t *)(src + pos),
        (utf8proc_ssize_t)(str_len - pos),
        &cp
      );
      if (n > 0) {
        consumed = (size_t)n;
        encoded_len = consumed;
        units = cp >= 0x10000 ? 2 : 1;
      } else {
        memcpy(encoded, "\xef\xbf\xbd", 3);
        piece = encoded;
      }
    }

    if (encoded_len > dst_len - written) break;
    if (dst) memcpy(dst + written, piece, encoded_len);
    written += encoded_len;
    read_units += units;
    pos += consumed;
  }

  if (out_read_units) *out_read_units = read_units;
  return written;
}

size_t utf8_export_length_slow(const char *str, size_t str_len) {
  if (str_is_valid_utf8(str)) return str_len;

  size_t len = utf8_export_wtf8(str, str_len, NULL, SIZE_MAX, NULL);
  if (len == str_len) {
    ant_flat_string_t *flat = str_flat_from_bytes(str);
    str_flat_set_utf_valid_state(flat, STR_UTF_INVALID_SAME_LENGTH);
  }
  
  return len;
}

size_t utf8_export_into(
  const char *str, size_t str_len,
  uint8_t *dst, size_t dst_len,
  size_t *out_read_units
) {
  if (str_len == 0) {
    if (out_read_units) *out_read_units = 0;
    return 0;
  }
  if (!str_is_valid_utf8(str)) {
    return utf8_export_wtf8(
      str, str_len, dst, dst_len, out_read_units
    );
  }

  size_t len = str_len < dst_len ? str_len : dst_len;
  if (len < str_len) {
    while (len > 0 && ((uint8_t)str[len] & 0xc0) == 0x80) len--;
  }
  if (len > 0 && dst) memcpy(dst, str, len);
  if (out_read_units) *out_read_units = utf16_strlen(str, len);
  return len;
}

static __attribute__((noinline)) int utf16_index_to_byte_offset_slow(
  const char *str,
  size_t byte_len,
  size_t utf16_idx,
  size_t *out_char_bytes
) {
  utf16_pos_t pos;
  utf16_pos_open(&pos, str, byte_len);

  const unsigned char *s = (const unsigned char *)str;
  size_t slen, units;
  utf16_pos_seek_to(&pos, utf16_idx, false);

  if (pos.units < utf16_idx && pos.byte < pos.len) {
    utf16_scan_decode(s + pos.byte, s + pos.len, &slen, &units, NULL);
    pos.byte = pos.byte + slen < pos.len ? pos.byte + slen : pos.len;
    pos.units += units;
  }

  utf16_pos_store(&pos);
  if (pos.byte >= pos.len) {
    if (pos.units != utf16_idx) return -1;
    if (out_char_bytes) *out_char_bytes = 0;
    return (int)byte_len;
  }

  utf16_scan_decode(s + pos.byte, s + pos.len, &slen, &units, NULL);
  if (out_char_bytes) *out_char_bytes = slen;
  return (int)pos.byte;
}

int utf16_index_to_byte_offset(
  const char *str,
  size_t byte_len,
  size_t utf16_idx,
  size_t *out_char_bytes
) {
  if (!str_is_ascii(str)) return utf16_index_to_byte_offset_slow(str, byte_len, utf16_idx, out_char_bytes);
  if (utf16_idx > byte_len) return -1;
  if (out_char_bytes) *out_char_bytes = (utf16_idx < byte_len) ? 1 : 0;
  return (int)utf16_idx;
}

size_t utf16_index_to_byte_offset_floor(
  const char *str,
  size_t byte_len,
  size_t utf16_idx
) {
  if (str_is_ascii(str)) return utf16_idx < byte_len ? utf16_idx : byte_len;

  utf16_pos_t pos;
  utf16_pos_open(&pos, str, byte_len);
  size_t byte = utf16_pos_seek_to(&pos, utf16_idx, false);
  utf16_pos_store(&pos);
  return byte;
}

static __attribute__((noinline)) utf16_range_splits_t utf16_range_to_byte_range_slow(
  const char *str,
  size_t byte_len,
  size_t utf16_start,
  size_t utf16_end,
  size_t *byte_start,
  size_t *byte_end
) {
  utf16_range_splits_t splits = {0};
  utf16_pos_t pos;
  utf16_pos_open(&pos, str, byte_len);

  const unsigned char *s = (const unsigned char *)str;
  size_t slen, units;
  uint32_t cp;

  utf16_pos_seek_to(&pos, utf16_start, false);
  if (pos.units < utf16_start && pos.byte < pos.len) {
    utf16_scan_decode(s + pos.byte, s + pos.len, &slen, &units, &cp);
    splits.prefix_surrogate = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    pos.byte += slen;
    pos.units += units;
  }

  *byte_start = pos.byte;
  utf16_pos_seek_to(&pos, utf16_end, false);
  if (pos.units < utf16_end && pos.byte < pos.len) {
    utf16_scan_decode(s + pos.byte, s + pos.len, &slen, &units, &cp);
    splits.suffix_surrogate = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
  }

  *byte_end = pos.byte;
  utf16_pos_store(&pos);

  return splits;
}

utf16_range_splits_t utf16_range_to_byte_range(
  const char *str,
  size_t byte_len,
  size_t utf16_start,
  size_t utf16_end,
  size_t *byte_start,
  size_t *byte_end
) {
  if (utf16_start >= utf16_end) {
    *byte_start = 0;
    *byte_end = 0;
    return (utf16_range_splits_t){0};
  }

  if (!str_is_ascii(str)) return utf16_range_to_byte_range_slow(
    str, byte_len, utf16_start, utf16_end, byte_start, byte_end
  );

  *byte_start = (utf16_start <= byte_len) ? utf16_start : byte_len;
  *byte_end = (utf16_end <= byte_len) ? utf16_end : byte_len;
  return (utf16_range_splits_t){0};
}

size_t byte_offset_to_utf16(const char *str, size_t byte_off) {
  if (str_is_ascii(str)) return byte_off;

  utf16_pos_t pos;
  utf16_pos_open(&pos, str, byte_off);
  size_t units = utf16_pos_units_to(&pos, byte_off);
  utf16_pos_store(&pos);
  return units;
}

bool utf16_next_code_unit(utf16_iterator_t *iterator, uint16_t *unit) {
  if (iterator->trailing_surrogate) {
    *unit = iterator->trailing_surrogate;
    iterator->trailing_surrogate = 0;
    return true;
  }
  
  if (iterator->next == iterator->end) return false;
  size_t bytes, units; uint32_t codepoint;
  
  utf16_scan_decode(iterator->next, iterator->end, &bytes, &units, &codepoint);
  iterator->next += bytes;
  
  if (units == 2) {
    codepoint -= 0x10000;
    iterator->trailing_surrogate = (uint16_t)(0xDC00 + (codepoint & 0x3FF));
    *unit = (uint16_t)(0xD800 + (codepoint >> 10));
  } else *unit = (uint16_t)codepoint;
  
  return true;
}

static __attribute__((noinline)) uint32_t utf16_unit_lookup(
  const char *str, size_t byte_len, size_t utf16_idx, bool codepoint
) {
  utf16_pos_t pos;
  utf16_pos_open(&pos, str, byte_len);
  utf16_pos_seek_to(&pos, utf16_idx, true);

  if (pos.byte >= pos.len) {
    utf16_pos_store(&pos);
    return 0xFFFFFFFF;
  }

  const unsigned char *s = (const unsigned char *)str;
  size_t slen, units;
  uint32_t cp;
  utf16_scan_decode(s + pos.byte, s + pos.len, &slen, &units, &cp);

  if (pos.units != utf16_idx) {
    utf16_pos_store(&pos);
    return 0xDC00 + ((cp - 0x10000) & 0x3FF);
  }

  if (units == 1) {
    pos.byte += slen;
    pos.units++;
  }

  utf16_pos_store(&pos);
  if (units == 2 && !codepoint) return 0xD800 + ((cp - 0x10000) >> 10);
  return cp;
}

uint32_t utf16_code_unit_at(const char *str, size_t byte_len, size_t utf16_idx) {
  if (!str_is_ascii(str)) return utf16_unit_lookup(str, byte_len, utf16_idx, false);
  return utf16_idx < byte_len ? (unsigned char)str[utf16_idx] : 0xFFFFFFFF;
}

utf8proc_ssize_t utf8_whatwg_decode(
  utf8_dec_t *dec, const uint8_t *src, size_t len,
  char *out, bool fatal, bool stream
) {
  size_t i = 0, o = 0;
  
  if (
    !dec->bom_seen && !dec->ignore_bom && len >= 3 &&
    src[0] == 0xef && src[1] == 0xbb && src[2] == 0xbf
  ) {
    dec->bom_seen = true;
    i = 3;
  }

  while (i < len) {
    size_t run = utf8_valid_prefix((const char *)src + i, len - i, NULL);
    if (run > 0) {
      memcpy(out + o, src + i, run);
      o += run;
      i += run;
      dec->bom_seen = true;
      if (i == len) break;
    }

    size_t span = utf8_invalid_span(src + i, len - i);
    bool truncated = i + span == len && src[i] >= 0xc2 && src[i] <= 0xf4;
    
    if (truncated && stream) {
      dec->pend_pos = (int)span;
      memcpy(dec->pend_buf, src + i, span);
      break;
    }
    
    if (fatal) return -1;
    memcpy(out + o, "\xef\xbf\xbd", 3);
    o += 3;
    i += span;
    dec->bom_seen = true;
  }

  return (utf8proc_ssize_t)o;
}

uint32_t utf16_codepoint_at(const char *str, size_t byte_len, size_t utf16_idx) {
  if (!str_is_ascii(str)) return utf16_unit_lookup(str, byte_len, utf16_idx, true);
  return utf16_idx < byte_len ? (unsigned char)str[utf16_idx] : 0xFFFFFFFF;
}

typedef struct {
  uint32_t cp;
  uint8_t count;
  uint32_t mapped[3];
} utf8_special_case_t;

static const utf8_special_case_t utf8_special_upper[] = {
  { 0x00DF, 2, { 0x0053, 0x0053, 0x0000 } },
  { 0x0149, 2, { 0x02BC, 0x004E, 0x0000 } },
  { 0x01F0, 2, { 0x004A, 0x030C, 0x0000 } },
  { 0x0390, 3, { 0x0399, 0x0308, 0x0301 } },
  { 0x03B0, 3, { 0x03A5, 0x0308, 0x0301 } },
  { 0x0587, 2, { 0x0535, 0x0552, 0x0000 } },
  { 0x1E96, 2, { 0x0048, 0x0331, 0x0000 } },
  { 0x1E97, 2, { 0x0054, 0x0308, 0x0000 } },
  { 0x1E98, 2, { 0x0057, 0x030A, 0x0000 } },
  { 0x1E99, 2, { 0x0059, 0x030A, 0x0000 } },
  { 0x1E9A, 2, { 0x0041, 0x02BE, 0x0000 } },
  { 0x1F50, 2, { 0x03A5, 0x0313, 0x0000 } },
  { 0x1F52, 3, { 0x03A5, 0x0313, 0x0300 } },
  { 0x1F54, 3, { 0x03A5, 0x0313, 0x0301 } },
  { 0x1F56, 3, { 0x03A5, 0x0313, 0x0342 } },
  { 0x1F80, 2, { 0x1F08, 0x0399, 0x0000 } },
  { 0x1F81, 2, { 0x1F09, 0x0399, 0x0000 } },
  { 0x1F82, 2, { 0x1F0A, 0x0399, 0x0000 } },
  { 0x1F83, 2, { 0x1F0B, 0x0399, 0x0000 } },
  { 0x1F84, 2, { 0x1F0C, 0x0399, 0x0000 } },
  { 0x1F85, 2, { 0x1F0D, 0x0399, 0x0000 } },
  { 0x1F86, 2, { 0x1F0E, 0x0399, 0x0000 } },
  { 0x1F87, 2, { 0x1F0F, 0x0399, 0x0000 } },
  { 0x1F88, 2, { 0x1F08, 0x0399, 0x0000 } },
  { 0x1F89, 2, { 0x1F09, 0x0399, 0x0000 } },
  { 0x1F8A, 2, { 0x1F0A, 0x0399, 0x0000 } },
  { 0x1F8B, 2, { 0x1F0B, 0x0399, 0x0000 } },
  { 0x1F8C, 2, { 0x1F0C, 0x0399, 0x0000 } },
  { 0x1F8D, 2, { 0x1F0D, 0x0399, 0x0000 } },
  { 0x1F8E, 2, { 0x1F0E, 0x0399, 0x0000 } },
  { 0x1F8F, 2, { 0x1F0F, 0x0399, 0x0000 } },
  { 0x1F90, 2, { 0x1F28, 0x0399, 0x0000 } },
  { 0x1F91, 2, { 0x1F29, 0x0399, 0x0000 } },
  { 0x1F92, 2, { 0x1F2A, 0x0399, 0x0000 } },
  { 0x1F93, 2, { 0x1F2B, 0x0399, 0x0000 } },
  { 0x1F94, 2, { 0x1F2C, 0x0399, 0x0000 } },
  { 0x1F95, 2, { 0x1F2D, 0x0399, 0x0000 } },
  { 0x1F96, 2, { 0x1F2E, 0x0399, 0x0000 } },
  { 0x1F97, 2, { 0x1F2F, 0x0399, 0x0000 } },
  { 0x1F98, 2, { 0x1F28, 0x0399, 0x0000 } },
  { 0x1F99, 2, { 0x1F29, 0x0399, 0x0000 } },
  { 0x1F9A, 2, { 0x1F2A, 0x0399, 0x0000 } },
  { 0x1F9B, 2, { 0x1F2B, 0x0399, 0x0000 } },
  { 0x1F9C, 2, { 0x1F2C, 0x0399, 0x0000 } },
  { 0x1F9D, 2, { 0x1F2D, 0x0399, 0x0000 } },
  { 0x1F9E, 2, { 0x1F2E, 0x0399, 0x0000 } },
  { 0x1F9F, 2, { 0x1F2F, 0x0399, 0x0000 } },
  { 0x1FA0, 2, { 0x1F68, 0x0399, 0x0000 } },
  { 0x1FA1, 2, { 0x1F69, 0x0399, 0x0000 } },
  { 0x1FA2, 2, { 0x1F6A, 0x0399, 0x0000 } },
  { 0x1FA3, 2, { 0x1F6B, 0x0399, 0x0000 } },
  { 0x1FA4, 2, { 0x1F6C, 0x0399, 0x0000 } },
  { 0x1FA5, 2, { 0x1F6D, 0x0399, 0x0000 } },
  { 0x1FA6, 2, { 0x1F6E, 0x0399, 0x0000 } },
  { 0x1FA7, 2, { 0x1F6F, 0x0399, 0x0000 } },
  { 0x1FA8, 2, { 0x1F68, 0x0399, 0x0000 } },
  { 0x1FA9, 2, { 0x1F69, 0x0399, 0x0000 } },
  { 0x1FAA, 2, { 0x1F6A, 0x0399, 0x0000 } },
  { 0x1FAB, 2, { 0x1F6B, 0x0399, 0x0000 } },
  { 0x1FAC, 2, { 0x1F6C, 0x0399, 0x0000 } },
  { 0x1FAD, 2, { 0x1F6D, 0x0399, 0x0000 } },
  { 0x1FAE, 2, { 0x1F6E, 0x0399, 0x0000 } },
  { 0x1FAF, 2, { 0x1F6F, 0x0399, 0x0000 } },
  { 0x1FB2, 2, { 0x1FBA, 0x0399, 0x0000 } },
  { 0x1FB3, 2, { 0x0391, 0x0399, 0x0000 } },
  { 0x1FB4, 2, { 0x0386, 0x0399, 0x0000 } },
  { 0x1FB6, 2, { 0x0391, 0x0342, 0x0000 } },
  { 0x1FB7, 3, { 0x0391, 0x0342, 0x0399 } },
  { 0x1FBC, 2, { 0x0391, 0x0399, 0x0000 } },
  { 0x1FC2, 2, { 0x1FCA, 0x0399, 0x0000 } },
  { 0x1FC3, 2, { 0x0397, 0x0399, 0x0000 } },
  { 0x1FC4, 2, { 0x0389, 0x0399, 0x0000 } },
  { 0x1FC6, 2, { 0x0397, 0x0342, 0x0000 } },
  { 0x1FC7, 3, { 0x0397, 0x0342, 0x0399 } },
  { 0x1FCC, 2, { 0x0397, 0x0399, 0x0000 } },
  { 0x1FD2, 3, { 0x0399, 0x0308, 0x0300 } },
  { 0x1FD3, 3, { 0x0399, 0x0308, 0x0301 } },
  { 0x1FD6, 2, { 0x0399, 0x0342, 0x0000 } },
  { 0x1FD7, 3, { 0x0399, 0x0308, 0x0342 } },
  { 0x1FE2, 3, { 0x03A5, 0x0308, 0x0300 } },
  { 0x1FE3, 3, { 0x03A5, 0x0308, 0x0301 } },
  { 0x1FE4, 2, { 0x03A1, 0x0313, 0x0000 } },
  { 0x1FE6, 2, { 0x03A5, 0x0342, 0x0000 } },
  { 0x1FE7, 3, { 0x03A5, 0x0308, 0x0342 } },
  { 0x1FF2, 2, { 0x1FFA, 0x0399, 0x0000 } },
  { 0x1FF3, 2, { 0x03A9, 0x0399, 0x0000 } },
  { 0x1FF4, 2, { 0x038F, 0x0399, 0x0000 } },
  { 0x1FF6, 2, { 0x03A9, 0x0342, 0x0000 } },
  { 0x1FF7, 3, { 0x03A9, 0x0342, 0x0399 } },
  { 0x1FFC, 2, { 0x03A9, 0x0399, 0x0000 } },
  { 0xFB00, 2, { 0x0046, 0x0046, 0x0000 } },
  { 0xFB01, 2, { 0x0046, 0x0049, 0x0000 } },
  { 0xFB02, 2, { 0x0046, 0x004C, 0x0000 } },
  { 0xFB03, 3, { 0x0046, 0x0046, 0x0049 } },
  { 0xFB04, 3, { 0x0046, 0x0046, 0x004C } },
  { 0xFB05, 2, { 0x0053, 0x0054, 0x0000 } },
  { 0xFB06, 2, { 0x0053, 0x0054, 0x0000 } },
  { 0xFB13, 2, { 0x0544, 0x0546, 0x0000 } },
  { 0xFB14, 2, { 0x0544, 0x0535, 0x0000 } },
  { 0xFB15, 2, { 0x0544, 0x053B, 0x0000 } },
  { 0xFB16, 2, { 0x054E, 0x0546, 0x0000 } },
  { 0xFB17, 2, { 0x0544, 0x053D, 0x0000 } },
};

static bool utf8_case_ignorable(utf8proc_int32_t cp) {
  switch (cp) {
    case 0x0027: case 0x002E: case 0x003A: case 0x00B7: case 0x0387: case 0x05F4:
    case 0x2018: case 0x2019: case 0x2024: case 0x2027: case 0xFE13: case 0xFE52:
    case 0xFE55: case 0xFF07: case 0xFF0E: case 0xFF1A:
      return true;
  }
  switch (utf8proc_category(cp)) {
    case UTF8PROC_CATEGORY_MN: case UTF8PROC_CATEGORY_ME: case UTF8PROC_CATEGORY_CF:
    case UTF8PROC_CATEGORY_LM: case UTF8PROC_CATEGORY_SK:
      return true;
    default:
      return false;
  }
}

static bool utf8_cased(utf8proc_int32_t cp) {
  utf8proc_category_t category = utf8proc_category(cp);
  if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT) return true;
  return utf8proc_tolower(cp) != cp || utf8proc_toupper(cp) != cp;
}

static bool utf8_sigma_is_final(const utf8proc_uint8_t *src, utf8proc_ssize_t len, utf8proc_ssize_t pos, utf8proc_ssize_t n) {
  bool cased_before = false;
  for (utf8proc_ssize_t i = pos; i > 0;) {
    utf8proc_ssize_t start = i - 1;
    while (start > 0 && (src[start] & 0xC0) == 0x80) start--;
    utf8proc_int32_t cp;
    utf8_next(src + start, i - start, &cp);
    i = start;
    if (cp < 0) break;
    if (utf8_case_ignorable(cp)) continue;
    cased_before = utf8_cased(cp);
    break;
  }
  if (!cased_before) return false;

  for (utf8proc_ssize_t i = pos + n; i < len;) {
    utf8proc_int32_t cp;
    i += utf8_next(src + i, len - i, &cp);
    if (cp < 0) return true;
    if (utf8_case_ignorable(cp)) continue;
    return !utf8_cased(cp);
  }
  return true;
}

int utf8_full_case_map(
  const utf8proc_uint8_t *src, utf8proc_ssize_t len, utf8proc_ssize_t pos,
  utf8proc_ssize_t n, utf8proc_int32_t cp, bool upper, utf8proc_int32_t out[3]
) {
  if (upper) {
    if (cp >= 0xDF) {
      size_t lo = 0, hi = sizeof(utf8_special_upper) / sizeof(utf8_special_upper[0]);
      while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (utf8_special_upper[mid].cp < (uint32_t)cp) lo = mid + 1;
        else hi = mid;
      }
      if (lo < sizeof(utf8_special_upper) / sizeof(utf8_special_upper[0]) && utf8_special_upper[lo].cp == (uint32_t)cp) {
        const utf8_special_case_t *entry = &utf8_special_upper[lo];
        for (int i = 0; i < entry->count; i++) out[i] = (utf8proc_int32_t)entry->mapped[i];
        return entry->count;
      }
    }
    out[0] = utf8proc_toupper(cp);
    return 1;
  }

  if (cp == 0x0130) {
    out[0] = 0x0069;
    out[1] = 0x0307;
    return 2;
  }
  if (cp == 0x03A3) {
    out[0] = utf8_sigma_is_final(src, len, pos, n) ? 0x03C2 : 0x03C3;
    return 1;
  }
  out[0] = utf8proc_tolower(cp);
  return 1;
}

size_t wtf8_join_surrogate_pairs(char *bytes, size_t len) {
  unsigned char *s = (unsigned char *)bytes;
  size_t r = 0, w = 0;
  
  for (;;) {
    unsigned char *hit = r + 6 <= len ? (unsigned char *)wtf8_find_surrogate_lead(s + r, s + len - 4) : NULL;
    size_t next = hit ? (size_t)(hit - s) : len;
    
    if (w != r) memmove(s + w, s + r, next - r);
    w += next - r;
    r = next;
    if (!hit) return w;
    
    bool pair = (s[r + 1] & 0xF0) == 0xA0 && s[r + 3] == 0xED && (s[r + 4] & 0xF0) == 0xB0;
    if (!pair) {
      s[w++] = s[r++];
      continue;
    }
    
    uint32_t hi = ((uint32_t)(s[r + 1] & 0x0F) << 6) | (s[r + 2] & 0x3F);
    uint32_t lo = ((uint32_t)(s[r + 4] & 0x0F) << 6) | (s[r + 5] & 0x3F);
    uint32_t cp = 0x10000 + (hi << 10) + lo;
    
    s[w++] = (unsigned char)(0xF0 | (cp >> 18));
    s[w++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    s[w++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    s[w++] = (unsigned char)(0x80 | (cp & 0x3F));
    r += 6;
  }
}

bool wtf8_needle_splits_pairs(const char *needle, size_t len) {
  const unsigned char *s = (const unsigned char *)needle;
  if (len < 3) return false;
  if (s[0] == 0xED && s[1] >= 0xB0) return true;
  return s[len - 3] == 0xED && (s[len - 2] & 0xF0) == 0xA0;
}

bool wtf8_has_supplementary(const char *str, size_t len) {
  const unsigned char *s = (const unsigned char *)str;
  for (size_t i = 0; i < len; i++) if (s[i] >= 0xF0) return true;
  return false;
}

char *wtf8_split_pairs_dup(const char *str, size_t len, size_t *out_len) {
  const unsigned char *s = (const unsigned char *)str;
  char *out = malloc(len + len / 2 + 1);
  if (!out) return NULL;

  size_t w = 0;
  for (size_t r = 0; r < len;) {
    if (s[r] < 0xF0 || r + 4 > len) {
      out[w++] = (char)s[r++];
      continue;
    }
    uint32_t cp = ((uint32_t)(s[r] & 0x07) << 18) | ((uint32_t)(s[r + 1] & 0x3F) << 12) |
      ((uint32_t)(s[r + 2] & 0x3F) << 6) | (uint32_t)(s[r + 3] & 0x3F);
    uint32_t units[2] = { 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 0x3FF) };
    for (int i = 0; i < 2; i++) {
      out[w++] = (char)0xED;
      out[w++] = (char)(0x80 | ((units[i] >> 6) & 0x3F));
      out[w++] = (char)(0x80 | (units[i] & 0x3F));
    }
    r += 4;
  }

  out[w] = '\0';
  *out_len = w;
  return out;
}

int wtf8_compare_utf16_slow(const char *a, size_t na, const char *b, size_t nb) {
  utf16_iterator_t ia = utf16_iterator(a, na);
  utf16_iterator_t ib = utf16_iterator(b, nb);
  for (;;) {
    uint16_t ua, ub;
    bool ha = utf16_next_code_unit(&ia, &ua);
    bool hb = utf16_next_code_unit(&ib, &ub);
    if (!ha || !hb) return (int)ha - (int)hb;
    if (ua != ub) return ua < ub ? -1 : 1;
  }
}
