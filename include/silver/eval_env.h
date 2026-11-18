#pragma once

#include "silver/eval_scope_flags.h"
#include "types.h"

bool sv_global_lexical_lookup(
  ant_t *js, const char *name,
  size_t len, ant_value_t *out
);


typedef void (*sv_eval_with_marker_fn)(void *ctx, const char *marker, uint32_t len);

typedef struct {
  bool shadows_undefined;
  bool shadows_global_this;
  bool has_with;
} sv_eval_env_facts_t;

sv_eval_env_facts_t sv_eval_env_facts(ant_t *js, ant_value_t env);
void sv_eval_env_each_with(
  ant_t *js, ant_value_t env, const char *name, uint32_t len,
  bool declared_by_eval, sv_eval_with_marker_fn fn, void *ctx
);

static inline bool sv_eval_name_is_with_marker(const char *name, uint32_t len) {
  return len > 0 && name[0] == '\x01';
}

sv_eval_scope_flags_t sv_eval_env_flags(ant_value_t env);
bool sv_eval_env_shadows_global(
  ant_t *js, ant_value_t env, 
  const char *name, uint32_t len
);

ant_value_t sv_eval_read_import(
  ant_t *js, ant_value_t value, 
  const char *name, uint32_t len, bool is_default
);

void sv_eval_env_gc_mark(ant_t *js, ant_object_t *obj);
void sv_eval_env_gc_free(ant_object_t *obj);
