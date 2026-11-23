#include "silver/compiler.h"
#include "silver/compile_ctx.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

static void sv_compile_ctx_rebuild_local_lookup(sv_compiler_t *ctx, int cap) {
  if (cap < 16) cap = 16;
  int *heads = malloc((size_t)cap * sizeof(int));
  if (!heads) return;

  for (int i = 0; i < cap; i++) heads[i] = -1;
  for (int i = 0; i < ctx->local_count; i++) {
    sv_local_t *loc = &ctx->locals[i];
    int bucket = (int)(loc->name_hash & (uint32_t)(cap - 1));
    loc->lookup_next = heads[bucket];
    heads[bucket] = i;
  }

  free(ctx->local_lookup_heads);
  ctx->local_lookup_heads = heads;
  ctx->local_lookup_cap = cap;
}

void sv_compile_ctx_init_root(
  sv_compiler_t *ctx, ant_t *js,
  const char *filename,
  const char *source,
  ant_offset_t source_len,
  sv_compile_mode_t mode,
  bool is_strict,
  sv_line_table_t *line_table
) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->js = js;
  ctx->filename = filename;
  ctx->source = source;
  ctx->source_len = source_len;
  ctx->mode = mode;
  ctx->inherits_eval_env = sv_compile_mode_is_eval(mode);
  ctx->allows_new_target = mode == SV_COMPILE_EVAL_FUNCTION;
  ctx->is_strict = is_strict;
  memset(ctx->var_name_bloom, 0xff, sizeof(ctx->var_name_bloom));
  ctx->completion_local = -1;
  ctx->arguments_local = -1;
  ctx->new_target_local = -1;
  ctx->super_local = -1;
  ctx->this_local = -1;
  ctx->this_check_atom = -1;
  ctx->derived_return_local = -1;
  ctx->using_stack_local = -1;
  ctx->line_table = line_table;
  ctx->private_scope = NULL;
}

static inline void ctx_free(void *ptr) {
  if (ptr) free(ptr);
}

static void type_seed_adopt(sv_type_scratch_t *types, const sv_compiler_t *enclosing) {
  sv_type_seed_t *seed = enclosing->type_seed;
  if (!seed) return;
  types->seed = seed;
  types->deps = seed->deps;
  types->dep_cap = (int)(sizeof(seed->deps) / sizeof(seed->deps[0]));
  types->uses = seed->uses;
  types->use_cap = (int)(sizeof(seed->uses) / sizeof(seed->uses[0]));
  types->promos = seed->promos;
  types->promo_cap = (int)(sizeof(seed->promos) / sizeof(seed->promos[0]));
}

static inline bool type_seed_holds(const sv_type_seed_t *seed, const void *items) {
  return seed && (const char *)items >= (const char *)seed && (const char *)items < (const char *)(seed + 1);
}

void *sv_compile_ctx_grow_types(const sv_type_scratch_t *types, void *items, size_t used, size_t size) {
  if (!type_seed_holds(types->seed, items)) return realloc(items, size);
  void *next = malloc(size);
  if (next) memcpy(next, items, used);
  return next;
}

void sv_compile_ctx_init_child(
  sv_compiler_t *ctx,
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->js = enclosing->js;
  ctx->filename = enclosing->filename;
  ctx->source = enclosing->source;
  ctx->source_len = enclosing->source_len;
  ctx->line_table = enclosing->line_table;
  ctx->enclosing = enclosing;
  ctx->scope_depth = 0;
  ctx->is_arrow = node && !!(node->flags & FN_ARROW);
  ctx->with_inherited = enclosing->with_inherited || enclosing->with_depth > 0 || enclosing->eval_with;
  ctx->allows_new_target = (!enclosing->enclosing || ctx->is_arrow)
    ? enclosing->allows_new_target : true;
  ctx->is_async = node && !!(node->flags & FN_ASYNC);
  ctx->is_async_generator = ctx->is_async && !!(node->flags & FN_GENERATOR);
  ctx->args_length_only = node && !!(node->flags & FN_ARGS_LENGTH_ONLY);
  ctx->is_strict = enclosing->is_strict || (node && !!(node->flags & FN_CLASS_BODY));
  memset(ctx->var_name_bloom, 0xff, sizeof(ctx->var_name_bloom));
  ctx->mode = mode;
  ctx->inherits_eval_env =
    enclosing->inherits_eval_env || sv_compile_mode_is_eval(mode);
  ctx->completion_local = -1;
  ctx->arguments_local = -1;
  ctx->new_target_local = -1;
  ctx->super_local = -1;
  ctx->this_local = -1;
  ctx->this_check_atom = -1;
  ctx->derived_return_local = -1;
  ctx->using_stack_local = -1;
  ctx->param_count = node ? node->args.count : 0;
  ctx->private_scope = enclosing->private_scope;
  enclosing->type_child_count++;
  if (enclosing->types) ctx->types = enclosing->types;
  else {
    ctx->types = &ctx->type_storage;
    type_seed_adopt(&ctx->type_storage, enclosing);
  }
  ctx->type_dep_mark = ctx->types->dep_count;
  ctx->type_use_mark = ctx->types->use_count;
  ctx->type_promo_mark = ctx->types->promo_count;
}

