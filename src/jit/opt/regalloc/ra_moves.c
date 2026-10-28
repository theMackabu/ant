#include "regalloc.h"
#include <assert.h>

void ra_resolve_parallel_moves(const ra_move_t *moves, uint32_t count, ra_location_t temp, opt_arena_t *arena, ra_moves_t *result) {
  ra_moves_t pending = {0};
  for (uint32_t i = 0; i < count; i++) {
    if (ra_location_equal(moves[i].from, moves[i].to)) continue;
    assert(moves[i].to.kind != RA_LOC_CONSTANT);
    assert(!ra_location_equal(moves[i].from, temp) && !ra_location_equal(moves[i].to, temp));
    opt_vec_push(arena, &pending, moves[i]);
  }
  result->len = 0;
  while (pending.len) {
    uint32_t ready = pending.len;
    for (uint32_t i = 0; i < pending.len && ready == pending.len; i++) {
      bool needed = false;
      for (uint32_t j = 0; j < pending.len && !needed; j++) needed = ra_location_equal(pending.items[j].from, pending.items[i].to);
      if (!needed) ready = i;
    }
    if (ready < pending.len) {
      opt_vec_push(arena, result, pending.items[ready]);
      opt_vec_remove(&pending, ready);
      continue;
    }
    ra_location_t saved = pending.items[0].to;
    ra_move_t save = {.from = saved, .to = temp};
    opt_vec_push(arena, result, save);
    for (uint32_t i = 0; i < pending.len; i++)
      if (ra_location_equal(pending.items[i].from, saved)) pending.items[i].from = temp;
  }
}
