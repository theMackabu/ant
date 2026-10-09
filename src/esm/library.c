#include "esm/library.h"
#include "gc/roots.h"
#include "ant.h"
#include "internal.h"

#include <stdarg.h>
#include <string.h>
#include <uthash.h>

struct ant_library_entry {
  char name[256];
  char display_name[256];
  ant_library_init_fn init_fn;
  ant_builtin_name_kind_t kind;
  ant_value_t cached_ns;
  bool ns_initialized;
  bool root_registered;
  struct ant_library_entry *canonical;
  UT_hash_handle hh;
};

static ant_library_entry_t *library_registry = NULL;

static ant_library_entry_t *library_add(
  ant_library_init_fn init_fn, const char *name,
  ant_builtin_name_kind_t kind, ant_library_entry_t *canonical
) {
  ant_library_entry_t *lib = (ant_library_entry_t *)calloc(1, sizeof(ant_library_entry_t));
  if (!lib) return canonical;

  strncpy(lib->name, name, sizeof(lib->name) - 1);
  lib->init_fn = init_fn;
  lib->kind = kind;

  if (!canonical) canonical = lib;
  lib->canonical = canonical;

  if (kind == ANT_BUILTIN_NAME_NODE || !canonical->display_name[0])
    strncpy(canonical->display_name, name, sizeof(canonical->display_name) - 1);

  HASH_ADD_STR(library_registry, name, lib);
  return canonical;
}

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...) {
  va_list args;
  ant_library_entry_t *canonical = NULL;

  va_start(args, name);
  for (const char *alias = name; alias; alias = va_arg(args, const char *))
    canonical = library_add(init_fn, alias, ANT_BUILTIN_NAME_ANT, canonical);
  va_end(args);
}

void ant_register_standard_library(ant_library_init_fn init_fn, const char *bare, const char *ant_name, const char *node_name) {
  ant_library_entry_t *canonical = library_add(init_fn, bare, ANT_BUILTIN_NAME_BARE, NULL);
  library_add(init_fn, ant_name, ANT_BUILTIN_NAME_ANT, canonical);
  library_add(init_fn, node_name, ANT_BUILTIN_NAME_NODE, canonical);
}

ant_library_entry_t *ant_library_find(const char *specifier, size_t spec_len) {
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
  HASH_ITER(hh, library_registry, lib, tmp) if (lib->kind == ANT_BUILTIN_NAME_BARE) cb(lib->name, userdata);
}

bool js_esm_is_registered_library(const char *specifier, size_t spec_len) {
  return ant_library_find(specifier, spec_len) != NULL;
}

ant_builtin_name_kind_t ant_library_kind(const ant_library_entry_t *lib) {
  return lib->kind;
}

ant_value_t js_esm_load_registered_library(ant_t *js, const char *specifier, size_t spec_len, bool *loaded) {
  ant_library_entry_t *lib = ant_library_find(specifier, spec_len);
  if (loaded) *loaded = lib != NULL;
  return lib ? ant_library_load(js, lib) : js_mkundef();
}

ant_value_t ant_library_load(ant_t *js, ant_library_entry_t *lib) {
  ant_library_entry_t *canon = lib->canonical;
  if (canon->ns_initialized) return canon->cached_ns;

  if (!canon->root_registered) {
    gc_register_root(&canon->cached_ns);
    canon->root_registered = true;
  }

  canon->cached_ns = canon->init_fn(js);
  if (is_object_type(canon->cached_ns)) {
    const char *display_name = canon->display_name[0] ? canon->display_name : canon->name;
    ant_value_t module_ctx = js_create_module_context(js, display_name, false);
    if (is_err(module_ctx)) return module_ctx;
    js_module_ctx_link_namespace(js, module_ctx, canon->cached_ns);
  }

  canon->ns_initialized = true;
  return canon->cached_ns;
}