void sv_compile_ctx_cleanup(sv_compiler_t *ctx) {
  ctx_free(ctx->code);
  ctx_free(ctx->constants);
  ctx_free(ctx->atoms);
  ctx_free(ctx->atom_index);
  ctx_free(ctx->map_templates);
  ctx_free(ctx->locals);
  ctx_free(ctx->local_lookup_heads);
  ctx_free(ctx->upval_descs);
  ctx_free(ctx->upval_bindings);
  ctx_free(ctx->write_counts);
  if (ctx->types == &ctx->type_storage) {
    const sv_type_seed_t *seed = ctx->type_storage.seed;
    if (!type_seed_holds(seed, ctx->type_storage.deps)) ctx_free(ctx->type_storage.deps);
    if (!type_seed_holds(seed, ctx->type_storage.uses)) ctx_free(ctx->type_storage.uses);
    if (!type_seed_holds(seed, ctx->type_storage.promos)) ctx_free(ctx->type_storage.promos);
  } else {
    ctx->types->dep_count = ctx->type_dep_mark;
    ctx->types->use_count = ctx->type_use_mark;
    ctx->types->promo_count = ctx->type_promo_mark;
  }
  for (int i = 0; i < ctx->eval_scope_count; i++)
    ctx_free(ctx->eval_scopes[i].bindings);
  ctx_free(ctx->eval_scopes);
  ctx_free(ctx->eval_vars);
  ctx_free(ctx->eval_var_index);
  ctx_free(ctx->upvalue_index);
  ctx_free(ctx->global_lexicals);
  ctx_free(ctx->loops);
  ctx_free(ctx->with_scopes);
  ctx_free(ctx->spills);
  ctx_free(ctx->spill_heads);
  ctx_free(ctx->unwind_kinds);
  ctx_free(ctx->unwind_marks);
  ctx_free(ctx->srcpos);
  ctx_free(ctx->slot_types);
  ctx_free(ctx->deferred_exports);
  ctx_free(ctx->using_cleanups);
  ctx_free(ctx->shaped_sites);
  ctx_free(ctx->shaped_keys);
  for (int i = 0; i < ctx->literal_site_count; i++)
    if (ctx->literal_sites[i].block) gc_shared_elements_release(ctx->js, ctx->literal_sites[i].block);
  ctx_free(ctx->literal_sites);
  ctx_free(ctx->literal_keys);

  const_dedup_entry_t *entry;
  const_dedup_entry_t *tmp;
  HASH_ITER(hh, ctx->const_dedup, entry, tmp) {
    HASH_DEL(ctx->const_dedup, entry);
    free(entry);
  }
}

