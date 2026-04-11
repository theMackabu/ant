#ifndef SILVER_COMPILE_CTX_H
#define SILVER_COMPILE_CTX_H

#include <uthash.h>
#include "silver/compiler.h"

void sv_compile_ctx_init_root(
  sv_compiler_t *ctx,
  ant_t *js,
  const char *filename,
  const char *source,
  ant_offset_t source_len,
  sv_compile_mode_t mode,
  bool is_strict,
  sv_line_table_t *line_table
);

void sv_compile_ctx_init_child(
  sv_compiler_t *ctx,
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
);

void sv_compile_ctx_line_table_lookup(
  sv_line_table_t *lt,
  uint32_t off,
  uint32_t *out_line,
  uint32_t *out_col
);

uint32_t sv_compile_ctx_hash_local_name(const char *name, uint32_t len);
sv_line_table_t *sv_compile_ctx_build_line_table(const char *source, ant_offset_t source_len);

void sv_compile_ctx_cleanup(sv_compiler_t *ctx);
void sv_compile_ctx_free_line_table(sv_line_table_t *lt);

void sv_compile_ctx_ensure_local_lookup_capacity(sv_compiler_t *ctx, int next_count);
void sv_compile_ctx_local_lookup_insert(sv_compiler_t *ctx, int idx);
void sv_compile_ctx_local_lookup_remove(sv_compiler_t *ctx, int idx);

#endif
