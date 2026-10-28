#include "passes.h"

typedef struct {
  int64_t min;
  int64_t max;
} pass_range_t;

static bool pass_range(int64_t min, int64_t max, pass_range_t *out) {
  if (!(INT32_MIN <= min && min <= max && max <= INT32_MAX)) return false;
  *out = (pass_range_t){min, max};
  return true;
}

static bool pass_range_product(pass_range_t a, pass_range_t b, pass_range_t *out) {
  int64_t products[4] = {a.min * b.min, a.min * b.max, a.max * b.min, a.max * b.max};
  int64_t min = products[0], max = products[0];
  for (int i = 1; i < 4; i++) {
    if (products[i] < min) min = products[i];
    if (products[i] > max) max = products[i];
  }
  return pass_range(min, max, out);
}

typedef enum : uint8_t {
  PASS_RANGE_UNKNOWN,
  PASS_RANGE_NONE,
  PASS_RANGE_KNOWN,
} pass_range_state_t;

typedef struct {
  const ir_graph_t *graph;
  const pass_dominators_t *dominators;
  ir_block_id_t *phi_blocks;
  pass_range_state_t *states;
  pass_range_t *known;
  const pass_range_t *assumed;
  const opt_bitset_t *assumes;
} pass_ranges_t;

static bool pass_int32_constant(const ir_graph_t *graph, ir_node_id_t value, int64_t *out) {
  const ir_node_t *node = ir_node(graph, ir_unrefined(graph, value));
  if (node->op.code != IR_OP_CONSTANT || node->repr != OPT_REPR_INT32) return false;
  *out = (int32_t)(uint32_t)node->op.bits;
  return true;
}

static bool pass_range_of(pass_ranges_t *ranges, ir_node_id_t value, pass_range_t *out);

static bool pass_below_count(pass_ranges_t *ranges, const pass_range_t *range, ir_node_id_t count, pass_range_t *out) {
  pass_range_t counts;
  int64_t constant;
  if (pass_int32_constant(ranges->graph, count, &constant)) {
    if (!pass_range(constant, constant, &counts)) return false;
  } else if (!pass_range_of(ranges, count, &counts) || counts.min < 0) {
    if (!range) return false;
    *out = *range;
    return true;
  }
  pass_range_t below;
  if (!pass_range(0, counts.max - 1, &below)) return false;
  if (!range) {
    *out = below;
    return true;
  }
  return pass_range(range->min > below.min ? range->min : below.min, range->max < below.max ? range->max : below.max, out);
}

static bool pass_is_merge(const pass_ranges_t *ranges, ir_node_id_t phi) {
  ir_block_id_t block = ranges->phi_blocks[phi];
  if (block == IR_NONE) return false;
  const ir_block_vec_t *predecessors = &ir_block(ranges->graph, block)->predecessors;
  for (uint32_t i = 0; i < predecessors->len; i++)
    if (pass_dominates(ranges->dominators, block, predecessors->items[i])) return false;
  return true;
}

