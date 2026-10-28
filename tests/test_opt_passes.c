#include "../src/jit/opt/ir/ir.h"
#include "../src/jit/opt/ir/ir_value.h"
#include "../src/jit/opt/passes/pass_facts.h"
#include "../src/jit/opt/passes/passes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t tests_run;

static ir_node_id_t node_with(ir_graph_t *graph, ir_op_t op, const ir_node_id_t *inputs, uint32_t count, opt_repr_t repr) {
  return ir_add_node(graph, op, inputs, count, repr, 0);
}

#define NODE(graph, op, repr, ...) \
  node_with((graph), (op), (const ir_node_id_t[]){__VA_ARGS__}, \
    (uint32_t)(sizeof((const ir_node_id_t[]){__VA_ARGS__}) / sizeof(ir_node_id_t)), (repr))
#define NODE0(graph, op, repr) node_with((graph), (op), NULL, 0, (repr))

static ir_op_t load_slot(uint32_t slot) {
  ir_op_t op = ir_op(IR_OP_LOAD_SLOT);
  op.slot = slot;
  return op;
}

static ir_op_t constant(uint64_t bits) {
  ir_op_t op = ir_op(IR_OP_CONSTANT);
  op.bits = bits;
  return op;
}

static ir_op_t jump(ir_block_id_t target) {
  ir_op_t op = ir_op(IR_OP_JUMP);
  op.target = target;
  return op;
}

static ir_op_t branch(ir_condition_t condition, ir_block_id_t if_true, ir_block_id_t if_false) {
  ir_op_t op = ir_op(IR_OP_BRANCH);
  op.branch.condition = condition;
  op.branch.if_true = if_true;
  op.branch.if_false = if_false;
  return op;
}

static ir_condition_t int32_condition(ir_comparison_t comparison) {
  ir_condition_t condition = ir_cond(IR_COND_INT32);
  condition.comparison = comparison;
  return condition;
}

static ir_op_t elements_op(ir_opcode_t code, ir_elements_kind_t kind) {
  ir_op_t op = ir_op(code);
  op.elements = kind;
  return op;
}

static ir_op_t slow_path(uint16_t opcode, bool saves_registers) {
  ir_op_t op = ir_op(IR_OP_CALL_SLOW_PATH);
  op.slow.opcode = opcode;
  op.slow.saves_registers = saves_registers;
  return op;
}

static ir_block_id_t block_with(ir_graph_t *graph, const ir_block_id_t *predecessors, uint32_t count) {
  ir_block_id_t block = ir_add_block(graph);
  for (uint32_t i = 0; i < count; i++) opt_vec_push(&graph->arena, &ir_block(graph, block)->predecessors, predecessors[i]);
  return block;
}

#define BLOCK(graph, ...) \
  block_with((graph), (const ir_block_id_t[]){__VA_ARGS__}, \
    (uint32_t)(sizeof((const ir_block_id_t[]){__VA_ARGS__}) / sizeof(ir_block_id_t)))
#define BLOCK0(graph) block_with((graph), NULL, 0)

static void push_body(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  opt_vec_push(&graph->arena, &ir_block(graph, block)->body, node);
}

static void set_control(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  ir_block(graph, block)->control = node;
}

static bool body_contains(const ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  return ir_node_vec_contains(&ir_block(graph, block)->body, node);
}

static ir_frame_state_id_t empty_frame_state(ir_graph_t *graph) {
  ir_frame_state_t state = {
    .bc_off = 0, .mode = OPT_RESUME_AT, .dst = OPT_INVALID_SLOT, .parent = IR_NONE, .passed_argument_count = IR_NONE,
  };
  return ir_add_frame_state(graph, &state);
}

static const ir_frame_layout_t test_layout = {
  .reserved_count = 5, .number_of_registers = 8, .registers_and_locals_count = 10, .number_of_constants = 2, .number_of_arguments = 1,
};

static void assert_valid(const ir_graph_t *graph) {
  opt_text_t errors = {0};
  if (pass_verify(graph, &errors)) return;
  opt_text_t dump = {0};
  ir_dump(graph, &test_layout, &dump);
  fprintf(stderr, "%s\n%s", errors.data, dump.data);
  assert(false);
}

static void assert_invalid(const ir_graph_t *graph, const char *expected) {
  opt_text_t errors = {0};
  bool valid = pass_verify(graph, &errors);
  if (!valid && strstr(errors.data, expected)) {
    opt_text_free(&errors);
    return;
  }
  fprintf(stderr, "expected an error containing \"%s\", got: %s\n", expected, valid ? "(valid)" : errors.data);
  assert(false);
}

