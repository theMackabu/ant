#pragma once

#include "types.h"
#include <stdbool.h>

bool esm_registry_specifier(const char *specifier);
char *esm_registry_resolve(ant_t *js, const char *specifier, const char *base_path, bool prefer_require);