static bool pass_range_compute(pass_ranges_t *ranges, ir_node_id_t value, pass_range_t *out) {
  const ir_graph_t *graph = ranges->graph;
  const ir_node_t *node = ir_node(graph, value);
  if (node->repr != OPT_REPR_INT32) return false;
  int64_t constant;
  if (pass_int32_constant(graph, value, &constant)) return pass_range(constant, constant, out);
  pass_range_t lhs, rhs;
  if (node->op.code == IR_OP_CHECK_BOUNDS) {
    bool has_index = pass_range_of(ranges, node->inputs.items[0], &lhs);
    return pass_below_count(ranges, has_index ? &lhs : NULL, node->inputs.items[1], out);
  }
  int refined = ir_op_refined_input(&node->op);
  if (refined >= 0) return pass_range_of(ranges, node->inputs.items[refined], out);
  switch (node->op.code) {
    case IR_OP_PHI: {
      if (!pass_is_merge(ranges, value) || !node->inputs.len) return false;
      for (uint32_t i = 0; i < node->inputs.len; i++) {
        pass_range_t input;
        if (!pass_range_of(ranges, node->inputs.items[i], &input)) return false;
        if (i == 0) lhs = input;
        else if (!pass_range(input.min < lhs.min ? input.min : lhs.min, input.max > lhs.max ? input.max : lhs.max, &lhs)) return false;
      }
      *out = lhs;
      return true;
    }
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY: {
      ir_node_id_t left = node->inputs.items[0], right = node->inputs.items[1];
      switch (node->op.binary) {
        case IR_BINARY_ADD:
          if (!pass_range_of(ranges, left, &lhs) || !pass_range_of(ranges, right, &rhs)) return false;
          return pass_range(lhs.min + rhs.min, lhs.max + rhs.max, out);
        case IR_BINARY_SUB:
          if (!pass_range_of(ranges, left, &lhs) || !pass_range_of(ranges, right, &rhs)) return false;
          return pass_range(lhs.min - rhs.max, lhs.max - rhs.min, out);
        case IR_BINARY_MUL:
          if (!pass_range_of(ranges, left, &lhs) || !pass_range_of(ranges, right, &rhs)) return false;
          return pass_range_product(lhs, rhs, out);
        case IR_BINARY_LEFT_SHIFT:
          if (!pass_range_of(ranges, left, &lhs) || !pass_int32_constant(graph, right, &constant)) return false;
          return pass_range(lhs.min << (constant & 31), lhs.max << (constant & 31), out);
        case IR_BINARY_RIGHT_SHIFT:
          if (!pass_int32_constant(graph, right, &constant)) return false;
          if (!pass_range_of(ranges, left, &lhs)) lhs = (pass_range_t){INT32_MIN, INT32_MAX};
          return pass_range(lhs.min >> (constant & 31), lhs.max >> (constant & 31), out);
        case IR_BINARY_UNSIGNED_RIGHT_SHIFT:
          if (!pass_int32_constant(graph, right, &constant)) return false;
          if (pass_range_of(ranges, left, &lhs) && lhs.min >= 0) return pass_range(lhs.min >> (constant & 31), lhs.max >> (constant & 31), out);
          return (constant & 31) && pass_range(0, (int64_t)UINT32_MAX >> (constant & 31), out);
        case IR_BINARY_BITWISE_AND:
          if (!pass_int32_constant(graph, right, &constant) || constant < 0) return false;
          if (pass_range_of(ranges, left, &lhs) && lhs.min >= 0) return pass_range(0, lhs.max < constant ? lhs.max : constant, out);
          return pass_range(0, constant, out);
        default: return false;
      }
    }
    case IR_OP_LOAD_ELEMENTS_LENGTH:
    case IR_OP_LOAD_ELEMENTS_CAPACITY: return pass_range(0, INT32_MAX, out);
    default: return false;
  }
}

static bool pass_range_of(pass_ranges_t *ranges, ir_node_id_t value, pass_range_t *out) {
  if (ranges->assumes && opt_bitset_contains(ranges->assumes, value)) {
    *out = ranges->assumed[value];
    return true;
  }
  switch (ranges->states[value]) {
    case PASS_RANGE_KNOWN: *out = ranges->known[value]; return true;
    case PASS_RANGE_NONE: return false;
    case PASS_RANGE_UNKNOWN: break;
  }
  ranges->states[value] = PASS_RANGE_NONE;
  pass_range_t range;
  if (!pass_range_compute(ranges, value, &range)) return false;
  ranges->states[value] = PASS_RANGE_KNOWN;
  ranges->known[value] = range;
  *out = range;
  return true;
}

typedef struct {
  ir_node_id_t index;
  ir_block_id_t block;
  uint32_t position;
  ir_node_id_t count;
} pass_bounded_t;

typedef struct {
  ir_node_id_t node;
  ir_block_id_t block;
  uint32_t position;
} pass_arithmetic_t;

