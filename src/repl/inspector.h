#ifndef ANT_REPL_INSPECTOR_H
#define ANT_REPL_INSPECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

#define REPL_PREVIEW_EXPR_MAX 4096
#define REPL_PREVIEW_TEXT_MAX 1024

typedef struct {
  char *expr;
  size_t expr_len;
} repl_preview_entry_t;

typedef struct {
  repl_preview_entry_t *items;
  size_t count;
  size_t cap;
} repl_preview_snapshot_t;

void repl_preview_snapshot_free(repl_preview_snapshot_t *snapshot);
bool repl_preview_snapshot_build(ant_t *js, repl_preview_snapshot_t *snapshot);

bool repl_preview_compute(
  ant_t *js,
  const repl_preview_snapshot_t *snapshot, const char *line,
  size_t len, char *suffix_out, size_t suffix_len,
  char *preview_out, size_t preview_len
);

#endif
