#ifndef ESM_REMOTE_H
#define ESM_REMOTE_H

#include <stdbool.h>
#include <stddef.h>

bool esm_is_url(const char *spec);
char *esm_fetch_url(const char *url, size_t *out_len, char **out_error);
char *esm_resolve_url(const char *specifier, const char *base_url);

#endif