static bool pass_operand_range(pass_ranges_t *ranges, const pass_bounded_t *bounded, uint32_t bounded_count, const pass_arithmetic_t *at, ir_node_id_t input, ir_node_vec_t *counts, opt_arena_t *arena, pass_range_t *out) {
  pass_range_t range;
  bool has_range = pass_range_of(ranges, input, &range);
  ir_node_id_t unrefined = ir_unrefined(ranges->graph, input);
  for (uint32_t i = 0; i < bounded_count; i++) {
    const pass_bounded_t *check = &bounded[i];
    if (check->index != unrefined) continue;
    bool dominates = check->block == at->block ? check->position < at->position : pass_dominates(ranges->dominators, check->block, at->block);
    if (dominates && pass_below_count(ranges, has_range ? &range : NULL, check->count, out)) {
      opt_vec_push(arena, counts, check->count);
      return true;
    }
  }
  if (has_range) *out = range;
  return has_range;
}

static opt_bitset_t pass_osr_only_blocks(const ir_graph_t *graph, opt_arena_t *arena) {
  uint32_t count = ir_block_count(graph);
  opt_bitset_t reached[2] = {opt_bitset_new(arena, count), opt_bitset_new(arena, count)};
  ir_block_vec_t pending = {0};
  for (uint32_t from = 0; from < 2; from++) {
    if (from == 0) for (uint32_t i = 0; i < graph->osr_entries.len; i++) opt_vec_push(arena, &pending, graph->osr_entries.items[i].block);
    else opt_vec_push(arena, &pending, 0);
    while (pending.len) {
      ir_block_id_t block = pending.items[--pending.len];
      if (opt_bitset_contains(&reached[from], block)) continue;
      opt_bitset_insert(&reached[from], block);
      ir_successors_t successors = ir_block_successors(graph, block);
      for (uint32_t i = 0; i < successors.len; i++) opt_vec_push(arena, &pending, ir_successor_at(&successors, i));
    }
  }
  opt_bitset_subtract(&reached[0], &reached[1]);
  return reached[0];
}

static void pass_entering_values(pass_ranges_t *ranges, const ir_block_id_t *block_of, opt_bitset_t *assumes, pass_range_t *assumed) {
  const ir_graph_t *graph = ranges->graph;
  if (!graph->osr_entries.len) return;
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  opt_bitset_t osr_only = pass_osr_only_blocks(graph, &scratch);
  opt_bitset_t carried = opt_bitset_new(&scratch, ir_node_count(graph));
  for (ir_node_id_t phi = 0; phi < ir_node_count(graph); phi++) {
    if (ranges->phi_blocks[phi] == IR_NONE || pass_is_merge(ranges, phi)) continue;
    const ir_node_vec_t *inputs = &ir_node(graph, phi)->inputs;
    for (uint32_t i = 0; i < inputs->len; i++) opt_bitset_insert(&carried, inputs->items[i]);
  }
  for (ir_node_id_t phi = 0; phi < ir_node_count(graph); phi++) {
    if (!pass_is_merge(ranges, phi) || ir_node(graph, phi)->repr != OPT_REPR_INT32 || opt_bitset_contains(&carried, phi)) continue;
    const ir_node_vec_t *inputs = &ir_node(graph, phi)->inputs;
    pass_range_t merged = {0, -1}, range;
    ir_node_vec_t entering = {0};
    bool ok = true;
    for (uint32_t i = 0; i < inputs->len && ok; i++) {
      ir_node_id_t input = ir_unrefined(graph, inputs->items[i]);
      const ir_node_t *node = ir_node(graph, input);
      ir_block_id_t block = block_of[input];
      if (node->op.code == IR_OP_CHECK_INT32 && block != IR_NONE && opt_bitset_contains(&osr_only, block)) {
        const ir_frame_state_t *state = ir_frame_state(graph, node->frame_state);
        ok = !ir_exit_sites_contain(graph, state->func, state->bc_off, OPT_EXIT_OUT_OF_BOUNDS);
        opt_vec_push(&scratch, &entering, input);
      } else if ((ok = pass_range_of(ranges, input, &range))) {
        if (merged.min > merged.max) merged = range;
        merged.min = range.min < merged.min ? range.min : merged.min;
        merged.max = range.max > merged.max ? range.max : merged.max;
      }
    }
    if (!ok || !entering.len || merged.min > merged.max || merged.min < 0 || merged.max >= INT32_MAX) continue;
    for (uint32_t i = 0; i < entering.len; i++) {
      ir_node_id_t value = entering.items[i];
      if (opt_bitset_contains(assumes, value) && assumed[value].max <= merged.max) continue;
      opt_bitset_insert(assumes, value);
      assumed[value] = (pass_range_t){0, merged.max};
    }
  }
  opt_arena_free(&scratch);
}

