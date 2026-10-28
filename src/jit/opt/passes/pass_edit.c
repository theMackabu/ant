#include "passes.h"
#include <assert.h>

void pass_replacements_init(pass_replacements_t *replacements, const ir_graph_t *graph, opt_arena_t *arena) {
  uint32_t len = ir_node_count(graph);
  ir_node_id_t *map = opt_arena_alloc(arena, (len ? len : 1) * sizeof *map);
  for (uint32_t i = 0; i < len; i++) map[i] = IR_NONE;
  *replacements = (pass_replacements_t){.arena = arena, .map = map, .len = len};
}

void pass_replace(pass_replacements_t *replacements, ir_node_id_t value, ir_node_id_t replacement) {
  assert(value != replacement);
  if (value >= replacements->len) {
    uint32_t len = value + 1 > replacements->len * 2 ? value + 1 : replacements->len * 2;
    ir_node_id_t *map = opt_arena_alloc(replacements->arena, len * sizeof *map);
    if (replacements->len) memcpy(map, replacements->map, replacements->len * sizeof *map);
    for (uint32_t i = replacements->len; i < len; i++) map[i] = IR_NONE;
    replacements->map = map;
    replacements->len = len;
  }
  if (replacements->map[value] == IR_NONE) replacements->count++;
  replacements->map[value] = replacement;
}

ir_node_id_t pass_resolve(const pass_replacements_t *replacements, ir_node_id_t value) {
  while (value < replacements->len && replacements->map[value] != IR_NONE) value = replacements->map[value];
  return value;
}

void pass_replacements_apply(const pass_replacements_t *replacements, ir_graph_t *graph) {
  if (pass_replacements_is_empty(replacements)) return;
  for (uint32_t i = 0; i < graph->nodes.len; i++) {
    ir_node_t *node = &graph->nodes.items[i];
    for (uint32_t j = 0; j < node->inputs.len; j++) node->inputs.items[j] = pass_resolve(replacements, node->inputs.items[j]);
  }
  for (uint32_t i = 0; i < graph->frame_states.len; i++) {
    ir_frame_state_t *state = &graph->frame_states.items[i];
    for (uint32_t j = 0; j < state->values.len; j++)
      state->values.items[j].value = pass_resolve(replacements, state->values.items[j].value);
  }
}

void pass_remove_refinements(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
  ir_node_vec_t checks = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->body.len; i++) {
      ir_node_id_t node = data->body.items[i];
      const ir_node_t *node_data = ir_node(graph, node);
      int index = ir_op_refined_input(&node_data->op);
      if (index < 0) continue;
      if (node_data->repr != OPT_REPR_NONE) pass_replace(&replacements, node, node_data->inputs.items[index]);
      if (node_data->op.code == IR_OP_REFINE) opt_bitset_insert(&removed, node);
      else opt_vec_push(&scratch, &checks, node);
    }
  }
  pass_replacements_apply(&replacements, graph);
  pass_remove_nodes(graph, &removed);
  for (uint32_t i = 0; i < checks.len; i++) ir_node(graph, checks.items[i])->repr = OPT_REPR_NONE;
  opt_arena_free(&scratch);
}

static void pass_retain_unremoved(ir_node_vec_t *nodes, const opt_bitset_t *removed) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < nodes->len; i++)
    if (!opt_bitset_has(removed, nodes->items[i])) nodes->items[kept++] = nodes->items[i];
  nodes->len = kept;
}

void pass_remove_nodes(ir_graph_t *graph, const opt_bitset_t *removed) {
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    pass_retain_unremoved(&ir_block(graph, block)->body, removed);
    pass_retain_unremoved(&ir_block(graph, block)->phis, removed);
  }
}

void pass_constants_init(pass_constants_t *constants, const ir_graph_t *graph, opt_arena_t *arena) {
  opt_map_init(&constants->nodes, arena, 2);
  for (ir_node_id_t id = 0; id < ir_node_count(graph); id++) {
    const ir_node_t *node = ir_node(graph, id);
    if (node->op.code != IR_OP_CONSTANT || node->repr == OPT_REPR_NONE) continue;
    opt_map_set2(&constants->nodes, node->op.bits, node->repr, id);
  }
}

ir_node_id_t pass_constant_typed(pass_constants_t *constants, ir_graph_t *graph, uint64_t bits, opt_repr_t repr) {
  bool inserted;
  uint64_t *entry = opt_map_entry(&constants->nodes, (uint64_t[]){bits, repr}, &inserted);
  if (!inserted) return (ir_node_id_t)*entry;
  ir_op_t op = ir_op(IR_OP_CONSTANT);
  op.bits = bits;
  ir_node_id_t node = ir_add_node(graph, op, NULL, 0, repr, 0);
  *entry = node;
  return node;
}