static ir_node_id_t diamond(ir_graph_t *graph) {
  ir_graph_init(graph);
  BLOCK0(graph);
  BLOCK(graph, 0);
  BLOCK(graph, 0);
  BLOCK(graph, 1, 2);
  ir_node_id_t condition = NODE0(graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t branch_node = NODE(graph, branch(ir_cond(IR_COND_UNDEFINED), 1, 2), OPT_REPR_NONE, condition);
  push_body(graph, 0, condition);
  set_control(graph, 0, branch_node);
  ir_node_id_t loads[2];
  for (ir_block_id_t block = 1; block < 3; block++) {
    ir_node_id_t load = NODE0(graph, load_slot(block), OPT_REPR_TAGGED);
    ir_node_id_t jump_node = NODE0(graph, jump(3), OPT_REPR_NONE);
    push_body(graph, block, load);
    set_control(graph, block, jump_node);
    loads[block - 1] = load;
  }
  ir_node_id_t phi = NODE(graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, loads[0], loads[1]);
  ir_node_id_t ret = NODE(graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, phi);
  opt_vec_push(&graph->arena, &ir_block(graph, 3)->phis, phi);
  set_control(graph, 3, ret);
  return condition;
}

static void dominators_of_a_diamond(void) {
  ir_graph_t graph;
  diamond(&graph);
  opt_arena_t arena;
  opt_arena_init(&arena);
  pass_dominators_t dominators;
  pass_dominators_compute(&dominators, &graph, &arena);
  assert(pass_idom(&dominators, 0) == IR_NONE);
  assert(pass_idom(&dominators, 1) == 0);
  assert(pass_idom(&dominators, 3) == 0);
  assert(pass_dominates(&dominators, 0, 3));
  assert(!pass_dominates(&dominators, 1, 3));
  assert(pass_dominates(&dominators, 3, 3));
  opt_arena_free(&arena);
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_accepts_a_valid_graph(void) {
  ir_graph_t graph;
  diamond(&graph);
  assert_valid(&graph);
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_uses_of_values_from_blocks_that_do_not_dominate(void) {
  ir_graph_t graph;
  diamond(&graph);
  // The return uses b1's load, which b2 does not see.
  ir_node_id_t load = ir_block(&graph, 1)->body.items[0];
  ir_set_inputs(&graph, ir_block(&graph, 3)->control, &load, 1);
  assert_invalid(&graph, "not defined before it");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_refinements_away_from_the_edge_they_refine(void) {
  ir_graph_t graph;
  diamond(&graph);
  // The join has two predecessors, so no one edge refines anything there.
  ir_node_id_t load = ir_block(&graph, 0)->body.items[0];
  ir_op_t refine = ir_op(IR_OP_REFINE);
  refine.refine.condition = ir_cond(IR_COND_OBJECT);
  refine.refine.holds = true;
  ir_node_id_t refinement = NODE(&graph, refine, ir_node(&graph, load)->repr, load);
  opt_vec_insert(&graph.arena, &ir_block(&graph, 3)->body, 0, refinement);
  assert_invalid(&graph, "is not at the head of");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_inconsistent_edges_and_phis(void) {
  ir_graph_t graph;
  diamond(&graph);
  ir_block(&graph, 3)->predecessors.len--;
  assert_invalid(&graph, "phi");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_edges_missing_from_predecessor_lists(void) {
  ir_graph_t graph;
  diamond(&graph);
  ir_node(&graph, ir_block(&graph, 2)->control)->op = jump(1);
  assert_invalid(&graph, "edges to b1");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_inputs_of_the_wrong_representation(void) {
  ir_graph_t graph;
  diamond(&graph);
  ir_node(&graph, ir_block(&graph, 0)->control)->op.branch.condition = ir_cond(IR_COND_BOOL);
  assert_invalid(&graph, "takes");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_blocks_no_entry_reaches(void) {
  ir_graph_t graph;
  diamond(&graph);
  // b4 is a loop of its own that nothing enters.
  ir_block_id_t block = BLOCK(&graph, 4);
  ir_block(&graph, block)->is_loop_header = true;
  set_control(&graph, block, NODE0(&graph, jump(block), OPT_REPR_NONE));
  assert_invalid(&graph, "b4 is reachable from no entry");
  ir_graph_free(&graph);
  tests_run++;
}

static void verifier_rejects_slow_path_outputs_away_from_their_call(void) {
  ir_graph_t graph;
  ir_node_id_t condition = diamond(&graph);
  ir_node_id_t call = NODE(&graph, slow_path(IR_BC_CALL_METHOD, false), OPT_REPR_TAGGED, condition);
  ir_node(&graph, call)->frame_state = empty_frame_state(&graph);
  ir_op_t output_op = ir_op(IR_OP_SLOW_PATH_OUTPUT);
  output_op.output_index = 1;
  ir_node_id_t output = NODE(&graph, output_op, OPT_REPR_TAGGED, call);
  push_body(&graph, 0, call);
  push_body(&graph, 0, output);
  assert_valid(&graph);
  // Anything between the call and the output may reuse the record.
  ir_node_id_t load = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED);
  opt_vec_insert(&graph.arena, &ir_block(&graph, 0)->body, 2, load);
  assert_invalid(&graph, "does not follow its slow path call");
  ir_graph_free(&graph);
  tests_run++;
}

static void simplification_removes_number_checks_of_numeric_constants(void) {
  struct { uint64_t bits; bool folds; double expected; } cases[] = {
    {ir_value_double_bits(0.5), true, 0.5},
    {ir_value_int32(-3), true, -3.0},
    {IR_VALUE_UNDEFINED, false, 0},
  };
  for (uint32_t i = 0; i < 3; i++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t value = NODE0(&graph, constant(cases[i].bits), OPT_REPR_TAGGED);
    ir_node_id_t check = NODE(&graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, value);
    ir_node(&graph, check)->frame_state = empty_frame_state(&graph);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, check);
    ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed);
    push_body(&graph, 0, check);
    push_body(&graph, 0, boxed);
    set_control(&graph, 0, ret);
    assert_valid(&graph);

    pass_simplify(&graph);
    assert_valid(&graph);
    assert(!body_contains(&graph, 0, check) == cases[i].folds);
    uint64_t bits;
    bool is_constant = ir_constant_value(&graph, ir_node(&graph, boxed)->inputs.items[0], &bits);
    assert(is_constant == cases[i].folds);
    if (is_constant) assert(ir_value_to_double(bits) == cases[i].expected);
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void element_counts_move_out_of_loops_that_only_store_elements(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0, 1);
  BLOCK(&graph, 1);
  ir_node_id_t object = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  ir_node_id_t enter = NODE0(&graph, jump(1), OPT_REPR_NONE);
  push_body(&graph, 0, object);
  push_body(&graph, 0, address);
  set_control(&graph, 0, enter);
  ir_node_id_t length = NODE(&graph, ir_op(IR_OP_LOAD_ELEMENTS_LENGTH), OPT_REPR_INT32, address);
  ir_node_id_t store = NODE(&graph, elements_op(IR_OP_STORE_ELEMENT_AT, IR_ELEMENTS_PACKED), OPT_REPR_NONE, address, zero, object);
  ir_node_id_t more = NODE(&graph, branch(int32_condition(IR_CMP_LESS_THAN), 1, 2), OPT_REPR_NONE, zero, length);
  ir_block(&graph, 1)->is_loop_header = true;
  push_body(&graph, 1, length);
  push_body(&graph, 1, store);
  set_control(&graph, 1, more);
  set_control(&graph, 2, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, object));
  assert_valid(&graph);

  pass_licm(&graph);
  assert(body_contains(&graph, 0, length));
  assert(ir_block(&graph, 1)->body.len == 1 && ir_block(&graph, 1)->body.items[0] == store);
  ir_graph_free(&graph);
  tests_run++;
}

static void typed_array_storage_offsets_are_reused_across_stores_but_not_calls(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_elements_kind_t int32_elements = ir_elements_typed(IR_TYPED_INT32);
  ir_node_id_t object = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object);
  ir_node_id_t first = NODE(&graph, ir_op(IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET), OPT_REPR_POINTER, address);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  ir_node_id_t store = NODE(&graph, elements_op(IR_OP_STORE_ELEMENT_AT, int32_elements), OPT_REPR_NONE, first, zero, zero);
  ir_node_id_t second = NODE(&graph, ir_op(IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET), OPT_REPR_POINTER, address);
  ir_node_id_t load = NODE(&graph, elements_op(IR_OP_LOAD_ELEMENT_AT, int32_elements), OPT_REPR_INT32, second, zero);
  ir_op_t generic = ir_op(IR_OP_GENERIC);
  generic.slow.opcode = IR_BC_CALL;
  ir_node_id_t call = NODE0(&graph, generic, OPT_REPR_NONE);
  ir_node_id_t third = NODE(&graph, ir_op(IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET), OPT_REPR_POINTER, address);
  ir_node(&graph, call)->frame_state = empty_frame_state(&graph);
  ir_node_id_t body[] = {object, address, first, store, second, load, call, third};
  for (uint32_t i = 0; i < 8; i++) push_body(&graph, 0, body[i]);
  set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, object));
  assert_valid(&graph);
  pass_check_elimination(&graph);
  assert(!body_contains(&graph, 0, second));
  assert(ir_node(&graph, load)->inputs.items[0] == first);
  assert(body_contains(&graph, 0, third));
  assert_valid(&graph);
  ir_graph_free(&graph);
  tests_run++;
}

static void typed_array_storage_metadata_moves_out_of_loops_that_only_store_elements(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0, 1);
  BLOCK(&graph, 1);
  ir_node_id_t object = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  ir_node_id_t enter = NODE0(&graph, jump(1), OPT_REPR_NONE);
  push_body(&graph, 0, object);
  push_body(&graph, 0, address);
  set_control(&graph, 0, enter);
  ir_node_id_t length = NODE(&graph, ir_op(IR_OP_LOAD_TYPED_ARRAY_LENGTH), OPT_REPR_INT32, address);
  ir_node_id_t offset = NODE(&graph, ir_op(IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET), OPT_REPR_POINTER, address);
  ir_node_id_t store = NODE(&graph, elements_op(IR_OP_STORE_ELEMENT_AT, ir_elements_typed(IR_TYPED_INT32)), OPT_REPR_NONE, offset, zero, zero);
  ir_node_id_t more = NODE(&graph, branch(int32_condition(IR_CMP_LESS_THAN), 1, 2), OPT_REPR_NONE, zero, length);
  ir_block(&graph, 1)->is_loop_header = true;
  push_body(&graph, 1, length);
  push_body(&graph, 1, offset);
  push_body(&graph, 1, store);
  set_control(&graph, 1, more);
  set_control(&graph, 2, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, object));
  assert_valid(&graph);
  pass_licm(&graph);
  assert(body_contains(&graph, 0, length) && body_contains(&graph, 0, offset));
  assert(ir_block(&graph, 1)->body.len == 1 && ir_block(&graph, 1)->body.items[0] == store);
  assert_valid(&graph);
  ir_graph_free(&graph);
  tests_run++;
}

static void branches_on_known_kinds_fold(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0);
  BLOCK(&graph, 0);
  BLOCK(&graph, 1);
  BLOCK(&graph, 1);
  ir_node_id_t value = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t first = NODE(&graph, branch(ir_cond(IR_COND_STRING), 1, 2), OPT_REPR_NONE, value);
  push_body(&graph, 0, value);
  set_control(&graph, 0, first);
  set_control(&graph, 1, NODE(&graph, branch(ir_cond(IR_COND_STRING), 3, 4), OPT_REPR_NONE, value));
  for (ir_block_id_t block = 2; block <= 4; block++) set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, value));
  assert_valid(&graph);

  pass_check_elimination(&graph);
  uint32_t branches = 0;
  for (ir_block_id_t block = 0; block < ir_block_count(&graph); block++)
    branches += ir_control(&graph, block)->op.code == IR_OP_BRANCH;
  assert(branches == 1);
  ir_graph_free(&graph);
  tests_run++;
}

