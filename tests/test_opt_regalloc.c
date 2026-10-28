#include "../src/jit/opt/ir/ir.h"
#include "../src/jit/opt/ir/ir_value.h"
#include "../src/jit/opt/passes/passes.h"
#include "../src/jit/opt/regalloc/regalloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t tests_run;

static const ir_frame_layout_t test_layout = {
  .reserved_count = 5, .number_of_registers = 8, .registers_and_locals_count = 10, .number_of_constants = 2, .number_of_arguments = 1,
};

enum { SLOT_R5 = 5, SLOT_R6 = 6, SLOT_R7 = 7, SLOT_L0 = 8, SLOT_L1 = 9, SLOT_A0 = 12 };

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

static ir_op_t branch(ir_condition_kind_t kind, ir_block_id_t if_true, ir_block_id_t if_false) {
  ir_op_t op = ir_op(IR_OP_BRANCH);
  op.branch.condition = ir_cond(kind);
  op.branch.if_true = if_true;
  op.branch.if_false = if_false;
  return op;
}

static ir_op_t slow_path(uint16_t opcode) {
  ir_op_t op = ir_op(IR_OP_CALL_SLOW_PATH);
  op.slow.opcode = opcode;
  return op;
}

static ir_block_id_t new_block(ir_graph_t *graph) {
  return ir_add_block(graph);
}

static void add_predecessor(ir_graph_t *graph, ir_block_id_t block, ir_block_id_t predecessor) {
  opt_vec_push(&graph->arena, &ir_block(graph, block)->predecessors, predecessor);
}

static void push_body(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  opt_vec_push(&graph->arena, &ir_block(graph, block)->body, node);
}

static void push_phi(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  opt_vec_push(&graph->arena, &ir_block(graph, block)->phis, node);
}

static void set_control(ir_graph_t *graph, ir_block_id_t block, ir_node_id_t node) {
  ir_block(graph, block)->control = node;
}

static ir_frame_state_id_t frame_state(ir_graph_t *graph, opt_resume_mode_t mode, uint32_t dst, const ir_slot_value_t *values, uint32_t count) {
  ir_frame_state_t state = {.mode = mode, .dst = dst, .parent = IR_NONE, .passed_argument_count = IR_NONE};
  for (uint32_t i = 0; i < count; i++) opt_vec_push(&graph->arena, &state.values, values[i]);
  return ir_add_frame_state(graph, &state);
}

static void assert_graph_valid(const ir_graph_t *graph) {
  opt_text_t errors = {0};
  if (pass_verify(graph, &errors)) return;
  opt_text_t dump = {0};
  ir_dump(graph, &test_layout, &dump);
  fprintf(stderr, "%s\n%s", errors.data, dump.data);
  assert(false);
}

static void allocate_and_verify(const ir_graph_t *graph, const opt_register_set_t *registers, ra_allocation_t *allocation) {
  ra_allocate(graph, registers, allocation);
  opt_text_t errors = {0};
  if (ra_verify(graph, registers, allocation, &errors)) return;
  opt_text_t dump = {0}, allocation_dump = {0};
  ir_dump(graph, &test_layout, &dump);
  ra_dump_allocation(graph, allocation, &allocation_dump);
  fprintf(stderr, "%s\n%s\n%s", errors.data, dump.data, allocation_dump.data);
  assert(false);
}

static void assert_text(const char *actual, const char *expected) {
  if (!strcmp(actual, expected)) return;
  fprintf(stderr, "actual:\n%s\nexpected:\n%s\n", actual, expected);
  assert(false);
}

typedef struct {
  ra_location_t location;
  uint32_t value;
} sim_entry_t;

typedef struct {
  sim_entry_t entries[32];
  uint32_t count;
} sim_state_t;

static uint32_t *sim_find(sim_state_t *state, ra_location_t location) {
  for (uint32_t i = 0; i < state->count; i++)
    if (ra_location_equal(state->entries[i].location, location)) return &state->entries[i].value;
  return NULL;
}

static void sim_set(sim_state_t *state, ra_location_t location, uint32_t value) {
  uint32_t *existing = sim_find(state, location);
  if (existing) *existing = value;
  else state->entries[state->count++] = (sim_entry_t){location, value};
}

static uint32_t sim_read(sim_state_t *state, ra_location_t location) {
  if (location.kind == RA_LOC_CONSTANT) return (uint32_t)location.bits;
  uint32_t *value = sim_find(state, location);
  assert(value);
  return *value;
}

static void check_parallel_moves(const ra_move_t *moves, uint32_t count) {
  sim_state_t before = {0};
  for (uint32_t i = 0; i < count; i++) {
    ra_location_t locations[2] = {moves[i].from, moves[i].to};
    for (uint32_t j = 0; j < 2; j++)
      if (locations[j].kind != RA_LOC_CONSTANT && !sim_find(&before, locations[j])) sim_set(&before, locations[j], 1000 + before.count);
  }
  opt_arena_t arena;
  opt_arena_init(&arena);
  ra_moves_t sequence = {0};
  ra_resolve_parallel_moves(moves, count, ra_stack(99), &arena, &sequence);
  sim_state_t after = before;
  for (uint32_t i = 0; i < sequence.len; i++) sim_set(&after, sequence.items[i].to, sim_read(&after, sequence.items[i].from));
  for (uint32_t i = 0; i < count; i++) {
    uint32_t expected = sim_read(&before, moves[i].from);
    assert(sim_read(&after, moves[i].to) == expected);
  }
  opt_arena_free(&arena);
}

static ra_move_t mv(ra_location_t from, ra_location_t to) {
  return (ra_move_t){from, to};
}

static void resolves_chains_cycles_and_fan_out(void) {
  // A chain must be emitted back to front.
  check_parallel_moves((ra_move_t[]){mv(ra_register(1), ra_register(2)), mv(ra_register(2), ra_register(3))}, 2);
  // A swap needs the temp.
  check_parallel_moves((ra_move_t[]){mv(ra_stack(0), ra_stack(1)), mv(ra_stack(1), ra_stack(0))}, 2);
  // A three element rotation with a branch off the cycle.
  check_parallel_moves((ra_move_t[]){
    mv(ra_register(0), ra_register(1)),
    mv(ra_register(1), ra_stack(2)),
    mv(ra_stack(2), ra_register(0)),
    mv(ra_register(1), ra_stack(5)),
    mv(ra_constant(7), ra_register(4)),
  }, 5);
  // Two independent cycles.
  check_parallel_moves((ra_move_t[]){
    mv(ra_stack(0), ra_stack(1)), mv(ra_stack(1), ra_stack(0)),
    mv(ra_register(3), ra_register(4)), mv(ra_register(4), ra_register(3)),
  }, 4);
  // Self moves disappear.
  opt_arena_t arena;
  opt_arena_init(&arena);
  ra_moves_t sequence = {0};
  ra_resolve_parallel_moves((ra_move_t[]){mv(ra_register(1), ra_register(1))}, 1, ra_stack(9), &arena, &sequence);
  assert(sequence.len == 0);
  opt_arena_free(&arena);
  tests_run++;
}