static bool *pass_reachable_blocks(const ir_graph_t *graph, opt_arena_t *arena) {
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
  return reachable;
}

void pass_remove_unreachable_blocks(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  bool *reachable = pass_reachable_blocks(graph, &scratch);
  bool any_unreachable = false;
  for (uint32_t i = 0; i < ir_block_count(graph); i++) any_unreachable |= !reachable[i];
  opt_arena_free(&scratch);
  if (any_unreachable) pass_clean_up_control_flow(graph);
}

static uint32_t pass_edges_to(const ir_graph_t *graph, ir_block_id_t from, ir_block_id_t to) {
  ir_successors_t successors = ir_block_successors(graph, from);
  uint32_t edges = 0;
  for (uint32_t i = 0; i < successors.len; i++) edges += ir_successor_at(&successors, i) == to;
  return edges;
}

void pass_clean_up_control_flow(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  uint32_t block_count = ir_block_count(graph);
  bool *reachable = pass_reachable_blocks(graph, &scratch);

  for (ir_block_id_t block = 0; block < block_count; block++) {
    if (!reachable[block]) continue;
    ir_block_t *data = ir_block(graph, block);
    uint32_t count = data->predecessors.len;
    const ir_block_id_t *predecessors = data->predecessors.items;
    uint32_t *remaining = opt_arena_alloc(&scratch, (count ? count : 1) * sizeof *remaining);
    bool *keep = opt_arena_alloc(&scratch, (count ? count : 1) * sizeof *keep);
    bool keep_all = true;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t first = i;
      for (uint32_t j = 0; j < i; j++) {
        if (predecessors[j] == predecessors[i]) {
          first = j;
          break;
        }
      }
      if (first == i) remaining[i] = reachable[predecessors[i]] ? pass_edges_to(graph, predecessors[i], block) : 0;
      keep[i] = remaining[first] > 0;
      if (keep[i]) remaining[first]--;
      keep_all &= keep[i];
    }
    if (keep_all) continue;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++)
      if (keep[i]) data->predecessors.items[kept++] = predecessors[i];
    data->predecessors.len = kept;
    for (uint32_t p = 0; p < data->phis.len; p++) {
      ir_node_t *phi = ir_node(graph, data->phis.items[p]);
      uint32_t inputs = 0;
      for (uint32_t i = 0; i < phi->inputs.len && i < count; i++)
        if (keep[i]) phi->inputs.items[inputs++] = phi->inputs.items[i];
      phi->inputs.len = inputs;
    }
  }

  ir_block_vec_t order = {0};
  for (ir_block_id_t block = 0; block < block_count; block++)
    if (reachable[block]) opt_vec_push(&scratch, &order, block);
  if (order.len != block_count) ir_reorder_blocks(graph, order.items, order.len);
  opt_arena_free(&scratch);
  pass_remove_trivial_phis(graph);
}

void pass_make_jump(ir_graph_t *graph, ir_node_id_t control, ir_block_id_t target) {
  ir_node_t *node = ir_node(graph, control);
  node->op = ir_op(IR_OP_JUMP);
  node->op.target = target;
  node->inputs.len = 0;
  node->frame_state = IR_NONE;
}

static void pass_end_of(const ir_graph_t *graph, ir_block_id_t block, ir_block_id_t from, ir_block_id_t *end, ir_block_id_t *end_from) {
  for (uint32_t step = 0; step < ir_block_count(graph); step++) {
    const ir_block_t *data = ir_block(graph, block);
    const ir_op_t *op = &ir_control(graph, block)->op;
    if (op->code != IR_OP_JUMP || data->phis.len || data->body.len || op->target == block) break;
    from = block;
    block = op->target;
  }
  *end = block;
  *end_from = from;
}

bool pass_fold_branches_to_one_target(ir_graph_t *graph) {
  bool changed = false;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    ir_node_id_t control = ir_control_id(graph, block);
    const ir_op_t *op = &ir_node(graph, control)->op;
    if (op->code != IR_OP_BRANCH) continue;
    ir_block_id_t if_true = op->branch.if_true, if_false = op->branch.if_false;
    ir_block_id_t true_end, true_from, false_end, false_from;
    pass_end_of(graph, if_true, block, &true_end, &true_from);
    pass_end_of(graph, if_false, block, &false_end, &false_from);
    if (true_end != false_end) continue;
    const ir_block_t *target = ir_block(graph, true_end);
    int true_index = ir_block_vec_position(&target->predecessors, true_from);
    int false_index = ir_block_vec_position(&target->predecessors, false_from);
    if (true_index < 0 || false_index < 0) continue;
    bool same = true;
    for (uint32_t i = 0; i < target->phis.len && same; i++) {
      const ir_node_t *phi = ir_node(graph, target->phis.items[i]);
      same = phi->inputs.items[true_index] == phi->inputs.items[false_index];
    }
    if (!same) continue;
    pass_make_jump(graph, control, if_true);
    changed = true;
  }
  if (changed) pass_clean_up_control_flow(graph);
  return changed;
}