static void check_elimination_keeps_checks_that_a_later_entry_reaches_blocks_before_it(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 3);
  BLOCK(&graph, 1);
  BLOCK(&graph, 0, 2);
  ir_block(&graph, 2)->is_loop_header = true;
  opt_vec_push(&graph.arena, &graph.osr_entries, ((ir_osr_entry_t){.bc_off = 0, .block = 1}));
  ir_node_id_t value = NODE0(&graph, constant(0x1234), OPT_REPR_TAGGED);
  ir_frame_state_id_t frame_state = empty_frame_state(&graph);
  ir_node_id_t first = NODE(&graph, ir_op(IR_OP_CHECK_OBJECT), OPT_REPR_TAGGED, value);
  ir_node(&graph, first)->frame_state = frame_state;
  push_body(&graph, 0, first);
  ir_block_id_t jumps[4][2] = {{0, 4}, {1, 3}, {2, 4}, {3, 2}};
  for (uint32_t i = 0; i < 4; i++) set_control(&graph, jumps[i][0], NODE0(&graph, jump(jumps[i][1]), OPT_REPR_NONE));
  ir_node_id_t second = NODE(&graph, ir_op(IR_OP_CHECK_OBJECT), OPT_REPR_TAGGED, value);
  ir_node(&graph, second)->frame_state = frame_state;
  push_body(&graph, 4, second);
  set_control(&graph, 4, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, second));
  assert_valid(&graph);

  pass_check_elimination(&graph);
  assert_valid(&graph);
  assert(ir_block(&graph, 4)->body.len == 1 && ir_block(&graph, 4)->body.items[0] == second);
  ir_graph_free(&graph);
  tests_run++;
}

static void simplification_removes_integer_double_round_trips(void) {
  ir_opcode_t conversions[] = {IR_OP_INT32_TO_FLOAT64, IR_OP_UINT32_TO_FLOAT64};
  for (uint32_t i = 0; i < 2; i++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t bits = NODE0(&graph, constant(0xffffffff), OPT_REPR_INT32);
    ir_node_id_t dbl = NODE(&graph, ir_op(conversions[i]), OPT_REPR_FLOAT64, bits);
    ir_node_id_t integer = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, dbl);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, integer);
    ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed);
    push_body(&graph, 0, dbl);
    push_body(&graph, 0, integer);
    push_body(&graph, 0, boxed);
    set_control(&graph, 0, ret);
    assert_valid(&graph);

    pass_simplify(&graph);
    assert_valid(&graph);
    assert(!body_contains(&graph, 0, integer));
    uint64_t value;
    assert(ir_constant_value(&graph, ir_node(&graph, ret)->inputs.items[0], &value) && value == ir_value_int32(-1));
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void simplification_tests_the_truthiness_of_boxed_int32_values_unboxed(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  for (uint32_t i = 1; i < 4; i++) BLOCK(&graph, 0);
  ir_node_id_t array = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED);
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, array);
  ir_node_id_t unboxed = NODE(&graph, ir_op(IR_OP_LOAD_ELEMENTS_LENGTH), OPT_REPR_INT32, address);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, unboxed);
  ir_op_t truthy = ir_op(IR_OP_BRANCH_TRUTHY);
  truthy.truthy.if_true = 1;
  truthy.truthy.if_false = 2;
  truthy.truthy.fallback = 3;
  ir_node_id_t branch_node = NODE(&graph, truthy, OPT_REPR_NONE, boxed);
  push_body(&graph, 0, array);
  push_body(&graph, 0, address);
  push_body(&graph, 0, unboxed);
  push_body(&graph, 0, boxed);
  set_control(&graph, 0, branch_node);
  for (ir_block_id_t block = 1; block < 4; block++) set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, array));
  assert_valid(&graph);

  pass_simplify(&graph);
  assert_valid(&graph);
  const ir_node_t *control = ir_control(&graph, 0);
  ir_op_t expected = branch(int32_condition(IR_CMP_STRICTLY_INEQUALS), 1, 2);
  assert(ir_op_equal(&control->op, &expected));
  assert(control->inputs.items[0] == unboxed);
  uint64_t bits;
  assert(ir_constant_value(&graph, control->inputs.items[1], &bits) && bits == 0);
  ir_graph_free(&graph);
  tests_run++;
}

static void simplification_keeps_double_integer_round_trips(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t dbl = NODE0(&graph, constant(ir_value_double_bits(1.5)), OPT_REPR_FLOAT64);
  ir_node_id_t integer = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, dbl);
  ir_node_id_t converted = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, integer);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, converted);
  ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed);
  push_body(&graph, 0, integer);
  push_body(&graph, 0, converted);
  push_body(&graph, 0, boxed);
  set_control(&graph, 0, ret);
  assert_valid(&graph);

  pass_simplify(&graph);
  assert_valid(&graph);
  assert(ir_node(&graph, converted)->inputs.len == 1 && ir_node(&graph, converted)->inputs.items[0] == integer);
  assert(body_contains(&graph, 0, integer));
  ir_graph_free(&graph);
  tests_run++;
}

static void constant_bounds_checks_only_fold_when_the_unsigned_index_is_in_bounds(void) {
  struct { int32_t index, count; bool folds; } cases[] = {
    {0, 1, true}, {7, 8, true}, {8, 8, false}, {-1, 8, false}, {0, 0, false}, {INT32_MAX, INT32_MIN, true},
  };
  for (uint32_t i = 0; i < 6; i++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t index = NODE0(&graph, constant((uint32_t)cases[i].index), OPT_REPR_INT32);
    ir_node_id_t count = NODE0(&graph, constant((uint32_t)cases[i].count), OPT_REPR_INT32);
    ir_node_id_t check = NODE(&graph, ir_op(IR_OP_CHECK_BOUNDS), OPT_REPR_INT32, index, count);
    ir_node(&graph, check)->frame_state = empty_frame_state(&graph);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, check);
    push_body(&graph, 0, check);
    push_body(&graph, 0, boxed);
    set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
    assert_valid(&graph);
    pass_simplify(&graph);
    assert(body_contains(&graph, 0, check) == !cases[i].folds);
    assert_valid(&graph);
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void frame_writes_after_slow_paths_that_may_initialize_the_frame_check_first(void) {
  ir_frame_layout_t layout = {
    .reserved_count = 5, .number_of_registers = 6, .registers_and_locals_count = 6, .number_of_constants = 0, .number_of_arguments = 0,
  };
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t initialize = NODE0(&graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE);
  ir_node_id_t call = NODE0(&graph, slow_path(IR_BC_CALL_METHOD, true), OPT_REPR_TAGGED);
  ir_op_t store_op = ir_op(IR_OP_STORE_SLOT);
  store_op.slot = 5;
  ir_node_id_t store = NODE(&graph, store_op, OPT_REPR_NONE, call);
  ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, call);
  push_body(&graph, 0, initialize);
  push_body(&graph, 0, call);
  push_body(&graph, 0, store);
  set_control(&graph, 0, ret);

  pass_frame_initialization(&graph, &layout);
  assert(graph.slow_paths_initializing_frame.len == 1 && graph.slow_paths_initializing_frame.items[0] == call);
  const ir_node_vec_t *body = &ir_block(&graph, 0)->body;
  assert(body->len == 3);
  assert(ir_node(&graph, body->items[0])->op.code == IR_OP_CALL_SLOW_PATH);
  assert(ir_node(&graph, body->items[1])->op.code == IR_OP_ENSURE_FRAME_INITIALIZED);
  assert(ir_node(&graph, body->items[2])->op.code == IR_OP_STORE_SLOT);
  ir_graph_free(&graph);
  tests_run++;
}

static ir_shape_check_t shape(ir_cell_t id) {
  return (ir_shape_check_t){.shape = id};
}

static pass_location_t named(ir_node_id_t object, uint32_t offset) {
  return (pass_location_t){PASS_LOC_NAMED, object, offset};
}

static bool contents_are(const pass_facts_t *facts, pass_location_t location, ir_node_id_t expected) {
  ir_node_id_t value;
  bool known = pass_facts_contents(facts, location, &value);
  return expected == IR_NONE ? !known : known && value == expected;
}

static bool shapes_are(const pass_facts_t *facts, ir_node_id_t value, const ir_shape_check_t *expected, uint32_t count) {
  uint32_t known_count = 0;
  const ir_shape_check_t *known = pass_facts_shapes(facts, value, &known_count);
  if (!expected) return known == NULL;
  if (!known || known_count != count) return false;
  for (uint32_t i = 0; i < count; i++)
    if (!ir_shape_check_equal(known[i], expected[i])) return false;
  return true;
}

