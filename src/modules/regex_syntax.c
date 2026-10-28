// TODO: cleanup module, make cleaner. MODULARIZE.
#include "modules/regex_syntax.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

enum {
  RX_ERR_NONE,
#define REGEXP_SYNTAX_ERROR(id, msg) RX_ERR_##id,
#include "modules/regex_syntax_errors.h"
  RX_ERR_OOM,
};

static const char *const rx_error_messages[] = {
  NULL,
#define REGEXP_SYNTAX_ERROR(id, msg) msg,
#include "modules/regex_syntax_errors.h"
  NULL,
};

enum {
  RX_PROP_GENERAL_CATEGORY,
  RX_PROP_BINARY,
  RX_PROP_STRING,
  RX_PROP_SCRIPT,
};

typedef struct {
  uint8_t kind;
  const char *name;
} rx_property_t;

static const rx_property_t rx_properties[] = {
#define REGEXP_UNICODE_PROPERTY(kind, name) { RX_PROP_##kind, name },
#include "modules/regex_unicode_properties.h"
};

static constexpr uint32_t RX_END = 0x200000;
static constexpr int RX_MAX_CAPTURES = (1 << 15) - 1;
static constexpr int RX_INFINITY = INT32_MAX;
static constexpr size_t RX_INLINE_UNITS = 256;

enum {
  RX_GROUP_ROOT,
  RX_GROUP_CAPTURE,
  RX_GROUP_PLAIN,
  RX_GROUP_LOOKAHEAD,
  RX_GROUP_LOOKBEHIND,
};

enum {
  RX_TERM_ATOM,
  RX_TERM_LOOKAHEAD,
  RX_TERM_LOOKBEHIND,
};

enum {
  RX_OPERAND_CHAR,
  RX_OPERAND_ESCAPE,
  RX_OPERAND_STRINGS,
  RX_OPERAND_NESTED,
  RX_OPERAND_RANGE,
};

enum {
  RX_NEXT_FIRST,
  RX_NEXT_UNION,
  RX_NEXT_RANGE,
  RX_NEXT_INTERSECTION,
  RX_NEXT_SUBTRACTION,
};

typedef struct {
  uint8_t type;
  bool named;
  uint32_t id;
  uint32_t alt;
  uint32_t name;
  uint32_t name_len;
  uint32_t path;
  uint32_t path_len;
  uint32_t slot;
} rx_group_t;

typedef struct {
  uint32_t name;
  uint32_t name_len;
  uint32_t path;
  uint32_t path_len;
} rx_name_t;

typedef struct {
  bool negated;
  bool strings;
  uint8_t next;
  uint8_t last;
  uint32_t from;
} rx_class_frame_t;

typedef struct {
  uint8_t type;
  bool strings;
  uint32_t value;
} rx_operand_t;

typedef struct {
  const uint32_t *s;
  size_t n;
  size_t pos;
  bool unicode;
  bool sets;
  bool has_named;
  bool scanned;
  int err;
  int captures_started;
  int capture_count;
  uint32_t next_id;
  rx_group_t *groups;
  size_t groups_len, groups_cap;
  rx_name_t *names;
  size_t names_len, names_cap;
  rx_name_t *refs;
  size_t refs_len, refs_cap;
  uint32_t *pool;
  size_t pool_len, pool_cap;
  rx_class_frame_t *frames;
  size_t frames_cap;
} rx_t;

static bool rx_reserve(rx_t *p, void **buf, size_t *cap, size_t need, size_t size) {
  if (need <= *cap) return true;
  size_t next = *cap ? *cap * 2 : 16;
  while (next < need) next *= 2;
  void *grown = realloc(*buf, next * size);
  if (!grown) {
    p->err = RX_ERR_OOM;
    return false;
  }
  *buf = grown;
  *cap = next;
  return true;
}

static inline bool rx_fail(rx_t *p, int err) {
  if (!p->err) p->err = err;
  return false;
}

static inline uint32_t rx_cur(const rx_t *p) {
  return p->pos < p->n ? p->s[p->pos] : RX_END;
}

static inline uint32_t rx_next(const rx_t *p) {
  return p->pos + 1 < p->n ? p->s[p->pos + 1] : RX_END;
}

static inline bool rx_has_more(const rx_t *p) {
  return p->pos < p->n;
}

static inline bool rx_has_next(const rx_t *p) {
  return p->pos + 1 < p->n;
}

static inline void rx_advance(rx_t *p) {
  if (p->pos < p->n) p->pos++;
}

static inline void rx_advance2(rx_t *p) {
  rx_advance(p);
  rx_advance(p);
}

static inline bool rx_is_digit(uint32_t c) {
  return c >= '0' && c <= '9';
}

static inline int rx_hex_value(uint32_t c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (int)((c | 0x20) - 'a' + 10);
  return -1;
}

static inline bool rx_is_lead(uint32_t c) {
  return c >= 0xD800 && c <= 0xDBFF;
}

static inline bool rx_is_trail(uint32_t c) {
  return c >= 0xDC00 && c <= 0xDFFF;
}

static inline uint32_t rx_combine(uint32_t lead, uint32_t trail) {
  return 0x10000 + ((lead - 0xD800) << 10) + (trail - 0xDC00);
}

