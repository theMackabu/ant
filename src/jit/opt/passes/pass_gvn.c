#include "passes.h"

static bool pass_is_numbered(const ir_op_t *op) {
  ir_node_properties_t properties = ir_op_properties(op);
  return properties.reads == IR_LOC_NONE && properties.writes == IR_LOC_NONE && !properties.allocates
    && !properties.runs_code && !properties.can_lazy_exit
    && op->code != IR_OP_CONSTANT && op->code != IR_OP_PHI && !ir_op_is_control(op);
}

typedef struct {
  ir_opcode_t code;
  const ir_node_id_t *inputs;
  uint32_t input_count;
  uint32_t next;
  ir_node_vec_t nodes;
} pass_gvn_key_t;

typedef OPT_VEC(pass_gvn_key_t) pass_gvn_keys_t;

typedef struct {
  bool leave;
  ir_block_id_t block;
  uint32_t *keys;
  uint32_t key_count;
} pass_gvn_step_t;

static uint32_t pass_gvn_key(opt_arena_t *arena, opt_map_t *heads, pass_gvn_keys_t *keys, ir_opcode_t code, const ir_node_id_t *inputs, uint32_t count) {
  uint64_t hash = opt_hash_add(0, code);
  for (uint32_t i = 0; i < count; i++) hash = opt_hash_add(hash, inputs[i]);
  bool inserted;
  uint64_t *head = opt_map_entry(heads, &hash, &inserted);
  if (!inserted) {
    for (uint32_t index = (uint32_t)*head; index != IR_NONE; index = keys->items[index].next) {
      const pass_gvn_key_t *key = &keys->items[index];
      if (key->code == code && key->input_count == count && (!count || !memcmp(key->inputs, inputs, count * sizeof *inputs)))
        return index;
    }
  }
  uint32_t index = keys->len;
  pass_gvn_key_t key = {
    .code = code,
    .inputs = opt_arena_dup(arena, inputs, count * sizeof *inputs),
    .input_count = count,
    .next = inserted ? IR_NONE : (uint32_t)*head,
  };
  *head = index;
  opt_vec_push(arena, keys, key);
  return index;
}

void pass_gvn(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_dominators_t dominators;
  pass_dominators_compute(&dominators, graph, &scratch);
  ir_block_vec_t *children = pass_dominator_children(&dominators, &scratch);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
  opt_map_t heads;
  opt_map_init(&heads, &scratch, 1);
  pass_gvn_keys_t keys = {0};

  OPT_VEC(pass_gvn_step_t) steps = {0};
  for (ir_block_id_t block = ir_block_count(graph); block-- > 0;) {
    if (!dominators.reachable[block] || pass_idom(&dominators, block) != IR_NONE) continue;
    pass_gvn_step_t step = {.block = block};
    opt_vec_push(&scratch, &steps, step);
  }
  ir_node_vec_t inputs = {0};
  OPT_VEC(uint32_t) added = {0};
  while (steps.len) {
    pass_gvn_step_t step = steps.items[--steps.len];
    if (step.leave) {
      for (uint32_t i = 0; i < step.key_count; i++) keys.items[step.keys[i]].nodes.len--;
      continue;
    }
    ir_block_id_t block = step.block;
    added.len = 0;
    ir_node_vec_t body = {0};
    opt_vec_clone(&scratch, &body, &ir_block(graph, block)->body);
    for (uint32_t b = 0; b < body.len; b++) {
      ir_node_id_t node_id = body.items[b];
      const ir_node_t *node = ir_node(graph, node_id);
      if (!pass_is_numbered(&node->op)) continue;
      inputs.len = 0;
      for (uint32_t i = 0; i < node->inputs.len; i++) opt_vec_push(&scratch, &inputs, pass_resolve(&replacements, node->inputs.items[i]));
      uint32_t key = pass_gvn_key(&scratch, &heads, &keys, node->op.code, inputs.items, inputs.len);
      ir_node_vec_t *candidates = &keys.items[key].nodes;
      ir_node_id_t existing = IR_NONE;
      for (uint32_t i = candidates->len; i-- > 0 && existing == IR_NONE;)
        if (ir_op_equal(&ir_node(graph, candidates->items[i])->op, &node->op)) existing = candidates->items[i];
      if (existing != IR_NONE) {
        if (node->repr != OPT_REPR_NONE) pass_replace(&replacements, node_id, existing);
        opt_bitset_insert(&removed, node_id);
        continue;
      }
      opt_vec_push(&scratch, candidates, node_id);
      opt_vec_push(&scratch, &added, key);
    }
    pass_gvn_step_t leave = {
      .leave = true,
      .keys = opt_arena_dup(&scratch, added.items, added.len * sizeof *added.items),
      .key_count = added.len,
    };
    opt_vec_push(&scratch, &steps, leave);
    const ir_block_vec_t *block_children = &children[block];
    for (uint32_t i = block_children->len; i-- > 0;) {
      pass_gvn_step_t enter = {.block = block_children->items[i]};
      opt_vec_push(&scratch, &steps, enter);
    }
  }
  pass_replacements_apply(&replacements, graph);
  pass_remove_nodes(graph, &removed);
  opt_arena_free(&scratch);
}
