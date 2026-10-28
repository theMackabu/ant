#include "../src/jit/opt/bitset.h"
#include "../src/jit/opt/hash.h"
#include "../src/jit/opt/ir/ir.h"
#include "../src/jit/opt/ir/ir_value.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void sets_of_locations(void) {
  assert(ir_locations_intersect(IR_LOC_NAMED, IR_LOC_SHAPES));
  assert(!ir_locations_intersect(IR_LOC_NAMED, IR_LOC_ELEMENTS_AND_COUNTS));
  assert(!ir_locations_intersect(IR_LOC_ELEMENTS, IR_LOC_ELEMENT_COUNTS));
  assert(ir_locations_intersect(IR_LOC_ALL, IR_LOC_GLOBAL_BINDINGS));
  assert(IR_LOC_NONE == 0 && !ir_locations_intersect(IR_LOC_NONE, IR_LOC_ALL));
  ir_locations_t locations = IR_LOC_FRAME;
  locations |= IR_LOC_ELEMENTS;
  assert(locations == (IR_LOC_FRAME | IR_LOC_ELEMENTS));
}

static void expect_description(uint64_t bits, const char *expected) {
  char text[64];
  ir_value_describe(bits, text, sizeof text);
  if (strcmp(text, expected)) {
    fprintf(stderr, "describe(0x%016llx) = %s, expected %s\n", (unsigned long long)bits, text, expected);
    assert(false);
  }
}

static void describes_known_values(void) {
  expect_description(IR_VALUE_EMPTY, "empty");
  expect_description(IR_VALUE_TRUE, "true");
  expect_description(ir_value_int32(-5), "-5");
  expect_description(ir_value_int32(7), "7");
  // Ant stores numbers as doubles: 2.0 is the int32 2, 0.5 is no int32.
  expect_description(UINT64_C(0x4000000000000000), "2");
  expect_description(UINT64_C(0x3fe0000000000000), "0x3fe0000000000000");
}

static void classifies_values_in_ants_encoding(void) {
  int32_t integer;
  assert(ir_value_as_int32(ir_value_int32(INT32_MIN), &integer) && integer == INT32_MIN);
  assert(!ir_value_as_int32(ir_value_double_bits(-0.0), NULL));
  assert(!ir_value_as_int32(ir_value_double_bits(2147483648.0), NULL));
  assert(ir_value_is_double(ir_value_double_bits(0.5)) && ir_value_is_double(IR_VALUE_CANONICAL_NAN));
  assert(!ir_value_is_double(ir_value_int32(3)) && !ir_value_is_double(IR_VALUE_UNDEFINED));
  assert(ir_value_is_number(ir_value_double_bits(-1e300)) && !ir_value_is_number(IR_VALUE_NULL));
  assert(ir_value_is_object(mkval(kTypeArray, 64)) && ir_value_is_object(mkval(kTypeFunction, 64)));
  assert(!ir_value_is_object(mkval(kTypeString, 64)) && ir_value_is_string(mkval(kTypeString, 64)));
  assert(ir_value_boxed_constant(1, OPT_REPR_BOOL) == IR_VALUE_TRUE);
  assert(ir_value_boxed_constant((uint32_t)-4, OPT_REPR_INT32) == ir_value_int32(-4));
  assert(ir_value_virtual_arguments_kind(IR_VALUE_VIRTUAL_MAPPED_ARGUMENTS) == 1);
  assert(ir_value_virtual_arguments_kind(IR_VALUE_EMPTY) == -1);
}

static void inserts_removes_and_iterates_across_words(void) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  opt_bitset_t set = opt_bitset_new(&arena, 130);
  assert(opt_bitset_is_empty(&set));
  uint32_t indices[] = {0, 63, 64, 129};
  for (uint32_t i = 0; i < 4; i++) opt_bitset_insert(&set, indices[i]);
  assert(opt_bitset_contains(&set, 63) && opt_bitset_contains(&set, 64) && !opt_bitset_contains(&set, 65));
  uint32_t seen[8], count = 0;
  opt_bitset_for_each(&set, index) seen[count++] = index;
  assert(count == 4 && seen[0] == 0 && seen[1] == 63 && seen[2] == 64 && seen[3] == 129);
  opt_bitset_remove(&set, 63);
  assert(opt_bitset_count(&set) == 3);

  opt_bitset_t other = opt_bitset_new(&arena, 130);
  opt_bitset_insert(&other, 1);
  assert(opt_bitset_union_with(&set, &other));
  assert(!opt_bitset_union_with(&set, &other));
  count = 0;
  opt_bitset_for_each(&set, index) seen[count++] = index;
  assert(count == 4 && seen[0] == 0 && seen[1] == 1 && seen[2] == 64 && seen[3] == 129);
  opt_bitset_insert(&other, 129);
  opt_bitset_subtract(&set, &other);
  count = 0;
  opt_bitset_for_each(&set, index) seen[count++] = index;
  assert(count == 2 && seen[0] == 0 && seen[1] == 64);
  opt_arena_free(&arena);
}

static void maps_insert_find_and_remove(void) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  opt_map_t map;
  opt_map_init(&map, &arena, 2);
  for (uint64_t i = 0; i < 1000; i++) opt_map_set2(&map, i, i * 7, i + 1);
  assert(map.len == 1000);
  for (uint64_t i = 0; i < 1000; i++) assert(*opt_map_get2(&map, i, i * 7) == i + 1);
  assert(!opt_map_get2(&map, 3, 3));
  for (uint64_t i = 0; i < 1000; i += 2) assert(opt_map_remove(&map, (uint64_t[]){i, i * 7}));
  for (uint64_t i = 0; i < 1000; i++) assert((opt_map_get2(&map, i, i * 7) != NULL) == (i % 2 == 1));
  assert(map.len == 500);
  opt_arena_free(&arena);
}