static void merges_intersect_knowledge_and_union_shape_sets(void) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  pass_facts_context_t context = {.arena = &arena};
  ir_node_id_t a = 1, b = 2, c = 3;
  pass_facts_t left, right, merged;
  pass_facts_init(&left, &context);
  pass_facts_set_object(&left, a);
  pass_facts_set_object(&left, b);
  pass_facts_set_value_kind(&left, c, PASS_KIND_INT32);
  pass_facts_set_shapes(&left, a, (ir_shape_check_t[]){shape(10)}, 1);
  pass_facts_set_shapes(&left, b, (ir_shape_check_t[]){shape(20)}, 1);
  pass_facts_set_contents(&left, named(a, 0), c);
  pass_facts_init(&right, &context);
  pass_facts_set_object(&right, a);
  pass_facts_set_value_kind(&right, c, PASS_KIND_DOUBLE);
  pass_facts_set_shapes(&right, a, (ir_shape_check_t[]){shape(11)}, 1);
  pass_facts_set_contents(&right, named(a, 0), c);

  pass_facts_merge(&merged, (const pass_facts_t *[]){&left, &right}, 2, &context);
  assert(pass_facts_is_object(&merged, a));
  assert(!pass_facts_is_object(&merged, b));
  assert(pass_facts_value_kind(&merged, c) == PASS_KIND_NUMBER);
  assert(shapes_are(&merged, a, (ir_shape_check_t[]){shape(10), shape(11)}, 2));
  assert(shapes_are(&merged, b, NULL, 0));
  assert(contents_are(&merged, named(a, 0), c));
  // The facts merged from are unchanged.
  assert(shapes_are(&left, a, (ir_shape_check_t[]){shape(10)}, 1));
  opt_arena_free(&arena);
  tests_run++;
}

static void narrowing_and_killing(void) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  pass_facts_context_t context = {.arena = &arena};
  ir_node_id_t a = 1, b = 2, count = 3;
  pass_facts_t facts;
  pass_facts_init(&facts, &context);
  pass_facts_set_shapes(&facts, a, (ir_shape_check_t[]){shape(1), shape(2)}, 2);
  pass_facts_set_shapes(&facts, a, (ir_shape_check_t[]){shape(2), shape(3)}, 2);
  assert(shapes_are(&facts, a, (ir_shape_check_t[]){shape(2)}, 1));
  pass_facts_set_contents(&facts, named(a, 1), b);
  pass_facts_set_contents(&facts, named(b, 2), a);
  pass_facts_forget_named_properties_at(&facts, 1);
  assert(contents_are(&facts, named(a, 1), IR_NONE));
  assert(contents_are(&facts, named(b, 2), a));
  pass_location_t elements = {PASS_LOC_ELEMENT_COUNT, PASS_COUNT_LENGTH, a};
  pass_facts_set_contents(&facts, elements, count);
  pass_facts_set_object(&facts, a);
  pass_facts_set_elements_kind(&facts, a, IR_ELEMENTS_PACKED);
  // Stores of elements kill no counts, and nothing kills kinds.
  pass_facts_apply(&facts, (ir_node_properties_t){.writes = IR_LOC_ELEMENTS});
  ir_elements_kind_t kind;
  assert(contents_are(&facts, elements, count));
  assert(pass_facts_elements_kind(&facts, a, &kind) && kind == IR_ELEMENTS_PACKED);
  pass_facts_apply(&facts, (ir_node_properties_t){.writes = IR_LOC_ALL, .runs_code = true});
  assert(pass_facts_is_object(&facts, a));
  assert(shapes_are(&facts, a, NULL, 0));
  assert(!pass_facts_elements_kind(&facts, a, &kind));
  assert(contents_are(&facts, named(b, 2), IR_NONE));
  assert(contents_are(&facts, elements, IR_NONE));
  opt_arena_free(&arena);
  tests_run++;
}

static void stores_to_a_binding_forget_that_binding_of_every_environment(void) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  pass_facts_context_t context = {.arena = &arena};
  ir_node_id_t first = 1, second = 2, value = 3;
  pass_location_t first_binding = {PASS_LOC_BINDING, first, 0};
  pass_location_t second_binding = {PASS_LOC_BINDING, second, 1};
  pass_location_t global = {PASS_LOC_GLOBAL_BINDING, 7, 0};
  pass_facts_t facts;
  pass_facts_init(&facts, &context);
  pass_facts_set_contents(&facts, first_binding, value);
  pass_facts_set_contents(&facts, second_binding, value);
  pass_facts_set_contents(&facts, global, value);
  pass_facts_forget_bindings_at(&facts, 0);
  assert(contents_are(&facts, first_binding, IR_NONE));
  assert(contents_are(&facts, global, IR_NONE));
  assert(contents_are(&facts, second_binding, value));
  opt_arena_free(&arena);
  tests_run++;
}

static void value_kinds_decide_branches(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  opt_repr_t reprs[] = {OPT_REPR_TAGGED, OPT_REPR_TAGGED, OPT_REPR_TAGGED, OPT_REPR_TAGGED, OPT_REPR_TAGGED, OPT_REPR_INT32};
  for (uint32_t i = 0; i < 6; i++) NODE0(&graph, load_slot(0), reprs[i]);
  opt_arena_t arena;
  opt_arena_init(&arena);
  pass_facts_context_t context = {.arena = &arena};
  pass_facts_t facts;
  pass_facts_init(&facts, &context);
  ir_node_id_t value = 1;
  assert(pass_facts_decides(&facts, &graph, value, ir_cond(IR_COND_OBJECT)) == -1);
  pass_facts_set_passed(&facts, value, ir_cond(IR_COND_INT32_VALUE));
  assert(pass_facts_decides(&facts, &graph, value, ir_cond(IR_COND_INT32_VALUE)) == 1);
  assert(pass_facts_decides(&facts, &graph, value, ir_cond(IR_COND_DOUBLE)) == 0);
  assert(pass_facts_decides(&facts, &graph, value, ir_cond(IR_COND_OBJECT)) == 0);
  ir_node_id_t number = 3;
  pass_facts_set_value_kind(&facts, number, PASS_KIND_NUMBER);
  assert(pass_facts_decides(&facts, &graph, number, ir_cond(IR_COND_INT32_VALUE)) == -1);
  // A number that is no int32 is a double.
  pass_facts_set_failed(&facts, number, ir_cond(IR_COND_INT32_VALUE));
  assert(pass_facts_value_kind(&facts, number) == PASS_KIND_DOUBLE);
  assert(pass_facts_decides(&facts, &graph, number, ir_cond(IR_COND_DOUBLE)) == 1);
  // What fails a test fails it again.
  ir_node_id_t other = 4;
  pass_facts_set_failed(&facts, other, ir_cond(IR_COND_STRING));
  assert(pass_facts_decides(&facts, &graph, other, ir_cond(IR_COND_STRING)) == 0);
  assert(pass_facts_value_kind(&facts, other) == PASS_KIND_UNKNOWN);
  // Values whose condition depends on more than their kind are not known to pass it.
  pass_facts_set_passed(&facts, other, ir_cond(IR_COND_NON_NEGATIVE_INT32));
  assert(pass_facts_decides(&facts, &graph, other, ir_cond(IR_COND_NON_NEGATIVE_INT32)) == -1);
  // Unboxed values are of the kind of what they box to.
  assert(pass_facts_decides(&facts, &graph, 5, ir_cond(IR_COND_INT32_VALUE)) == 1);
  ir_node_id_t object = 2;
  ir_condition_t packed = ir_cond(IR_COND_ELEMENTS_KIND);
  packed.elements = IR_ELEMENTS_PACKED;
  pass_facts_set_passed(&facts, object, packed);
  assert(pass_facts_is_object(&facts, object));
  ir_condition_t holey = ir_cond(IR_COND_ELEMENTS_KIND);
  holey.elements = IR_ELEMENTS_HOLEY;
  assert(pass_facts_decides(&facts, &graph, object, holey) == 1);
  opt_arena_free(&arena);
  ir_graph_free(&graph);
  tests_run++;
}

