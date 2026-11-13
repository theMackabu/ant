#pragma once

#include "modules/regex_syntax.h"
#include "unicode.h"

#include <string.h>

enum {
  RX_ERR_NONE,
#define REGEXP_SYNTAX_ERROR(id, msg) RX_ERR_##id,
#include "modules/regex_syntax_errors.h"
  RX_ERR_OOM,
};

static constexpr uint32_t RX_END = 0x200000;

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

uint32_t rx_parse_octal(rx_t *p);
uint32_t rx_parse_character_escape(rx_t *p, bool in_class);

bool rx_pool_push(rx_t *p, uint32_t value);
bool rx_parse_class_ranges(rx_t *p);
bool rx_parse_class_sets(rx_t *p);
void rx_scan_captures(rx_t *p, bool in_class);
bool rx_has_named_captures(rx_t *p, bool in_class);
bool rx_parse_property(rx_t *p, bool negated, bool *strings);
bool rx_parse_group_name(rx_t *p, uint32_t *name, uint32_t *name_len);
bool rx_reserve(rx_t *p, void **buf, size_t *cap, size_t need, size_t size);
bool rx_names_equal(const rx_t *p, uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len);
bool rx_paths_exclusive(const rx_t *p, const rx_name_t *a, const rx_name_t *b);

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

static inline bool rx_is_class_set_syntax_char(uint32_t c) {
  return rx_in_set(c, "()[]{}/-\\|");
}

static inline bool rx_is_class_set_double_punctuator(const rx_t *p, uint32_t c) {
  return rx_in_set(c, "&!#$%*+,.:;<=>?@^`~") && rx_next(p) == c;
}

static inline bool rx_is_class_escape_char(uint32_t c) {
  return rx_in_set(c, "dDsSwW");
}
