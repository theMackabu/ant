#include "ra_internal.h"
#include <assert.h>

static void ra_frame_state_uses(const ir_graph_t *graph, ir_node_id_t node, opt_arena_t *arena, ir_slot_values_t *values, ir_node_vec_t *uses) {
  uses->len = 0;
  ir_frame_state_id_t frame_state = ir_node(graph, node)->frame_state;
  if (frame_state == IR_NONE) return;
  ir_frame_state_values(graph, frame_state, arena, values);
  opt_vec_reserve(arena, uses, values->len);
  for (uint32_t i = 0; i < values->len; i++)
    if (ra_is_allocated_value(graph, values->items[i].value)) uses->items[uses->len++] = values->items[i].value;
}

ir_block_id_t ra_state_predecessor(const ir_graph_t *graph, ir_block_id_t block) {
  const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
  if (predecessors->len < 2) return IR_NONE;
  ir_block_id_t first = predecessors->items[0];
  for (uint32_t i = 1; i < predecessors->len; i++)
    if (predecessors->items[i] < first) first = predecessors->items[i];
  if (first >= block) return IR_NONE;
  for (uint32_t i = 0; i < predecessors->len; i++) {
    ir_block_id_t predecessor = predecessors->items[i];
    if (predecessor != first && ir_control(graph, predecessor)->op.code != IR_OP_JUMP) return IR_NONE;
  }
  return first;
}

void ra_phi_inputs_from(const ir_graph_t *graph, ir_block_id_t block, ir_block_id_t successor, opt_arena_t *arena, ra_phi_inputs_t *inputs) {
  inputs->len = 0;
  const ir_block_t *successor_block = ir_block(graph, successor);
  if (!successor_block->phis.len) return;
  int index = ir_block_vec_position(&successor_block->predecessors, block);
  assert(index >= 0);
  for (uint32_t i = 0; i < successor_block->phis.len; i++) {
    ir_node_id_t phi = successor_block->phis.items[i];
    ra_phi_input_t entry = {.phi = phi, .input = ir_node(graph, phi)->inputs.items[index]};
    opt_vec_push(arena, inputs, entry);
  }
}

typedef struct {
  const ir_graph_t *graph;
  ra_liveness_t *liveness;
  opt_arena_t *arena;
  opt_arena_t *scratch;
  ir_slot_values_t frame_values;
  ir_node_vec_t frame_uses;
} ra_transfer_t;

typedef void (*ra_visit_t)(ra_transfer_t *transfer, ir_node_id_t node, const opt_bitset_t *live_after, const ir_node_vec_t *used);

static void ra_transfer_block(ra_transfer_t *transfer, ir_block_id_t block, opt_bitset_t *live, ir_node_vec_t *used, ra_visit_t visit) {
  const ir_graph_t *graph = transfer->graph;
  const ir_block_t *data = ir_block(graph, block);
  uint32_t count = data->body.len + (data->control != IR_NONE);
  for (uint32_t i = count; i-- > 0;) {
    ir_node_id_t node = i < data->body.len ? data->body.items[i] : data->control;
    used->len = 0;
    const ir_node_vec_t *inputs = &ir_node(graph, node)->inputs;
    for (uint32_t j = 0; j < inputs->len; j++)
      if (ra_is_allocated_value(graph, inputs->items[j])) opt_vec_push(transfer->scratch, used, inputs->items[j]);
    ra_frame_state_uses(graph, node, transfer->scratch, &transfer->frame_values, &transfer->frame_uses);
    for (uint32_t j = 0; j < transfer->frame_uses.len; j++) opt_vec_push(transfer->scratch, used, transfer->frame_uses.items[j]);
    if (visit) visit(transfer, node, live, used);
    opt_bitset_remove(live, node);
    for (uint32_t j = 0; j < used->len; j++) opt_bitset_insert(live, used->items[j]);
  }
  for (uint32_t i = 0; i < data->phis.len; i++) opt_bitset_remove(live, data->phis.items[i]);
}

static void ra_mark_spills(ra_transfer_t *transfer, ir_node_id_t node, const opt_bitset_t *live_after, bool floats_only) {
  const ir_graph_t *graph = transfer->graph;
  opt_bitset_t *needs_spill = &transfer->liveness->needs_spill;
  opt_bitset_for_each(live_after, value) {
    if (value == node) continue;
    if (!floats_only || ir_node(graph, value)->repr == OPT_REPR_FLOAT64) opt_bitset_insert(needs_spill, value);
  }
  ra_frame_state_uses(graph, node, transfer->scratch, &transfer->frame_values, &transfer->frame_uses);
  for (uint32_t i = 0; i < transfer->frame_uses.len; i++) {
    ir_node_id_t value = transfer->frame_uses.items[i];
    if (value == node) continue;
    if (!floats_only || ir_node(graph, value)->repr == OPT_REPR_FLOAT64) opt_bitset_insert(needs_spill, value);
  }
}