// Ant: an exit of the entering check of one phi does not keep the other phis of its block tagged.
static bool phi_unboxed_with_exit_site(uint32_t exit_slot, bool record) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = BLOCK0(&graph);
  ir_block_id_t left = BLOCK(&graph, entry);
  ir_block_id_t right = BLOCK(&graph, entry);
  ir_block_id_t merge = BLOCK(&graph, left, right);
  ir_node_id_t value = NODE0(&graph, load_slot(8), OPT_REPR_TAGGED);
  push_body(&graph, entry, value);
  ir_op_t undefined = branch(ir_cond(IR_COND_UNDEFINED), left, right);
  set_control(&graph, entry, NODE(&graph, undefined, OPT_REPR_NONE, value));
  ir_node_id_t other = NODE0(&graph, load_slot(9), OPT_REPR_TAGGED);
  push_body(&graph, left, other);
  set_control(&graph, left, NODE0(&graph, jump(merge), OPT_REPR_NONE));
  ir_node_id_t integer = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, value);
  ir_node(&graph, integer)->frame_state = empty_frame_state(&graph);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, integer);
  push_body(&graph, right, integer);
  push_body(&graph, right, boxed);
  set_control(&graph, right, NODE0(&graph, jump(merge), OPT_REPR_NONE));
  ir_op_t phi_op = ir_op(IR_OP_PHI);
  phi_op.slot = 9;
  ir_node_id_t phi = NODE(&graph, phi_op, OPT_REPR_TAGGED, other, boxed);
  opt_vec_push(&graph.arena, &ir_block(&graph, merge)->phis, phi);
  ir_frame_state_t state = {.bc_off = 7, .mode = OPT_RESUME_AT, .dst = OPT_INVALID_SLOT, .parent = IR_NONE, .passed_argument_count = IR_NONE};
  opt_vec_push(&graph.arena, &state.values, ((ir_slot_value_t){9, phi}));
  ir_block(&graph, merge)->start_frame_state = ir_add_frame_state(&graph, &state);
  ir_block(&graph, merge)->bytecode_start = 7;
  ir_node_id_t check = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, phi);
  ir_node(&graph, check)->frame_state = ir_block(&graph, merge)->start_frame_state;
  ir_node_id_t result = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, check);
  push_body(&graph, merge, check);
  push_body(&graph, merge, result);
  set_control(&graph, merge, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, result));
  if (record) opt_vec_push(&graph.arena, &graph.exit_sites, ((ir_exit_site_t){.bc_off = 7, .kind = OPT_EXIT_NOT_INT32, .slot = exit_slot}));
  assert_valid(&graph);
  pass_representation(&graph);
  assert_valid(&graph);
  bool unboxed = ir_node(&graph, phi)->repr == OPT_REPR_INT32;
  ir_graph_free(&graph);
  return unboxed;
}

static void phi_entering_check_exits_are_per_slot(void) {
  tests_run++;
  assert(phi_unboxed_with_exit_site(0, false));
  assert(!phi_unboxed_with_exit_site(9, true));
  assert(phi_unboxed_with_exit_site(4, true));
  assert(!phi_unboxed_with_exit_site(0, true));
}

static ir_op_t int32_binary(ir_opcode_t code, ir_binary_op_t binary) {
  ir_op_t op = ir_op(code);
  op.binary = binary;
  return op;
}

static ir_op_t int32_compare(ir_comparison_t comparison) {
  ir_op_t op = ir_op(IR_OP_INT32_COMPARE);
  op.comparison = comparison;
  return op;
}

static ir_frame_state_id_t frame_state_holding(ir_graph_t *graph, ir_node_id_t value) {
  ir_frame_state_t state = {
    .bc_off = 0, .mode = OPT_RESUME_AT, .dst = OPT_INVALID_SLOT, .parent = IR_NONE, .passed_argument_count = IR_NONE,
  };
  ir_slot_value_t slot = {.slot = 6, .value = value};
  opt_vec_push(&graph->arena, &state.values, slot);
  return ir_add_frame_state(graph, &state);
}

// `(a + b) | 0` wraps, unless a frame state still holds the sum.
static void truncation_wraps_sums_whose_users_only_read_low_bits(void) {
  for (uint32_t held = 0; held < 2; held++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), b = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
    ir_node_id_t ia = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, a), ib = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, b);
    ir_node(&graph, ia)->frame_state = ir_node(&graph, ib)->frame_state = empty_frame_state(&graph);
    ir_node_id_t sum = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, ia, ib);
    ir_node(&graph, sum)->frame_state = empty_frame_state(&graph);
    ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
    ir_node_id_t ored = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_BITWISE_OR), OPT_REPR_INT32, sum, zero);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, ored);
    ir_node_id_t check = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, b);
    ir_node(&graph, check)->frame_state = held ? frame_state_holding(&graph, sum) : empty_frame_state(&graph);
    ir_node_id_t nodes[] = {a, b, ia, ib, sum, ored, boxed, check};
    for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 0, nodes[i]);
    set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
    assert_valid(&graph);
    pass_truncation(&graph);
    assert_valid(&graph);
    assert((ir_node(&graph, sum)->op.code == IR_OP_INT32_WRAPPING_BINARY) == !held);
    assert((ir_node(&graph, sum)->frame_state == IR_NONE) == !held);
    pass_simplify(&graph);
    assert_valid(&graph);
    assert(ir_node(&graph, boxed)->inputs.items[0] == sum);
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void boxed_arithmetic_moves_boxed_int32_sums_to_doubles(void) {
  for (uint32_t masked = 0; masked < 2; masked++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), b = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
    ir_node_id_t ia = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, a), ib = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, b);
    ir_node(&graph, ia)->frame_state = ir_node(&graph, ib)->frame_state = empty_frame_state(&graph);
    ir_node_id_t one = NODE0(&graph, constant(1), OPT_REPR_INT32);
    ir_node_id_t sum = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, ia, one);
    ir_node(&graph, sum)->frame_state = empty_frame_state(&graph);
    ir_node_id_t less = NODE(&graph, int32_compare(IR_CMP_LESS_THAN), OPT_REPR_BOOL, sum, ib);
    ir_node_id_t used = sum;
    if (masked) used = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_BITWISE_AND), OPT_REPR_INT32, sum, one);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, used);
    ir_node_id_t nodes[] = {a, b, ia, ib, sum, less, used, boxed};
    for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++)
      if (i != 6 || masked) push_body(&graph, 0, nodes[i]);
    set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
    assert_valid(&graph);
    pass_boxed_arithmetic(&graph);
    assert_valid(&graph);
    assert((ir_node(&graph, ia)->op.code == IR_OP_CHECK_NUMBER) == !masked);
    assert((ir_node(&graph, ib)->op.code == IR_OP_CHECK_NUMBER) == !masked);
    assert((ir_node(&graph, sum)->op.code == IR_OP_FLOAT64_BINARY) == !masked);
    assert((ir_node(&graph, sum)->frame_state == IR_NONE) == !masked);
    assert((ir_node(&graph, less)->op.code == IR_OP_FLOAT64_COMPARE) == !masked);
    assert((ir_node(&graph, boxed)->op.code == IR_OP_BOX_FLOAT64) == !masked);
    if (!masked) {
      ir_node_id_t addend = ir_node(&graph, sum)->inputs.items[1];
      assert(ir_node(&graph, addend)->repr == OPT_REPR_FLOAT64 && ir_value_to_double(ir_node(&graph, addend)->op.bits) == 1.0);
    }
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void boxed_arithmetic_keeps_loop_counters_that_never_cross_calls(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0, 1);
  BLOCK(&graph, 1);
  ir_node_id_t start = NODE0(&graph, constant(IR_VALUE_UNDEFINED), OPT_REPR_TAGGED);
  ir_node_id_t into = NODE0(&graph, jump(1), OPT_REPR_NONE);
  set_control(&graph, 0, into);
  ir_node_id_t phi = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, start, start);
  opt_vec_push(&graph.arena, &ir_block(&graph, 1)->phis, phi);
  ir_node_id_t counter = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, phi);
  ir_node(&graph, counter)->frame_state = empty_frame_state(&graph);
  ir_node_id_t one = NODE0(&graph, constant(1), OPT_REPR_INT32);
  ir_node_id_t next = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, counter, one);
  ir_node(&graph, next)->frame_state = empty_frame_state(&graph);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, next);
  ir_node(&graph, phi)->inputs.items[1] = boxed;
  ir_node_id_t limit = NODE0(&graph, constant(100), OPT_REPR_INT32);
  ir_node_id_t nodes[] = {counter, next, boxed};
  for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 1, nodes[i]);
  set_control(&graph, 1, NODE(&graph, branch(int32_condition(IR_CMP_LESS_THAN), 1, 2), OPT_REPR_NONE, next, limit));
  set_control(&graph, 2, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  ir_block(&graph, 1)->is_loop_header = true;
  pass_boxed_arithmetic(&graph);
  assert(ir_node(&graph, counter)->op.code == IR_OP_CHECK_INT32);
  assert(ir_node(&graph, next)->op.code == IR_OP_INT32_BINARY);
  ir_graph_free(&graph);
  tests_run++;
}