static void target_register_sets_follow_libjs(void) {
  opt_register_set_t set;
  opt_pinned_registers_t pinned;
  opt_target_registers(OPT_TARGET_AARCH64, &set, &pinned);
  assert(pinned.vm == 19 && pinned.frame == 20 && pinned.scratch == 21);
  assert(set.allocatable_gprs == 0x1fc0ffff && set.scratch_gpr == 21 && set.return_gpr == 0);
  assert(set.allocatable_fprs == 0x7ffffff8 && set.argument_count == 8);
  opt_target_registers(OPT_TARGET_X86_64, &set, &pinned);
  // rbx, r12 and r13 are pinned; rax, rcx, rdx, rsi, rdi, r8, r9, r14 and r15 get values.
  assert(pinned.vm == 3 && pinned.frame == 12 && pinned.scratch == 13);
  assert(set.allocatable_gprs == 0xc3c7 && set.allocatable_fprs == 0x7ff8);
  assert(set.argument_count == 6 && set.argument_gprs[0] == 7 && set.argument_gprs[1] == 6 && set.return_gpr == 0);
  opt_register_set_t test = opt_test_registers();
  assert(opt_few_registers(&test, 2) == 0x3f);
  assert(opt_few_registers(&test, 0) == 0x0f);
  tests_run++;
}

static void straight_line_code(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), body = new_block(&graph);
  add_predecessor(&graph, body, entry);
  set_control(&graph, entry, NODE0(&graph, jump(body), OPT_REPR_NONE));
  ir_node_id_t ten = NODE0(&graph, constant(ir_value_int32(10)), OPT_REPR_TAGGED);
  for (uint32_t i = 2; i < 5; i++) NODE0(&graph, constant(ir_value_int32((int32_t)i)), OPT_REPR_TAGGED);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t exp = NODE(&graph, slow_path(IR_BC_EXP), OPT_REPR_TAGGED, ten, argument);
  ir_node(&graph, exp)->frame_state = frame_state(&graph, OPT_RESUME_AFTER, SLOT_R7, NULL, 0);
  ir_node_id_t result = NODE(&graph, ir_op(IR_OP_EMPTY_TO_UNDEFINED), OPT_REPR_TAGGED, exp);
  ir_node_id_t ret = NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, result);
  ir_node_id_t initialize = NODE0(&graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE);
  push_body(&graph, body, argument);
  push_body(&graph, body, initialize);
  push_body(&graph, body, exp);
  push_body(&graph, body, result);
  set_control(&graph, body, ret);
  assert_graph_valid(&graph);

  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  opt_text_t text = {0};
  ra_dump_allocation(&graph, &allocation, &text);
  // Ant: frame initialization calls the runtime, so the argument lives across it in a spill slot.
  assert_text(text.data,
    "b0:\n"
    "  v0 Jump\n"
    "b1:\n"
    "  v5 LoadSlot -> r0 spill s0\n"
    "  v9 InitializeFrame temps r1, r2\n"
    "  v6 CallSlowPath (#10, s0) -> r0\n"
    "  v7 EmptyToUndefined (r0) -> r1\n"
    "  moves r1 -> r0\n"
    "  v8 Return (r0)\n"
    "spill slots: 1\n");
  opt_text_free(&text);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

