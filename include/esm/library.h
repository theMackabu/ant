#ifndef ESM_LIBRARY_H
#define ESM_LIBRARY_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct ant_library_entry ant_library_entry_t;
typedef ant_value_t (*ant_library_init_fn)(ant_t *js);
typedef void (*ant_library_iter_fn)(const char *name, void *userdata);

typedef enum {
  ANT_BUILTIN_NAME_NONE,
  ANT_BUILTIN_NAME_ANT,   // ant:x, Ant-only
  ANT_BUILTIN_NAME_NODE,  // node:x
  ANT_BUILTIN_NAME_BARE,  // x
} ant_builtin_name_kind_t;

bool js_esm_is_registered_library(const char *specifier, size_t spec_len);

void ant_library_foreach(ant_library_iter_fn cb, void *userdata);
void ant_register_library(ant_library_init_fn init_fn, const char *name, ...);
void ant_register_standard_library(ant_library_init_fn init_fn, const char *bare, const char *ant_name, const char *node_name);

ant_library_entry_t *ant_library_find(const char *specifier, size_t spec_len);
ant_builtin_name_kind_t ant_library_kind(const ant_library_entry_t *lib);

ant_value_t ant_library_load(ant_t *js, ant_library_entry_t *lib);
ant_value_t js_esm_load_registered_library(ant_t *js, const char *specifier, size_t spec_len, bool *loaded);

#define ant_standard_library(name, lib) ant_register_standard_library(lib, name, "ant:" name, "node:" name)

#endif
