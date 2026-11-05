#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unicode.h"
#include "unicode_data.h"

static constexpr int32_t HANGUL_SBASE = 0xAC00;
static constexpr int32_t HANGUL_LBASE = 0x1100;
static constexpr int32_t HANGUL_VBASE = 0x1161;
static constexpr int32_t HANGUL_TBASE = 0x11A7;

static constexpr int32_t HANGUL_LCOUNT = 19;
static constexpr int32_t HANGUL_VCOUNT = 21;
static constexpr int32_t HANGUL_TCOUNT = 28;

static constexpr int32_t HANGUL_NCOUNT = HANGUL_VCOUNT * HANGUL_TCOUNT;
static constexpr int32_t HANGUL_SCOUNT = HANGUL_LCOUNT * HANGUL_NCOUNT;

static const unicode_record_t *unicode_record(int32_t cp) {
  if ((uint32_t)cp >= 0x110000) return &unicode_records[0];
  
  uint32_t mid = (uint32_t)unicode_top[(uint32_t)cp >> (UNICODE_LEAF_SHIFT + UNICODE_MID_SHIFT)] << UNICODE_MID_SHIFT;
  uint32_t leaf = unicode_mid[mid | (((uint32_t)cp >> UNICODE_LEAF_SHIFT) & ((1u << UNICODE_MID_SHIFT) - 1))];
  
  return &unicode_records[unicode_leaf[leaf << UNICODE_LEAF_SHIFT | ((uint32_t)cp & ((1u << UNICODE_LEAF_SHIFT) - 1))]];
}

utf8proc_category_t unicode_category(int32_t cp) {
  if ((uint32_t)cp >= 0x110000) return UTF8PROC_CATEGORY_CN;
  return (utf8proc_category_t)unicode_record(cp)->category;
}

static const uint32_t id_start_ranges[][2] = {
#define UNICODE_ID_START(lo, hi) { lo, hi },
#include "unicode_id.h"
};

static const uint32_t id_continue_ranges[][2] = {
#define UNICODE_ID_CONTINUE(lo, hi) { lo, hi },
#include "unicode_id.h"
};

static bool unicode_in_ranges(uint32_t c, const uint32_t (*ranges)[2], size_t count) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c < ranges[mid][0]) hi = mid;
    else if (c > ranges[mid][1]) lo = mid + 1;
    else return true;
  }
  return false;
}

bool unicode_is_id_start(uint32_t c) {
  if (c == '$' || c == '_') return true;
  return unicode_in_ranges(c, id_start_ranges, sizeof(id_start_ranges) / sizeof(id_start_ranges[0]));
}

bool unicode_is_id_continue(uint32_t c) {
  if (c == '$' || c == 0x200C || c == 0x200D) return true;
  return unicode_in_ranges(c, id_continue_ranges, sizeof(id_continue_ranges) / sizeof(id_continue_ranges[0]));
}

int unicode_width(int32_t cp) {
  return (uint32_t)cp >= 0x110000 ? 1 : unicode_record(cp)->width;
}

int32_t unicode_toupper(int32_t cp) {
  return (uint32_t)cp >= 0x110000 ? cp : cp + unicode_record(cp)->upper;
}

int32_t unicode_tolower(int32_t cp) {
  return (uint32_t)cp >= 0x110000 ? cp : cp + unicode_record(cp)->lower;
}

static int unicode_find(const uint32_t *keys, int count, uint32_t key) {
  int lo = 0, hi = count;
  
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (keys[mid] < key) lo = mid + 1;
    else hi = mid;
  }
  
  return lo < count && keys[lo] == key ? lo : -1;
}

typedef struct {
  int32_t *items;
  size_t len, cap;
} cp_buf_t;

static bool cp_buf_push(cp_buf_t *b, int32_t cp) {
  if (b->len == b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    int32_t *items = realloc(b->items, cap * sizeof(*items));
    if (!items) return false;
    b->items = items;
    b->cap = cap;
  }
  b->items[b->len++] = cp;
  return true;
}

static bool unicode_push_pool(cp_buf_t *b, uint16_t from, uint16_t to) {
  for (uint32_t i = from; i < to; i++) {
    int32_t c = unicode_decomp_pool[i];
    if (c >= 0xD800 && c < 0xDC00) c = 0x10000 + ((c - 0xD800) << 10) + (unicode_decomp_pool[++i] - 0xDC00);
    if (!cp_buf_push(b, c)) return false;
  }
  return true;
}

static bool unicode_decompose_into(cp_buf_t *b, int32_t cp, bool compat) {
  int32_t s = cp - HANGUL_SBASE;
  
  if (s >= 0 && s < HANGUL_SCOUNT) {
    int32_t t = s % HANGUL_TCOUNT;
    if (!cp_buf_push(b, HANGUL_LBASE + s / HANGUL_NCOUNT)) return false;
    if (!cp_buf_push(b, HANGUL_VBASE + (s % HANGUL_NCOUNT) / HANGUL_TCOUNT)) return false;
    return !t || cp_buf_push(b, HANGUL_TBASE + t);
  }
  
  uint8_t flags = unicode_record(cp)->flags;
  if (compat && (flags & UNICODE_FLAG_COMPAT)) {
    int i = unicode_find(unicode_compat_cp, (int)(sizeof(unicode_compat_cp) / sizeof(*unicode_compat_cp)), (uint32_t)cp);
    return unicode_push_pool(b, unicode_compat_off[i], unicode_compat_off[i + 1]);
  }
  
  if (flags & UNICODE_FLAG_CANON) {
    int i = unicode_find(unicode_canon_cp, (int)(sizeof(unicode_canon_cp) / sizeof(*unicode_canon_cp)), (uint32_t)cp);
    return unicode_push_pool(b, unicode_canon_off[i], unicode_canon_off[i + 1]);
  }
  
  return cp_buf_push(b, cp);
}