static void pass_assumptions_of(const ir_graph_t *graph, const opt_bitset_t *assumes, ir_node_id_t value, opt_bitset_t *used, opt_bitset_t *visited, opt_arena_t *arena) {
  ir_node_vec_t pending = {0};
  opt_vec_push(arena, &pending, value);
  while (pending.len) {
    ir_node_id_t node = pending.items[--pending.len];
    if (opt_bitset_contains(visited, node)) continue;
    opt_bitset_insert(visited, node);
    if (opt_bitset_contains(assumes, node)) {
      opt_bitset_insert(used, node);
      continue;
    }
    const ir_node_vec_t *inputs = &ir_node(graph, node)->inputs;
    for (uint32_t i = 0; i < inputs->len; i++) opt_vec_push(arena, &pending, inputs->items[i]);
  }
}

void pass_bounds_check_elimination(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  uint32_t count = ir_node_count(graph);
  ir_block_id_t *phi_blocks = opt_arena_alloc(&scratch, (count + 1) * sizeof *phi_blocks);
  for (uint32_t i = 0; i < count; i++) phi_blocks[i] = IR_NONE;
  OPT_VEC(pass_bounded_t) bounded = {0};
  OPT_VEC(pass_arithmetic_t) arithmetic = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->phis.len; i++) phi_blocks[data->phis.items[i]] = block;
    for (uint32_t position = 0; position < data->body.len; position++) {
      ir_node_id_t node = data->body.items[position];
      const ir_node_t *node_data = ir_node(graph, node);
      int64_t constant;
      if (node_data->op.code == IR_OP_CHECK_BOUNDS && !pass_int32_constant(graph, node_data->inputs.items[1], &constant)) {
        pass_bounded_t check = {ir_unrefined(graph, node_data->inputs.items[0]), block, position, node_data->inputs.items[1]};
        opt_vec_push(&scratch, &bounded, check);
      }
      if (node_data->op.code != IR_OP_INT32_BINARY) continue;
      ir_binary_op_t op = node_data->op.binary;
      if (op != IR_BINARY_ADD && op != IR_BINARY_SUB && op != IR_BINARY_MUL) continue;
      pass_arithmetic_t entry = {node, block, position};
      opt_vec_push(&scratch, &arithmetic, entry);
    }
  }
  if (!arithmetic.len) {
    opt_arena_free(&scratch);
    return;
  }
  pass_dominators_t dominators;
  pass_dominators_compute(&dominators, graph, &scratch);
  ir_block_id_t *block_of = opt_arena_alloc(&scratch, (count + 1) * sizeof *block_of);
  for (uint32_t i = 0; i < count; i++) block_of[i] = IR_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->body.len; i++) block_of[data->body.items[i]] = block;
  }
  opt_bitset_t assumes = opt_bitset_new(&scratch, count);
  pass_range_t *assumed = opt_arena_alloc(&scratch, (count + 1) * sizeof *assumed);
  pass_ranges_t plain = {
    .graph = graph,
    .dominators = &dominators,
    .phi_blocks = phi_blocks,
    .states = opt_arena_new(&scratch, pass_range_state_t, count + 1),
    .known = opt_arena_alloc(&scratch, (count + 1) * sizeof(pass_range_t)),
  };
  pass_entering_values(&plain, block_of, &assumes, assumed);
  pass_ranges_t ranges = plain;
  ranges.states = opt_arena_new(&scratch, pass_range_state_t, count + 1);
  ranges.assumed = assumed;
  ranges.assumes = &assumes;
  ir_node_vec_t exact = {0}, counts = {0}, relied = {0};
  for (uint32_t i = 0; i < arithmetic.len; i++) {
    const pass_arithmetic_t *at = &arithmetic.items[i];
    const ir_node_t *node = ir_node(graph, at->node);
    pass_range_t lhs, rhs, result;
    counts.len = 0;
    if (!pass_operand_range(&ranges, bounded.items, bounded.len, at, node->inputs.items[0], &counts, &scratch, &lhs)) continue;
    if (!pass_operand_range(&ranges, bounded.items, bounded.len, at, node->inputs.items[1], &counts, &scratch, &rhs)) continue;
    bool fits;
    switch (node->op.binary) {
      case IR_BINARY_ADD: fits = pass_range(lhs.min + rhs.min, lhs.max + rhs.max, &result); break;
      case IR_BINARY_SUB: fits = pass_range(lhs.min - rhs.max, lhs.max - rhs.min, &result); break;
      default: {
        bool lhs_zero = lhs.min <= 0 && 0 <= lhs.max, rhs_zero = rhs.min <= 0 && 0 <= rhs.max;
        fits = pass_range_product(lhs, rhs, &result) && ((lhs.min >= 0 && rhs.min >= 0) || (!lhs_zero && !rhs_zero));
        break;
      }
    }
    if (!fits) continue;
    opt_vec_push(&scratch, &exact, at->node);
    for (uint32_t j = 0; j < counts.len; j++) opt_vec_push(&scratch, &relied, counts.items[j]);
  }
  opt_bitset_t used = opt_bitset_new(&scratch, count), visited = opt_bitset_new(&scratch, count);
  for (uint32_t i = 0; i < exact.len; i++) pass_assumptions_of(graph, &assumes, exact.items[i], &used, &visited, &scratch);
  for (uint32_t i = 0; i < relied.len; i++) pass_assumptions_of(graph, &assumes, relied.items[i], &used, &visited, &scratch);
  for (uint32_t i = 0; i < exact.len; i++) {
    ir_node_t *node = ir_node(graph, exact.items[i]);
    node->op.code = IR_OP_INT32_WRAPPING_BINARY;
    node->frame_state = IR_NONE;
  }
  if (opt_bitset_is_empty(&used)) {
    opt_arena_free(&scratch);
    return;
  }
  pass_constants_t constants;
  pass_constants_init(&constants, graph, &scratch);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  ir_node_vec_t guards = {0};
  opt_bitset_for_each(&used, value) {
    ir_node_id_t bound = pass_constant_typed(&constants, graph, (uint32_t)(assumed[value].max + 1), OPT_REPR_INT32);
    ir_node_t *data = ir_node(graph, value);
    ir_frame_state_id_t state = data->frame_state;
    uint32_t pc = data->bc_off;
    ir_node_id_t guard = ir_add_node(graph, ir_op(IR_OP_CHECK_BOUNDS), (ir_node_id_t[]){value, bound}, 2, OPT_REPR_INT32, pc);
    ir_node(graph, guard)->frame_state = state;
    ir_node_vec_t *body = &ir_block(graph, block_of[value])->body;
    opt_vec_insert(&graph->arena, body, (uint32_t)ir_node_vec_position(body, value) + 1, guard);
    pass_replace(&replacements, value, guard);
    opt_vec_push(&scratch, &guards, guard);
    opt_vec_push(&scratch, &guards, value);
  }
  pass_replacements_apply(&replacements, graph);
  for (uint32_t i = 0; i < guards.len; i += 2) ir_node(graph, guards.items[i])->inputs.items[0] = guards.items[i + 1];
  opt_arena_free(&scratch);
}
