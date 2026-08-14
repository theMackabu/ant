#include "utf8.h"
#include "utils.h"
#include "internal.h"
#include "gc/objects.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  uint64_t epoch;
  const char *str;
  size_t byte_len;
  size_t byte_pos;
  size_t utf16_pos;
} utf16_scan_cache_t;

typedef struct {
  const char *str;
  size_t byte_len;
  const unsigned char *start;
  const unsigned char *end;
  const unsigned char *p;
  size_t utf16_pos;
} utf16_scan_cursor_t;

static _Thread_local utf16_scan_cache_t utf16_scan_cache = { 0 };

static inline void utf16_scan_cache_sync_epoch(void) {
  uint64_t epoch = gc_get_epoch();
  if (utf16_scan_cache.epoch == epoch) return;
  utf16_scan_cache = (utf16_scan_cache_t){ .epoch = epoch };
}

static inline void utf16_scan_cursor_init(
  utf16_scan_cursor_t *cursor,
  const char *str,
  size_t byte_len
) {
  utf16_scan_cache_sync_epoch();
  cursor->str = str;
  cursor->byte_len = byte_len;
  cursor->start = (const unsigned char *)str;
  cursor->end = cursor->start + byte_len;
  cursor->p = cursor->start;
  cursor->utf16_pos = 0;
}

static inline bool utf16_scan_cache_matches(const utf16_scan_cursor_t *cursor) {
  return utf16_scan_cache.str == cursor->str
    && utf16_scan_cache.byte_pos <= cursor->byte_len;
}

static inline void utf16_scan_cursor_resume_utf16(
  utf16_scan_cursor_t *cursor,
  size_t target_utf16
) {
  if (!utf16_scan_cache_matches(cursor)) return;
  if (target_utf16 < utf16_scan_cache.utf16_pos) return;
  cursor->p = cursor->start + utf16_scan_cache.byte_pos;
  cursor->utf16_pos = utf16_scan_cache.utf16_pos;
}

static inline void utf16_scan_cursor_resume_byte(
  utf16_scan_cursor_t *cursor,
  size_t target_byte
) {
  if (!utf16_scan_cache_matches(cursor)) return;
  if (target_byte < utf16_scan_cache.byte_pos) return;
  cursor->p = cursor->start + utf16_scan_cache.byte_pos;
  cursor->utf16_pos = utf16_scan_cache.utf16_pos;
}

static inline void utf16_scan_cursor_store(const utf16_scan_cursor_t *cursor) {
  utf16_scan_cache.str = cursor->str;
  utf16_scan_cache.byte_len = cursor->byte_len;
  utf16_scan_cache.byte_pos = (size_t)(cursor->p - cursor->start);
  utf16_scan_cache.utf16_pos = cursor->utf16_pos;
}

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

static constexpr size_t UTF16_INDEX_CHUNK = 64;
static constexpr size_t UTF16_INDEX_WAYS = 8;
static constexpr size_t UTF16_INDEX_MIN_BYTES = 256;

typedef struct {
  uint32_t byte_off;
  uint32_t utf16_pos;
} utf16_index_point_t;

typedef struct {
  const char *str;
  size_t byte_len;
  utf16_index_point_t *points;
  size_t point_count;
} utf16_index_entry_t;

static _Thread_local struct {
  uint64_t epoch;
  size_t victim;
  utf16_index_entry_t entries[UTF16_INDEX_WAYS];
} utf16_index_cache;

static void utf16_index_sync_epoch(void) {
  uint64_t epoch = gc_get_epoch();
  if (utf16_index_cache.epoch == epoch) return;

  for (size_t i = 0; i < UTF16_INDEX_WAYS; i++) {
    free(utf16_index_cache.entries[i].points);
    utf16_index_cache.entries[i] = (utf16_index_entry_t){0};
  }

  utf16_index_cache.victim = 0;
  utf16_index_cache.epoch = epoch;
}

