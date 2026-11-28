#include "regex_internal.h"

// A non-unicode match records its code unit view as the subject; RegExp.input
// must hand back the normalized string instead.
static ant_value_t regexp_static_input(ant_t *js, ant_value_t subject) {
  ant_offset_t len;
  const char *bytes = (const char *)(uintptr_t)vstr(js, subject, &len);
  if (str_is_ascii(bytes) || !wtf8_has_surrogate(bytes, (size_t)len)) return subject;
  return js_mkstr(js, bytes, (size_t)len);
}

static ant_value_t regexp_static_compute(
  ant_t *js, size_t idx, ant_value_t subject,
  const PCRE2_SIZE *ovector, uint32_t last_group, const PCRE2_SIZE *paren
) {
  if (idx == REGEXP_STATIC_INDEX_INPUT) return regexp_static_input(js, subject);

  ant_offset_t subject_len;
  const char *subject_ptr = (const char *)(uintptr_t)vstr(js, subject, &subject_len);
  PCRE2_SIZE start = PCRE2_UNSET, end = PCRE2_UNSET;
  bool matched = ovector[0] != PCRE2_UNSET;

  switch (idx) {
    case REGEXP_STATIC_INDEX_LAST_MATCH: if (matched) start = ovector[0], end = ovector[1]; break;
    case REGEXP_STATIC_INDEX_LAST_PAREN:
      if (last_group >= ANT_REGEXP_STATIC_OVECTOR_COUNT) start = paren[0], end = paren[1];
      else if (last_group) start = ovector[2 * last_group], end = ovector[2 * last_group + 1];
      break;
    case REGEXP_STATIC_INDEX_LEFT_CONTEXT: if (matched) start = 0, end = ovector[0]; break;
    case REGEXP_STATIC_INDEX_RIGHT_CONTEXT: if (matched) start = ovector[1], end = (PCRE2_SIZE)subject_len; break;
    default: {
      size_t capture = idx - REGEXP_STATIC_INDEX_D1 + 1;
      if (capture <= last_group) start = ovector[2 * capture], end = ovector[2 * capture + 1];
    }
  }

  if (start == PCRE2_UNSET) return regexp_static_empty(js);
  if (end < start || end > (PCRE2_SIZE)subject_len)
    return js_mkerr(js, "invalid regexp static snapshot");
  return js_mkstr_byte_range(js, subject_ptr, start, end - start);
}

static ant_value_t regexp_static_value(ant_t *js, size_t idx) {
  ant_regexp_realm_t *statics = &js->regexp;
  uint16_t bit = (uint16_t)(1u << idx);
  if (!statics->static_pending || (statics->static_materialized_mask & bit)) {
    ant_value_t value = js->mutable_roots.regexp_static_values[idx];
    return value ? value : regexp_static_empty(js);
  }

  ant_value_t value = regexp_static_compute(
    js, idx, js->mutable_roots.regexp_static_subject,
    js->regexp.static_ovector, js->regexp.static_last_group, js->regexp.static_paren
  );
  if (is_err(value)) return value;

  js->mutable_roots.regexp_static_values[idx] = value;
  js->regexp.static_materialized_mask |= bit;
  if (js->regexp.static_materialized_mask == REGEXP_STATIC_ALL_MATERIALIZED) {
    js->regexp.static_pending = false;
    js->mutable_roots.regexp_static_subject = regexp_static_empty(js);
  }
  return value;
}

static ant_value_t regexp_static_store(ant_t *js, size_t idx, ant_value_t value) {
  if (idx != REGEXP_STATIC_INDEX_INPUT) return Ant_Value_Undefined;
  ant_value_t input = js_template_to_string(js, value);
  if (is_err(input)) return input;

  js->mutable_roots.regexp_static_values[idx] = input;
  js->regexp.static_materialized_mask |= (uint16_t)(1u << idx);
  return Ant_Value_Undefined;
}
#define REGEXP_STATIC(id, name, alias) \
  ant_value_t regexp_static_get_##id(ant_params_t) { \
    return regexp_static_value(js, REGEXP_STATIC_INDEX_##id); \
  } \
  ant_value_t regexp_static_set_##id(ant_params_t) { \
    return regexp_static_store(js, REGEXP_STATIC_INDEX_##id, nargs > 0 ? args[0] : Ant_Value_Undefined); \
  }
#include "modules/regex_statics.h"

__attribute__((noinline, cold)) void update_regexp_statics_eager(
  ant_t *js, ant_value_t subject,
  PCRE2_SIZE *ovector, uint32_t ovcount
) {
  PCRE2_SIZE paren[2] = { PCRE2_UNSET, PCRE2_UNSET };
  if (ovcount > ANT_REGEXP_STATIC_OVECTOR_COUNT) {
    paren[0] = ovector[2 * (ovcount - 1)];
    paren[1] = ovector[2 * (ovcount - 1) + 1];
  }
  ant_value_t empty = regexp_static_empty(js);
  for (size_t idx = 0; idx < REGEXP_STATIC_VALUE_COUNT; idx++) {
    ant_value_t value = regexp_static_compute(js, idx, subject, ovector, ovcount - 1, paren);
    js->mutable_roots.regexp_static_values[idx] = is_err(value) ? empty : value;
  }
}