// The graph LibJS builds for `Mov r6, a0; Call r7, r5, r5, [r6]; Return r6`.
static ir_node_id_t call_keeping_an_argument(ir_graph_t *graph) {
  ir_graph_init(graph);
  ir_block_id_t entry = new_block(graph), body = new_block(graph);
  add_predecessor(graph, body, entry);
  set_control(graph, entry, NODE0(graph, jump(body), OPT_REPR_NONE));
  ir_node_id_t argument = NODE0(graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t callee = NODE0(graph, load_slot(SLOT_R5), OPT_REPR_TAGGED);
  ir_node_id_t call = NODE(graph, slow_path(IR_BC_CALL), OPT_REPR_TAGGED, callee, callee, argument);
  ir_node(graph, call)->frame_state =
    frame_state(graph, OPT_RESUME_AFTER, SLOT_R7, (ir_slot_value_t[]){{.slot = SLOT_R6, .value = argument}}, 1);
  ir_node_id_t ret = NODE(graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, argument);
  push_body(graph, body, argument);
  push_body(graph, body, callee);
  push_body(graph, body, call);
  set_control(graph, body, ret);
  assert_graph_valid(graph);
  return argument;
}

static void values_live_across_calls_are_spilled_at_their_definition(void) {
  ir_graph_t graph;
  ir_node_id_t argument = call_keeping_an_argument(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  assert(allocation.nodes[argument].spill_slot != IR_NONE);
  opt_text_t text = {0};
  ra_dump_allocation(&graph, &allocation, &text);
  assert_text(text.data,
    "b0:\n"
    "  v0 Jump\n"
    "b1:\n"
    "  v1 LoadSlot -> r0 spill s0\n"
    "  v2 LoadSlot -> r1\n"
    "  v3 CallSlowPath (r1, r1, r0) -> r0 exit {6=s0}\n"
    "  moves s0 -> r0\n"
    "  v4 Return (r0)\n"
    "spill slots: 1\n");
  opt_text_free(&text);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void many_simultaneously_live_values_are_spilled(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t block = new_block(&graph);
  ir_node_id_t values[10];
  for (uint32_t i = 0; i < 10; i++) {
    values[i] = NODE0(&graph, ir_op(IR_OP_ARGUMENT_COUNT), OPT_REPR_INT32);
    push_body(&graph, block, values[i]);
  }
  for (uint32_t i = 0; i < 10; i++) {
    ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, values[i]);
    ir_node_id_t set = NODE(&graph, ir_op(IR_OP_SET_LEXICAL_ENVIRONMENT), OPT_REPR_POINTER, boxed);
    push_body(&graph, block, boxed);
    push_body(&graph, block, set);
  }
  set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, values[0])));
  push_body(&graph, block, ir_node(&graph, ir_control_id(&graph, block))->inputs.items[0]);
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  // Ten values are live at once, but there are only six registers.
  assert(allocation.spill_slot_count >= 4);
  uint32_t reloads = 0;
  for (uint32_t i = 0; i < allocation.node_count; i++)
    for (uint32_t j = 0; j < allocation.nodes[i].moves_before.len; j++) reloads += allocation.nodes[i].moves_before.items[j].from.kind == RA_LOC_STACK;
  assert(reloads >= 4);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void if_else_phis_and_truthiness_fallbacks(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), fallback = new_block(&graph), then_block = new_block(&graph);
  ir_block_id_t else_block = new_block(&graph), join = new_block(&graph);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t condition = NODE0(&graph, load_slot(SLOT_R5), OPT_REPR_TAGGED);
  ir_op_t truthy = ir_op(IR_OP_BRANCH_TRUTHY);
  truthy.truthy.if_true = then_block;
  truthy.truthy.if_false = else_block;
  truthy.truthy.fallback = fallback;
  push_body(&graph, entry, argument);
  push_body(&graph, entry, condition);
  set_control(&graph, entry, NODE(&graph, truthy, OPT_REPR_NONE, condition));
  add_predecessor(&graph, fallback, entry);
  ir_node_id_t to_boolean = NODE(&graph, ir_op(IR_OP_TO_BOOLEAN), OPT_REPR_BOOL, condition);
  push_body(&graph, fallback, to_boolean);
  set_control(&graph, fallback, NODE(&graph, branch(IR_COND_BOOL, then_block, else_block), OPT_REPR_NONE, to_boolean));
  ir_block_id_t then_edges[2], else_edges[2];
  ir_block_id_t sources[2] = {entry, fallback};
  for (uint32_t i = 0; i < 2; i++) {
    then_edges[i] = sources[i];
    else_edges[i] = sources[i];
  }
  add_predecessor(&graph, then_block, then_edges[0]);
  add_predecessor(&graph, then_block, then_edges[1]);
  add_predecessor(&graph, else_block, else_edges[0]);
  add_predecessor(&graph, else_block, else_edges[1]);
  ir_node_id_t zero = NODE0(&graph, constant(ir_value_int32(0)), OPT_REPR_TAGGED);
  set_control(&graph, then_block, NODE0(&graph, jump(join), OPT_REPR_NONE));
  set_control(&graph, else_block, NODE0(&graph, jump(join), OPT_REPR_NONE));
  add_predecessor(&graph, join, then_block);
  add_predecessor(&graph, join, else_block);
  ir_node_id_t phi = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, zero, argument);
  push_phi(&graph, join, phi);
  ir_node_id_t truth = NODE(&graph, ir_op(IR_OP_TO_BOOLEAN), OPT_REPR_BOOL, phi);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, truth);
  push_body(&graph, join, truth);
  push_body(&graph, join, boxed);
  set_control(&graph, join, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void nested_loops_with_generic_nodes(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), outer = new_block(&graph), inner_entry = new_block(&graph);
  ir_block_id_t inner = new_block(&graph), inner_body = new_block(&graph), outer_latch = new_block(&graph), done = new_block(&graph);
  ir_node_id_t zero = NODE0(&graph, constant(0), OPT_REPR_INT32);
  ir_node_id_t one = NODE0(&graph, constant(1), OPT_REPR_INT32);
  ir_node_id_t initialize = NODE0(&graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t limit = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, argument);
  ir_node(&graph, limit)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  push_body(&graph, entry, initialize);
  push_body(&graph, entry, argument);
  push_body(&graph, entry, limit);
  set_control(&graph, entry, NODE0(&graph, jump(outer), OPT_REPR_NONE));

  ir_op_t less = ir_op(IR_OP_INT32_COMPARE);
  less.comparison = IR_CMP_LESS_THAN;
  ir_op_t add = ir_op(IR_OP_INT32_BINARY);
  add.binary = IR_BINARY_ADD;

  add_predecessor(&graph, outer, entry);
  add_predecessor(&graph, outer, outer_latch);
  ir_block(&graph, outer)->is_loop_header = true;
  ir_node_id_t i = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_INT32, zero, zero);
  push_phi(&graph, outer, i);
  ir_node_id_t outer_test = NODE(&graph, less, OPT_REPR_BOOL, i, limit);
  push_body(&graph, outer, outer_test);
  set_control(&graph, outer, NODE(&graph, branch(IR_COND_BOOL, inner_entry, done), OPT_REPR_NONE, outer_test));

  add_predecessor(&graph, inner_entry, outer);
  set_control(&graph, inner_entry, NODE0(&graph, jump(inner), OPT_REPR_NONE));
  add_predecessor(&graph, inner, inner_entry);
  add_predecessor(&graph, inner, inner_body);
  ir_block(&graph, inner)->is_loop_header = true;
  ir_node_id_t j = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_INT32, zero, zero);
  push_phi(&graph, inner, j);
  ir_node_id_t inner_test = NODE(&graph, less, OPT_REPR_BOOL, j, limit);
  push_body(&graph, inner, inner_test);
  set_control(&graph, inner, NODE(&graph, branch(IR_COND_BOOL, inner_body, outer_latch), OPT_REPR_NONE, inner_test));

  add_predecessor(&graph, inner_body, inner);
  ir_node_id_t boxed_j = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, j);
  ir_op_t store = ir_op(IR_OP_STORE_SLOT);
  store.slot = SLOT_L0;
  ir_node_id_t stored = NODE(&graph, store, OPT_REPR_NONE, boxed_j);
  ir_op_t generic = ir_op(IR_OP_GENERIC);
  generic.slow.opcode = IR_BC_INC_LOCAL;
  ir_node_id_t call = NODE0(&graph, generic, OPT_REPR_NONE);
  ir_node_id_t boxed_i = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, i);
  ir_node(&graph, call)->frame_state = frame_state(&graph, OPT_RESUME_AFTER, OPT_INVALID_SLOT,
    (ir_slot_value_t[]){{.slot = SLOT_R5, .value = boxed_i}, {.slot = SLOT_L1, .value = argument}}, 2);
  ir_node_id_t next_j = NODE(&graph, add, OPT_REPR_INT32, j, one);
  ir_node(&graph, next_j)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  push_body(&graph, inner_body, boxed_j);
  push_body(&graph, inner_body, stored);
  push_body(&graph, inner_body, boxed_i);
  push_body(&graph, inner_body, call);
  push_body(&graph, inner_body, next_j);
  set_control(&graph, inner_body, NODE0(&graph, jump(inner), OPT_REPR_NONE));
  ir_node(&graph, j)->inputs.items[1] = next_j;

  add_predecessor(&graph, outer_latch, inner);
  ir_node_id_t next_i = NODE(&graph, add, OPT_REPR_INT32, i, one);
  ir_node(&graph, next_i)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  push_body(&graph, outer_latch, next_i);
  set_control(&graph, outer_latch, NODE0(&graph, jump(outer), OPT_REPR_NONE));
  ir_node(&graph, i)->inputs.items[1] = next_i;

  add_predecessor(&graph, done, outer);
  ir_node_id_t sum = NODE(&graph, add, OPT_REPR_INT32, i, limit);
  ir_node(&graph, sum)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, sum);
  push_body(&graph, done, sum);
  push_body(&graph, done, boxed);
  set_control(&graph, done, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void swapping_loop_phis_go_through_a_temp_slot(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), header = new_block(&graph), latch = new_block(&graph), done = new_block(&graph);
  ir_node_id_t condition = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  push_body(&graph, entry, condition);
  set_control(&graph, entry, NODE0(&graph, jump(header), OPT_REPR_NONE));
  ir_node_id_t zero = NODE0(&graph, constant(ir_value_int32(0)), OPT_REPR_TAGGED);
  ir_node_id_t ten = NODE0(&graph, constant(ir_value_int32(10)), OPT_REPR_TAGGED);
  add_predecessor(&graph, header, entry);
  add_predecessor(&graph, header, latch);
  ir_block(&graph, header)->is_loop_header = true;
  ir_node_id_t first = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, zero, zero);
  ir_node_id_t second = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, ten, ten);
  ir_node(&graph, first)->inputs.items[1] = second;
  ir_node(&graph, second)->inputs.items[1] = first;
  push_phi(&graph, header, first);
  push_phi(&graph, header, second);
  set_control(&graph, header, NODE(&graph, branch(IR_COND_UNDEFINED, latch, done), OPT_REPR_NONE, condition));
  add_predecessor(&graph, latch, header);
  set_control(&graph, latch, NODE0(&graph, jump(header), OPT_REPR_NONE));
  add_predecessor(&graph, done, header);
  set_control(&graph, done, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, second));
  assert_graph_valid(&graph);

  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  ir_block_id_t back_edge = ir_block(&graph, header)->predecessors.items[1];
  const ra_moves_t *moves = &allocation.block_end_moves[back_edge];
  assert(moves->len == 3);
  bool through_stack = false;
  for (uint32_t i = 0; i < moves->len; i++) through_stack |= moves->items[i].to.kind == RA_LOC_STACK;
  assert(through_stack);
  opt_text_t text = {0};
  ra_dump_allocation(&graph, &allocation, &text);
  assert_text(text.data,
    "b0:\n"
    "  v0 LoadSlot -> r0\n"
    "  moves #0 -> r1, #10 -> r2\n"
    "  v1 Jump\n"
    "b1:\n"
    "  v4 Phi -> r1\n"
    "  v5 Phi -> r2\n"
    "  v6 Branch (r0)\n"
    "b2:\n"
    "  moves r1 -> s0, r2 -> r1, s0 -> r2\n"
    "  v7 Jump\n"
    "b3:\n"
    "  moves r2 -> r0\n"
    "  v8 Return (r0)\n"
    "spill slots: 1\n");
  opt_text_free(&text);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void exits_find_constants_registers_and_spilled_values(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t block = new_block(&graph);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t ten = NODE0(&graph, constant(ir_value_int32(10)), OPT_REPR_TAGGED);
  ir_op_t exit_op = ir_op(IR_OP_EXIT);
  exit_op.exit_kind = OPT_EXIT_NO_FEEDBACK;
  ir_node_id_t exit = NODE0(&graph, exit_op, OPT_REPR_NONE);
  ir_node(&graph, exit)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT,
    (ir_slot_value_t[]){{.slot = SLOT_R5, .value = ten}, {.slot = SLOT_R6, .value = argument}}, 2);
  push_body(&graph, block, argument);
  set_control(&graph, block, exit);
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  assert(allocation.nodes[argument].has_output && allocation.nodes[argument].output.kind == RA_LOC_REGISTER);
  uint8_t argument_register = allocation.nodes[argument].output.reg;
  ra_exit_values_t decoded = {0};
  ra_exit_values(&allocation, exit, &allocation.arena, &decoded);
  const ra_exit_values_t *values = &decoded;
  assert(values->len == 2);
  assert(values->items[0].slot == SLOT_R5 && ra_location_equal(values->items[0].location, ra_constant(ir_value_int32(10))));
  assert(values->items[1].slot == SLOT_R6 && ra_location_equal(values->items[1].location, ra_register(argument_register)));
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void the_verifier_finds_values_in_the_wrong_place(void) {
  ir_graph_t graph;
  ir_node_id_t argument = call_keeping_an_argument(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  ra_allocate(&graph, &registers, &allocation);
  assert(ra_verify(&graph, &registers, &allocation, NULL));

  // Forgetting to spill a value that is live across a call loses it.
  ra_allocation_t broken;
  ra_allocation_clone(&broken, &allocation);
  broken.nodes[argument].spill_slot = IR_NONE;
  assert(!ra_verify(&graph, &registers, &broken, NULL));
  ra_allocation_free(&broken);

  // Reading an input from a register that holds something else.
  ir_node_id_t call = ir_block(&graph, 1)->body.items[2];
  ra_allocation_clone(&broken, &allocation);
  ra_location_t *input = &broken.nodes[call].inputs.items[2];
  assert(input->kind == RA_LOC_REGISTER);
  *input = ra_register((uint8_t)((input->reg + 1) % 6));
  assert(!ra_verify(&graph, &registers, &broken, NULL));
  ra_allocation_free(&broken);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void cold_blocks_cost_the_code_around_them_no_spills(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), then_block = new_block(&graph), else_block = new_block(&graph), join = new_block(&graph);
  ir_node_id_t kept = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t condition = NODE0(&graph, load_slot(SLOT_R5), OPT_REPR_TAGGED);
  push_body(&graph, entry, kept);
  push_body(&graph, entry, condition);
  set_control(&graph, entry, NODE(&graph, branch(IR_COND_NULLISH, then_block, else_block), OPT_REPR_NONE, condition));
  ir_node_id_t zero = NODE0(&graph, constant(ir_value_int32(0)), OPT_REPR_TAGGED);
  add_predecessor(&graph, then_block, entry);
  add_predecessor(&graph, else_block, entry);
  set_control(&graph, then_block, NODE0(&graph, jump(join), OPT_REPR_NONE));
  set_control(&graph, else_block, NODE0(&graph, jump(join), OPT_REPR_NONE));
  add_predecessor(&graph, join, then_block);
  add_predecessor(&graph, join, else_block);
  ir_node_id_t phi = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, zero, condition);
  push_phi(&graph, join, phi);
  ir_op_t equals = ir_op(IR_OP_TAGGED_EQUALS);
  equals.equal = true;
  ir_node_id_t same = NODE(&graph, equals, OPT_REPR_BOOL, phi, kept);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, same);
  push_body(&graph, join, same);
  push_body(&graph, join, boxed);
  set_control(&graph, join, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  ir_block(&graph, then_block)->is_cold = true;
  pass_move_cold_blocks_last(&graph);
  assert_graph_valid(&graph);
  ir_block_id_t merge = IR_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(&graph); block++)
    if (ra_state_predecessor(&graph, block) != IR_NONE) merge = block;
  assert(merge != IR_NONE);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  assert(allocation.nodes[kept].spill_slot == IR_NONE);
  ir_node_id_t merged = ir_block(&graph, merge)->phis.items[0];
  assert(allocation.nodes[merged].output.kind == RA_LOC_REGISTER && allocation.nodes[merged].spill_slot == IR_NONE);
  // Only the cycle temp of the cold block's moves.
  assert(allocation.spill_slot_count == 1);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void float64_values_get_floating_point_registers(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t block = new_block(&graph);
  ir_node_id_t initialize = NODE0(&graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t number = NODE(&graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, argument);
  ir_node(&graph, number)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  ir_op_t sqrt = ir_op(IR_OP_FLOAT64_UNARY);
  sqrt.unary = IR_UNARY_SQRT;
  ir_node_id_t root = NODE(&graph, sqrt, OPT_REPR_FLOAT64, number);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, root);
  push_body(&graph, block, initialize);
  push_body(&graph, block, argument);
  push_body(&graph, block, number);
  push_body(&graph, block, root);
  push_body(&graph, block, boxed);
  set_control(&graph, block, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  uint32_t floats = 0;
  for (ir_node_id_t node = 0; node < ir_node_count(&graph); node++) {
    const ra_node_allocation_t *placed = &allocation.nodes[node];
    if (!placed->has_output || placed->output.kind != RA_LOC_REGISTER) continue;
    if (ir_node(&graph, node)->repr == OPT_REPR_FLOAT64) {
      assert(placed->output.reg >= OPT_FPR_BASE);
      floats++;
    } else {
      assert(placed->output.reg < OPT_FPR_BASE);
    }
  }
  assert(floats >= 2);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

// Ant: the value a back edge brings to a loop phi gets the phi's register once the phi died, which saves the move.
static void loop_carried_values_take_their_phis_register(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), header = new_block(&graph), latch = new_block(&graph), done = new_block(&graph);
  ir_node_id_t condition = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  push_body(&graph, entry, condition);
  set_control(&graph, entry, NODE0(&graph, jump(header), OPT_REPR_NONE));
  ir_node_id_t start = NODE0(&graph, constant(ir_value_double_bits(0.5)), OPT_REPR_FLOAT64);
  ir_node_id_t step = NODE0(&graph, constant(ir_value_double_bits(0.25)), OPT_REPR_FLOAT64);
  add_predecessor(&graph, header, entry);
  add_predecessor(&graph, header, latch);
  ir_block(&graph, header)->is_loop_header = true;
  ir_node_id_t phi = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_FLOAT64, start, start);
  push_phi(&graph, header, phi);
  set_control(&graph, header, NODE(&graph, branch(IR_COND_UNDEFINED, latch, done), OPT_REPR_NONE, condition));
  add_predecessor(&graph, latch, header);
  ir_op_t multiply = ir_op(IR_OP_FLOAT64_BINARY), add = ir_op(IR_OP_FLOAT64_BINARY);
  multiply.binary = IR_BINARY_MUL;
  add.binary = IR_BINARY_ADD;
  ir_node_id_t scaled = NODE(&graph, multiply, OPT_REPR_FLOAT64, phi, step);
  ir_node_id_t next = NODE(&graph, add, OPT_REPR_FLOAT64, scaled, step);
  ir_node(&graph, phi)->inputs.items[1] = next;
  push_body(&graph, latch, scaled);
  push_body(&graph, latch, next);
  set_control(&graph, latch, NODE0(&graph, jump(header), OPT_REPR_NONE));
  add_predecessor(&graph, done, header);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, phi);
  push_body(&graph, done, boxed);
  set_control(&graph, done, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);

  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  assert(allocation.nodes[phi].output.kind == RA_LOC_REGISTER);
  assert(ra_location_equal(allocation.nodes[next].output, allocation.nodes[phi].output));
  assert(allocation.block_end_moves[latch].len == 0);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void loop_phis_share_spill_slots_with_their_inputs(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph), header = new_block(&graph), body = new_block(&graph), done = new_block(&graph);
  ir_op_t add = ir_op(IR_OP_INT32_BINARY);
  add.binary = IR_BINARY_ADD;
  ir_node_id_t one = NODE0(&graph, constant(1), OPT_REPR_INT32);
  ir_node_id_t initialize = NODE0(&graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t base = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, argument);
  ir_node(&graph, base)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  push_body(&graph, entry, initialize);
  push_body(&graph, entry, argument);
  push_body(&graph, entry, base);
  ir_node_id_t starts[3], phis[3], nexts[3];
  ir_node_id_t previous = base;
  for (uint32_t i = 0; i < 3; i++) {
    starts[i] = NODE(&graph, add, OPT_REPR_INT32, previous, one);
    ir_node(&graph, starts[i])->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
    push_body(&graph, entry, starts[i]);
    previous = starts[i];
  }
  ir_op_t generic = ir_op(IR_OP_GENERIC);
  generic.slow.opcode = IR_BC_INC_LOCAL;
  ir_node_id_t first_call = NODE0(&graph, generic, OPT_REPR_NONE);
  ir_node(&graph, first_call)->frame_state = frame_state(&graph, OPT_RESUME_AFTER, OPT_INVALID_SLOT,
    (ir_slot_value_t[]){{.slot = SLOT_R5, .value = starts[0]}, {.slot = SLOT_L0, .value = starts[1]}}, 2);
  push_body(&graph, entry, first_call);
  set_control(&graph, entry, NODE0(&graph, jump(header), OPT_REPR_NONE));

  add_predecessor(&graph, header, entry);
  add_predecessor(&graph, header, body);
  ir_block(&graph, header)->is_loop_header = true;
  for (uint32_t i = 0; i < 3; i++) {
    phis[i] = NODE(&graph, ir_op(IR_OP_PHI), OPT_REPR_INT32, starts[i], starts[i]);
    push_phi(&graph, header, phis[i]);
  }
  set_control(&graph, header, NODE(&graph, branch(IR_COND_UNDEFINED, done, body), OPT_REPR_NONE, argument));

  add_predecessor(&graph, body, header);
  ir_node_id_t call = NODE0(&graph, generic, OPT_REPR_NONE);
  ir_node(&graph, call)->frame_state = frame_state(&graph, OPT_RESUME_AFTER, OPT_INVALID_SLOT,
    (ir_slot_value_t[]){{.slot = SLOT_R5, .value = phis[0]}, {.slot = SLOT_L0, .value = phis[1]}, {.slot = SLOT_L1, .value = phis[2]}}, 3);
  push_body(&graph, body, call);
  for (uint32_t i = 0; i < 3; i++) {
    nexts[i] = NODE(&graph, add, OPT_REPR_INT32, phis[i], one);
    ir_node(&graph, nexts[i])->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
    push_body(&graph, body, nexts[i]);
    ir_node(&graph, phis[i])->inputs.items[1] = nexts[i];
  }
  ir_node_id_t second_call = NODE0(&graph, generic, OPT_REPR_NONE);
  ir_node(&graph, second_call)->frame_state = frame_state(&graph, OPT_RESUME_AFTER, OPT_INVALID_SLOT,
    (ir_slot_value_t[]){{.slot = SLOT_R5, .value = nexts[0]}, {.slot = SLOT_L0, .value = nexts[1]}, {.slot = SLOT_L1, .value = nexts[2]}}, 3);
  push_body(&graph, body, second_call);
  set_control(&graph, body, NODE0(&graph, jump(header), OPT_REPR_NONE));

  add_predecessor(&graph, done, header);
  ir_node_id_t sum = NODE(&graph, add, OPT_REPR_INT32, phis[0], phis[1]);
  ir_node(&graph, sum)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  ir_node_id_t total = NODE(&graph, add, OPT_REPR_INT32, sum, phis[2]);
  ir_node(&graph, total)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, total);
  push_body(&graph, done, sum);
  push_body(&graph, done, total);
  push_body(&graph, done, boxed);
  set_control(&graph, done, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);

  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  uint32_t shared = 0, filled = 0;
  for (uint32_t i = 0; i < 3; i++) {
    uint32_t phi_slot = allocation.nodes[phis[i]].spill_slot, input_slot = allocation.nodes[nexts[i]].spill_slot;
    if (phi_slot != IR_NONE && input_slot != IR_NONE) {
      assert(phi_slot == input_slot);
      shared++;
    }
    if (phi_slot != IR_NONE && allocation.nodes[starts[i]].spill_slot == phi_slot && input_slot == phi_slot) {
      assert(ra_phi_spill_store(&graph, &allocation, phis[i]) == IR_NONE);
      filled++;
    }
  }
  assert(shared == 3);
  assert(filled == 3);
  for (uint32_t i = 0; i < allocation.block_end_moves[body].len; i++) assert(allocation.block_end_moves[body].items[i].to.kind != RA_LOC_STACK);
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

