#include "passes.h"
#include <assert.h>

static bool pass_is_removable(const ir_op_t *op) {
  if (op->code == IR_OP_PHI || op->code == IR_OP_INT32_BINARY || op->code == IR_OP_LOAD_NAMED_STORAGE) return true;
  ir_node_properties_t properties = ir_op_properties(op);
  return !ir_op_is_control(op) && !ir_op_can_eager_exit(op) && !properties.can_lazy_exit
    && !properties.allocates && properties.writes == IR_LOC_NONE && !properties.runs_code;
}

static void pass_count_uses(const ir_graph_t *graph, uint32_t node, uint32_t *uses, int delta, opt_arena_t *scratch, ir_node_vec_t *values) {
  ir_used_values(graph, node, scratch, values);
  for (uint32_t i = 0; i < values->len; i++) {
    ir_node_id_t value = values->items[i];
    if (value == node) continue;
    assert(delta > 0 || uses[value] > 0);
    uses[value] = (uint32_t)((int64_t)uses[value] + delta);
  }
}

static void pass_remove_dead_nodes(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  uint32_t count = ir_node_count(graph);
  uint32_t *uses = opt_arena_new(&scratch, uint32_t, count + 1);
  opt_bitset_t placed = opt_bitset_new(&scratch, count);
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) opt_bitset_insert(&placed, ir_block_node_at(data, i));
  }
  ir_node_vec_t values = {0};
  opt_bitset_for_each(&placed, node) pass_count_uses(graph, node, uses, 1, &scratch, &values);

  opt_bitset_t removed = opt_bitset_new(&scratch, count);
  ir_node_vec_t pending = {0};
  opt_bitset_for_each(&placed, node)
    if (uses[node] == 0 && ir_node(graph, node)->repr != OPT_REPR_NONE) opt_vec_push(&scratch, &pending, node);
  while (pending.len) {
    ir_node_id_t node = pending.items[--pending.len];
    if (opt_bitset_contains(&removed, node) || uses[node] != 0 || !pass_is_removable(&ir_node(graph, node)->op)) continue;
    opt_bitset_insert(&removed, node);
    pass_count_uses(graph, node, uses, -1, &scratch, &values);
    for (uint32_t i = 0; i < values.len; i++) {
      ir_node_id_t input = values.items[i];
      if (uses[input] == 0 && opt_bitset_contains(&placed, input)) opt_vec_push(&scratch, &pending, input);
    }
  }
  pass_remove_nodes(graph, &removed);
  opt_arena_free(&scratch);
}

void pass_dce(ir_graph_t *graph) {
  pass_remove_dead_nodes(graph);
  if (pass_fold_branches_to_one_target(graph)) pass_remove_dead_nodes(graph);
}
