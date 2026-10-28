#include "passes.h"
#include "../ir/ir_value.h"

typedef struct {
  ir_graph_t *graph;
  ir_node_vec_t *users;
  uint32_t *parent;
  bool *poisoned;
  bool *checks;
  bool *loop_carried;
  bool *crosses_calls;
  opt_bitset_t members;
} pass_boxed_t;

static uint32_t pass_boxed_find(pass_boxed_t *s, uint32_t node) {
  while (s->parent[node] != node) {
    s->parent[node] = s->parent[s->parent[node]];
    node = s->parent[node];
  }
  return node;
}

static void pass_boxed_union(pass_boxed_t *s, uint32_t a, uint32_t b) {
  a = pass_boxed_find(s, a);
  b = pass_boxed_find(s, b);
  if (a == b) return;
  s->parent[b] = a;
  s->poisoned[a] |= s->poisoned[b];
  s->checks[a] |= s->checks[b];
  s->loop_carried[a] |= s->loop_carried[b];
  s->crosses_calls[a] |= s->crosses_calls[b];
}

static bool pass_boxed_is_call(ir_opcode_t code) {
  return code == IR_OP_CALL_DIRECT || code == IR_OP_CALL_NATIVE || code == IR_OP_CALL_CONSTRUCT;
}

static bool pass_boxed_member(const ir_graph_t *graph, ir_node_id_t node) {
  const ir_node_t *data = ir_node(graph, node);
  if (data->op.code == IR_OP_CHECK_INT32) return ir_node(graph, data->inputs.items[0])->repr == OPT_REPR_TAGGED;
  return data->op.code == IR_OP_INT32_BINARY && (data->op.binary == IR_BINARY_ADD || data->op.binary == IR_BINARY_SUB);
}

static bool pass_boxed_int32_constant(const ir_graph_t *graph, ir_node_id_t node) {
  const ir_node_t *data = ir_node(graph, node);
  return data->op.code == IR_OP_CONSTANT && data->repr == OPT_REPR_INT32;
}

static bool pass_boxed_operands(pass_boxed_t *s, ir_node_id_t node, ir_node_id_t member) {
  const ir_node_t *data = ir_node(s->graph, node);
  bool convertible = true;
  for (uint32_t i = 0; i < data->inputs.len; i++) {
    ir_node_id_t input = data->inputs.items[i];
    if (opt_bitset_contains(&s->members, input)) pass_boxed_union(s, member, input);
    else if (!pass_boxed_int32_constant(s->graph, input)) convertible = false;
  }
  return convertible;
}

static bool pass_boxed_feeds_call(const pass_boxed_t *s, ir_node_id_t box) {
  for (uint32_t i = 0; i < s->users[box].len; i++)
    if (pass_boxed_is_call(ir_node(s->graph, s->users[box].items[i])->op.code)) return true;
  return false;
}

static void pass_boxed_classify(pass_boxed_t *s, ir_node_id_t member) {
  const ir_node_t *data = ir_node(s->graph, member);
  if (data->op.code == IR_OP_CHECK_INT32) {
    ir_opcode_t source = ir_node(s->graph, data->inputs.items[0])->op.code;
    uint32_t root = pass_boxed_find(s, member);
    s->checks[root] = true;
    s->loop_carried[root] |= source == IR_OP_PHI;
    s->crosses_calls[root] |= pass_boxed_is_call(source);
  } else if (!pass_boxed_operands(s, member, member)) s->poisoned[pass_boxed_find(s, member)] = true;
  for (uint32_t i = 0; i < s->users[member].len; i++) {
    ir_node_id_t user = s->users[member].items[i];
    bool allowed = true;
    switch (ir_node(s->graph, user)->op.code) {
      case IR_OP_BOX_INT32:
        if (pass_boxed_feeds_call(s, user)) s->crosses_calls[pass_boxed_find(s, member)] = true;
        break;
      case IR_OP_INT32_TO_FLOAT64: break;
      case IR_OP_INT32_COMPARE: allowed = pass_boxed_operands(s, user, member); break;
      default:
        allowed = opt_bitset_contains(&s->members, user);
        if (allowed) pass_boxed_union(s, member, user);
        break;
    }
    if (!allowed) s->poisoned[pass_boxed_find(s, member)] = true;
  }
}

static bool pass_boxed_converts(pass_boxed_t *s, ir_node_id_t member) {
  uint32_t root = pass_boxed_find(s, member);
  return !s->poisoned[root] && s->checks[root] && (!s->loop_carried[root] || s->crosses_calls[root]);
}