static inline bool rx_in_set(uint32_t c, const char *set) {
  return c && c < 0x80 && strchr(set, (int)c) != NULL;
}

static inline bool rx_is_syntax_char(uint32_t c) {
  return rx_in_set(c, "^$\\.*+?()[]{}|/");
}

static inline bool rx_is_class_set_syntax_char(uint32_t c) {
  return rx_in_set(c, "()[]{}/-\\|");
}

static inline bool rx_is_class_set_reserved_punctuator(uint32_t c) {
  return rx_in_set(c, "&-!#%,:;<=>@`~");
}

static inline bool rx_is_class_set_double_punctuator(const rx_t *p, uint32_t c) {
  return rx_in_set(c, "&!#$%*+,.:;<=>?@^`~") && rx_next(p) == c;
}

static inline bool rx_is_class_escape_char(uint32_t c) {
  return rx_in_set(c, "dDsSwW");
}

static const uint32_t rx_id_start_extra[][2] = {
  { 0x88F, 0x88F }, { 0xC5C, 0xC5C }, { 0xCDC, 0xCDC }, { 0xA7CE, 0xA7CF },
  { 0xA7D2, 0xA7D2 }, { 0xA7D4, 0xA7D4 }, { 0xA7F1, 0xA7F1 }, { 0x10940, 0x10959 },
  { 0x10EC5, 0x10EC7 }, { 0x11DB0, 0x11DDB }, { 0x16EA0, 0x16EB8 }, { 0x16EBB, 0x16ED3 },
  { 0x16FF2, 0x16FF6 }, { 0x187F8, 0x187FF }, { 0x18D09, 0x18D1E }, { 0x18D80, 0x18DF2 },
  { 0x1E6C0, 0x1E6DE }, { 0x1E6E0, 0x1E6E2 }, { 0x1E6E4, 0x1E6E5 }, { 0x1E6E7, 0x1E6ED },
  { 0x1E6F0, 0x1E6F4 }, { 0x1E6FE, 0x1E6FF }, { 0x2B73A, 0x2B73F }, { 0x2CEA2, 0x2CEAD },
  { 0x323B0, 0x33479 },
};

static const uint32_t rx_id_part_extra[][2] = {
  { 0x1ACF, 0x1ADD }, { 0x1AE0, 0x1AEB }, { 0x10EFA, 0x10EFB }, { 0x11B60, 0x11B67 },
  { 0x11DE0, 0x11DE9 }, { 0x1E6E3, 0x1E6E3 }, { 0x1E6E6, 0x1E6E6 }, { 0x1E6EE, 0x1E6EF },
  { 0x1E6F5, 0x1E6F5 },
};

static bool rx_in_ranges(uint32_t c, const uint32_t (*ranges)[2], size_t count) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c < ranges[mid][0]) hi = mid;
    else if (c > ranges[mid][1]) lo = mid + 1;
    else return true;
  }
  return false;
}

static bool rx_id_start(uint32_t c) {
  if (c < 0x80) return c == '$' || c == '_' || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
  if (c > 0x10FFFF) return false;
  switch (c) {
    case 0x1885: case 0x1886: case 0x2118: case 0x212E: case 0x309B: case 0x309C: return true;
    case 0x2E2F: return false;
  }
  if (rx_in_ranges(c, rx_id_start_extra, sizeof(rx_id_start_extra) / sizeof(rx_id_start_extra[0]))) return true;
  switch (utf8proc_category((utf8proc_int32_t)c)) {
    case UTF8PROC_CATEGORY_LU: case UTF8PROC_CATEGORY_LL: case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM: case UTF8PROC_CATEGORY_LO: case UTF8PROC_CATEGORY_NL:
      return true;
    default:
      return false;
  }
}

static bool rx_id_part(uint32_t c) {
  if (c < 0x80) return rx_id_start(c) || rx_is_digit(c);
  if (rx_id_start(c)) return true;
  if (c > 0x10FFFF) return false;
  switch (c) {
    case 0x00B7: case 0x0387: case 0x19DA: case 0x200C: case 0x200D: case 0x30FB: case 0xFF65: return true;
  }
  if (c >= 0x1369 && c <= 0x1371) return true;
  if (rx_in_ranges(c, rx_id_part_extra, sizeof(rx_id_part_extra) / sizeof(rx_id_part_extra[0]))) return true;
  switch (utf8proc_category((utf8proc_int32_t)c)) {
    case UTF8PROC_CATEGORY_MN: case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ND: case UTF8PROC_CATEGORY_PC:
      return true;
    default:
      return false;
  }
}

static bool rx_pool_push(rx_t *p, uint32_t value) {
  if (!rx_reserve(p, (void **)&p->pool, &p->pool_cap, p->pool_len + 1, sizeof(*p->pool))) return false;
  p->pool[p->pool_len++] = value;
  return true;
}