static void int32_operations_write_over_their_first_input_where_it_dies(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_block_id_t entry = new_block(&graph);
  ir_node_id_t argument = NODE0(&graph, load_slot(SLOT_A0), OPT_REPR_TAGGED);
  ir_node_id_t value = NODE(&graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, argument);
  ir_node(&graph, value)->frame_state = frame_state(&graph, OPT_RESUME_AT, OPT_INVALID_SLOT, NULL, 0);
  ir_op_t xor = ir_op(IR_OP_INT32_BINARY);
  xor.binary = IR_BINARY_BITWISE_XOR;
  ir_node_id_t first = NODE(&graph, xor, OPT_REPR_INT32, value, value);
  ir_node_id_t second = NODE(&graph, xor, OPT_REPR_INT32, first, value);
  ir_node_id_t boxed = NODE(&graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, second);
  push_body(&graph, entry, argument);
  push_body(&graph, entry, value);
  push_body(&graph, entry, first);
  push_body(&graph, entry, second);
  push_body(&graph, entry, boxed);
  set_control(&graph, entry, NODE(&graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, boxed));
  assert_graph_valid(&graph);
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  allocate_and_verify(&graph, &registers, &allocation);
  assert(allocation.nodes[second].has_output);
  assert(ra_location_equal(allocation.nodes[second].output, allocation.nodes[second].inputs.items[0]));
  assert(!ra_location_equal(allocation.nodes[first].output, allocation.nodes[first].inputs.items[0]));
  ra_allocation_free(&allocation);
  ir_graph_free(&graph);
  tests_run++;
}

