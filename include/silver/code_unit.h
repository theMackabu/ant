#ifndef SILVER_CODE_UNIT_H
#define SILVER_CODE_UNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"

typedef struct sv_code_block sv_code_block_t;

struct sv_code_unit {
  ant_t *js;
  sv_code_unit_t *next;
  sv_code_unit_t *outer;

  sv_code_unit_t *pinned_prev;
  sv_code_unit_t *pinned_next;

  sv_func_t *funcs;
  sv_code_block_t **blocks;
  ant_value_t *compile_roots;

  uint64_t gc_epoch;
  uint32_t func_count;
  uint32_t block_count, block_cap;
  uint32_t compile_root_count, compile_root_cap;
  uint32_t pins;

  bool compiling;
  bool immortal;
};

typedef struct {
  sv_code_unit_t *units;
  sv_code_unit_t *pinned;
  
  sv_code_block_t *current;
  sv_code_block_t *spare;
  uint32_t spare_count;
  
  uint32_t unit_count;
  size_t held_bytes;
  
  ant_value_t *young_values;
  size_t young_len;
  size_t young_cap;

  sv_code_unit_t *active;
  sv_func_t **fb_watch;
  size_t fb_watch_len;
  size_t fb_watch_cap;
} sv_code_units_t;

static constexpr size_t SV_CODE_UNIT_OVERHEAD = sizeof(sv_code_unit_t) + 8 * sizeof(void *);

sv_code_unit_t *sv_code_unit_begin(ant_t *js);
const char *sv_code_text(ant_t *js, const char *code, size_t len);

void sv_code_unit_finish(sv_code_unit_t *unit);
void *sv_code_bump(ant_t *js, size_t size);
void *sv_code_unit_bump(sv_code_unit_t *unit, size_t size);
void sv_code_unit_add_func(ant_t *js, sv_func_t *func);

bool sv_code_unit_root(ant_t *js, ant_value_t value);
bool sv_code_unit_retain_template(ant_t *js, sv_func_t *func, ant_value_t value);
bool sv_code_units_watch_feedback(sv_func_t *func, sv_func_t *callee);

void sv_code_unit_pin(sv_code_unit_t *unit);
void sv_code_unit_unpin(sv_code_unit_t *unit);

static inline bool sv_code_unit_dying(const sv_code_unit_t *unit, uint64_t epoch) {
  return unit && !unit->compiling && !unit->pins && !unit->immortal && unit->gc_epoch != epoch;
}

void sv_code_units_sweep(ant_t *js, uint64_t epoch);
void sv_code_units_destroy(ant_t *js);

#endif