sv_line_table_t *sv_compile_ctx_build_line_table(const char *source, ant_offset_t source_len) {
  if (!source || source_len <= 0) return NULL;

  sv_line_table_t *lt = malloc(sizeof(sv_line_table_t));
  if (!lt) return NULL;

  int cap = (int)(source_len / 32) + 64;
  lt->offsets = malloc((size_t)cap * sizeof(uint32_t));
  lt->source = source;
  lt->count = 0;
  lt->hint = 0;
  lt->col_line = -1;
  lt->offsets[lt->count++] = 0;

  size_t len = (size_t)source_len;
  uint64_t high = 0;
  size_t i = 0;
  
  for (; i + 8 <= len; i += 8) {
    uint64_t word;
    memcpy(&word, source + i, 8);
    high |= word;
  }
  for (; i < len; i++) high |= (uint64_t)(unsigned char)source[i];

  const char *end = source + len;
  for (const char *nl = source; (nl = memchr(nl, '\n', (size_t)(end - nl))); nl++) {
    if (lt->count >= cap) {
      cap *= 2;
      lt->offsets = realloc(lt->offsets, (size_t)cap * sizeof(uint32_t));
    }
    lt->offsets[lt->count++] = (uint32_t)(nl + 1 - source);
  }

  lt->ascii = (high & 0x8080808080808080ull) == 0;
  return lt;
}

void sv_compile_ctx_free_line_table(sv_line_table_t *lt) {
  if (!lt) return;
  free(lt->offsets);
  free(lt);
}

void sv_compile_ctx_line_table_lookup(
  sv_line_table_t *lt,
  uint32_t off,
  uint32_t *out_line,
  uint32_t *out_col
) {
  int line = lt->hint;
  bool ahead = line < lt->count && lt->offsets[line] <= off;
  for (int step = 0; ahead && step < 8; step++) {
    if (line + 1 >= lt->count || lt->offsets[line + 1] > off) goto found;
    line++;
  }

  int lo = ahead ? line : 0;
  int hi = lt->count - 1;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (lt->offsets[mid] <= off) lo = mid;
    else hi = mid - 1;
  }
  line = lo;

found:
  lt->hint = line;
  *out_line = (uint32_t)(line + 1);
  uint32_t start = lt->offsets[line];
  if (lt->ascii) {
    *out_col = off - start + 1;
    return;
  }

  uint32_t pos = start, col = 1;
  if (lt->col_line == line && lt->col_off <= off) {
    pos = lt->col_off;
    col = lt->col;
  } else if (lt->col_line == line && lt->col_off - off < off - start) {
    col = lt->col;
    for (pos = lt->col_off; pos > off; pos--) 
      col -= utf16_units_for_utf8_byte((unsigned char)lt->source[pos - 1]);
  }
  
  for (; pos < off; pos++) 
    col += utf16_units_for_utf8_byte((unsigned char)lt->source[pos]);

  lt->col_line = line;
  lt->col_off = off;
  lt->col = col;
  *out_col = col;
}

uint32_t sv_compile_ctx_hash_local_name(const char *name, uint32_t len) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < len; i++) {
    hash ^= (uint8_t)name[i];
    hash *= 16777619u;
  }
  return hash;
}

void sv_compile_ctx_ensure_local_lookup_capacity(sv_compiler_t *ctx, int next_count) {
  int needed = 16;
  while (needed < next_count * 2) needed <<= 1;
  if (needed <= ctx->local_lookup_cap) return;
  sv_compile_ctx_rebuild_local_lookup(ctx, needed);
}

void sv_compile_ctx_local_lookup_insert(sv_compiler_t *ctx, int idx) {
  if (idx < 0 || idx >= ctx->local_count) return;
  if (!ctx->local_lookup_heads || ctx->local_lookup_cap <= 0) return;

  sv_local_t *loc = &ctx->locals[idx];
  int bucket = (int)(loc->name_hash & (uint32_t)(ctx->local_lookup_cap - 1));
  loc->lookup_next = ctx->local_lookup_heads[bucket];
  ctx->local_lookup_heads[bucket] = idx;
}

void sv_compile_ctx_local_lookup_remove(sv_compiler_t *ctx, int idx) {
  if (
    !ctx->local_lookup_heads || ctx->local_lookup_cap <= 0 ||
    idx < 0 || idx >= ctx->local_count
  ) return;

  sv_local_t *loc = &ctx->locals[idx];
  int bucket = (int)(loc->name_hash & (uint32_t)(ctx->local_lookup_cap - 1));
  int cur = ctx->local_lookup_heads[bucket];
  int prev = -1;

  while (cur != -1) {
    if (cur == idx) {
      int next = ctx->locals[cur].lookup_next;
      if (prev == -1) ctx->local_lookup_heads[bucket] = next;
      else ctx->locals[prev].lookup_next = next;
      break;
    }
    prev = cur;
    cur = ctx->locals[cur].lookup_next;
  }
}