// A tiny deterministic random number generator (xorshift64).
typedef struct {
  uint64_t state;
} random_t;

static uint64_t random_next(random_t *random) {
  random->state ^= random->state << 13;
  random->state ^= random->state >> 7;
  random->state ^= random->state << 17;
  return random->state;
}

static uint32_t random_below(random_t *random, uint32_t bound) {
  return (uint32_t)(random_next(random) % bound);
}

static bool random_chance(random_t *random, uint32_t percent) {
  return random_below(random, 100) < percent;
}

static constexpr uint32_t VARIABLE_COUNT = 6;

typedef struct {
  random_t random;
  ir_graph_t *graph;
  ir_block_id_t block;
  ir_node_id_t variables[VARIABLE_COUNT];
  bool returned;
} generator_t;

static ir_node_id_t generator_add(generator_t *generator, ir_node_id_t node) {
  push_body(generator->graph, generator->block, node);
  return node;
}

static ir_frame_state_id_t generator_state(generator_t *generator, opt_resume_mode_t mode) {
  ir_slot_value_t values[VARIABLE_COUNT];
  uint32_t count = 0;
  for (uint32_t i = 0; i < VARIABLE_COUNT; i++)
    if (random_chance(&generator->random, 70)) values[count++] = (ir_slot_value_t){.slot = SLOT_R5 + i, .value = generator->variables[i]};
  return frame_state(generator->graph, mode, OPT_INVALID_SLOT, values, count);
}

