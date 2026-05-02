#include "silver/compiler.h"
#include "silver/compile_ctx.h"

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
  ctx->is_strict = is_strict;
  ctx->completion_local = -1;
  ctx->strict_args_local = -1;
  ctx->new_target_local = -1;
  ctx->super_local = -1;
  ctx->using_stack_local = -1;
  ctx->line_table = line_table;
  ctx->private_scope = NULL;
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
  ctx->is_async = node && !!(node->flags & FN_ASYNC);
  ctx->is_strict = enclosing->is_strict;
  ctx->mode = mode;
  ctx->completion_local = -1;
  ctx->strict_args_local = -1;
  ctx->new_target_local = -1;
  ctx->super_local = -1;
  ctx->using_stack_local = -1;
  ctx->param_count = node ? node->args.count : 0;
  ctx->private_scope = enclosing->private_scope;
}

void sv_compile_ctx_cleanup(sv_compiler_t *ctx) {
  free(ctx->code);
  free(ctx->constants);
  free(ctx->atoms);
  free(ctx->locals);
  free(ctx->local_lookup_heads);
  free(ctx->upval_descs);
  free(ctx->loops);
  free(ctx->srcpos);
  free(ctx->slot_types);
  free(ctx->deferred_exports);
  free(ctx->using_cleanups);

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
  lt->count = 0;
  lt->offsets[lt->count++] = 0;

  for (ant_offset_t i = 0; i < source_len; i++) {
  if (source[i] == '\n') {
    if (lt->count >= cap) {
      cap *= 2;
      lt->offsets = realloc(lt->offsets, (size_t)cap * sizeof(uint32_t));
    }
    lt->offsets[lt->count++] = (uint32_t)(i + 1);
  }}

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
  int lo = 0;
  int hi = lt->count - 1;

  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (lt->offsets[mid] <= off) lo = mid;
    else hi = mid - 1;
  }

  *out_line = (uint32_t)(lo + 1);
  *out_col = off - lt->offsets[lo] + 1;
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
