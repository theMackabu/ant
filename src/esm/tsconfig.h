#pragma once

#include "types.h"
#include <stdbool.h>

struct ant_esm_state;

typedef char *(*esm_tsconfig_probe_fn)(ant_t *js, const char *candidate, void *ctx);
char *esm_tsconfig_resolve( ant_t *js, const char *specifier, const char *start_dir, esm_tsconfig_probe_fn probe, void *ctx);

void esm_tsconfig_cache_cleanup(struct ant_esm_state *st);
