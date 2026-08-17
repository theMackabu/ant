#ifndef ESM_TRACE_H
#define ESM_TRACE_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *abs_path;
  char *key;
  uint8_t format;
  uint8_t kind;
  bool lenient;
  uint8_t *data;
  size_t data_len;
} ant_trace_module_t;

typedef struct {
  uint32_t parent_idx;
  char *spec;
  uint32_t child_idx;
  bool is_require;
} ant_trace_edge_t;

typedef struct {
  ant_trace_module_t *modules;
  uint32_t module_count, module_cap;

  ant_trace_edge_t *edges;
  uint32_t edge_count, edge_cap;

  char **warnings;
  uint32_t warning_count, warning_cap;

  uint32_t entry_idx;
  uint32_t external_count;
  char error[1024];
} ant_trace_result_t;

int ant_trace_default_root(
  const char *entry_abs_path, 
  char *out, size_t out_len
);

int ant_esm_trace_graph(
  ant_t *js, const char *entry_abs_path,
  const char *root_dir, ant_trace_result_t *out
);

void ant_esm_trace_free(ant_trace_result_t *result);

#endif
