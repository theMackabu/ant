#pragma once

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef ant_value_t (*ant_library_init_fn)(ant_t *js);
typedef void (*ant_library_iter_fn)(const char *name, void *userdata);

typedef struct ant_library_ns {
  ant_value_t ns;
  ant_value_t esm_ns;
  bool loaded;
} ant_library_ns_t;

typedef enum {
  ANT_BUILTIN_NAME_NONE,
  ANT_BUILTIN_NAME_ANT,   // ant:x, Ant-only
  ANT_BUILTIN_NAME_NODE,  // node:x
  ANT_BUILTIN_NAME_BARE,  // x
} ant_builtin_name_kind_t;

typedef struct ant_library_entry {
  const char *name;
  const char *display_name;
  ant_library_init_fn init_fn;
  uint8_t name_len;
  bool standard;
} ant_library_entry_t;

enum {
  ANT_LIBRARY_COUNT = 0
  #define ANT_LIBRARY(scope, name, fn) + 1
  #include "esm/libraries.h"
};

void ant_library_foreach(ant_library_iter_fn cb, void *userdata);

ant_value_t ant_library_load(ant_t *js, const ant_library_entry_t *lib);
ant_value_t ant_library_namespace(ant_t *js, const ant_library_entry_t *lib);

const ant_library_entry_t *ant_library_find(const char *specifier, size_t spec_len, ant_builtin_name_kind_t *kind);
const char *ant_library_name(const ant_library_entry_t *lib);
