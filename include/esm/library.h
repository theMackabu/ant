#ifndef ESM_LIBRARY_H
#define ESM_LIBRARY_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

typedef jsval_t (*ant_library_init_fn)(ant_t *js);
typedef void (*ant_library_iter_fn)(const char *name, void *userdata);

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...);
void ant_library_foreach(ant_library_iter_fn cb, void *userdata);

#define ant_standard_library(name, lib) \
  ant_register_library(lib, name, "ant:" name, "node:" name, NULL)

jsval_t js_esm_load_registered_library(
  ant_t *js, const char *specifier,
  size_t spec_len, bool *loaded
);

#endif
