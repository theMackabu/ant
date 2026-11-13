#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  INTL_TAG_OK = 0,
  INTL_TAG_INVALID,
  INTL_TAG_OOM,
} intl_tag_status_t;

intl_tag_status_t Ant_Intl_CanonicalizeTag(const char *tag, size_t len, char **out, size_t *out_len);