static void ops_describe_their_effects(void) {
  ir_op_t call = ir_op(IR_OP_GENERIC);
  ir_node_properties_t properties = ir_op_properties(&call);
  assert(properties.is_call && properties.can_lazy_exit && properties.runs_code && properties.reads == IR_LOC_ALL);
  ir_op_t slow_path = ir_op(IR_OP_CALL_SLOW_PATH);
  slow_path.slow.saves_registers = true;
  properties = ir_op_properties(&slow_path);
  assert(!properties.is_call && !properties.can_lazy_exit && properties.runs_code);
  ir_op_t elements = ir_op(IR_OP_CHECK_ELEMENTS);
  elements.elements = ir_elements_typed(IR_TYPED_INT8);
  assert(ir_op_properties(&elements).reads == IR_LOC_NONE);
  elements.elements = IR_ELEMENTS_HOLEY;
  assert(ir_op_properties(&elements).reads == IR_LOC_ELEMENT_COUNTS);
  ir_op_t add = ir_op(IR_OP_INT32_BINARY);
  add.binary = IR_BINARY_ADD;
  opt_exit_kind_t kind;
  assert(ir_op_exit_kind(&add, &kind) && kind == OPT_EXIT_OVERFLOW);
  add.binary = IR_BINARY_BITWISE_AND;
  assert(!ir_op_can_eager_exit(&add));
  assert(ir_op_refined_input(&elements) == 0 && ir_op_refined_input(&add) == -1);
  assert(!strcmp(ir_op_name(IR_OP_PROBE_HAS_PROPERTY), "ProbeHasProperty"));
  assert(!strcmp(ir_op_name(IR_OP_CONSTANT), "Constant"));
}

static void graphs_dump_as_text(void) {
  ir_graph_t graph;
  ir_graph_init(&graph);
  ir_frame_layout_t layout = {.reserved_count = 5, .number_of_registers = 8, .registers_and_locals_count = 10, .number_of_constants = 2, .number_of_arguments = 1};
  ir_block_id_t entry = ir_add_block(&graph);
  ir_op_t load = ir_op(IR_OP_LOAD_SLOT);
  load.slot = 12;
  ir_node_id_t value = ir_add_node(&graph, load, NULL, 0, OPT_REPR_TAGGED, 0);
  ir_op_t constant = ir_op(IR_OP_CONSTANT);
  constant.bits = 3;
  ir_node_id_t three = ir_add_node(&graph, constant, NULL, 0, OPT_REPR_INT32, 0);
  ir_node_id_t check = ir_add_node(&graph, ir_op(IR_OP_CHECK_INT32), &value, 1, OPT_REPR_INT32, 0);
  ir_op_t add_op = ir_op(IR_OP_INT32_BINARY);
  add_op.binary = IR_BINARY_ADD;
  ir_node_id_t sum = ir_add_node(&graph, add_op, (ir_node_id_t[]){check, three}, 2, OPT_REPR_INT32, 0);
  ir_node_id_t boxed = ir_add_node(&graph, ir_op(IR_OP_BOX_INT32), &sum, 1, OPT_REPR_TAGGED, 0);
  ir_node_id_t ret = ir_add_node(&graph, ir_op(IR_OP_RETURN), &boxed, 1, OPT_REPR_NONE, 0);
  ir_frame_state_t state = {.bc_off = 4, .mode = OPT_RESUME_AT, .dst = OPT_INVALID_SLOT, .parent = IR_NONE, .passed_argument_count = IR_NONE};
  opt_vec_push(&graph.arena, &state.values, ((ir_slot_value_t){.slot = 5, .value = value}));
  ir_node(&graph, check)->frame_state = ir_add_frame_state(&graph, &state);
  ir_block_t *block = ir_block(&graph, entry);
  opt_vec_push(&graph.arena, &block->body, value);
  opt_vec_push(&graph.arena, &block->body, check);
  opt_vec_push(&graph.arena, &block->body, sum);
  opt_vec_push(&graph.arena, &block->body, boxed);
  block->control = ret;
  opt_text_t text = {0};
  ir_dump(&graph, &layout, &text);
  const char *expected =
    "b0:\n"
    "  v0 = LoadSlot a0\n"
    "  v2:int32 = CheckInt32 (v0) [fs0]\n"
    "  v3:int32 = Int32Add (v2, #i32:3)\n"
    "  v4 = BoxInt32 (v3)\n"
    "  Return (v4)\n"
    "frame states:\n"
    "  fs0: pc 4 at {r5=v0}\n";
  if (strcmp(text.data, expected)) {
    fprintf(stderr, "dump:\n%s\nexpected:\n%s\n", text.data, expected);
    assert(false);
  }
  opt_text_free(&text);

  ir_graph_t copy;
  ir_graph_clone(&copy, &graph);
  ir_graph_free(&graph);
  assert(ir_node(&copy, sum)->inputs.items[1] == three);
  assert(ir_frame_state(&copy, 0)->values.items[0].value == value);
  ir_graph_free(&copy);
}

int main(void) {
  sets_of_locations();
  describes_known_values();
  classifies_values_in_ants_encoding();
  inserts_removes_and_iterates_across_words();
  maps_insert_find_and_remove();
  ops_describe_their_effects();
  graphs_dump_as_text();
  puts("opt-ir: 7 tests passed");
  return 0;
}
