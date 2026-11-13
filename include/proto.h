#pragma once

#include "internal.h"

typedef struct {
  int depth;
  bool overflow;
  ant_value_t fast;
} proto_overflow_guard_t;

typedef enum: uint8_t {
  PROTO_WALK_F_OBJECT_ONLY = 1u << 0,
  PROTO_WALK_F_LOOKUP      = 1u << 1,
} proto_walk_flags_t;

typedef struct {
  int depth;
  bool overflow;
  bool fast_active;
  ant_value_t fast_cur;
  uint8_t fast_t;
} proto_walk_overflow_guard_t;

ant_value_t walk_prototype_chain(ant_t *js, ant_value_t l, ant_value_t ctor_proto);

void proto_overflow_guard_init(proto_overflow_guard_t *g);
void proto_walk_overflow_guard_init(proto_walk_overflow_guard_t *g);

bool same_object_identity(ant_value_t a, ant_value_t b);
bool proto_overflow_guard_hit_cycle(ant_t *js, proto_overflow_guard_t *g, ant_value_t cur);
bool proto_walk_next(ant_t *js, ant_value_t *cur, uint8_t *t, proto_walk_flags_t flags);
bool is_wrapper_ctor_target(ant_t *js, ant_value_t this_val, ant_value_t new_target);

bool proto_walk_overflow_guard_hit_cycle(
  ant_t *js, proto_walk_overflow_guard_t *g,
  ant_value_t cur, uint8_t cur_t, proto_walk_flags_t flags
);