static void rx_scan_captures(rx_t *p, bool in_class) {
  size_t saved = p->pos;
  int count = p->captures_started;
  uint32_t c;
  if (in_class) {
    while ((c = rx_cur(p)) != RX_END) {
      rx_advance(p);
      if (c == '\\') rx_advance(p);
      else if (c == ']') break;
    }
  }
  while ((c = rx_cur(p)) != RX_END) {
    rx_advance(p);
    if (c == '\\') rx_advance(p);
    else if (c == '[') {
      uint32_t d;
      size_t depth = 1;
      while ((d = rx_cur(p)) != RX_END) {
        rx_advance(p);
        if (d == '\\') rx_advance(p);
        else if (d == '[' && p->sets) depth++;
        else if (d == ']' && --depth == 0) break;
      }
    } else if (c == '(') {
      if (rx_cur(p) == '?') {
        rx_advance(p);
        if (rx_cur(p) != '<') continue;
        rx_advance(p);
        if (rx_cur(p) == '=' || rx_cur(p) == '!') continue;
        p->has_named = true;
      }
      count++;
    }
  }
  p->capture_count = count;
  p->scanned = true;
  p->pos = saved;
}

static bool rx_has_named_captures(rx_t *p, bool in_class) {
  if (p->has_named || p->scanned) return p->has_named;
  rx_scan_captures(p, in_class);
  return p->has_named;
}

static bool rx_parse_hex(rx_t *p, int digits, uint32_t *out) {
  size_t start = p->pos;
  uint32_t value = 0;
  for (int i = 0; i < digits; i++) {
    int d = rx_hex_value(rx_cur(p));
    if (d < 0) {
      p->pos = start;
      return false;
    }
    value = value * 16 + (uint32_t)d;
    rx_advance(p);
  }
  *out = value;
  return true;
}

static bool rx_parse_unicode_escape(rx_t *p, uint32_t *out, bool force_unicode) {
  bool unicode = p->unicode || force_unicode;
  if (rx_cur(p) == '{' && unicode) {
    size_t start = p->pos;
    rx_advance(p);
    int d = rx_hex_value(rx_cur(p));
    uint32_t value = 0;
    bool ok = d >= 0;
    while (ok && d >= 0) {
      value = value * 16 + (uint32_t)d;
      if (value > 0x10FFFF) ok = false;
      else {
        rx_advance(p);
        d = rx_hex_value(rx_cur(p));
      }
    }
    if (ok && rx_cur(p) == '}') {
      rx_advance(p);
      *out = value;
      return true;
    }
    p->pos = start;
    return false;
  }
  if (!rx_parse_hex(p, 4, out)) return false;
  if (unicode && rx_is_lead(*out) && rx_cur(p) == '\\') {
    size_t start = p->pos;
    if (rx_next(p) == 'u') {
      rx_advance2(p);
      uint32_t trail;
      if (rx_parse_hex(p, 4, &trail) && rx_is_trail(trail)) {
        *out = rx_combine(*out, trail);
        return true;
      }
    }
    p->pos = start;
  }
  return true;
}

static uint32_t rx_parse_octal(rx_t *p) {
  uint32_t value = rx_cur(p) - '0';
  rx_advance(p);
  if (rx_cur(p) >= '0' && rx_cur(p) <= '7') {
    value = value * 8 + rx_cur(p) - '0';
    rx_advance(p);
    if (value < 32 && rx_cur(p) >= '0' && rx_cur(p) <= '7') {
      value = value * 8 + rx_cur(p) - '0';
      rx_advance(p);
    }
  }
  return value;
}

static uint32_t rx_parse_character_escape(rx_t *p, bool in_class) {
  rx_advance(p);
  uint32_t c = rx_cur(p);
  switch (c) {
    case 'f': rx_advance(p); return '\f';
    case 'n': rx_advance(p); return '\n';
    case 'r': rx_advance(p); return '\r';
    case 't': rx_advance(p); return '\t';
    case 'v': rx_advance(p); return '\v';
    case 'c': {
      uint32_t control = rx_next(p);
      uint32_t letter = control & ~(uint32_t)0x20;
      if (letter >= 'A' && letter <= 'Z') {
        rx_advance2(p);
        return control & 0x1F;
      }
      if (p->unicode) {
        rx_fail(p, RX_ERR_INVALID_UNICODE_ESCAPE);
        return 0;
      }
      if (in_class && (rx_is_digit(control) || control == '_')) {
        rx_advance2(p);
        return control & 0x1F;
      }
      return '\\';
    }
    case '0':
      if (!rx_is_digit(rx_next(p))) {
        rx_advance(p);
        return 0;
      }
      [[fallthrough]];
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
      if (p->unicode) {
        rx_fail(p, RX_ERR_INVALID_DECIMAL_ESCAPE);
        return 0;
      }
      return rx_parse_octal(p);
    case 'x': {
      rx_advance(p);
      uint32_t value;
      if (rx_parse_hex(p, 2, &value)) return value;
      if (p->unicode) {
        rx_fail(p, RX_ERR_INVALID_ESCAPE);
        return 0;
      }
      return 'x';
    }
    case 'u': {
      rx_advance(p);
      uint32_t value;
      if (rx_parse_unicode_escape(p, &value, false)) return value;
      if (p->unicode) {
        rx_fail(p, RX_ERR_INVALID_UNICODE_ESCAPE);
        return 0;
      }
      return 'u';
    }
    default:
      break;
  }
  if (!p->unicode) {
    if (c == 'k' && in_class && rx_has_named_captures(p, true)) {
      rx_fail(p, RX_ERR_INVALID_ESCAPE);
      return 0;
    }
    rx_advance(p);
    return c;
  }
  if (rx_is_syntax_char(c) || (p->sets && in_class && rx_is_class_set_reserved_punctuator(c))) {
    rx_advance(p);
    return c;
  }
  rx_fail(p, RX_ERR_INVALID_ESCAPE);
  return 0;
}

