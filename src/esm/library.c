#include <stdlib.h>
#include <string.h>

#include "esm/library.h"
#include "internal.h"
#include "loader_internal.h"
#include "esm/builtin_bundle.h"

#define ANT_LIBRARY(scope, name, fn) ant_value_t fn(ant_t *js);
#include "esm/libraries.h"

#define LIBRARY_PREFIX_ANT "ant:"
#define LIBRARY_PREFIX_NODE "node:"
#define LIBRARY_STANDARD_ANT false
#define LIBRARY_STANDARD_NODE true

static const ant_library_entry_t libraries[] = {
  #define ANT_LIBRARY(scope, id, fn) {               \
    .name         = id,                              \
    .display_name = LIBRARY_PREFIX_##scope id,       \
    .init_fn      = fn,                              \
    .name_len     = sizeof(id) - 1,                  \
    .standard     = LIBRARY_STANDARD_##scope,        \
  },
  #include "esm/libraries.h"
};

_Static_assert(sizeof(libraries) / sizeof(libraries[0]) == ANT_LIBRARY_COUNT, "library count");

static inline ant_library_ns_t *library_ns(ant_t *js, const ant_library_entry_t *lib) {
  if (lib->host) return &((ant_host_module_t *)lib)->ns;
  return &js->libraries[lib - libraries];
}

static bool library_strip_prefix(const char **specifier, size_t *spec_len, const char *prefix, size_t prefix_len) {
  if (*spec_len < prefix_len || memcmp(*specifier, prefix, prefix_len) != 0) return false;
  *specifier += prefix_len;
  *spec_len -= prefix_len;
  return true;
}

ant_value_t ant_library_namespace(ant_t *js, const ant_library_entry_t *lib) {
  ant_library_ns_t *cache = library_ns(js, lib);
  if (is_object_type(cache->esm_ns)) return cache->esm_ns;

  ant_value_t exports = ant_library_load(js, lib);
  if (is_err(exports) || !is_object_type(exports)) return exports;

  ant_value_t default_value = js_get_slot(exports, SLOT_DEFAULT);
  if (vtype(default_value) == kTypeUndefined) default_value = exports;

  ant_value_t ns = esm_value_namespace(js, exports, default_value);
  if (is_err(ns)) return ns;

  js_set_slot(ns, SLOT_MODULE_LOADING, Ant_Value_Undefined);
  js_set_slot_wb(js, ns, SLOT_MODULE_CTX, js_get_slot(exports, SLOT_MODULE_CTX));

  cache->esm_ns = ns;
  return ns;
}

const ant_library_entry_t *ant_library_find(const char *specifier, size_t spec_len, ant_builtin_name_kind_t *kind) {
  ant_builtin_name_kind_t found = ANT_BUILTIN_NAME_BARE;

  if (library_strip_prefix(&specifier, &spec_len, LIBRARY_PREFIX_NODE, sizeof(LIBRARY_PREFIX_NODE) - 1))
    found = ANT_BUILTIN_NAME_NODE;
  else if (library_strip_prefix(&specifier, &spec_len, LIBRARY_PREFIX_ANT, sizeof(LIBRARY_PREFIX_ANT) - 1))
    found = ANT_BUILTIN_NAME_ANT;

  for (size_t i = 0; i < ANT_LIBRARY_COUNT; i++) {
    const ant_library_entry_t *lib = &libraries[i];
    if (!lib->standard && found != ANT_BUILTIN_NAME_ANT) continue;
    if (lib->name_len != spec_len || memcmp(lib->name, specifier, spec_len) != 0) continue;
    if (kind) *kind = found;
    return lib;
  }

  return NULL;
}

void ant_library_foreach(ant_library_iter_fn cb, void *userdata) {
  for (size_t i = 0; i < ANT_LIBRARY_COUNT; i++)
    if (libraries[i].standard) cb(libraries[i].name, userdata);
}

const char *ant_library_name(const ant_library_entry_t *lib) {
  return lib->display_name;
}

ant_value_t ant_library_load(ant_t *js, const ant_library_entry_t *lib) {
  ant_library_ns_t *cache = library_ns(js, lib);
  if (cache->loaded) return cache->ns;

  ant_value_t ns = lib->init_fn(js);
  if (is_err(ns)) return ns;
  cache->ns = ns;

  if (is_object_type(ns)) {
    ant_value_t module_ctx = js_create_module_context(js, lib->display_name, false);
    if (is_err(module_ctx)) return module_ctx;
    js_module_ctx_link_namespace(js, module_ctx, ns);
  }

  cache->loaded = true;
  return ns;
}

const ant_host_module_t *ant_host_module_find(ant_t *js, const char *specifier, size_t spec_len) {
  for (const ant_host_module_t *module = js->host_modules; module; module = module->next)
    if (module->entry.name_len == spec_len && memcmp(module->entry.name, specifier, spec_len) == 0) return module;
  return NULL;
}

static bool host_module_register(
  ant_t *js, const char *specifier, ant_library_init_fn init_fn, const char *source, size_t source_len
) {
  size_t spec_len = strlen(specifier);
  if (spec_len == 0 || spec_len > UINT8_MAX) return false;
  if (esm_lookup_builtin_alias(specifier, spec_len) || ant_library_find(specifier, spec_len, NULL)) return false;
  if (ant_host_module_find(js, specifier, spec_len)) return false;

  ant_host_module_t *module = calloc(1, sizeof(*module) + spec_len + 1 + source_len + 1);
  if (!module) return false;

  char *name = (char *)(module + 1);
  memcpy(name, specifier, spec_len);

  if (source) {
    char *code = name + spec_len + 1;
    memcpy(code, source, source_len);
    module->source = (const uint8_t *)code;
    module->source_len = source_len;
  }

  module->entry = (ant_library_entry_t){
    .name = name, .display_name = name, .init_fn = init_fn,
    .name_len = (uint8_t)spec_len, .host = true,
  };

  module->next = js->host_modules;
  js->host_modules = module;

  return true;
}

bool Ant_Module_Register(ant_t *js, const char *specifier, ant_library_init_fn init_fn) {
  return init_fn && host_module_register(js, specifier, init_fn, NULL, 0);
}

bool Ant_Module_RegisterSource(ant_t *js, const char *specifier, const char *source, size_t source_len) {
  return source && host_module_register(js, specifier, NULL, source, source_len);
}

void ant_host_modules_destroy(ant_t *js) {
  ant_host_module_t *module = js->host_modules;
  js->host_modules = NULL;

  while (module) {
    ant_host_module_t *next = module->next;
    free(module);
    module = next;
  }
}