static const utf16_index_entry_t *utf16_index_get(const char *str, size_t byte_len) {
  if (byte_len < UTF16_INDEX_MIN_BYTES || byte_len > UINT32_MAX) return NULL;

  utf16_index_sync_epoch();
  for (size_t i = 0; i < UTF16_INDEX_WAYS; i++) {
    utf16_index_entry_t *entry = &utf16_index_cache.entries[i];
    if (entry->str == str && entry->byte_len == byte_len) return entry;
  }

  size_t max_points = byte_len / UTF16_INDEX_CHUNK + 2;
  utf16_index_point_t *points = malloc(max_points * sizeof(*points));
  if (!points) return NULL;

  const unsigned char *start = (const unsigned char *)str;
  const unsigned char *end = start + byte_len;
  const unsigned char *p = start;

  size_t utf16_pos = 0;
  size_t point_count = 0;
  points[point_count++] = (utf16_index_point_t){0, 0};

  while (p < end) {
    size_t slen, units;
    utf16_scan_decode(p, end, &slen, &units, NULL);
    
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

  utf16_index_entry_t *entry = &utf16_index_cache.entries[utf16_index_cache.victim];
  utf16_index_cache.victim = (utf16_index_cache.victim + 1) % UTF16_INDEX_WAYS;
  free(entry->points);
  
  *entry = (utf16_index_entry_t){
    .str = str,
    .byte_len = byte_len,
    .points = points,
    .point_count = point_count,
  };
  
  return entry;
}

static inline void utf16_index_seek(
  const utf16_index_entry_t *index,
  utf16_scan_cursor_t *cursor,
  size_t target
) {
  size_t point = target / UTF16_INDEX_CHUNK;
  if (point >= index->point_count) point = index->point_count - 1;
  while (point > 0 && index->points[point].utf16_pos > target) point--;

  const utf16_index_point_t *checkpoint = &index->points[point];
  if (checkpoint->utf16_pos <= cursor->utf16_pos) return;
  cursor->p = cursor->start + checkpoint->byte_off;
  cursor->utf16_pos = checkpoint->utf16_pos;
}

static inline bool utf16_scan_cursor_advance(
  utf16_scan_cursor_t *cursor,
  const unsigned char *bound_end
) {
  size_t slen, units;
  const unsigned char *next;

  utf16_scan_decode(cursor->p, cursor->end, &slen, &units, NULL);
  next = cursor->p + slen;
  cursor->utf16_pos += units;
  
  if (next > bound_end) {
    cursor->p = bound_end;
    return false;
  }
  
  cursor->p = next;
  return true;
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

size_t utf8_strlen(const char *str, size_t byte_len) {
  size_t count = 0;
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  while (p < end) {
    int seq_len = utf8_sequence_length(*p);
    if (seq_len <= 0 || (size_t)seq_len > (size_t)(end - p)) {
      count++; p++;
    } else { count++; p += seq_len; }
  }
  return count;
}

size_t utf16_strlen(const char *str, size_t byte_len) {
  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);

  while (cursor.p < cursor.end)
    utf16_scan_cursor_advance(&cursor, cursor.end);

  return cursor.utf16_pos;
}

bool utf8_validate_bytes(const char *str, size_t len) {
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
    else if (c == 0xed) { cont = 2; hi = 0x9f; }
    else if (c == 0xee || c == 0xef) cont = 2;
    else if (c == 0xf0) { cont = 3; lo = 0x90; }
    else if (c >= 0xf1 && c <= 0xf3) cont = 3;
    else if (c == 0xf4) { cont = 3; hi = 0x8f; }
    else return false;

    if ((size_t)(end - s) <= cont) return false;
    if (s[1] < lo || s[1] > hi) return false;
    for (size_t i = 2; i <= cont; i++) {
      if (s[i] < 0x80 || s[i] > 0xbf) return false;
    }
    s += cont + 1;
  }
  return true;
}