static int rx_property_cmp(const void *key, const void *entry) {
  const rx_property_t *a = key, *b = entry;
  if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
  return strcmp(a->name, b->name);
}

static bool rx_property_known(uint8_t kind, const char *name) {
  rx_property_t key = { kind, name };
  return bsearch(&key, rx_properties, sizeof(rx_properties) / sizeof(rx_properties[0]), sizeof(rx_properties[0]), rx_property_cmp) != NULL;
}

static inline bool rx_is_property_char(uint32_t c) {
  return c < 0x80 && (c == '_' || rx_is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'));
}

static bool rx_parse_property(rx_t *p, bool negated, bool *strings) {
  char name[64], value[64];
  size_t name_len = 0, value_len = 0;
  bool has_value = false;
  *strings = false;
  if (rx_cur(p) != '{') return false;
  for (rx_advance(p); rx_cur(p) != '}' && rx_cur(p) != '='; rx_advance(p)) {
    if (!rx_is_property_char(rx_cur(p)) || !rx_has_next(p)) return false;
    if (name_len + 1 < sizeof(name)) name[name_len] = (char)rx_cur(p);
    name_len++;
  }
  if (rx_cur(p) == '=') {
    has_value = true;
    for (rx_advance(p); rx_cur(p) != '}'; rx_advance(p)) {
      if (!rx_is_property_char(rx_cur(p)) || !rx_has_next(p)) return false;
      if (value_len + 1 < sizeof(value)) value[value_len] = (char)rx_cur(p);
      value_len++;
    }
  }
  rx_advance(p);
  if (name_len >= sizeof(name) || value_len >= sizeof(value)) return false;
  name[name_len] = '\0';
  value[value_len] = '\0';
  if (!has_value) {
    if (rx_property_known(RX_PROP_GENERAL_CATEGORY, name)) return true;
    if (rx_property_known(RX_PROP_BINARY, name)) return true;
    if (p->sets && !negated && rx_property_known(RX_PROP_STRING, name)) {
      *strings = true;
      return true;
    }
    return false;
  }
  if (!strcmp(name, "General_Category") || !strcmp(name, "gc"))
    return rx_property_known(RX_PROP_GENERAL_CATEGORY, value);
  if (!strcmp(name, "Script") || !strcmp(name, "sc") || !strcmp(name, "Script_Extensions") || !strcmp(name, "scx"))
    return rx_property_known(RX_PROP_SCRIPT, value);
  return false;
}

static bool rx_parse_group_name(rx_t *p, uint32_t *name, uint32_t *name_len) {
  *name = (uint32_t)p->pool_len;
  *name_len = 0;
  bool at_start = true;
  for (;;) {
    uint32_t c = rx_cur(p);
    rx_advance(p);
    if (!p->unicode && rx_is_lead(c) && rx_is_trail(rx_cur(p))) {
      c = rx_combine(c, rx_cur(p));
      rx_advance(p);
    }
    if (c == '\\' && rx_cur(p) == 'u') {
      rx_advance(p);
      if (!rx_parse_unicode_escape(p, &c, true)) return rx_fail(p, RX_ERR_INVALID_UNICODE_ESCAPE);
    }
    if (c == '\\') return rx_fail(p, RX_ERR_INVALID_CAPTURE_GROUP_NAME);
    if (at_start) {
      if (!rx_id_start(c)) return rx_fail(p, RX_ERR_INVALID_CAPTURE_GROUP_NAME);
      at_start = false;
    } else if (c == '>') break;
    else if (!rx_id_part(c)) return rx_fail(p, RX_ERR_INVALID_CAPTURE_GROUP_NAME);
    if (!rx_pool_push(p, c)) return false;
    (*name_len)++;
  }
  return true;
}

static bool rx_names_equal(const rx_t *p, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
  return a_len == b_len && memcmp(p->pool + a, p->pool + b, a_len * sizeof(uint32_t)) == 0;
}

static bool rx_paths_exclusive(const rx_t *p, const rx_name_t *a, const rx_name_t *b) {
  uint32_t len = a->path_len < b->path_len ? a->path_len : b->path_len;
  for (uint32_t i = 0; i < len; i += 2) {
    const uint32_t *x = p->pool + a->path + i, *y = p->pool + b->path + i;
    if (x[0] != y[0]) return false;
    if (x[1] != y[1]) return true;
  }
  return false;
}

static bool rx_parse_open_paren(rx_t *p) {
  uint8_t type = RX_GROUP_CAPTURE;
  bool named = false;
  rx_advance(p);
  if (rx_cur(p) == '?') {
    bool modifiers = false, positive = true;
    unsigned seen = 0;
    do {
      uint32_t c = rx_next(p);
      if (c == '-') {
        rx_advance(p);
        modifiers = true;
        if (!positive) return rx_fail(p, RX_ERR_MULTIPLE_FLAG_DASHES);
        positive = false;
      } else if (c == 'i' || c == 'm' || c == 's') {
        rx_advance(p);
        modifiers = true;
        unsigned bit = c == 'i' ? 1u : c == 'm' ? 2u : 4u;
        if (seen & bit) return rx_fail(p, RX_ERR_REPEATED_FLAG);
        seen |= bit;
      } else if (c == ':') {
        rx_advance2(p);
        modifiers = false;
        type = RX_GROUP_PLAIN;
      } else if (!modifiers && (c == '=' || c == '!')) {
        rx_advance2(p);
        type = RX_GROUP_LOOKAHEAD;
      } else if (!modifiers && c == '<') {
        rx_advance(p);
        if (rx_next(p) == '=' || rx_next(p) == '!') {
          rx_advance2(p);
          type = RX_GROUP_LOOKBEHIND;
        } else {
          named = true;
          p->has_named = true;
          rx_advance(p);
        }
      } else return rx_fail(p, RX_ERR_INVALID_GROUP);
    } while (modifiers);
    if (!positive && !seen) return rx_fail(p, RX_ERR_INVALID_FLAG_GROUP);
  }

  rx_group_t group = { .type = type, .id = ++p->next_id };
  if (type == RX_GROUP_CAPTURE) {
    if (p->captures_started >= RX_MAX_CAPTURES) return rx_fail(p, RX_ERR_TOO_MANY_CAPTURES);
    p->captures_started++;
    if (named) {
      group.named = true;
      if (!rx_parse_group_name(p, &group.name, &group.name_len)) return false;
      group.path = (uint32_t)p->pool_len;
      for (size_t i = 0; i < p->groups_len; i++) {
        if (!rx_pool_push(p, p->groups[i].id) || !rx_pool_push(p, p->groups[i].alt)) return false;
      }
      group.path_len = (uint32_t)(p->pool_len - group.path);
      group.slot = (uint32_t)p->names_len;
      if (!rx_reserve(p, (void **)&p->names, &p->names_cap, p->names_len + 1, sizeof(*p->names))) return false;
      p->names[p->names_len++] = (rx_name_t){ group.name, group.name_len, group.path, group.path_len };
    }
  }
  if (!rx_reserve(p, (void **)&p->groups, &p->groups_cap, p->groups_len + 1, sizeof(*p->groups))) return false;
  p->groups[p->groups_len++] = group;
  return true;
}

static bool rx_close_group(rx_t *p, int *term) {
  rx_group_t group = p->groups[--p->groups_len];
  *term = group.type == RX_GROUP_LOOKAHEAD ? RX_TERM_LOOKAHEAD
    : group.type == RX_GROUP_LOOKBEHIND ? RX_TERM_LOOKBEHIND
    : RX_TERM_ATOM;
  if (!group.named) return true;
  const rx_name_t *entry = &p->names[group.slot];
  for (size_t i = 0; i < p->names_len; i++) {
    const rx_name_t *other = &p->names[i];
    if (i == group.slot || !rx_names_equal(p, other->name, other->name_len, entry->name, entry->name_len)) continue;
    if (!rx_paths_exclusive(p, other, entry)) return rx_fail(p, RX_ERR_DUPLICATE_CAPTURE_GROUP_NAME);
  }
  return true;
}

static bool rx_parse_backref_index(rx_t *p) {
  size_t start = p->pos;
  int value = (int)(rx_next(p) - '0');
  rx_advance2(p);
  while (rx_is_digit(rx_cur(p))) {
    value = value * 10 + (int)(rx_cur(p) - '0');
    if (value > RX_MAX_CAPTURES) {
      p->pos = start;
      return false;
    }
    rx_advance(p);
  }
  if (value > p->captures_started) {
    if (!p->scanned) rx_scan_captures(p, false);
    if (value > p->capture_count) {
      p->pos = start;
      return false;
    }
  }
  return true;
}

static bool rx_parse_named_backref(rx_t *p) {
  if (rx_cur(p) != '<') return rx_fail(p, RX_ERR_INVALID_NAMED_REFERENCE);
  rx_advance(p);
  rx_name_t ref = {0};
  if (!rx_parse_group_name(p, &ref.name, &ref.name_len)) return false;
  if (!rx_reserve(p, (void **)&p->refs, &p->refs_cap, p->refs_len + 1, sizeof(*p->refs))) return false;
  p->refs[p->refs_len++] = ref;
  return true;
}

static int rx_parse_decimal(rx_t *p) {
  int value = 0;
  while (rx_is_digit(rx_cur(p))) {
    int d = (int)(rx_cur(p) - '0');
    if (value > (RX_INFINITY - d) / 10) {
      do rx_advance(p); while (rx_is_digit(rx_cur(p)));
      return RX_INFINITY;
    }
    value = value * 10 + d;
    rx_advance(p);
  }
  return value;
}

static bool rx_parse_interval(rx_t *p, int *min_out, int *max_out) {
  size_t start = p->pos;
  rx_advance(p);
  if (!rx_is_digit(rx_cur(p))) {
    p->pos = start;
    return false;
  }
  int min = rx_parse_decimal(p), max;
  if (rx_cur(p) == '}') {
    max = min;
    rx_advance(p);
  } else if (rx_cur(p) == ',') {
    rx_advance(p);
    if (rx_cur(p) == '}') {
      max = RX_INFINITY;
      rx_advance(p);
    } else {
      max = rx_parse_decimal(p);
      if (rx_cur(p) != '}') {
        p->pos = start;
        return false;
      }
      rx_advance(p);
    }
  } else {
    p->pos = start;
    return false;
  }
  *min_out = min;
  *max_out = max;
  return true;
}

static bool rx_parse_class_escape(rx_t *p, uint32_t *value, bool *is_class) {
  *is_class = false;
  if (rx_cur(p) != '\\') {
    *value = rx_cur(p);
    rx_advance(p);
    return true;
  }
  uint32_t next = rx_next(p);
  if (next == 'b') {
    *value = '\b';
    rx_advance2(p);
    return true;
  }
  if (next == '-' && p->unicode) {
    *value = '-';
    rx_advance2(p);
    return true;
  }
  if (next == RX_END) return rx_fail(p, RX_ERR_ESCAPE_AT_END);
  if (rx_is_class_escape_char(next)) {
    *is_class = true;
    rx_advance2(p);
    return true;
  }
  if ((next == 'p' || next == 'P') && p->unicode) {
    *is_class = true;
    rx_advance2(p);
    bool strings;
    if (!rx_parse_property(p, next == 'P', &strings)) return rx_fail(p, RX_ERR_INVALID_CLASS_PROPERTY_NAME);
    return true;
  }
  *value = rx_parse_character_escape(p, true);
  return !p->err;
}

static bool rx_parse_class_ranges(rx_t *p) {
  rx_advance(p);
  if (rx_cur(p) == '^') rx_advance(p);
  if (rx_cur(p) == ']') {
    rx_advance(p);
    return true;
  }
  while (rx_has_more(p) && rx_cur(p) != ']') {
    uint32_t first, second;
    bool first_class, second_class;
    if (!rx_parse_class_escape(p, &first, &first_class)) return false;
    if (rx_cur(p) != '-') continue;
    rx_advance(p);
    if (rx_cur(p) == RX_END || rx_cur(p) == ']') break;
    if (!rx_parse_class_escape(p, &second, &second_class)) return false;
    if (first_class || second_class) {
      if (p->unicode) return rx_fail(p, RX_ERR_INVALID_CHARACTER_CLASS);
      continue;
    }
    if (first > second) return rx_fail(p, RX_ERR_OUT_OF_ORDER_CHARACTER_CLASS);
  }
  if (!rx_has_more(p)) return rx_fail(p, RX_ERR_UNTERMINATED_CHARACTER_CLASS);
  rx_advance(p);
  return true;
}

static uint32_t rx_parse_class_set_character(rx_t *p) {
  uint32_t c = rx_cur(p);
  if (c == '\\') {
    uint32_t next = rx_next(p);
    if (next == 'b') {
      rx_advance2(p);
      return '\b';
    }
    if (next == RX_END) {
      rx_fail(p, RX_ERR_ESCAPE_AT_END);
      return 0;
    }
    return rx_parse_character_escape(p, true);
  }
  if (rx_is_class_set_syntax_char(c)) {
    rx_fail(p, RX_ERR_INVALID_CHARACTER_IN_CLASS);
    return 0;
  }
  if (rx_is_class_set_double_punctuator(p, c)) {
    rx_fail(p, RX_ERR_INVALID_CLASS_SET_OPERATION);
    return 0;
  }
  rx_advance(p);
  return c;
}

static bool rx_parse_class_strings(rx_t *p, bool *strings) {
  rx_advance2(p);
  if (rx_cur(p) != '{') return rx_fail(p, RX_ERR_INVALID_ESCAPE);
  rx_advance(p);
  size_t length = 0;
  *strings = false;
  while (rx_has_more(p) && rx_cur(p) != '}') {
    if (rx_cur(p) == '|') {
      if (length != 1) *strings = true;
      length = 0;
      rx_advance(p);
      continue;
    }
    rx_parse_class_set_character(p);
    if (p->err) return false;
    length++;
  }
  if (length != 1) *strings = true;
  rx_advance(p);
  return true;
}

static bool rx_parse_set_operand(rx_t *p, rx_operand_t *op) {
  *op = (rx_operand_t){ .type = RX_OPERAND_CHAR };
  if (rx_cur(p) == '\\') {
    uint32_t next = rx_next(p);
    if (next == 'q') {
      op->type = RX_OPERAND_STRINGS;
      return rx_parse_class_strings(p, &op->strings);
    }
    if (rx_is_class_escape_char(next)) {
      op->type = RX_OPERAND_ESCAPE;
      rx_advance2(p);
      return true;
    }
    if (next == 'p' || next == 'P') {
      op->type = RX_OPERAND_ESCAPE;
      rx_advance2(p);
      if (!rx_parse_property(p, next == 'P', &op->strings)) return rx_fail(p, RX_ERR_INVALID_CLASS_PROPERTY_NAME);
      return true;
    }
  }
  op->value = rx_parse_class_set_character(p);
  return !p->err;
}

static bool rx_parse_class_sets(rx_t *p) {
  size_t depth = 0;
  rx_operand_t op;
  rx_class_frame_t *f = NULL;

open:
  rx_advance(p);
  {
    bool negated = rx_cur(p) == '^';
    if (negated) rx_advance(p);
    if (rx_cur(p) == ']') {
      rx_advance(p);
      op = (rx_operand_t){ .type = RX_OPERAND_NESTED };
      goto deliver;
    }
    if (!rx_reserve(p, (void **)&p->frames, &p->frames_cap, depth + 1, sizeof(*p->frames))) return false;
    p->frames[depth++] = (rx_class_frame_t){ .negated = negated, .next = RX_NEXT_FIRST };
    f = &p->frames[depth - 1];
  }

operand:
  if (rx_cur(p) == '[') goto open;
  if (!rx_parse_set_operand(p, &op)) return false;

handle:
  switch (f->next) {
    case RX_NEXT_FIRST:
      f->strings = op.strings;
      if (rx_cur(p) == '-' && rx_next(p) == '-') {
          if (f->negated && f->strings) return rx_fail(p, RX_ERR_NEGATED_CLASS_WITH_STRINGS);
        goto subtraction;
      }
      if (rx_cur(p) == '&' && rx_next(p) == '&') {
          goto intersection;
      }
      f->last = op.type;
      f->from = op.value;
      goto set_union;
    case RX_NEXT_UNION:
      f->strings |= op.strings;
      f->last = op.type;
      f->from = op.value;
      goto set_union;
    case RX_NEXT_RANGE:
      if (op.type != RX_OPERAND_CHAR) return rx_fail(p, RX_ERR_INVALID_CHARACTER_CLASS);
      if (f->from > op.value) return rx_fail(p, RX_ERR_OUT_OF_ORDER_CHARACTER_CLASS);
      f->last = RX_OPERAND_RANGE;
      goto set_union;
    case RX_NEXT_INTERSECTION:
      if (!op.strings) f->strings = false;
      goto intersection;
    default:
      goto subtraction;
  }

set_union:
  if (!rx_has_more(p) || rx_cur(p) == ']') goto close;
  if (rx_cur(p) == '-') {
    if (rx_next(p) == '-') return rx_fail(p, RX_ERR_INVALID_CLASS_SET_OPERATION);
    rx_advance(p);
    if (!rx_has_more(p)) goto close;
    if (f->last != RX_OPERAND_CHAR) return rx_fail(p, RX_ERR_INVALID_CHARACTER_CLASS);
    f->next = RX_NEXT_RANGE;
    goto operand;
  }
  if (rx_cur(p) == '&' && rx_next(p) == '&') return rx_fail(p, RX_ERR_INVALID_CLASS_SET_OPERATION);
  f->next = RX_NEXT_UNION;
  goto operand;

intersection:
  if (!rx_has_more(p) || rx_cur(p) == ']') goto close;
  if (rx_cur(p) != '&' || rx_next(p) != '&') return rx_fail(p, RX_ERR_INVALID_CLASS_SET_OPERATION);
  rx_advance2(p);
  if (rx_cur(p) == '&') return rx_fail(p, RX_ERR_INVALID_CHARACTER_IN_CLASS);
  f->next = RX_NEXT_INTERSECTION;
  goto operand;

subtraction:
  if (!rx_has_more(p) || rx_cur(p) == ']') goto close;
  if (rx_cur(p) != '-' || rx_next(p) != '-') return rx_fail(p, RX_ERR_INVALID_CLASS_SET_OPERATION);
  rx_advance2(p);
  f->next = RX_NEXT_SUBTRACTION;
  goto operand;

close:
  if (!rx_has_more(p)) return rx_fail(p, RX_ERR_UNTERMINATED_CHARACTER_CLASS);
  rx_advance(p);
  if (f->negated && f->strings) return rx_fail(p, RX_ERR_NEGATED_CLASS_WITH_STRINGS);
  op = (rx_operand_t){ .type = RX_OPERAND_NESTED, .strings = f->strings };
  depth--;
  f = depth ? &p->frames[depth - 1] : NULL;

deliver:
  if (!depth) return true;
  op.type = RX_OPERAND_NESTED;
  goto handle;
}

static bool rx_parse_atom_escape(rx_t *p, bool *assertion) {
  uint32_t next = rx_next(p);
  *assertion = false;
  switch (next) {
    case RX_END:
      return rx_fail(p, RX_ERR_ESCAPE_AT_END);
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
      if (rx_parse_backref_index(p)) return true;
      if (p->unicode) return rx_fail(p, RX_ERR_INVALID_ESCAPE);
      if (next == '8' || next == '9') {
        rx_advance2(p);
        return true;
      }
      [[fallthrough]];
    case '0':
      rx_advance(p);
      if (p->unicode && rx_is_digit(rx_next(p))) return rx_fail(p, RX_ERR_INVALID_DECIMAL_ESCAPE);
      rx_parse_octal(p);
      return true;
    case 'b': case 'B':
      rx_advance2(p);
      *assertion = true;
      return true;
    case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
      rx_advance2(p);
      return true;
    case 'p': case 'P':
      rx_advance2(p);
      if (p->unicode) {
        bool strings;
        if (!rx_parse_property(p, next == 'P', &strings)) return rx_fail(p, RX_ERR_INVALID_PROPERTY_NAME);
      }
      return true;
    case 'k':
      if (p->unicode || rx_has_named_captures(p, false)) {
        rx_advance2(p);
        return rx_parse_named_backref(p);
      }
      [[fallthrough]];
    default:
      rx_parse_character_escape(p, false);
      return !p->err;
  }
}

static bool rx_parse_pattern(rx_t *p) {
  if (!rx_reserve(p, (void **)&p->groups, &p->groups_cap, 1, sizeof(*p->groups))) return false;
  p->groups[p->groups_len++] = (rx_group_t){ .type = RX_GROUP_ROOT };

  for (;;) {
    int term = RX_TERM_ATOM;
    uint32_t c = rx_cur(p);
    switch (c) {
      case RX_END:
        if (p->groups_len > 1) return rx_fail(p, RX_ERR_UNTERMINATED_GROUP);
        goto done;
      case ')':
        if (p->groups_len <= 1) return rx_fail(p, RX_ERR_UNMATCHED_PAREN);
        rx_advance(p);
        if (!rx_close_group(p, &term)) return false;
        break;
      case '|':
        rx_advance(p);
        p->groups[p->groups_len - 1].alt++;
        continue;
      case '*': case '+': case '?':
        return rx_fail(p, RX_ERR_NOTHING_TO_REPEAT);
      case '^': case '$':
        rx_advance(p);
        continue;
      case '(':
        if (!rx_parse_open_paren(p)) return false;
        continue;
      case '[':
        if (!(p->sets ? rx_parse_class_sets(p) : rx_parse_class_ranges(p))) return false;
        break;
      case '\\': {
        bool assertion;
        if (!rx_parse_atom_escape(p, &assertion)) return false;
        if (assertion) continue;
        break;
      }
      case '{': {
        int min, max;
        if (rx_parse_interval(p, &min, &max)) return rx_fail(p, RX_ERR_NOTHING_TO_REPEAT);
        if (p->unicode) return rx_fail(p, RX_ERR_LONE_QUANTIFIER_BRACKETS);
        rx_advance(p);
        break;
      }
      case '}': case ']':
        if (p->unicode) return rx_fail(p, RX_ERR_LONE_QUANTIFIER_BRACKETS);
        rx_advance(p);
        break;
      default:
        rx_advance(p);
        break;
    }

    int min, max;
    switch (rx_cur(p)) {
      case '*': case '+': case '?':
        rx_advance(p);
        break;
      case '{':
        if (rx_parse_interval(p, &min, &max)) {
          if (max < min) return rx_fail(p, RX_ERR_RANGE_OUT_OF_ORDER);
          break;
        }
        if (p->unicode) return rx_fail(p, RX_ERR_INCOMPLETE_QUANTIFIER);
        continue;
      default:
        continue;
    }
    if (rx_cur(p) == '?') rx_advance(p);
    if (term == RX_TERM_LOOKBEHIND || (term == RX_TERM_LOOKAHEAD && p->unicode))
      return rx_fail(p, RX_ERR_INVALID_QUANTIFIER);
  }

done:
  for (size_t i = 0; i < p->refs_len; i++) {
    const rx_name_t *ref = &p->refs[i];
    bool found = false;
    for (size_t j = 0; j < p->names_len && !found; j++)
      found = rx_names_equal(p, ref->name, ref->name_len, p->names[j].name, p->names[j].name_len);
    if (!found) return rx_fail(p, RX_ERR_INVALID_NAMED_CAPTURE_REFERENCED);
  }
  return true;
}

static size_t rx_decode(const char *src, size_t len, bool unicode, uint32_t *out) {
  const unsigned char *b = (const unsigned char *)src;
  size_t n = 0;
  bool prev_lead = false;
  for (size_t i = 0; i < len;) {
    uint32_t c = b[i];
    size_t width = 1;
    if (c >= 0x80) {
      size_t need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
      bool ok = need && i + need <= len;
      for (size_t k = 1; ok && k < need; k++) ok = (b[i + k] & 0xC0) == 0x80;
      if (ok) {
        c = need == 2 ? c & 0x1F : need == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < need; k++) c = (c << 6) | (b[i + k] & 0x3F);
        width = need;
      } else c = 0xFFFD;
    }
    i += width;
    if (c >= 0x10000 && !unicode) {
      out[n++] = 0xD800 + ((c - 0x10000) >> 10);
      out[n++] = 0xDC00 + ((c - 0x10000) & 0x3FF);
      prev_lead = false;
      continue;
    }
    if (unicode && prev_lead && rx_is_trail(c)) {
      out[n - 1] = rx_combine(out[n - 1], c);
      prev_lead = false;
      continue;
    }
    prev_lead = rx_is_lead(c);
    out[n++] = c;
  }
  return n;
}

const char *regexp_syntax_error(const char *src, size_t len, bool unicode, bool unicode_sets) {
  uint32_t inline_units[RX_INLINE_UNITS];
  uint32_t *units = len <= RX_INLINE_UNITS ? inline_units : malloc(len * sizeof(*units));
  if (!units) return NULL;

  rx_t p = { .unicode = unicode || unicode_sets, .sets = unicode_sets };
  p.n = rx_decode(src, len, p.unicode, units);
  p.s = units;
  rx_parse_pattern(&p);

  free(p.groups);
  free(p.names);
  free(p.refs);
  free(p.pool);
  free(p.frames);
  if (units != inline_units) free(units);
  return rx_error_messages[p.err];
}