static void pass_boxed_float_inputs(pass_boxed_t *s, pass_constants_t *constants, ir_node_id_t node) {
  for (uint32_t i = 0; i < ir_node(s->graph, node)->inputs.len; i++) {
    ir_node_id_t input = ir_node(s->graph, node)->inputs.items[i];
    if (!pass_boxed_int32_constant(s->graph, input)) continue;
    int32_t integer = (int32_t)(uint32_t)ir_node(s->graph, input)->op.bits;
    input = pass_constant_typed(constants, s->graph, ir_value_double_bits((double)integer), OPT_REPR_FLOAT64);
    ir_node(s->graph, node)->inputs.items[i] = input;
  }
}

void pass_boxed_arithmetic(ir_graph_t *graph) {
  uint32_t count = ir_node_count(graph);
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_boxed_t s = {
    .graph = graph,
    .users = opt_arena_new(&scratch, ir_node_vec_t, count + 1),
    .parent = opt_arena_new(&scratch, uint32_t, count + 1),
    .poisoned = opt_arena_new(&scratch, bool, count + 1),
    .checks = opt_arena_new(&scratch, bool, count + 1),
    .loop_carried = opt_arena_new(&scratch, bool, count + 1),
    .crosses_calls = opt_arena_new(&scratch, bool, count + 1),
    .members = opt_bitset_new(&scratch, count),
  };
  ir_node_id_t *checked = opt_arena_new(&scratch, ir_node_id_t, count + 1);
  bool any = false;
  for (ir_node_id_t node = 0; node < count; node++) {
    s.parent[node] = node;
    checked[node] = IR_NONE;
  }
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) {
      ir_node_id_t node = ir_block_node_at(data, i);
      const ir_node_t *node_data = ir_node(graph, node);
      for (uint32_t j = 0; j < node_data->inputs.len; j++) opt_vec_push(&scratch, &s.users[node_data->inputs.items[j]], node);
      if (pass_boxed_member(graph, node)) {
        opt_bitset_insert(&s.members, node);
        any |= node_data->op.code == IR_OP_CHECK_INT32;
      }
    }
  }
  if (!any) {
    opt_arena_free(&scratch);
    return;
  }
  opt_bitset_for_each(&s.members, member) {
    const ir_node_t *data = ir_node(graph, member);
    if (data->op.code != IR_OP_CHECK_INT32) continue;
    ir_node_id_t input = data->inputs.items[0];
    if (checked[input] == IR_NONE) checked[input] = member;
    else pass_boxed_union(&s, checked[input], member);
  }
  opt_bitset_for_each(&s.members, member) pass_boxed_classify(&s, member);

  pass_constants_t constants;
  pass_constants_init(&constants, graph, &scratch);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  opt_bitset_t converted_users = opt_bitset_new(&scratch, count);
  opt_bitset_t removed = opt_bitset_new(&scratch, count);
  bool changed = false;
  opt_bitset_for_each(&s.members, member) {
    if (!pass_boxed_converts(&s, member)) continue;
    changed = true;
    ir_node_t *data = ir_node(graph, member);
    data->repr = OPT_REPR_FLOAT64;
    if (data->op.code == IR_OP_CHECK_INT32) data->op.code = IR_OP_CHECK_NUMBER;
    else {
      data->op.code = IR_OP_FLOAT64_BINARY;
      data->frame_state = IR_NONE;
      pass_boxed_float_inputs(&s, &constants, member);
    }
    for (uint32_t i = 0; i < s.users[member].len; i++) {
      ir_node_id_t user = s.users[member].items[i];
      if (opt_bitset_contains(&converted_users, user)) continue;
      ir_node_t *user_data = ir_node(graph, user);
      switch (user_data->op.code) {
        case IR_OP_BOX_INT32: user_data->op.code = IR_OP_BOX_FLOAT64; break;
        case IR_OP_INT32_TO_FLOAT64:
          pass_replace(&replacements, user, member);
          opt_bitset_insert(&removed, user);
          break;
        case IR_OP_INT32_COMPARE:
          user_data->op.code = IR_OP_FLOAT64_COMPARE;
          pass_boxed_float_inputs(&s, &constants, user);
          break;
        default: continue;
      }
      opt_bitset_insert(&converted_users, user);
    }
  }
  if (changed) {
    pass_replacements_apply(&replacements, graph);
    pass_remove_nodes(graph, &removed);
  }
  opt_arena_free(&scratch);
}