// `(a >>> 0) < (b >>> 0)` compares the bits of a and b unsigned.
static void truncation_compares_uint32_values_unsigned(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), b = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
  ir_node_id_t ia = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, a), ib = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, b);
  ir_node(&graph, ia)->frame_state = ir_node(&graph, ib)->frame_state = empty_frame_state(&graph);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  ir_node_id_t ua = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_UNSIGNED_RIGHT_SHIFT), OPT_REPR_INT32, ia, zero);
  ir_node_id_t ub = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_UNSIGNED_RIGHT_SHIFT), OPT_REPR_INT32, ib, zero);
  ir_node(&graph, ua)->frame_state = ir_node(&graph, ub)->frame_state = empty_frame_state(&graph);
  ir_node_id_t less = NODE(&graph, int32_compare(IR_CMP_LESS_THAN), OPT_REPR_BOOL, ua, ub);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, less);
  ir_node_id_t nodes[] = {a, b, ia, ib, ua, ub, less, boxed};
  for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 0, nodes[i]);
  set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_valid(&graph);
  pass_truncation(&graph);
  assert_valid(&graph);
  const ir_node_t *compare = ir_node(&graph, less);
  assert(compare->op.code == IR_OP_INT32_COMPARE && compare->op.comparison == IR_CMP_BELOW);
  assert(compare->inputs.items[0] == ia && compare->inputs.items[1] == ib);
  pass_dce(&graph);
  assert(!body_contains(&graph, 0, ua) && !body_contains(&graph, 0, ub));
  ir_graph_free(&graph);
  tests_run++;
}

// Products of masked values and increments of indices past their bounds checks stop checking for overflow; a product
// that could be -0 keeps checking.
static void ranges_remove_overflow_checks_they_rule_out(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t object = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), x = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
  ir_node_id_t ix = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, x);
  ir_node(&graph, ix)->frame_state = empty_frame_state(&graph);
  ir_node_id_t mask = NODE0(&graph, constant(0x3fff), OPT_REPR_INT32), shift = NODE0(&graph, constant(14), OPT_REPR_INT32);
  ir_node_id_t low = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_BITWISE_AND), OPT_REPR_INT32, ix, mask);
  ir_node_id_t high = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_RIGHT_SHIFT), OPT_REPR_INT32, ix, shift);
  ir_node_id_t square = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_MUL), OPT_REPR_INT32, low, low);
  ir_node_id_t signed_product = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_MUL), OPT_REPR_INT32, high, low);
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object);
  ir_node_id_t length = NODE(&graph, ir_op(IR_OP_LOAD_ELEMENTS_LENGTH), OPT_REPR_INT32, address);
  ir_node_id_t bounded = NODE(&graph, ir_op(IR_OP_CHECK_BOUNDS), OPT_REPR_INT32, ix, length);
  ir_node_id_t one = NODE0(&graph, constant(1), OPT_REPR_INT32);
  ir_node_id_t next = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, ix, one);
  ir_node_id_t unbounded = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, ix, ix);
  ir_node_id_t checked[] = {square, signed_product, bounded, next, unbounded};
  for (uint32_t i = 0; i < 5; i++) ir_node(&graph, checked[i])->frame_state = empty_frame_state(&graph);
  ir_node_id_t total = NODE(&graph, int32_binary(IR_OP_INT32_WRAPPING_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, square, signed_product);
  ir_node_id_t total2 = NODE(&graph, int32_binary(IR_OP_INT32_WRAPPING_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, next, unbounded);
  ir_node_id_t total3 = NODE(&graph, int32_binary(IR_OP_INT32_WRAPPING_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, total, total2);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, total3);
  ir_node_id_t nodes[] = {object, x, ix, low, high, square, signed_product, address, length, bounded, next, unbounded, total, total2, total3, boxed};
  for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 0, nodes[i]);
  set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_valid(&graph);
  pass_bounds_check_elimination(&graph);
  assert_valid(&graph);
  assert(ir_node(&graph, square)->op.code == IR_OP_INT32_WRAPPING_BINARY);
  assert(ir_node(&graph, signed_product)->op.code == IR_OP_INT32_BINARY);
  assert(ir_node(&graph, next)->op.code == IR_OP_INT32_WRAPPING_BINARY && ir_node(&graph, next)->frame_state == IR_NONE);
  assert(ir_node(&graph, unbounded)->op.code == IR_OP_INT32_BINARY);
  ir_graph_free(&graph);
  tests_run++;
}

// A second load of an element is the first, until elements change.
static void element_loads_are_reused_until_elements_change(void) {
  for (uint32_t stored = 0; stored < 2; stored++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t object = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), index = NODE0(&graph, constant(2), OPT_REPR_INT32);
    ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object);
    ir_node_id_t first = NODE(&graph, elements_op(IR_OP_LOAD_ELEMENT_AT, IR_ELEMENTS_HOLEY), OPT_REPR_TAGGED, address, index);
    ir_node_id_t store = NODE(&graph, elements_op(IR_OP_STORE_ELEMENT_AT, IR_ELEMENTS_PACKED), OPT_REPR_NONE, address, index, object);
    ir_node_id_t second = NODE(&graph, elements_op(IR_OP_LOAD_ELEMENT_AT, IR_ELEMENTS_HOLEY), OPT_REPR_TAGGED, address, index);
    push_body(&graph, 0, object);
    push_body(&graph, 0, address);
    push_body(&graph, 0, first);
    if (stored) push_body(&graph, 0, store);
    push_body(&graph, 0, second);
    ir_node_id_t equal = NODE(&graph, ir_op(IR_OP_TAGGED_EQUALS), OPT_REPR_BOOL, first, second);
    push_body(&graph, 0, equal);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, equal);
    push_body(&graph, 0, boxed);
    set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
    assert_valid(&graph);
    pass_check_elimination(&graph);
    assert_valid(&graph);
    assert(body_contains(&graph, 0, second) == (bool)stored);
    ir_graph_free(&graph);
  }
  tests_run++;
}

// ToInt32 of a double sum of int32 values is wrapping int32 arithmetic, a number check of a boxed int32 converts it,
// and a CheckInt32 of a boxed double only compared goes, the comparison comparing the doubles.
static void simplification_folds_int32_round_trips_through_doubles(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), b = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
  ir_node_id_t ia = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, a), ib = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, b);
  ir_node(&graph, ia)->frame_state = ir_node(&graph, ib)->frame_state = empty_frame_state(&graph);
  ir_node_id_t da = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, ia);
  ir_node_id_t db = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, ib);
  ir_op_t add = ir_op(IR_OP_FLOAT64_BINARY);
  add.binary = IR_BINARY_ADD;
  ir_node_id_t sum = NODE(&graph, add, OPT_REPR_FLOAT64, da, db);
  ir_node_id_t wrapped = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, sum);
  ir_node_id_t boxed_int = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, ia);
  ir_node_id_t number = NODE(&graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, boxed_int);
  ir_node(&graph, number)->frame_state = empty_frame_state(&graph);
  ir_node_id_t boxed_double = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, sum);
  ir_node_id_t checked = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, boxed_double);
  ir_node(&graph, checked)->frame_state = empty_frame_state(&graph);
  ir_node_id_t less = NODE(&graph, int32_compare(IR_CMP_LESS_THAN), OPT_REPR_BOOL, checked, ib);
  ir_node_id_t rewrapped = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, number);
  ir_op_t select = int32_binary(IR_OP_INT32_WRAPPING_BINARY, IR_BINARY_ADD);
  ir_node_id_t both = NODE(&graph, select, OPT_REPR_INT32, wrapped, rewrapped);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, both);
  ir_node_id_t nodes[] = {a, b, ia, ib, da, db, sum, wrapped, boxed_int, number, boxed_double, checked, less, rewrapped, both, boxed};
  for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 0, nodes[i]);
  ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed);
  set_control(&graph, 0, ret);
  ir_node(&graph, ret)->inputs.items[0] = boxed;
  assert_valid(&graph);
  pass_simplify(&graph);
  assert_valid(&graph);
  assert(!body_contains(&graph, 0, wrapped) && !body_contains(&graph, 0, number) && !body_contains(&graph, 0, checked));
  const ir_node_t *lhs = ir_node(&graph, ir_node(&graph, both)->inputs.items[0]);
  assert(lhs->op.code == IR_OP_INT32_WRAPPING_BINARY && lhs->op.binary == IR_BINARY_ADD && lhs->inputs.items[0] == ia);
  assert(ir_node(&graph, both)->inputs.items[1] == ia);
  ir_graph_free(&graph);
  tests_run++;
}

