#include "passes.h"

static ir_block_id_t pass_intersect(const ir_block_id_t *idom, ir_block_id_t a, ir_block_id_t b) {
  uint32_t depth_a = 0, depth_b = 0;
  for (ir_block_id_t block = a; block != IR_NONE; block = idom[block]) depth_a++;
  for (ir_block_id_t block = b; block != IR_NONE; block = idom[block]) depth_b++;
  while (depth_a > depth_b) {
    a = idom[a];
    depth_a--;
  }
  while (depth_b > depth_a) {
    b = idom[b];
    depth_b--;
  }
  while (a != b) {
    a = a == IR_NONE ? IR_NONE : idom[a];
    b = b == IR_NONE ? IR_NONE : idom[b];
  }
  return a;
}

void pass_dominators_compute(pass_dominators_t *dominators, const ir_graph_t *graph, opt_arena_t *arena) {
  uint32_t count = ir_block_count(graph);
  bool *reachable = opt_arena_new(arena, bool, count ? count : 1);
  ir_block_vec_t pending = {0};
  for (uint32_t i = 0; i < ir_entry_count(graph); i++) opt_vec_push(arena, &pending, ir_entry_at(graph, i));
  while (pending.len) {
    ir_block_id_t block = pending.items[--pending.len];
    if (reachable[block]) continue;
    reachable[block] = true;
    ir_successors_t successors = ir_block_successors(graph, block);
    for (uint32_t i = 0; i < successors.len; i++) opt_vec_push(arena, &pending, ir_successor_at(&successors, i));
  }

  ir_block_id_t *idom = opt_arena_alloc(arena, (count ? count : 1) * sizeof *idom);
  bool *processed = opt_arena_new(arena, bool, count ? count : 1);
  for (uint32_t i = 0; i < count; i++) idom[i] = IR_NONE;
  bool changed = true;
  while (changed) {
    changed = false;
    for (ir_block_id_t block = 0; block < count; block++) {
      if (!reachable[block]) continue;
      const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
      ir_block_id_t new_idom = IR_NONE;
      if (predecessors->len) {
        bool found = false;
        for (uint32_t i = 0; i < predecessors->len; i++) {
          ir_block_id_t predecessor = predecessors->items[i];
          if (!processed[predecessor]) continue;
          new_idom = found ? pass_intersect(idom, new_idom, predecessor) : predecessor;
          found = true;
        }
        if (!found) continue;
      }
      if (!processed[block] || idom[block] != new_idom) {
        processed[block] = true;
        idom[block] = new_idom;
        changed = true;
      }
    }
  }
  *dominators = (pass_dominators_t){.idom = idom, .reachable = reachable, .count = count};
}

bool pass_dominates(const pass_dominators_t *dominators, ir_block_id_t dominator, ir_block_id_t block) {
  for (ir_block_id_t current = block; current != IR_NONE; current = dominators->idom[current])
    if (current == dominator) return true;
  return false;
}

void pass_back_edge_sources(const pass_dominators_t *dominators, const ir_graph_t *graph, ir_block_id_t header, opt_arena_t *arena, ir_block_vec_t *sources) {
  sources->len = 0;
  const ir_block_vec_t *predecessors = &ir_block(graph, header)->predecessors;
  for (uint32_t i = 0; i < predecessors->len; i++)
    if (pass_dominates(dominators, header, predecessors->items[i])) opt_vec_push(arena, sources, predecessors->items[i]);
}

bool pass_runs_every_iteration(const pass_dominators_t *dominators, ir_block_id_t block, const ir_block_vec_t *back_edge_sources) {
  for (uint32_t i = 0; i < back_edge_sources->len; i++)
    if (!pass_dominates(dominators, block, back_edge_sources->items[i])) return false;
  return true;
}

ir_block_vec_t *pass_dominator_children(const pass_dominators_t *dominators, opt_arena_t *arena) {
  ir_block_vec_t *children = opt_arena_new(arena, ir_block_vec_t, dominators->count ? dominators->count : 1);
  for (ir_block_id_t block = 0; block < dominators->count; block++) {
    ir_block_id_t idom = dominators->idom[block];
    if (idom != IR_NONE && dominators->reachable[block]) opt_vec_push(arena, &children[idom], block);
  }
  return children;
}
