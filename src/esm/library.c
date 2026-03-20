#include "esm/library.h"
#include "ant.h"

#include <stdarg.h>
#include <string.h>
#include <uthash.h>

typedef struct ant_library_entry {
  char name[256];
  ant_library_init_fn init_fn;
  UT_hash_handle hh;
} ant_library_entry_t;

static ant_library_entry_t *library_registry = NULL;

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...) {
  va_list args;
  const char *alias = name;

  va_start(args, name);
  while (alias != NULL) {
    ant_library_entry_t *lib = (ant_library_entry_t *)ant_calloc(sizeof(ant_library_entry_t));
    if (!lib) break;

    strncpy(lib->name, alias, sizeof(lib->name) - 1);
    lib->name[sizeof(lib->name) - 1] = '\0';
    lib->init_fn = init_fn;

    HASH_ADD_STR(library_registry, name, lib);
    alias = va_arg(args, const char *);
  }
  va_end(args);
}

static ant_library_entry_t *find_library(const char *specifier, size_t spec_len) {
  ant_library_entry_t *lib = NULL;
  char key[256];

  if (spec_len >= sizeof(key)) return NULL;
  memcpy(key, specifier, spec_len);
  key[spec_len] = '\0';

  HASH_FIND_STR(library_registry, key, lib);
  return lib;
}

void ant_library_foreach(ant_library_iter_fn cb, void *userdata) {
  ant_library_entry_t *lib, *tmp;
  HASH_ITER(hh, library_registry, lib, tmp) {
    if (!strchr(lib->name, ':')) cb(lib->name, userdata);
  }
}

ant_value_t js_esm_load_registered_library(ant_t *js, const char *specifier, size_t spec_len, bool *loaded) {
  ant_library_entry_t *lib = find_library(specifier, spec_len);
  if (!lib) {
    if (loaded) *loaded = false;
    return js_mkundef();
  }
  if (loaded) *loaded = true;
  return lib->init_fn(js);
}