static void simplification_keeps_checks_of_doubles_that_identities_stand_for(void) {
  for (int truncates = 0; truncates < 2; truncates++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED);
    ir_node_id_t ia = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, a);
    ir_node(&graph, ia)->frame_state = empty_frame_state(&graph);
    ir_node_id_t da = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, ia);
    ir_op_t divide = ir_op(IR_OP_FLOAT64_BINARY);
    divide.binary = IR_BINARY_DIV;
    ir_node_id_t quotient = NODE(&graph, divide, OPT_REPR_FLOAT64, da, da);
    ir_node_id_t boxed_double = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, quotient);
    ir_node_id_t checked = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, boxed_double);
    ir_node(&graph, checked)->frame_state = empty_frame_state(&graph);
    ir_node_id_t nodes[] = {a, ia, da, quotient, boxed_double, checked};
    for (uint32_t i = 0; i < sizeof nodes / sizeof *nodes; i++) push_body(&graph, 0, nodes[i]);
    ir_node_id_t identity;
    if (truncates) {
      ir_node_id_t widened = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, checked);
      push_body(&graph, 0, widened);
      identity = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, widened);
    } else {
      ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
      identity = NODE(&graph, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), OPT_REPR_INT32, checked, zero);
      ir_node(&graph, identity)->frame_state = empty_frame_state(&graph);
    }
    push_body(&graph, 0, identity);
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, identity);
    push_body(&graph, 0, boxed);
    set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
    assert_valid(&graph);
    pass_simplify(&graph);
    pass_simplify(&graph);
    assert_valid(&graph);
    const ir_node_t *returned = ir_node(&graph, ir_node(&graph, ir_control_id(&graph, 0))->inputs.items[0]);
    assert(returned->op.code == IR_OP_BOX_INT32);
    const ir_node_t *result = ir_node(&graph, returned->inputs.items[0]);
    assert(truncates ? result->op.code == IR_OP_FLOAT64_TO_INT32 : body_contains(&graph, 0, checked));
    ir_graph_free(&graph);
  }
  tests_run++;
}

// A loop entered at its header and by on-stack replacement gets a preheader merging both; the header phi of a value
// the loop leaves as it is then goes, and what the loop computes from the value moves out of it.
static void licm_removes_trivial_header_phis_after_merging_entries(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0, 1, 4);
  BLOCK(&graph, 2);
  BLOCK(&graph, 2);
  ir_osr_entry_t osr = {.bc_off = 7, .block = 1};
  opt_vec_push(&graph.arena, &graph.osr_entries, osr);
  ir_node_id_t a = NODE0(&graph, load_slot(5), OPT_REPR_TAGGED), b = NODE0(&graph, load_slot(6), OPT_REPR_TAGGED);
  push_body(&graph, 0, a);
  set_control(&graph, 0, NODE0(&graph, jump(2), OPT_REPR_NONE));
  push_body(&graph, 1, b);
  set_control(&graph, 1, NODE0(&graph, jump(2), OPT_REPR_NONE));
  ir_node_id_t phi = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, a, b, a);
  ir_node(&graph, phi)->inputs.items[2] = phi;
  opt_vec_push(&graph.arena, &ir_block(&graph, 2)->phis, phi);
  ir_block(&graph, 2)->is_loop_header = true;
  ir_node_id_t address = NODE(&graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, phi);
  ir_node_id_t length = NODE(&graph, ir_op(IR_OP_LOAD_ELEMENTS_LENGTH), OPT_REPR_INT32, address);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  push_body(&graph, 2, address);
  push_body(&graph, 2, length);
  set_control(&graph, 2, NODE(&graph, branch(int32_condition(IR_CMP_LESS_THAN), 4, 3), OPT_REPR_NONE, zero, length));
  set_control(&graph, 3, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, phi));
  set_control(&graph, 4, NODE0(&graph, jump(2), OPT_REPR_NONE));
  assert_valid(&graph);
  pass_licm(&graph);
  assert_valid(&graph);
  ir_block_id_t header = IR_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(&graph); block++)
    if (ir_block(&graph, block)->is_loop_header) header = block;
  assert(header != IR_NONE && !ir_block(&graph, header)->phis.len);
  assert(!body_contains(&graph, header, address) && !body_contains(&graph, header, length));
  ir_graph_free(&graph);
  tests_run++;
}

static ir_node_id_t checked_int32(ir_graph_t *graph, ir_block_id_t block, uint32_t slot) {
  ir_node_id_t loaded = NODE0(graph, load_slot(slot), OPT_REPR_TAGGED);
  ir_node_id_t value = NODE(graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, loaded);
  ir_node(graph, value)->frame_state = empty_frame_state(graph);
  push_body(graph, block, loaded);
  push_body(graph, block, value);
  return value;
}

static ir_node_id_t checked_node(ir_graph_t *graph, ir_block_id_t block, ir_op_t op, ir_node_id_t lhs, ir_node_id_t rhs) {
  ir_node_id_t node = NODE(graph, op, OPT_REPR_INT32, lhs, rhs);
  ir_node(graph, node)->frame_state = empty_frame_state(graph);
  push_body(graph, block, node);
  return node;
}

static void return_int32(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t value) {
  ir_node_id_t boxed = NODE(graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, value);
  push_body(graph, block, boxed);
  set_control(graph, block, NODE(graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
}

static void unsigned_right_shifts_of_any_int32_have_ranges(void) {
  for (int32_t shift = 0; shift <= 24; shift += 24) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    ir_node_id_t h = checked_int32(&graph, 0, 6);
    ir_node_id_t amount = NODE0(&graph, constant((uint32_t)shift), OPT_REPR_INT32), three = NODE0(&graph, constant(3), OPT_REPR_INT32);
    ir_node_id_t shifted = checked_node(&graph, 0, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_UNSIGNED_RIGHT_SHIFT), h, amount);
    ir_node_id_t product = checked_node(&graph, 0, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_MUL), shifted, three);
    return_int32(&graph, 0, product);
    assert_valid(&graph);
    pass_bounds_check_elimination(&graph);
    assert_valid(&graph);
    assert((ir_node(&graph, product)->op.code == IR_OP_INT32_WRAPPING_BINARY) == (shift == 24));
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void overflow_checks_removed_by_bounds_checks_check_the_counters_of_their_counts(void) {
  for (uint32_t failed_before = 0; failed_before < 2; failed_before++) {
    ir_graph_t graph;
    ir_graph_init(&graph);
    BLOCK0(&graph);
    BLOCK0(&graph);
    ir_block_id_t merge = BLOCK(&graph, 0, 1);
    opt_vec_push(&graph.arena, &graph.osr_entries, ((ir_osr_entry_t){.bc_off = 7, .block = 1}));
    if (failed_before) opt_vec_push(&graph.arena, &graph.exit_sites, ((ir_exit_site_t){.bc_off = 7, .kind = OPT_EXIT_OUT_OF_BOUNDS}));
    ir_frame_state_t state = {.bc_off = 7, .mode = OPT_RESUME_AT, .dst = OPT_INVALID_SLOT, .parent = IR_NONE, .passed_argument_count = IR_NONE};
    ir_frame_state_id_t at_entry = ir_add_frame_state(&graph, &state);
    ir_node_id_t hundred = NODE0(&graph, constant(100), OPT_REPR_INT32), one = NODE0(&graph, constant(1), OPT_REPR_INT32);
    set_control(&graph, 0, NODE0(&graph, jump(merge), OPT_REPR_NONE));
    ir_node_id_t slot = NODE0(&graph, load_slot(8), OPT_REPR_TAGGED);
    ir_node_id_t counter = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, slot);
    ir_node(&graph, counter)->frame_state = at_entry;
    push_body(&graph, 1, slot);
    push_body(&graph, 1, counter);
    set_control(&graph, 1, NODE0(&graph, jump(merge), OPT_REPR_NONE));
    ir_node_id_t start = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_INT32, hundred, counter);
    opt_vec_push(&graph.arena, &ir_block(&graph, merge)->phis, start);
    ir_node_id_t index = checked_int32(&graph, merge, 6);
    checked_node(&graph, merge, ir_op(IR_OP_CHECK_BOUNDS), index, start);
    ir_node_id_t sum = checked_node(&graph, merge, int32_binary(IR_OP_INT32_BINARY, IR_BINARY_ADD), index, one);
    return_int32(&graph, merge, sum);
    assert_valid(&graph);
    pass_bounds_check_elimination(&graph);
    assert_valid(&graph);
    bool guarded = false;
    const ir_node_vec_t *body = &ir_block(&graph, 1)->body;
    for (uint32_t i = 0; i < body->len; i++) {
      const ir_node_t *node = ir_node(&graph, body->items[i]);
      guarded |= node->op.code == IR_OP_CHECK_BOUNDS && node->inputs.items[0] == counter;
    }
    bool wrapping = ir_node(&graph, sum)->op.code == IR_OP_INT32_WRAPPING_BINARY;
    assert(wrapping == !failed_before && guarded == wrapping);
    ir_graph_free(&graph);
  }
  tests_run++;
}