static int32_t unicode_compose_pair(int32_t starter, int32_t second) {
  int s = unicode_find(unicode_comp_second, (int)(sizeof(unicode_comp_second) / sizeof(*unicode_comp_second)), (uint32_t)second);
  if (s < 0) return -1;
  int i = unicode_find(unicode_comp_key, (int)(sizeof(unicode_comp_key) / sizeof(*unicode_comp_key)), (uint32_t)starter << 8 | (uint32_t)s);
  return i < 0 ? -1 : (int32_t)unicode_comp_value[i];
}

static int32_t unicode_try_compose(int32_t starter, int32_t cur, const unicode_record_t *rec) {
  int32_t l = starter - HANGUL_LBASE, v = cur - HANGUL_VBASE;
  if (l >= 0 && l < HANGUL_LCOUNT && v >= 0 && v < HANGUL_VCOUNT)
    return HANGUL_SBASE + (l * HANGUL_VCOUNT + v) * HANGUL_TCOUNT;

  int32_t s = starter - HANGUL_SBASE, t = cur - HANGUL_TBASE;
  if (s >= 0 && s < HANGUL_SCOUNT && s % HANGUL_TCOUNT == 0 && t > 0 && t < HANGUL_TCOUNT) return starter + t;

  if (!(rec->flags & UNICODE_FLAG_COMP_SECOND)) return -1;
  if (!(unicode_record(starter)->flags & UNICODE_FLAG_COMP_FIRST)) return -1;
  return unicode_compose_pair(starter, cur);
}

static size_t unicode_compose(int32_t *buf, size_t len) {
  int32_t *starter = NULL;
  int max_ccc = -1;
  size_t w = 0;

  for (size_t r = 0; r < len; r++) {
    int32_t cur = buf[r];
    const unicode_record_t *rec = unicode_record(cur);
    int32_t composed = starter && (int)rec->ccc > max_ccc ? unicode_try_compose(*starter, cur, rec) : -1;
    if (composed >= 0) {
      *starter = composed;
      continue;
    }

    buf[w] = cur;
    if (!rec->ccc) {
      starter = buf + w;
      max_ccc = -1;
    } else if ((int)rec->ccc > max_ccc) max_ccc = rec->ccc;
    w++;
  }

  return w;
}

static void unicode_reorder(int32_t *buf, size_t len) {
  size_t pos = 0;
  while (pos + 1 < len) {
    uint8_t a = unicode_record(buf[pos])->ccc, b = unicode_record(buf[pos + 1])->ccc;
    if (a > b && b > 0) {
      int32_t t = buf[pos];
      buf[pos] = buf[pos + 1];
      buf[pos + 1] = t;
      if (pos > 0) pos--;
      else pos++;
    } else pos++;
  }
}

static int unicode_decode(const unsigned char *s, size_t len, size_t i, int32_t *cp) {
  unsigned char c = s[i];
  if (c < 0x80) { *cp = c; return 1; }
  int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
  if (!n || n > 4 || i + (size_t)n > len) return 0;
  int32_t v = c & (0x7F >> n);
  for (int k = 1; k < n; k++) {
    if ((s[i + k] & 0xC0) != 0x80) return 0;
    v = v << 6 | (s[i + k] & 0x3F);
  }
  static const int32_t min[5] = { 0, 0, 0x80, 0x800, 0x10000 };
  if (v < min[n] || v > 0x10FFFF) return 0;
  *cp = v;
  return n;
}

static size_t unicode_encode(int32_t cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | cp >> 6);
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | cp >> 18);
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

bool unicode_is_normalized(const char *str, size_t len, unicode_form_t form) {
  static const uint8_t unstable[4] = {
    UNICODE_FLAG_NFC_UNSTABLE, UNICODE_FLAG_NFD_UNSTABLE, UNICODE_FLAG_NFKC_UNSTABLE, UNICODE_FLAG_NFKD_UNSTABLE,
  };
  const unsigned char *s = (const unsigned char *)str;
  uint8_t mask = unstable[form];
  for (size_t i = 0; i < len;) {
    if (s[i] < 0x80) { i++; continue; }
    int32_t cp;
    int n = unicode_decode(s, len, i, &cp);
    if (!n || (unicode_record(cp)->flags & mask)) return false;
    i += (size_t)n;
  }
  return true;
}

char *unicode_normalize(const char *str, size_t len, unicode_form_t form, size_t *out_len) {
  bool compat = form == UNICODE_NFKC || form == UNICODE_NFKD;
  bool compose = form == UNICODE_NFC || form == UNICODE_NFKC;
  const unsigned char *s = (const unsigned char *)str;
  cp_buf_t buf = {0};

  for (size_t i = 0; i < len;) {
    int32_t cp;
    int n = unicode_decode(s, len, i, &cp);
    if (!n || !unicode_decompose_into(&buf, cp, compat)) {
      free(buf.items);
      return NULL;
    }
    i += (size_t)n;
  }

  unicode_reorder(buf.items, buf.len);
  size_t count = compose ? unicode_compose(buf.items, buf.len) : buf.len;

  char *out = malloc(count * 4 + 1);
  if (!out) {
    free(buf.items);
    return NULL;
  }
  
  size_t w = 0;
  for (size_t i = 0; i < count; i++) w += unicode_encode(buf.items[i], out + w);
  out[w] = '\0';
  
  free(buf.items);
  *out_len = w;
  
  return out;
}