static void ra_visit_node(ra_transfer_t *transfer, ir_node_id_t node, const opt_bitset_t *live_after, const ir_node_vec_t *used) {
  const ir_graph_t *graph = transfer->graph;
  ra_liveness_t *liveness = transfer->liveness;
  if (ra_is_allocated_value(graph, node) && !opt_bitset_contains(live_after, node)) opt_bitset_insert(&liveness->dead_outputs, node);
  const ir_op_t *op = &ir_node(graph, node)->op;
  if (ra_clobbers_caller_saved(op)) ra_mark_spills(transfer, node, live_after, false);
  else if (ra_saves_only_gprs(op)) ra_mark_spills(transfer, node, live_after, true);
  ir_node_vec_t *dies = &liveness->dies_at[node];
  for (uint32_t i = 0; i < used->len; i++) {
    ir_node_id_t value = used->items[i];
    if (!opt_bitset_contains(live_after, value) && !ir_node_vec_contains(dies, value)) opt_vec_push(transfer->arena, dies, value);
  }
}

void ra_liveness_compute(ra_liveness_t *liveness, const ir_graph_t *graph, opt_arena_t *arena) {
  uint32_t value_count = ir_node_count(graph);
  uint32_t block_count = ir_block_count(graph);
  *liveness = (ra_liveness_t){
    .live_in = opt_arena_new(arena, opt_bitset_t, block_count + 1),
    .dies_at = opt_arena_new(arena, ir_node_vec_t, value_count + 1),
    .dead_outputs = opt_bitset_new(arena, value_count),
    .needs_spill = opt_bitset_new(arena, value_count),
    .positions = opt_arena_new(arena, uint32_t, value_count + 1),
    .use_positions = opt_arena_new(arena, ra_positions_t, value_count + 1),
  };

  uint32_t position = 0;
  for (ir_block_id_t block = 0; block < block_count; block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) {
      ir_node_id_t node = ir_block_node_at(data, i);
      liveness->positions[node] = position;
      const ir_node_t *node_data = ir_node(graph, node);
      if (node_data->op.code != IR_OP_PHI) {
        for (uint32_t j = 0; j < node_data->inputs.len; j++) {
          ir_node_id_t input = node_data->inputs.items[j];
          if (ra_is_allocated_value(graph, input)) opt_vec_push(arena, &liveness->use_positions[input], position);
        }
      }
      position++;
    }
  }

  opt_arena_t scratch;
  opt_arena_init(&scratch);
  ra_transfer_t transfer = {.graph = graph, .liveness = liveness, .arena = arena, .scratch = &scratch};
  opt_bitset_t *used_before_defined = opt_arena_new(&scratch, opt_bitset_t, block_count + 1);
  opt_bitset_t *defined = opt_arena_new(&scratch, opt_bitset_t, block_count + 1);
  ir_node_vec_t used = {0};
  for (ir_block_id_t block = 0; block < block_count; block++) {
    used_before_defined[block] = opt_bitset_new(&scratch, value_count);
    ra_transfer_block(&transfer, block, &used_before_defined[block], &used, NULL);
    defined[block] = opt_bitset_new(&scratch, value_count);
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) opt_bitset_insert(&defined[block], ir_block_node_at(data, i));
  }
  ir_node_vec_t *phi_inputs = opt_arena_new(&scratch, ir_node_vec_t, block_count + 1);
  ra_phi_inputs_t inputs = {0};
  for (ir_block_id_t block = 0; block < block_count; block++) {
    ir_successors_t successors = ir_block_successors(graph, block);
    for (uint32_t i = 0; i < successors.len; i++) {
      ra_phi_inputs_from(graph, block, ir_successor_at(&successors, i), &scratch, &inputs);
      for (uint32_t j = 0; j < inputs.len; j++)
        if (ra_is_allocated_value(graph, inputs.items[j].input)) opt_vec_push(&scratch, &phi_inputs[block], inputs.items[j].input);
    }
  }

  opt_bitset_t *live_out = opt_arena_new(arena, opt_bitset_t, block_count + 1);
  liveness->live_out = live_out;
  for (ir_block_id_t block = 0; block < block_count; block++) {
    liveness->live_in[block] = opt_bitset_new(arena, value_count);
    live_out[block] = opt_bitset_new(arena, value_count);
  }
  opt_bitset_t entering = opt_bitset_new(&scratch, value_count);
  bool changed = true;
  while (changed) {
    changed = false;
    for (ir_block_id_t block = block_count; block-- > 0;) {
      opt_bitset_t *live = &live_out[block];
      ir_successors_t successors = ir_block_successors(graph, block);
      for (uint32_t i = 0; i < successors.len; i++) opt_bitset_union_with(live, &liveness->live_in[ir_successor_at(&successors, i)]);
      for (uint32_t i = 0; i < phi_inputs[block].len; i++) opt_bitset_insert(live, phi_inputs[block].items[i]);
      opt_bitset_copy(&entering, live);
      opt_bitset_subtract(&entering, &defined[block]);
      opt_bitset_union_with(&entering, &used_before_defined[block]);
      changed |= opt_bitset_union_with(&liveness->live_in[block], &entering);
    }
  }

  opt_bitset_t live = opt_bitset_new(&scratch, value_count);
  for (ir_block_id_t block = 0; block < block_count; block++) {
    opt_bitset_copy(&live, &live_out[block]);
    ra_transfer_block(&transfer, block, &live, &used, ra_visit_node);
    const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
    bool merge = predecessors->len > 1 || (predecessors->len && predecessors->items[0] >= block);
    if (merge && ra_state_predecessor(graph, block) == IR_NONE) opt_bitset_union_with(&liveness->needs_spill, &liveness->live_in[block]);
  }
  opt_arena_free(&scratch);
}