static ir_node_id_t generator_readable(generator_t *generator) {
  ir_graph_t *graph = generator->graph;
  switch (random_below(&generator->random, 10)) {
    case 0: return NODE0(graph, constant(ir_value_int32((int32_t)random_below(&generator->random, 20))), OPT_REPR_TAGGED);
    case 1: return NODE0(graph, constant(IR_VALUE_UNDEFINED), OPT_REPR_TAGGED);
    default: return generator->variables[random_below(&generator->random, VARIABLE_COUNT)];
  }
}

static constexpr ir_cell_t GENERATED_SHAPE = 0x100;

static ir_node_id_t generator_object(generator_t *generator) {
  ir_graph_t *graph = generator->graph;
  random_t *random = &generator->random;
  switch (random_below(random, 3)) {
    case 0: {
      ir_op_t allocate = ir_op(IR_OP_ALLOCATE_OBJECT);
      allocate.allocate_object.shape = GENERATED_SHAPE;
      allocate.allocate_object.property_count = 2;
      allocate.allocate_object.reserve = 2;
      ir_node_id_t object = generator_add(generator, NODE0(graph, allocate, OPT_REPR_TAGGED));
      ir_op_t initialize = ir_op(IR_OP_INITIALIZE_NAMED);
      initialize.offset = random_below(random, 2);
      generator_add(generator, NODE(graph, initialize, OPT_REPR_NONE, object, generator_readable(generator)));
      return object;
    }
    case 1: {
      ir_node_id_t object = generator_readable(generator);
      ir_node_id_t address = generator_add(generator, NODE(graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object));
      ir_op_t check = ir_op(IR_OP_CHECK_SHAPE);
      check.shapes.items = ir_dup_shapes(graph, &(ir_shape_check_t){.shape = GENERATED_SHAPE}, 1);
      check.shapes.len = 1;
      ir_node_id_t checked = generator_add(generator, NODE(graph, check, OPT_REPR_TAGGED, object, address));
      ir_node(graph, checked)->frame_state = generator_state(generator, OPT_RESUME_AT);
      ir_op_t load = ir_op(IR_OP_LOAD_NAMED);
      load.offset = random_below(random, 2);
      ir_node_id_t loaded = generator_add(generator, NODE(graph, load, OPT_REPR_TAGGED, checked, address));
      ir_node(graph, loaded)->frame_state = generator_state(generator, OPT_RESUME_AT);
      return loaded;
    }
    default: {
      ir_node_id_t object = generator_readable(generator);
      ir_node_id_t address = generator_add(generator, NODE(graph, ir_op(IR_OP_CELL_ADDRESS), OPT_REPR_POINTER, object));
      ir_op_t store = ir_op(IR_OP_STORE_NAMED);
      store.offset = random_below(random, 2);
      ir_node_id_t stored = generator_add(generator, NODE(graph, store, OPT_REPR_NONE, object, generator_readable(generator), address));
      ir_node(graph, stored)->frame_state = generator_state(generator, OPT_RESUME_AT);
      return object;
    }
  }
}