static bool utf8_wtf8_surrogate_at(
  const uint8_t *str, size_t len, size_t pos, uint16_t *out
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

int utf16_index_to_byte_offset(
  const char *str,
  size_t byte_len,
  size_t utf16_idx,
  size_t *out_char_bytes
) {
  if (str_is_ascii(str)) {
    if (utf16_idx > byte_len) return -1;
    if (out_char_bytes) *out_char_bytes = (utf16_idx < byte_len) ? 1 : 0;
    return (int)utf16_idx;
  }

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_utf16(&cursor, utf16_idx);
  
  while (cursor.p < cursor.end && cursor.utf16_pos < utf16_idx) {
    utf16_scan_cursor_advance(&cursor, cursor.end);
  }
  
  if (cursor.p >= cursor.end) {
    if (cursor.utf16_pos == utf16_idx) {
      if (out_char_bytes) *out_char_bytes = 0;
      utf16_scan_cursor_store(&cursor);
      return (int)byte_len;
    }
    utf16_scan_cursor_store(&cursor);
    return -1;
  }
  
  size_t slen, units;
  utf16_scan_decode(cursor.p, cursor.end, &slen, &units, NULL);
    
  if (out_char_bytes) *out_char_bytes = slen;
  utf16_scan_cursor_store(&cursor);
  return (int)(cursor.p - cursor.start);
}

size_t utf16_index_to_byte_offset_floor(
  const char *str,
  size_t byte_len,
  size_t utf16_idx
) {
  if (str_is_ascii(str)) {
    if (utf16_idx > byte_len) return byte_len;
    return utf16_idx;
  }

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_utf16(&cursor, utf16_idx);

  while (cursor.p < cursor.end && cursor.utf16_pos < utf16_idx) {
    size_t slen, units;
    utf16_scan_decode(cursor.p, cursor.end, &slen, &units, NULL);
    if (cursor.utf16_pos + units > utf16_idx) break;
    cursor.p += slen;
    cursor.utf16_pos += units;
  }

  utf16_scan_cursor_store(&cursor);
  return (size_t)(cursor.p - cursor.start);
}

int utf16_range_to_byte_range(
  const char *str,
  size_t byte_len,
  size_t utf16_start,
  size_t utf16_end,
  size_t *byte_start,
  size_t *byte_end
) {
  if (str_is_ascii(str)) {
    *byte_start = (utf16_start <= byte_len) ? utf16_start : byte_len;
    *byte_end = (utf16_end <= byte_len) ? utf16_end : byte_len;
    return 0;
  }

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_utf16(&cursor, utf16_start);

  size_t b_start = 0, b_end = byte_len;
  int found_start = 0, found_end = 0;
  
  while (cursor.p < cursor.end) {
    if (cursor.utf16_pos == utf16_start) {
      b_start = (size_t)(cursor.p - cursor.start);
      found_start = 1;
    }
    if (cursor.utf16_pos == utf16_end) {
      b_end = (size_t)(cursor.p - cursor.start);
      found_end = 1;
      break;
    }
    utf16_scan_cursor_advance(&cursor, cursor.end);
  }
  
  if (!found_start && utf16_start >= cursor.utf16_pos) b_start = byte_len;
  if (!found_end && utf16_end >= cursor.utf16_pos) b_end = byte_len;
  
  *byte_start = b_start;
  *byte_end = b_end;
  utf16_scan_cursor_store(&cursor);
  
  return 0;
}

size_t byte_offset_to_utf16(const char *str, size_t byte_off) {
  if (str_is_ascii(str)) return byte_off;

  utf16_scan_cursor_t cursor;
  const unsigned char *bound_end;
  bool ended_on_boundary = true;

  utf16_scan_cursor_init(&cursor, str, byte_off);
  utf16_scan_cursor_resume_byte(&cursor, byte_off);
  bound_end = cursor.start + byte_off;

  while (cursor.p < bound_end) {
    if (!utf16_scan_cursor_advance(&cursor, bound_end)) {
      ended_on_boundary = false;
      break;
    }
  }

  if (ended_on_boundary) utf16_scan_cursor_store(&cursor);
  return cursor.utf16_pos;
}

uint32_t utf16_code_unit_at(const char *str, size_t byte_len, size_t utf16_idx) {
  if (str_is_ascii(str)) {
    if (utf16_idx >= byte_len) return 0xFFFFFFFF;
    return (unsigned char)str[utf16_idx];
  }

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_utf16(&cursor, utf16_idx);
  
  if (utf16_idx > cursor.utf16_pos && utf16_idx - cursor.utf16_pos > UTF16_INDEX_CHUNK) {
    const utf16_index_entry_t *index = utf16_index_get(str, byte_len);
    if (index) utf16_index_seek(index, &cursor, utf16_idx);
  }

  while (cursor.p < cursor.end) {
    size_t slen, units;
    uint32_t cp;
    
    utf16_scan_decode(cursor.p, cursor.end, &slen, &units, &cp);
    
    if (cursor.utf16_pos == utf16_idx) {
      utf16_scan_cursor_store(&cursor);
      if (units == 2) return 0xD800 + ((cp - 0x10000) >> 10);
      return cp;
    }
    if (units == 2 && cursor.utf16_pos + 1 == utf16_idx) {
      utf16_scan_cursor_store(&cursor);
      return 0xDC00 + ((cp - 0x10000) & 0x3FF);
    }
    cursor.p += slen;
    cursor.utf16_pos += units;
  }
  
  utf16_scan_cursor_store(&cursor);
  return 0xFFFFFFFF;
}

utf8proc_ssize_t utf8_whatwg_decode(
  utf8_dec_t *dec, const uint8_t *src, size_t len,
  char *out, bool fatal, bool stream
) {
  static const void *tbl[256] = {
    [0x00 ... 0x7F] = &&L_ASCII,
    [0x80 ... 0xBF] = &&L_LONE,
    [0xC0 ... 0xC1] = &&L_BAD,
    [0xC2 ... 0xDF] = &&L_2,
    [0xE0]          = &&L_E0,
    [0xE1 ... 0xEC] = &&L_3,
    [0xED]          = &&L_ED,
    [0xEE ... 0xEF] = &&L_3,
    [0xF0]          = &&L_F0,
    [0xF1 ... 0xF3] = &&L_4,
    [0xF4]          = &&L_F4,
    [0xF5 ... 0xFF] = &&L_BAD,
  };

  size_t i = 0, o = 0;
  int bc = 0;
  
  uint8_t lo = 0x80, hi = 0xBF;
  utf8proc_int32_t cp = 0;
  uint8_t pb[4]; int pp = 0;

#define FFFD() do { out[o++]=(char)0xEF; out[o++]=(char)0xBF; out[o++]=(char)0xBD; } while(0)
#define NEXT() do { i++; if (i < len) goto *tbl[src[i]]; goto done; } while(0)

  if (!len) goto done;
  goto *tbl[src[0]];

L_ASCII:
  dec->bom_seen = true;
  out[o++] = (char)src[i];
  NEXT();

L_LONE:
L_BAD:
  if (fatal) return -1;
  FFFD(); dec->bom_seen = true;
  NEXT();

L_E0: bc=2; lo=0xA0; hi=0xBF; cp=src[i]&0x0F; pb[0]=src[i]; pp=1; i++; goto cont;
L_ED: bc=2; lo=0x80; hi=0x9F; cp=src[i]&0x0F; pb[0]=src[i]; pp=1; i++; goto cont;
L_3:  bc=2; lo=0x80; hi=0xBF; cp=src[i]&0x0F; pb[0]=src[i]; pp=1; i++; goto cont;
L_F0: bc=3; lo=0x90; hi=0xBF; cp=src[i]&0x07; pb[0]=src[i]; pp=1; i++; goto cont;
L_F4: bc=3; lo=0x80; hi=0x8F; cp=src[i]&0x07; pb[0]=src[i]; pp=1; i++; goto cont;
L_4:  bc=3; lo=0x80; hi=0xBF; cp=src[i]&0x07; pb[0]=src[i]; pp=1; i++; goto cont;
L_2:  bc=1; lo=0x80; hi=0xBF; cp=src[i]&0x1F; pb[0]=src[i]; pp=1; i++; goto cont;

cont:
  while (bc > 0) {
    if (i >= len) {
      if (stream) { dec->pend_pos = pp; memcpy(dec->pend_buf, pb, pp); }
      else { if (fatal) return -1; FFFD(); }
      goto done;
    }
    uint8_t b = src[i];
    if (b < lo || b > hi) {
      bc = 0; cp = 0; pp = 0;
      if (fatal) return -1;
      FFFD(); dec->bom_seen = true;
      goto *tbl[b];
    }
    lo = 0x80; hi = 0xBF;
    cp = (cp << 6) | (b & 0x3F);
    pb[pp++] = b; bc--; i++;
  }
  pp = 0;
  if (!dec->bom_seen && cp == 0xFEFF && !dec->ignore_bom) dec->bom_seen = true;
  else {
    dec->bom_seen = true;
    utf8proc_ssize_t n = utf8proc_encode_char(cp, (utf8proc_uint8_t *)(out + o));
    if (n > 0) o += (size_t)n;
  }
  cp = 0;
  if (i < len) goto *tbl[src[i]];

done:
#undef FFFD
#undef NEXT
  return (utf8proc_ssize_t)o;
}

uint32_t utf16_codepoint_at(const char *str, size_t byte_len, size_t utf16_idx) {
  if (str_is_ascii(str)) {
    if (utf16_idx >= byte_len) return 0xFFFFFFFF;
    return (unsigned char)str[utf16_idx];
  }

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_utf16(&cursor, utf16_idx);
  
  while (cursor.p < cursor.end) {
    size_t slen, units;
    uint32_t cp;
    
    utf16_scan_decode(cursor.p, cursor.end, &slen, &units, &cp);
    
    if (cursor.utf16_pos == utf16_idx) {
      utf16_scan_cursor_store(&cursor);
      return cp;
    }
    if (units == 2 && cursor.utf16_pos + 1 == utf16_idx) {
      utf16_scan_cursor_store(&cursor);
      return 0xDC00 + ((cp - 0x10000) & 0x3FF);
    }
    
    cursor.p += slen;
    cursor.utf16_pos += units;
  }
  
  utf16_scan_cursor_store(&cursor);
  return 0xFFFFFFFF;
}
