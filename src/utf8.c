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

static inline void utf16_scan_cursor_resume_cached(utf16_scan_cursor_t *cursor) {
  if (!utf16_scan_cache_matches(cursor)) return;
  cursor->p = cursor->start + utf16_scan_cache.byte_pos;
  cursor->utf16_pos = utf16_scan_cache.utf16_pos;
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

static uint32_t utf8_decode(const unsigned char *buf, size_t len, int *seq_len) {
  if (len == 0) { *seq_len = 0; return 0; }
  utf8proc_int32_t cp;
  *seq_len = (int)utf8_next(buf, (utf8proc_ssize_t)len, &cp);
  return cp < 0 ? 0xFFFD : (uint32_t)cp;
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

char *utf8_json_quote(const char *str, size_t byte_len, size_t *out_len) {
  size_t utf16_len = utf16_strlen(str, byte_len);
  size_t raw_len = 0;
  size_t raw_cap = byte_len + 4;
  
  char *raw = malloc(raw_cap);
  if (!raw) return NULL;

  if (!utf8_json_quote_append_char(&raw, &raw_len, &raw_cap, '"')) goto oom;

  for (size_t i = 0; i < utf16_len; i++) {
    uint32_t cu = utf16_code_unit_at(str, byte_len, i);
    
    if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < utf16_len) {
    uint32_t cu2 = utf16_code_unit_at(str, byte_len, i + 1);
    if (cu2 >= 0xDC00 && cu2 <= 0xDFFF) {
      uint32_t cp = utf16_codepoint_at(str, byte_len, i);
      char utf8[4];
      int n = utf8_encode(cp, utf8);
      if (n <= 0 || !utf8_json_quote_append(&raw, &raw_len, &raw_cap, utf8, (size_t)n)) goto oom;
      i++;
      continue;
    }}
    
    if (cu >= 0xD800 && cu <= 0xDFFF) {
      if (!utf8_json_quote_append_u_escape(&raw, &raw_len, &raw_cap, cu)) goto oom;
      continue;
    }
    
    switch (cu) {
      case '"':  if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\\"", 2)) goto oom; continue;
      case '\\': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\\\", 2)) goto oom; continue;
      case '\b': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\b", 2)) goto oom; continue;
      case '\f': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\f", 2)) goto oom; continue;
      case '\n': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\n", 2)) goto oom; continue;
      case '\r': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\r", 2)) goto oom; continue;
      case '\t': if (!utf8_json_quote_append(&raw, &raw_len, &raw_cap, "\\t", 2)) goto oom; continue;
      default: break;
    }
    
    if (cu < 0x20) {
      if (!utf8_json_quote_append_u_escape(&raw, &raw_len, &raw_cap, cu)) goto oom;
      continue;
    }
    
    char utf8[4];
    int n = utf8_encode(cu, utf8);
    if (n <= 0 || !utf8_json_quote_append(&raw, &raw_len, &raw_cap, utf8, (size_t)n)) goto oom;
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
  if (str_is_ascii(str)) return byte_len;

  utf16_scan_cursor_t cursor;
  utf16_scan_cursor_init(&cursor, str, byte_len);
  utf16_scan_cursor_resume_cached(&cursor);

  while (cursor.p < cursor.end) {
    utf16_scan_cursor_advance(&cursor, cursor.end);
  }

  utf16_scan_cursor_store(&cursor);
  return cursor.utf16_pos;
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