static void generator_plain(generator_t *generator) {
  ir_graph_t *graph = generator->graph;
  random_t *random = &generator->random;
  uint32_t target = random_below(random, VARIABLE_COUNT);
  ir_node_id_t value = IR_NONE;
  if (random_chance(random, 25)) {
    generator->variables[target] = generator_object(generator);
    return;
  }
  switch (random_below(random, 11)) {
    case 0:
    case 1: value = generator_readable(generator); break;
    case 2: value = generator_add(generator, NODE0(graph, load_slot(SLOT_A0), OPT_REPR_TAGGED)); break;
    case 3: {
      ir_op_t equals = ir_op(IR_OP_TAGGED_EQUALS);
      equals.equal = random_chance(random, 50);
      ir_node_id_t same = generator_add(generator, NODE(graph, equals, OPT_REPR_BOOL, generator_readable(generator), generator_readable(generator)));
      value = generator_add(generator, NODE(graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, same));
      break;
    }
    case 4: {
      ir_node_id_t check = generator_add(generator, NODE(graph, ir_op(IR_OP_CHECK_INT32), OPT_REPR_INT32, generator_readable(generator)));
      ir_node(graph, check)->frame_state = generator_state(generator, OPT_RESUME_AT);
      ir_op_t add = ir_op(IR_OP_INT32_BINARY);
      add.binary = random_chance(random, 50) ? IR_BINARY_ADD : IR_BINARY_MOD;
      ir_node_id_t one = NODE0(graph, constant(1 + random_below(random, 5)), OPT_REPR_INT32);
      ir_node_id_t sum = generator_add(generator, NODE(graph, add, OPT_REPR_INT32, check, one));
      ir_node(graph, sum)->frame_state = generator_state(generator, OPT_RESUME_AT);
      value = generator_add(generator, NODE(graph, ir_op(IR_OP_BOX_INT32), OPT_REPR_TAGGED, sum));
      break;
    }
    case 5: {
      ir_node_id_t number = generator_add(generator, NODE(graph, ir_op(IR_OP_CHECK_NUMBER), OPT_REPR_FLOAT64, generator_readable(generator)));
      ir_node(graph, number)->frame_state = generator_state(generator, OPT_RESUME_AT);
      ir_op_t binary = ir_op(IR_OP_FLOAT64_BINARY);
      binary.binary = IR_BINARY_MUL;
      ir_node_id_t product = generator_add(generator, NODE(graph, binary, OPT_REPR_FLOAT64, number, number));
      value = generator_add(generator, NODE(graph, ir_op(IR_OP_BOX_FLOAT64), OPT_REPR_TAGGED, product));
      break;
    }
    case 6: {
      ir_node_id_t checked = generator_add(generator, NODE(graph, ir_op(IR_OP_CHECK_OBJECT), OPT_REPR_TAGGED, generator_readable(generator)));
      ir_node(graph, checked)->frame_state = generator_state(generator, OPT_RESUME_AT);
      value = checked;
      break;
    }
    case 7: {
      ir_op_t call = slow_path(IR_BC_CALL);
      uint32_t count = random_below(random, 4);
      ir_node_id_t inputs[4];
      for (uint32_t i = 0; i < count; i++) inputs[i] = generator_readable(generator);
      value = generator_add(generator, node_with(graph, call, inputs, count, OPT_REPR_TAGGED));
      ir_node(graph, value)->frame_state = generator_state(generator, OPT_RESUME_AFTER);
      break;
    }
    case 8: {
      ir_op_t generic = ir_op(IR_OP_GENERIC);
      generic.slow.opcode = IR_BC_TYPEOF;
      ir_node_id_t call = generator_add(generator, NODE0(graph, generic, OPT_REPR_TAGGED));
      ir_node(graph, call)->frame_state = generator_state(generator, OPT_RESUME_AFTER);
      value = call;
      break;
    }
    case 9: {
      ir_node_id_t truth = generator_add(generator, NODE(graph, ir_op(IR_OP_TO_BOOLEAN), OPT_REPR_BOOL, generator_readable(generator)));
      value = generator_add(generator, NODE(graph, ir_op(IR_OP_BOX_BOOL), OPT_REPR_TAGGED, truth));
      break;
    }
    default: {
      ir_node_id_t address = generator_add(generator, NODE(graph, ir_op(IR_OP_SET_LEXICAL_ENVIRONMENT), OPT_REPR_POINTER, generator_readable(generator)));
      value = generator_add(generator, NODE(graph, ir_op(IR_OP_BOX_CELL), OPT_REPR_TAGGED, address));
      break;
    }
  }
  generator->variables[target] = value;
}

static void generator_statements(generator_t *generator, uint32_t depth);

static void generator_join(generator_t *generator, const ir_block_id_t *ends, ir_node_id_t (*values)[VARIABLE_COUNT], uint32_t count) {
  ir_graph_t *graph = generator->graph;
  ir_block_id_t join = new_block(graph);
  for (uint32_t i = 0; i < count; i++) {
    set_control(graph, ends[i], NODE0(graph, jump(join), OPT_REPR_NONE));
    add_predecessor(graph, join, ends[i]);
  }
  for (uint32_t v = 0; v < VARIABLE_COUNT; v++) {
    bool same = true;
    for (uint32_t i = 1; i < count; i++) same &= values[i][v] == values[0][v];
    if (same) {
      generator->variables[v] = values[0][v];
      continue;
    }
    ir_node_id_t phi = NODE0(graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED);
    for (uint32_t i = 0; i < count; i++) ir_push_input(graph, phi, values[i][v]);
    push_phi(graph, join, phi);
    generator->variables[v] = phi;
  }
  generator->block = join;
}

static void generator_if_else(generator_t *generator, uint32_t depth) {
  ir_graph_t *graph = generator->graph;
  ir_node_id_t condition = generator_readable(generator);
  ir_block_id_t from = generator->block;
  ir_block_id_t then_block = new_block(graph);
  add_predecessor(graph, then_block, from);
  ir_node_id_t before[VARIABLE_COUNT];
  memcpy(before, generator->variables, sizeof before);
  generator->block = then_block;
  generator_statements(generator, depth);
  bool then_returned = generator->returned;
  ir_block_id_t then_end = generator->block;
  ir_node_id_t then_values[VARIABLE_COUNT];
  memcpy(then_values, generator->variables, sizeof then_values);

  ir_block_id_t else_block = new_block(graph);
  add_predecessor(graph, else_block, from);
  set_control(graph, from, NODE(graph, branch(random_chance(&generator->random, 50) ? IR_COND_UNDEFINED : IR_COND_OBJECT, then_block, else_block),
    OPT_REPR_NONE, condition));
  memcpy(generator->variables, before, sizeof before);
  generator->returned = false;
  generator->block = else_block;
  generator_statements(generator, depth);
  bool else_returned = generator->returned;
  ir_block_id_t ends[2];
  ir_node_id_t values[2][VARIABLE_COUNT];
  uint32_t count = 0;
  if (!then_returned) {
    ends[count] = then_end;
    memcpy(values[count++], then_values, sizeof then_values);
  }
  if (!else_returned) {
    ends[count] = generator->block;
    memcpy(values[count++], generator->variables, sizeof then_values);
  }
  generator->returned = count == 0;
  if (count) generator_join(generator, ends, values, count);
}