bool pass_frame_state_entering(const ir_graph_t *graph, ir_block_id_t block, ir_block_id_t predecessor, opt_arena_t *arena, ir_frame_state_t *state) {
  const ir_block_t *data = ir_block(graph, block);
  if (data->start_frame_state == IR_NONE) return false;
  int entry_index = ir_block_vec_position(&data->predecessors, predecessor);
  assert(entry_index >= 0);
  const ir_frame_state_t *template = ir_frame_state(graph, data->start_frame_state);
  *state = *template;
  opt_vec_clone(arena, &state->values, &template->values);
  opt_vec_clone(arena, &state->in_frame, &template->in_frame);
  for (uint32_t i = 0; i < state->values.len; i++) {
    ir_node_id_t value = state->values.items[i].value;
    if (ir_node_vec_contains(&data->phis, value)) state->values.items[i].value = ir_node(graph, value)->inputs.items[entry_index];
    else if (ir_node_vec_contains(&data->body, value)) return false;
  }
  return true;
}

void pass_move_cold_blocks_last(ir_graph_t *graph) {
  uint32_t count = ir_block_count(graph);
  bool any = false;
  for (ir_block_id_t block = 0; block < count; block++) any |= ir_block(graph, block)->is_cold;
  if (!any) return;
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  ir_block_id_t *order = opt_arena_alloc(&scratch, count * sizeof *order);
  uint32_t next = 0;
  for (ir_block_id_t block = 0; block < count; block++)
    if (!ir_block(graph, block)->is_cold) order[next++] = block;
  for (ir_block_id_t block = 0; block < count; block++)
    if (ir_block(graph, block)->is_cold) order[next++] = block;
  ir_reorder_blocks(graph, order, count);
  opt_arena_free(&scratch);
}

void pass_remove_trivial_phis(ir_graph_t *graph) {
  for (;;) {
    opt_arena_t scratch;
    opt_arena_init(&scratch);
    pass_replacements_t replacements;
    pass_replacements_init(&replacements, graph, &scratch);
    opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
    for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
      const ir_block_t *data = ir_block(graph, block);
      for (uint32_t p = 0; p < data->phis.len; p++) {
        ir_node_id_t phi = data->phis.items[p];
        const ir_node_t *node = ir_node(graph, phi);
        ir_node_id_t first = IR_NONE;
        bool trivial = true;
        for (uint32_t i = 0; i < node->inputs.len && trivial; i++) {
          ir_node_id_t input = pass_resolve(&replacements, node->inputs.items[i]);
          if (input == phi) continue;
          if (first == IR_NONE) first = input;
          else trivial = input == first;
        }
        if (first == IR_NONE || !trivial) continue;
        pass_replace(&replacements, phi, first);
        opt_bitset_insert(&removed, phi);
      }
    }
    bool done = pass_replacements_is_empty(&replacements);
    if (!done) {
      pass_replacements_apply(&replacements, graph);
      pass_remove_nodes(graph, &removed);
    }
    opt_arena_free(&scratch);
    if (done) return;
  }
}

void pass_remove_unused_frame_states(ir_graph_t *graph) {
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) ir_block(graph, block)->start_frame_state = IR_NONE;
  uint32_t count = graph->frame_states.len;
  if (!count) return;
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  bool *used = opt_arena_new(&scratch, bool, count);
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) {
      ir_frame_state_id_t state = ir_node(graph, ir_block_node_at(data, i))->frame_state;
      while (state != IR_NONE && !used[state]) {
        used[state] = true;
        state = ir_frame_state(graph, state)->parent;
      }
    }
  }
  bool all_used = true;
  for (uint32_t i = 0; i < count; i++) all_used &= used[i];
  if (all_used) {
    opt_arena_free(&scratch);
    return;
  }
  ir_frame_state_id_t *new_index = opt_arena_alloc(&scratch, count * sizeof *new_index);
  uint32_t next = 0;
  for (uint32_t i = 0; i < count; i++) new_index[i] = used[i] ? next++ : IR_NONE;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!used[i]) continue;
    ir_frame_state_t state = graph->frame_states.items[i];
    if (state.parent != IR_NONE) {
      assert(new_index[state.parent] != IR_NONE);
      state.parent = new_index[state.parent];
    }
    graph->frame_states.items[kept++] = state;
  }
  graph->frame_states.len = kept;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) {
      ir_node_t *node = ir_node(graph, ir_block_node_at(data, i));
      if (node->frame_state != IR_NONE) node->frame_state = new_index[node->frame_state];
    }
  }
  opt_arena_free(&scratch);
}
