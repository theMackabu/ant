#include "esm/library.h"
#include "gc/roots.h"
#include "ant.h"
#include "internal.h"

#include <stdarg.h>
#include <string.h>
#include <uthash.h>

typedef struct ant_library_entry {
  char name[256];
  char display_name[256];
  ant_library_init_fn init_fn;
  ant_value_t cached_ns;
  bool ns_initialized;
  bool root_registered;
  struct ant_library_entry *canonical;
  UT_hash_handle hh;
} ant_library_entry_t;

static ant_library_entry_t *library_registry = NULL;

static bool ant_library_display_name_is_better(const char *candidate, const char *current) {
  if (!candidate || !candidate[0]) return false;
  if (!current || !current[0]) return true;

  bool candidate_is_node = strncmp(candidate, "node:", 5) == 0;
  bool current_is_node = strncmp(current, "node:", 5) == 0;
  return candidate_is_node && !current_is_node;
}

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...) {
  va_list args;
  const char *alias = name;
  ant_library_entry_t *canonical_entry = NULL;

  va_start(args, name);
  while (alias != NULL) {
    ant_library_entry_t *lib = (ant_library_entry_t *)ant_calloc(sizeof(ant_library_entry_t));
    if (!lib) break;
    
    strncpy(lib->name, alias, sizeof(lib->name) - 1);
    lib->name[sizeof(lib->name) - 1] = '\0';
    lib->init_fn = init_fn;
    lib->ns_initialized = false;
    
    if (!canonical_entry) canonical_entry = lib;
    lib->canonical = canonical_entry;
    
    if (ant_library_display_name_is_better(alias, canonical_entry->display_name)) {
      strncpy(canonical_entry->display_name, alias, sizeof(canonical_entry->display_name) - 1);
      canonical_entry->display_name[sizeof(canonical_entry->display_name) - 1] = '\0';
    }
    
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
  HASH_ITER(hh, library_registry, lib, tmp) if (!strchr(lib->name, ':')) cb(lib->name, userdata);
}

ant_value_t js_esm_load_registered_library(ant_t *js, const char *specifier, size_t spec_len, bool *loaded) {
  ant_library_entry_t *lib = find_library(specifier, spec_len);
  if (!lib) {
    if (loaded) *loaded = false;
    return js_mkundef();
  }

  if (loaded) *loaded = true;
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
    if (is_object_type(module_ctx)) js_set_slot_wb(js, canon->cached_ns, SLOT_MODULE_CTX, module_ctx);
  }

  canon->ns_initialized = true;
  return canon->cached_ns;
}