static ir_node_id_t checked_double_int32(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t *double_value) {
  ir_frame_state_id_t frame_state = empty_frame_state(graph);
  ir_node_id_t tagged = NODE0(graph, load_slot(0), OPT_REPR_TAGGED);
  *double_value = NODE(graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, tagged);
  ir_node(graph, *double_value)->frame_state = frame_state;
  ir_node_id_t boxed = NODE(graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, *double_value);
  ir_node_id_t check = NODE(graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, boxed);
  ir_node(graph, check)->frame_state = frame_state;
  push_body(graph, block, tagged);
  push_body(graph, block, *double_value);
  push_body(graph, block, boxed);
  push_body(graph, block, check);
  return check;
}

static void simplification_keeps_int32_checks_that_int32_terms_use(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t double_value;
  ir_node_id_t check = checked_double_int32(&graph, 0, &double_value);
  ir_node_id_t converted = NODE(&graph, ir_op(IR_OP_INT32_TO_FLOAT64), OPT_REPR_FLOAT64, check);
  ir_op_t add = ir_op(IR_OP_FLOAT64_BINARY);
  add.binary = IR_BINARY_ADD;
  ir_node_id_t sum = NODE(&graph, add, OPT_REPR_FLOAT64, converted, converted);
  ir_node_id_t integer = NODE(&graph, ir_op(IR_OP_FLOAT64_TO_INT32), OPT_REPR_INT32, sum);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, integer);
  push_body(&graph, 0, converted);
  push_body(&graph, 0, sum);
  push_body(&graph, 0, integer);
  push_body(&graph, 0, boxed);
  set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_valid(&graph);

  pass_simplify(&graph);
  assert_valid(&graph);
  assert(body_contains(&graph, 0, check));
  ir_graph_free(&graph);
  tests_run++;
}

static void simplification_folds_number_checks_of_dissolved_checks_to_the_doubles(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  ir_node_id_t double_value;
  ir_node_id_t check = checked_double_int32(&graph, 0, &double_value);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, check);
  ir_node_id_t refined = NODE(&graph, ir_op(IR_OP_CHECK_IDENTITY_COMPARABLE), OPT_REPR_TAGGED, boxed);
  ir_node(&graph, refined)->frame_state = empty_frame_state(&graph);
  ir_node_id_t number = NODE(&graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, refined);
  ir_node(&graph, number)->frame_state = empty_frame_state(&graph);
  ir_node_id_t result = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, number);
  push_body(&graph, 0, boxed);
  push_body(&graph, 0, refined);
  push_body(&graph, 0, number);
  push_body(&graph, 0, result);
  set_control(&graph, 0, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, result));
  assert_valid(&graph);

  pass_simplify(&graph);
  assert_valid(&graph);
  assert(!body_contains(&graph, 0, check));
  assert(ir_node(&graph, result)->inputs.items[0] == double_value);
  ir_graph_free(&graph);
  tests_run++;
}

static uint32_t branches_after_check_elimination(bool merged) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  BLOCK0(&graph);
  BLOCK(&graph, 0);
  BLOCK(&graph, 0);
  ir_frame_state_id_t frame_state = empty_frame_state(&graph);
  ir_node_id_t lhs = NODE0(&graph, load_slot(0), OPT_REPR_TAGGED), rhs = NODE0(&graph, load_slot(1), OPT_REPR_TAGGED);
  ir_node_id_t a = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, lhs), b = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, rhs);
  ir_node(&graph, a)->frame_state = frame_state;
  ir_node(&graph, b)->frame_state = frame_state;
  push_body(&graph, 0, lhs);
  push_body(&graph, 0, rhs);
  push_body(&graph, 0, a);
  push_body(&graph, 0, b);
  ir_condition_t less = int32_condition(IR_CMP_LESS_THAN);
  set_control(&graph, 0, NODE(&graph, branch(less, 1, 2), OPT_REPR_NONE, a, b));
  if (merged) {
    BLOCK(&graph, 1, 2);
    BLOCK(&graph, 3);
    BLOCK(&graph, 3);
    set_control(&graph, 1, NODE0(&graph, jump(3), OPT_REPR_NONE));
    set_control(&graph, 2, NODE0(&graph, jump(3), OPT_REPR_NONE));
    set_control(&graph, 3, NODE(&graph, branch(less, 4, 5), OPT_REPR_NONE, a, b));
    for (ir_block_id_t block = 4; block <= 5; block++) set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, lhs));
  } else {
    for (ir_block_id_t block = 3; block <= 6; block++) BLOCK(&graph, block < 5 ? 1 : 2);
    set_control(&graph, 1, NODE(&graph, branch(less, 3, 4), OPT_REPR_NONE, a, b));
    set_control(&graph, 2, NODE(&graph, branch(less, 5, 6), OPT_REPR_NONE, a, b));
    for (ir_block_id_t block = 3; block <= 6; block++) set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, lhs));
  }
  assert_valid(&graph);
  pass_check_elimination(&graph);
  assert_valid(&graph);
  uint32_t branches = 0;
  for (ir_block_id_t block = 0; block < ir_block_count(&graph); block++)
    branches += ir_control(&graph, block)->op.code == IR_OP_BRANCH;
  ir_graph_free(&graph);
  return branches;
}

static void check_elimination_decides_branches_on_the_same_values(void) {
  assert(branches_after_check_elimination(false) == 1);
  assert(branches_after_check_elimination(true) == 2);
  tests_run++;
}

int main(void) {
  dominators_of_a_diamond();
  verifier_accepts_a_valid_graph();
  verifier_rejects_uses_of_values_from_blocks_that_do_not_dominate();
  verifier_rejects_refinements_away_from_the_edge_they_refine();
  verifier_rejects_inconsistent_edges_and_phis();
  verifier_rejects_edges_missing_from_predecessor_lists();
  verifier_rejects_inputs_of_the_wrong_representation();
  verifier_rejects_blocks_no_entry_reaches();
  verifier_rejects_slow_path_outputs_away_from_their_call();
  simplification_removes_number_checks_of_numeric_constants();
  element_counts_move_out_of_loops_that_only_store_elements();
  typed_array_storage_offsets_are_reused_across_stores_but_not_calls();
  typed_array_storage_metadata_moves_out_of_loops_that_only_store_elements();
  branches_on_known_kinds_fold();
  check_elimination_keeps_checks_that_a_later_entry_reaches_blocks_before_it();
  simplification_removes_integer_double_round_trips();
  simplification_tests_the_truthiness_of_boxed_int32_values_unboxed();
  simplification_keeps_double_integer_round_trips();
  constant_bounds_checks_only_fold_when_the_unsigned_index_is_in_bounds();
  frame_writes_after_slow_paths_that_may_initialize_the_frame_check_first();
  merges_intersect_knowledge_and_union_shape_sets();
  narrowing_and_killing();
  stores_to_a_binding_forget_that_binding_of_every_environment();
  value_kinds_decide_branches();
  phi_entering_check_exits_are_per_slot();
  truncation_wraps_sums_whose_users_only_read_low_bits();
  boxed_arithmetic_moves_boxed_int32_sums_to_doubles();
  boxed_arithmetic_keeps_loop_counters_that_never_cross_calls();
  truncation_compares_uint32_values_unsigned();
  ranges_remove_overflow_checks_they_rule_out();
  element_loads_are_reused_until_elements_change();
  simplification_folds_int32_round_trips_through_doubles();
  simplification_keeps_checks_of_doubles_that_identities_stand_for();
  licm_removes_trivial_header_phis_after_merging_entries();
  unsigned_right_shifts_of_any_int32_have_ranges();
  overflow_checks_removed_by_bounds_checks_check_the_counters_of_their_counts();
  simplification_keeps_int32_checks_that_int32_terms_use();
  simplification_folds_number_checks_of_dissolved_checks_to_the_doubles();
  check_elimination_decides_branches_on_the_same_values();
  printf("opt-passes: %u tests passed\n", tests_run);
  return 0;
}