static void generator_loop(generator_t *generator, uint32_t depth) {
  ir_graph_t *graph = generator->graph;
  ir_block_id_t header = new_block(graph);
  ir_block(graph, header)->is_loop_header = true;
  set_control(graph, generator->block, NODE0(graph, jump(header), OPT_REPR_NONE));
  add_predecessor(graph, header, generator->block);
  ir_node_id_t phis[VARIABLE_COUNT];
  for (uint32_t v = 0; v < VARIABLE_COUNT; v++) {
    phis[v] = NODE(graph, ir_op(IR_OP_PHI), OPT_REPR_TAGGED, generator->variables[v]);
    push_phi(graph, header, phis[v]);
    generator->variables[v] = phis[v];
  }
  generator->block = header;
  ir_node_id_t condition = generator_readable(generator);
  ir_block_id_t body = new_block(graph);
  add_predecessor(graph, body, header);
  generator->block = body;
  generator_statements(generator, depth);
  bool body_returned = generator->returned;
  if (!body_returned) {
    set_control(graph, generator->block, NODE0(graph, jump(header), OPT_REPR_NONE));
    add_predecessor(graph, header, generator->block);
    for (uint32_t v = 0; v < VARIABLE_COUNT; v++) ir_push_input(graph, phis[v], generator->variables[v]);
  }
  ir_block_id_t exit = new_block(graph);
  add_predecessor(graph, exit, header);
  set_control(graph, header, NODE(graph, branch(IR_COND_UNDEFINED, body, exit), OPT_REPR_NONE, condition));
  memcpy(generator->variables, phis, sizeof phis);
  generator->returned = false;
  generator->block = exit;
}

static void generator_statements(generator_t *generator, uint32_t depth) {
  uint32_t count = 1 + random_below(&generator->random, 5);
  for (uint32_t i = 0; i < count && !generator->returned; i++) {
    uint32_t choice = random_below(&generator->random, 10);
    if (choice < 2 && depth < 4) generator_if_else(generator, depth + 1);
    else if (choice == 2 && depth < 4) generator_loop(generator, depth + 1);
    else if (choice == 3 && random_chance(&generator->random, 15)) {
      set_control(generator->graph, generator->block, NODE(generator->graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, generator_readable(generator)));
      generator->returned = true;
    } else {
      generator_plain(generator);
    }
  }
}

static void generate(ir_graph_t *graph, uint64_t seed) {
  ir_graph_init(graph);
  generator_t generator = {.random = {seed}, .graph = graph, .block = new_block(graph)};
  push_body(graph, generator.block, NODE0(graph, ir_op(IR_OP_INITIALIZE_FRAME), OPT_REPR_NONE));
  for (uint32_t v = 0; v < VARIABLE_COUNT; v++)
    generator.variables[v] = generator_add(&generator, NODE0(graph, load_slot(SLOT_R5 + v), OPT_REPR_TAGGED));
  generator_statements(&generator, 0);
  if (!generator.returned)
    set_control(graph, generator.block, NODE(graph, ir_op(IR_OP_RETURN), OPT_REPR_NONE, generator_readable(&generator)));
}

static void allocate_random(ir_graph_t *graph, uint64_t seed, const char *what) {
  opt_text_t errors = {0};
  if (!pass_verify(graph, &errors)) {
    opt_text_t dump = {0};
    ir_dump(graph, &test_layout, &dump);
    fprintf(stderr, "seed %llu (%s): %s\n%s", (unsigned long long)seed, what, errors.data, dump.data);
    assert(false);
  }
  opt_register_set_t registers = opt_test_registers();
  ra_allocation_t allocation;
  ra_allocate(graph, &registers, &allocation);
  if (!ra_verify(graph, &registers, &allocation, &errors)) {
    opt_text_t dump = {0}, allocation_dump = {0};
    ir_dump(graph, &test_layout, &dump);
    ra_dump_allocation(graph, &allocation, &allocation_dump);
    fprintf(stderr, "seed %llu (%s): %s\n%s\n%s", (unsigned long long)seed, what, errors.data, dump.data, allocation_dump.data);
    assert(false);
  }
  ra_allocation_free(&allocation);
}

static void build(ir_graph_t *graph, uint64_t seed) {
  generate(graph, seed * UINT64_C(0x9E3779B97F4A7C15));
  pass_optimize(graph, &test_layout, true);
}

static void random_structured_programs_allocate_correctly(void) {
  for (uint64_t seed = 1; seed <= 2000; seed++) {
    ir_graph_t graph;
    build(&graph, seed);
    allocate_random(&graph, seed, "built");
    ir_graph_free(&graph);
  }
  tests_run++;
}

static void random_structured_programs_with_cold_blocks_allocate_correctly(void) {
  for (uint64_t seed = 1; seed <= 1000; seed++) {
    ir_graph_t graph;
    build(&graph, seed);
    random_t random = {seed};
    for (ir_block_id_t block = 1; block < ir_block_count(&graph); block++)
      if (!ir_block(&graph, block)->is_loop_header && random_chance(&random, 25)) ir_block(&graph, block)->is_cold = true;
    pass_move_cold_blocks_last(&graph);
    allocate_random(&graph, seed, "cold blocks");
    ir_graph_free(&graph);
  }
  tests_run++;
}

int main(void) {
  resolves_chains_cycles_and_fan_out();
  target_register_sets_follow_libjs();
  straight_line_code();
  values_live_across_calls_are_spilled_at_their_definition();
  many_simultaneously_live_values_are_spilled();
  if_else_phis_and_truthiness_fallbacks();
  nested_loops_with_generic_nodes();
  swapping_loop_phis_go_through_a_temp_slot();
  exits_find_constants_registers_and_spilled_values();
  the_verifier_finds_values_in_the_wrong_place();
  cold_blocks_cost_the_code_around_them_no_spills();
  float64_values_get_floating_point_registers();
  loop_carried_values_take_their_phis_register();
  loop_phis_share_spill_slots_with_their_inputs();
  int32_operations_write_over_their_first_input_where_it_dies();
  random_structured_programs_allocate_correctly();
  random_structured_programs_with_cold_blocks_allocate_correctly();
  printf("opt-regalloc: %u tests passed\n", tests_run);
  return 0;
}
