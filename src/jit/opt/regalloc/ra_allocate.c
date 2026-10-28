#include "ra_internal.h"
#include "../ir/ir_value.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static constexpr uint8_t RA_NO_REGISTER = 0xff;

bool ra_saves_only_gprs(const ir_op_t *op) {
  switch (op->code) {
    case IR_OP_CALL_SLOW_PATH: return op->slow.saves_registers;
    case IR_OP_CALL_ARRAY_PUSH:
    case IR_OP_CREATE_ARRAY_ITERATOR:
    case IR_OP_ADD_NAMED:
    case IR_OP_PROBE_KEYED_STORE:
    case IR_OP_PROBE_PROPERTY_CACHE:
    case IR_OP_PROBE_PROPERTY_STORE:
    case IR_OP_PROBE_HAS_PROPERTY:
    case IR_OP_ALLOCATE_OBJECT:
    case IR_OP_ALLOCATE_ARRAY:
    case IR_OP_ALLOCATE_FUNCTION:
    case IR_OP_ALLOCATE_ENVIRONMENT:
    case IR_OP_CONCATENATE_STRINGS: return true;
    default: return false;
  }
}

static bool ra_reuses_first_input(const ir_op_t *op) {
  return op->code == IR_OP_INT32_WRAPPING_BINARY || op->code == IR_OP_INT32_BINARY || op->code == IR_OP_BOOL_TO_INT32;
}

typedef enum : uint8_t {
  RA_INPUT_REGISTER,
  RA_INPUT_FIXED_REGISTER,
  RA_INPUT_REGISTER_OR_CONSTANT,
  RA_INPUT_ANYWHERE,
} ra_input_kind_t;

typedef struct {
  ra_input_kind_t kind;
  uint8_t reg;
} ra_input_constraint_t;

typedef enum : uint8_t {
  RA_OUTPUT_NONE,
  RA_OUTPUT_REGISTER,
  RA_OUTPUT_FIXED_REGISTER,
} ra_output_kind_t;

typedef struct {
  ra_input_constraint_t *inputs;
  ra_output_kind_t output;
  uint8_t output_reg;
  uint8_t temps;
  bool is_call;
} ra_constraints_t;

static void ra_all_inputs(ra_constraints_t *constraints, uint32_t count, ra_input_kind_t kind) {
  for (uint32_t i = 0; i < count; i++) constraints->inputs[i] = (ra_input_constraint_t){.kind = kind};
}

static void ra_fixed_output(ra_constraints_t *constraints, uint8_t reg) {
  constraints->output = RA_OUTPUT_FIXED_REGISTER;
  constraints->output_reg = reg;
}

static ra_constraints_t ra_constraints(const ir_node_t *node, const opt_register_set_t *registers, opt_arena_t *arena) {
  uint32_t count = node->inputs.len;
  ra_constraints_t constraints = {
    .inputs = opt_arena_new(arena, ra_input_constraint_t, count + 1),
    .output = node->repr != OPT_REPR_NONE ? RA_OUTPUT_REGISTER : RA_OUTPUT_NONE,
    .is_call = ra_clobbers_caller_saved(&node->op),
  };
  ra_input_constraint_t *inputs = constraints.inputs;
  const ir_op_t *op = &node->op;
  switch (op->code) {
    case IR_OP_CONSTANT:
    case IR_OP_PHI: assert(false && "constants and phis have no constraints"); break;
    case IR_OP_TO_BOOLEAN:
      inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[0]};
      ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_STRICT_EQUALS:
      inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[0]};
      inputs[1] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[1]};
      ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_PRIMITIVE_TO_STRING:
    case IR_OP_TO_OBJECT:
    case IR_OP_ARRAY_CREATE:
      for (uint32_t i = 0; i < count; i++) inputs[i] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[i + 1]};
      ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_GENERIC:
      if (node->repr != OPT_REPR_NONE) ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_SLICE_ARGUMENTS:
      inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[2]};
      ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_RETURN: inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->return_gpr}; break;
    case IR_OP_THROW: inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[0]}; break;
    case IR_OP_INT32_COMPARE:
    case IR_OP_TAGGED_EQUALS: inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT; break;
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY:
      if (op->binary == IR_BINARY_MUL) break;
      inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      if (op->binary == IR_BINARY_MOD) constraints.temps = 1;
      break;
    case IR_OP_UINT32_SHIFT_RIGHT: inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT; break;
    case IR_OP_CONCATENATE_STRINGS: constraints.temps = 5; break;
    case IR_OP_STRINGS_EQUAL: constraints.temps = 1; break;
    case IR_OP_STRING_ADDRESS: constraints.temps = 1; break;
    case IR_OP_INITIALIZE_FRAME:
    case IR_OP_ENSURE_FRAME_INITIALIZED: constraints.temps = 2; break;
    case IR_OP_STORE_SLOT: constraints.temps = 1; break;
    case IR_OP_ADD_NAMED:
    case IR_OP_CHECK_ACCESSOR_FUNCTION: constraints.temps = 1; break;
    case IR_OP_HAS_IN_PROTOTYPE_CHAIN: constraints.temps = 1; break;
    case IR_OP_STORE_GLOBAL_BINDING: constraints.temps = 1; break;
    case IR_OP_NEW_THIS:
      inputs[0] = (ra_input_constraint_t){RA_INPUT_FIXED_REGISTER, registers->argument_gprs[1]};
      ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_LOAD_UPVALUE_CELL:
      if (count) inputs[0].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      break;
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_NATIVE:
    case IR_OP_CALL_CONSTRUCT:
      if (op->call.stores_operands) {
        ra_all_inputs(&constraints, count, RA_INPUT_ANYWHERE);
        ra_fixed_output(&constraints, registers->return_gpr);
      } else if (count) {
        constraints.temps = OPT_DIRECT_CALL_TEMPS;
        if (node->repr != OPT_REPR_NONE) ra_fixed_output(&constraints, registers->return_gpr);
      }
      break;
    case IR_OP_PROBE_PROPERTY_CACHE:
    case IR_OP_PROBE_KEYED_CACHE: constraints.temps = 3; break;
    case IR_OP_PROBE_KEYED_STORE:
    case IR_OP_PROBE_PROPERTY_STORE: constraints.temps = 4; break;
    case IR_OP_PROBE_GLOBAL_CACHE:
    case IR_OP_PROBE_GLOBAL_STORE: constraints.temps = 4; break;
    case IR_OP_PROBE_HAS_PROPERTY: constraints.temps = 3; break;
    case IR_OP_BRANCH:
      switch (op->branch.condition.kind) {
        case IR_COND_INT32:
        case IR_COND_TAGGED_EQUALS: inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT; break;
        case IR_COND_ELEMENTS_KIND:
        case IR_COND_ITERABLE_ARRAY: constraints.temps = 1; break;
        case IR_COND_PROPERTY_ITERATOR_CACHE_VALID:
        case IR_COND_TYPED_ARRAY_LENGTH: constraints.temps = 3; break;
        case IR_COND_BINDING_MUTABLE:
        case IR_COND_NEXT_BINDING_OF_SHAPE: constraints.temps = 2; break;
        case IR_COND_BUILTIN: constraints.temps = 1; break;
        default: break;
      }
      break;
    case IR_OP_CALL_SLOW_PATH:
      ra_all_inputs(&constraints, count, RA_INPUT_ANYWHERE);
      if (!op->slow.saves_registers && node->repr != OPT_REPR_NONE) ra_fixed_output(&constraints, registers->return_gpr);
      break;
    case IR_OP_SLOW_PATH_OUTPUT: inputs[0].kind = RA_INPUT_ANYWHERE; break;
    case IR_OP_STORE_ENVIRONMENT_BINDING:
    case IR_OP_APPEND_ENVIRONMENT_BINDING: constraints.temps = 1; break;
    case IR_OP_CHECK_APPENDABLE_ARRAY: constraints.temps = 2; break;
    case IR_OP_STRING_BUILDER_APPEND:
      inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      constraints.temps = 4;
      break;
    case IR_OP_LOAD_ELEMENTS_CAPACITY:
    case IR_OP_APPEND_ELEMENT:
    case IR_OP_POP_ELEMENT: constraints.temps = 1; break;
    case IR_OP_CHECK_ELEMENTS:
    case IR_OP_CHECK_TYPED_ARRAY:
    case IR_OP_CHECK_IDENTITY_COMPARABLE:
    case IR_OP_CHECK_CLOSURE:
    case IR_OP_CHECK_UPVALUE_INITIALIZED: constraints.temps = 1; break;
    case IR_OP_LOAD_ELEMENT_AT:
    case IR_OP_STORE_ELEMENT_AT:
      inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      constraints.temps = 1;
      break;
    case IR_OP_CHECK_BOUNDS: inputs[0].kind = RA_INPUT_REGISTER_OR_CONSTANT; break;
    case IR_OP_ALLOCATE_OBJECT:
      if (count) inputs[0].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      constraints.temps = 2;
      break;
    case IR_OP_INITIALIZE_NAMED: constraints.temps = 1; break;
    case IR_OP_ALLOCATE_ARRAY: constraints.temps = 3; break;
    case IR_OP_ALLOCATE_FUNCTION:
    case IR_OP_ALLOCATE_ENVIRONMENT: constraints.temps = 2; break;
    case IR_OP_INITIALIZE_ELEMENT:
      inputs[1].kind = RA_INPUT_REGISTER_OR_CONSTANT;
      constraints.temps = 1;
      break;
    case IR_OP_STORE_UPVALUE:
    case IR_OP_CAN_ADD_NAMED: constraints.temps = 1; break;
    default: break;
  }
  return constraints;
}

typedef struct {
  const ir_graph_t *graph;
  const opt_register_set_t *registers;
  ra_liveness_t liveness;
  ra_allocation_t *allocation;
  opt_arena_t *scratch;
  ir_node_id_t contents[OPT_REGISTER_COUNT];
  uint8_t *value_registers;
  ir_node_id_t **end_contents;
  uint8_t *hints;
  uint32_t position;
  ra_phi_inputs_t phi_inputs;
  uint32_t *slot_classes;
  uint32_t *class_slots;
  ir_slot_values_t frame_values;
} ra_allocator_t;

static uint32_t ra_new_spill_slot(ra_allocator_t *allocator) {
  return allocator->allocation->spill_slot_count++;
}

static uint32_t ra_ensure_spill_slot(ra_allocator_t *allocator, ir_node_id_t value) {
  ra_node_allocation_t *node = &allocator->allocation->nodes[value];
  if (node->spill_slot != IR_NONE) return node->spill_slot;
  uint32_t *slot = &allocator->class_slots[allocator->slot_classes[value]];
  if (*slot == IR_NONE) *slot = ra_new_spill_slot(allocator);
  node->spill_slot = *slot;
  return node->spill_slot;
}

static void ra_place_phi(ra_allocator_t *allocator, ir_node_id_t phi, uint8_t reg) {
  uint32_t slot = IR_NONE;
  bool needs_slot = reg == RA_NO_REGISTER || opt_bitset_contains(&allocator->liveness.needs_spill, phi)
    || allocator->allocation->nodes[phi].spill_slot != IR_NONE;
  if (needs_slot) slot = ra_ensure_spill_slot(allocator, phi);
  ra_node_allocation_t *node = &allocator->allocation->nodes[phi];
  node->has_output = true;
  node->output = reg != RA_NO_REGISTER ? ra_register(reg) : ra_stack(slot);
  node->spill_slot = slot;
}

static void ra_set_contents(ra_allocator_t *allocator, const ir_node_id_t *contents) {
  for (uint32_t reg = 0; reg < OPT_REGISTER_COUNT; reg++)
    if (allocator->contents[reg] != IR_NONE) allocator->value_registers[allocator->contents[reg]] = RA_NO_REGISTER;
  memcpy(allocator->contents, contents, sizeof allocator->contents);
  for (uint32_t reg = 0; reg < OPT_REGISTER_COUNT; reg++)
    if (allocator->contents[reg] != IR_NONE) allocator->value_registers[allocator->contents[reg]] = (uint8_t)reg;
}

static void ra_contents_entering(const ra_allocator_t *allocator, ir_block_id_t predecessor, ir_block_id_t block, ir_node_id_t *contents) {
  const opt_bitset_t *live_in = &allocator->liveness.live_in[block];
  const ir_node_id_t *end = allocator->end_contents[predecessor];
  assert(end && "blocks come after the predecessor whose state they start with");
  for (uint32_t reg = 0; reg < OPT_REGISTER_COUNT; reg++)
    contents[reg] = end[reg] != IR_NONE && opt_bitset_contains(live_in, end[reg]) ? end[reg] : IR_NONE;
}

static bool ra_merge_contents(const ra_allocator_t *allocator, ir_block_id_t block, ir_node_id_t *contents) {
  ir_block_id_t predecessor = ra_state_predecessor(allocator->graph, block);
  if (predecessor == IR_NONE) return false;
  ra_contents_entering(allocator, predecessor, block, contents);
  const ir_node_vec_t *phis = &ir_block(allocator->graph, block)->phis;
  for (uint32_t i = 0; i < phis->len; i++) {
    const ra_node_allocation_t *phi = &allocator->allocation->nodes[phis->items[i]];
    if (phi->has_output && phi->output.kind == RA_LOC_REGISTER) {
      assert(contents[phi->output.reg] == IR_NONE);
      contents[phi->output.reg] = phis->items[i];
    }
  }
  return true;
}

static void ra_start_block(ra_allocator_t *allocator, ir_block_id_t block) {
  const ir_block_t *data = ir_block(allocator->graph, block);
  ir_node_id_t contents[OPT_REGISTER_COUNT];
  if (!ra_merge_contents(allocator, block, contents)) {
    if (data->predecessors.len == 1 && data->predecessors.items[0] < block) {
      ra_contents_entering(allocator, data->predecessors.items[0], block, contents);
    } else {
      for (uint32_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) contents[reg] = IR_NONE;
      for (uint32_t i = 0; i < data->phis.len; i++) {
        const ra_node_allocation_t *phi = &allocator->allocation->nodes[data->phis.items[i]];
        if (phi->has_output && phi->output.kind == RA_LOC_REGISTER) contents[phi->output.reg] = data->phis.items[i];
      }
    }
  }
  ra_set_contents(allocator, contents);
  if (ir_block_node_count(data)) allocator->position = allocator->liveness.positions[ir_block_node_at(data, 0)];
}

static void ra_free_register(ra_allocator_t *allocator, uint8_t reg) {
  ir_node_id_t value = allocator->contents[reg];
  if (value == IR_NONE) return;
  allocator->contents[reg] = IR_NONE;
  allocator->value_registers[value] = RA_NO_REGISTER;
}

static void ra_assign_register(ra_allocator_t *allocator, uint8_t reg, ir_node_id_t value) {
  assert(allocator->contents[reg] == IR_NONE);
  allocator->contents[reg] = value;
  allocator->value_registers[value] = reg;
}

static void ra_evict(ra_allocator_t *allocator, uint8_t reg) {
  ir_node_id_t value = allocator->contents[reg];
  if (value == IR_NONE) return;
  ra_ensure_spill_slot(allocator, value);
  ra_free_register(allocator, reg);
}

static uint32_t ra_next_use(const ra_allocator_t *allocator, ir_node_id_t value) {
  const ra_positions_t *uses = &allocator->liveness.use_positions[value];
  uint32_t low = 0, high = uses->len;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    if (uses->items[mid] <= allocator->position) low = mid + 1;
    else high = mid;
  }
  return low < uses->len ? uses->items[low] : UINT32_MAX;
}

static uint8_t ra_pick_register_from(ra_allocator_t *allocator, opt_register_mask_t blocked, opt_register_class_t cls, uint8_t preferred, bool high) {
  opt_register_mask_t candidates = opt_allocatable(allocator->registers, cls) & ~blocked;
  if (preferred != RA_NO_REGISTER && opt_mask_contains(candidates, preferred) && allocator->contents[preferred] == IR_NONE) return preferred;
  for (uint8_t i = 0; i < OPT_REGISTER_COUNT; i++) {
    uint8_t reg = high ? (uint8_t)(OPT_REGISTER_COUNT - 1 - i) : i;
    if (opt_mask_contains(candidates, reg) && allocator->contents[reg] == IR_NONE) return reg;
  }
  uint8_t victim = RA_NO_REGISTER;
  uint32_t furthest = 0;
  for (uint8_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) {
    if (!opt_mask_contains(candidates, reg)) continue;
    uint32_t next = ra_next_use(allocator, allocator->contents[reg]);
    if (victim == RA_NO_REGISTER || next > furthest) {
      victim = reg;
      furthest = next;
    }
  }
  if (victim == RA_NO_REGISTER) {
    fputs("ant: a node needs more registers than the target has\n", stderr);
    abort();
  }
  ra_evict(allocator, victim);
  return victim;
}

static uint8_t ra_pick_register(ra_allocator_t *allocator, opt_register_mask_t blocked, opt_register_class_t cls) {
  return ra_pick_register_from(allocator, blocked, cls, RA_NO_REGISTER, false);
}

static ra_location_t ra_current_location(const ra_allocator_t *allocator, ir_node_id_t value) {
  uint64_t bits;
  if (ir_constant_value(allocator->graph, value, &bits)) return ra_constant(bits);
  if (ir_node(allocator->graph, value)->op.code == IR_OP_VIRTUAL_OBJECT) return ra_constant(IR_VALUE_EMPTY);
  if (allocator->value_registers[value] != RA_NO_REGISTER) return ra_register(allocator->value_registers[value]);
  uint32_t slot = allocator->allocation->nodes[value].spill_slot;
  if (slot == IR_NONE) {
    fprintf(stderr, "ant: v%u is neither in a register nor spilled\n", value);
    abort();
  }
  return ra_stack(slot);
}

static bool ra_is_live_after(const ra_allocator_t *allocator, ir_node_id_t value, ir_node_id_t node) {
  return !ir_node_vec_contains(&allocator->liveness.dies_at[node], value);
}

static void ra_push_move(opt_arena_t *arena, ra_moves_t *moves, ra_location_t from, ra_location_t to) {
  ra_move_t move = {.from = from, .to = to};
  opt_vec_push(arena, moves, move);
}

static void ra_allocate_node(ra_allocator_t *allocator, ir_node_id_t node_id) {
  const ir_graph_t *graph = allocator->graph;
  opt_arena_t *arena = &allocator->allocation->arena;
  const ir_node_t *node = ir_node(graph, node_id);
  allocator->position = allocator->liveness.positions[node_id];
  ra_constraints_t constraints = ra_constraints(node, allocator->registers, allocator->scratch);
  ra_node_allocation_t result = {.spill_slot = allocator->allocation->nodes[node_id].spill_slot};
  opt_register_mask_t blocked = 0;
  uint32_t count = node->inputs.len;
  opt_vec_reserve(arena, &result.inputs, count);
  result.inputs.len = count;
  for (uint32_t i = 0; i < count; i++) result.inputs.items[i] = ra_register(0);

  for (uint32_t index = 0; index < count; index++) {
    if (constraints.inputs[index].kind != RA_INPUT_FIXED_REGISTER) continue;
    uint8_t reg = constraints.inputs[index].reg;
    ir_node_id_t input = node->inputs.items[index];
    bool is_value = ra_is_allocated_value(graph, input);
    if (!is_value || allocator->value_registers[input] != reg) {
      assert(!opt_mask_contains(blocked, reg) && "two inputs need the same register");
      ra_evict(allocator, reg);
      ra_location_t from = ra_current_location(allocator, input);
      ra_push_move(arena, &result.moves_before, from, ra_register(reg));
      if (from.kind == RA_LOC_REGISTER && !opt_mask_contains(blocked, from.reg)) {
        ra_free_register(allocator, from.reg);
        ra_assign_register(allocator, reg, input);
      } else if (from.kind == RA_LOC_STACK) {
        ra_assign_register(allocator, reg, input);
      }
    }
    blocked = opt_mask_with(blocked, reg);
    result.inputs.items[index] = ra_register(reg);
  }
  for (uint32_t index = 0; index < count; index++) {
    ra_input_kind_t kind = constraints.inputs[index].kind;
    ir_node_id_t input = node->inputs.items[index];
    uint64_t bits;
    if (kind == RA_INPUT_REGISTER_OR_CONSTANT && ir_constant_value(graph, input, &bits)) {
      result.inputs.items[index] = ra_constant(bits);
      continue;
    }
    if (kind == RA_INPUT_ANYWHERE) {
      result.inputs.items[index] = ra_current_location(allocator, input);
      continue;
    }
    if (kind == RA_INPUT_FIXED_REGISTER) continue;
    uint8_t reg = allocator->value_registers[input];
    if (reg == RA_NO_REGISTER || !ra_is_allocated_value(graph, input)) {
      bool value = ra_is_allocated_value(graph, input);
      reg = ra_pick_register_from(allocator, blocked, opt_register_class_of(ir_node(graph, input)->repr), RA_NO_REGISTER, !value);
      ra_push_move(arena, &result.moves_before, ra_current_location(allocator, input), ra_register(reg));
      if (ra_is_allocated_value(graph, input)) ra_assign_register(allocator, reg, input);
    }
    blocked = opt_mask_with(blocked, reg);
    result.inputs.items[index] = ra_register(reg);
  }

  for (uint32_t i = 0; i < constraints.temps; i++) {
    uint8_t reg = ra_pick_register(allocator, blocked, OPT_CLASS_GENERAL);
    blocked = opt_mask_with(blocked, reg);
    opt_vec_push(arena, &result.temps, reg);
  }

  opt_register_mask_t clobbered = 0;
  if (constraints.is_call) clobbered = opt_caller_saved(allocator->registers);
  else if (ra_saves_only_gprs(&node->op)) clobbered = opt_allocatable(allocator->registers, OPT_CLASS_FLOAT);
  for (uint8_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) {
    if (!opt_mask_contains(clobbered, reg)) continue;
    ir_node_id_t value = allocator->contents[reg];
    if (value == IR_NONE) continue;
    assert((allocator->allocation->nodes[value].spill_slot != IR_NONE || !ra_is_live_after(allocator, value, node_id))
      && "a value is live across a call that clobbers its register without a spill slot");
    ra_free_register(allocator, reg);
  }
  uint8_t output = RA_NO_REGISTER;
  uint8_t first_input = RA_NO_REGISTER;
  if (count && node->frame_state == IR_NONE && ra_reuses_first_input(&node->op) && result.inputs.items[0].kind == RA_LOC_REGISTER
    && ra_is_allocated_value(graph, node->inputs.items[0]) && ir_node_vec_contains(&allocator->liveness.dies_at[node_id], node->inputs.items[0])
    && opt_register_class_of_register(result.inputs.items[0].reg) == opt_register_class_of(node->repr)) {
    first_input = result.inputs.items[0].reg;
    for (uint32_t i = 0; i < result.temps.len; i++)
      if (result.temps.items[i] == first_input) first_input = RA_NO_REGISTER;
  }
  if (constraints.output == RA_OUTPUT_REGISTER && first_input != RA_NO_REGISTER) {
    output = first_input;
  } else if (constraints.output == RA_OUTPUT_REGISTER) {
    output = ra_pick_register_from(allocator, blocked, opt_register_class_of(node->repr), allocator->hints[node_id], false);
  } else if (constraints.output == RA_OUTPUT_FIXED_REGISTER) {
    output = constraints.output_reg;
    assert(constraints.is_call || !opt_mask_contains(blocked, output));
    ra_evict(allocator, output);
  }
  if (node->frame_state != IR_NONE) {
    ir_slot_values_t *values = &allocator->frame_values;
    opt_exit_table_t *exits = &allocator->allocation->exit_table;
    ir_frame_state_values(graph, node->frame_state, allocator->scratch, values);
    opt_exit_begin(exits);
    for (uint32_t i = 0; i < values->len; i++) {
      ir_node_id_t value = values->items[i].value;
      ra_location_t at = ra_current_location(allocator, value);
      opt_exit_push(exits, (opt_exit_value_t){
        .slot = values->items[i].slot, .kind = at.kind, .repr = ir_node(graph, value)->repr, .reg = at.reg,
        .bits = at.kind == RA_LOC_STACK ? at.slot : at.bits,
      });
    }
    result.exits = opt_exit_finish(exits);
  }

  const ir_node_vec_t *dies = &allocator->liveness.dies_at[node_id];
  for (uint32_t i = 0; i < dies->len; i++) {
    uint8_t reg = allocator->value_registers[dies->items[i]];
    if (reg != RA_NO_REGISTER) ra_free_register(allocator, reg);
  }

  if (output != RA_NO_REGISTER) {
    ra_assign_register(allocator, output, node_id);
    if (opt_bitset_contains(&allocator->liveness.needs_spill, node_id)) ra_ensure_spill_slot(allocator, node_id);
    if (opt_bitset_contains(&allocator->liveness.dead_outputs, node_id)) ra_free_register(allocator, output);
  }

  result.spill_slot = allocator->allocation->nodes[node_id].spill_slot;
  result.has_output = output != RA_NO_REGISTER;
  if (result.has_output) result.output = ra_register(output);
  allocator->allocation->nodes[node_id] = result;
}

static void ra_place_merge_phis(ra_allocator_t *allocator, ir_block_id_t predecessor, ir_block_id_t join) {
  const ir_graph_t *graph = allocator->graph;
  const opt_bitset_t *live_in = &allocator->liveness.live_in[join];
  opt_register_mask_t taken = 0;
  for (uint8_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) {
    ir_node_id_t value = allocator->contents[reg];
    if (value != IR_NONE && opt_bitset_contains(live_in, value)) taken = opt_mask_with(taken, reg);
  }
  ra_phi_inputs_from(graph, predecessor, join, allocator->scratch, &allocator->phi_inputs);
  for (uint32_t i = 0; i < allocator->phi_inputs.len; i++) {
    ir_node_id_t phi = allocator->phi_inputs.items[i].phi;
    ir_node_id_t input = allocator->phi_inputs.items[i].input;
    uint8_t reg = RA_NO_REGISTER;
    uint8_t own = allocator->value_registers[input];
    if (own != RA_NO_REGISTER && ra_is_allocated_value(graph, input) && !opt_mask_contains(taken, own)) reg = own;
    if (reg == RA_NO_REGISTER) {
      opt_register_mask_t free = opt_allocatable(allocator->registers, opt_register_class_of(ir_node(graph, phi)->repr)) & ~taken;
      uint8_t first = RA_NO_REGISTER;
      for (uint8_t candidate = 0; candidate < OPT_REGISTER_COUNT; candidate++) {
        if (!opt_mask_contains(free, candidate)) continue;
        if (first == RA_NO_REGISTER) first = candidate;
        if (allocator->contents[candidate] == IR_NONE) {
          reg = candidate;
          break;
        }
      }
      if (reg == RA_NO_REGISTER) reg = first;
    }
    ra_place_phi(allocator, phi, reg);
    if (reg != RA_NO_REGISTER) taken = opt_mask_with(taken, reg);
  }
}

static void ra_merge_moves(ra_allocator_t *allocator, ir_block_id_t block, ir_block_id_t join, ra_moves_t *moves) {
  ir_node_id_t contents[OPT_REGISTER_COUNT];
  [[maybe_unused]] bool merges = ra_merge_contents(allocator, join, contents);
  assert(merges && "the merge has a state predecessor");
  const ir_node_vec_t *phis = &ir_block(allocator->graph, join)->phis;
  for (uint8_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) {
    ir_node_id_t value = contents[reg];
    if (value == IR_NONE || ir_node_vec_contains(phis, value)) continue;
    ra_location_t from = ra_current_location(allocator, value);
    if (!ra_location_equal(from, ra_register(reg))) ra_push_move(allocator->scratch, moves, from, ra_register(reg));
  }
  ra_phi_inputs_from(allocator->graph, block, join, allocator->scratch, &allocator->phi_inputs);
  for (uint32_t i = 0; i < allocator->phi_inputs.len; i++) {
    const ra_node_allocation_t *phi = &allocator->allocation->nodes[allocator->phi_inputs.items[i].phi];
    assert(phi->has_output && "phis have locations");
    ra_push_move(allocator->scratch, moves, ra_current_location(allocator, allocator->phi_inputs.items[i].input), phi->output);
  }
}

static void ra_phi_moves(ra_allocator_t *allocator, ir_block_id_t block, uint32_t *cycle_temp, ra_moves_t *result) {
  const ir_graph_t *graph = allocator->graph;
  ir_successors_t successors = ir_block_successors(graph, block);
  result->len = 0;
  if (successors.len != 1) {
    for (uint32_t i = 0; i < successors.len; i++)
      assert(!ir_block(graph, ir_successor_at(&successors, i))->phis.len && "critical edge into a phi");
    return;
  }
  ir_block_id_t successor = ir_successor_at(&successors, 0);
  ra_moves_t moves = {0};
  ir_block_id_t predecessor = ra_state_predecessor(graph, successor);
  if (predecessor != IR_NONE && predecessor != block) {
    ra_merge_moves(allocator, block, successor, &moves);
  } else {
    if (predecessor != IR_NONE) ra_place_merge_phis(allocator, block, successor);
    ra_phi_inputs_from(graph, block, successor, allocator->scratch, &allocator->phi_inputs);
    for (uint32_t i = 0; i < allocator->phi_inputs.len; i++) {
      const ra_node_allocation_t *phi = &allocator->allocation->nodes[allocator->phi_inputs.items[i].phi];
      assert(phi->has_output && "phis have locations");
      ra_push_move(allocator->scratch, &moves, ra_current_location(allocator, allocator->phi_inputs.items[i].input), phi->output);
    }
  }
  if (!moves.len) return;
  if (*cycle_temp == IR_NONE) *cycle_temp = ra_new_spill_slot(allocator);
  ra_resolve_parallel_moves(moves.items, moves.len, ra_stack(*cycle_temp), &allocator->allocation->arena, result);
}

void ra_allocate(const ir_graph_t *graph, const opt_register_set_t *registers, ra_allocation_t *allocation) {
  assert(!(registers->allocatable_gprs >> OPT_FPR_BASE));
  assert(!opt_mask_contains(registers->allocatable_gprs, registers->scratch_gpr));
  uint32_t node_count = ir_node_count(graph);
  uint32_t block_count = ir_block_count(graph);
  *allocation = (ra_allocation_t){.node_count = node_count, .block_count = block_count};
  opt_arena_init(&allocation->arena);
  opt_exit_table_init(&allocation->exit_table, &allocation->arena);
  allocation->nodes = opt_arena_new(&allocation->arena, ra_node_allocation_t, node_count + 1);
  for (uint32_t i = 0; i < node_count; i++) allocation->nodes[i].spill_slot = IR_NONE;
  allocation->block_end_moves = opt_arena_new(&allocation->arena, ra_moves_t, block_count + 1);

  opt_arena_t scratch;
  opt_arena_init(&scratch);
  ra_allocator_t allocator = {
    .graph = graph,
    .registers = registers,
    .allocation = allocation,
    .scratch = &scratch,
    .value_registers = opt_arena_alloc(&scratch, node_count + 1),
    .end_contents = opt_arena_new(&scratch, ir_node_id_t *, block_count + 1),
  };
  ra_liveness_compute(&allocator.liveness, graph, &allocation->arena);
  allocator.slot_classes = ra_spill_slot_classes(graph, &allocator.liveness, &scratch);
  allocator.class_slots = opt_arena_new(&scratch, uint32_t, node_count + 1);
  for (uint32_t i = 0; i < node_count; i++) allocator.class_slots[i] = IR_NONE;
  memset(allocator.value_registers, RA_NO_REGISTER, node_count + 1);
  for (uint32_t reg = 0; reg < OPT_REGISTER_COUNT; reg++) allocator.contents[reg] = IR_NONE;

  for (ir_block_id_t index = 0; index < block_count; index++) {
    if (ra_state_predecessor(graph, index) != IR_NONE) continue;
    const ir_block_t *block = ir_block(graph, index);
    bool is_loop_header = false;
    for (uint32_t i = 0; i < block->predecessors.len; i++) is_loop_header |= block->predecessors.items[i] >= index;
    opt_register_mask_t general = opt_allocatable(registers, OPT_CLASS_GENERAL);
    opt_register_mask_t floats = opt_allocatable(registers, OPT_CLASS_FLOAT);
    for (uint32_t i = 0; i < block->phis.len; i++) {
      ir_node_id_t phi = block->phis.items[i];
      opt_register_mask_t *pool = opt_register_class_of(ir_node(graph, phi)->repr) == OPT_CLASS_FLOAT ? &floats : &general;
      uint8_t reg = RA_NO_REGISTER;
      if (is_loop_header && *pool) {
        reg = (uint8_t)__builtin_ctzll(*pool);
        *pool &= *pool - 1;
      }
      ra_place_phi(&allocator, phi, reg);
    }
  }

  allocator.hints = opt_arena_alloc(&scratch, node_count + 1);
  memset(allocator.hints, RA_NO_REGISTER, node_count + 1);
  for (ir_block_id_t index = 0; index < block_count; index++) {
    const ir_block_t *block = ir_block(graph, index);
    for (uint32_t p = 0; p < block->predecessors.len; p++) {
      if (block->predecessors.items[p] < index) continue;
      for (uint32_t i = 0; i < block->phis.len; i++) {
        const ra_node_allocation_t *phi = &allocation->nodes[block->phis.items[i]];
        ir_node_id_t input = ir_node(graph, block->phis.items[i])->inputs.items[p];
        if (phi->has_output && phi->output.kind == RA_LOC_REGISTER && ra_is_allocated_value(graph, input)
          && allocator.hints[input] == RA_NO_REGISTER)
          allocator.hints[input] = phi->output.reg;
      }
    }
  }

  for (ir_block_id_t index = 0; index < block_count; index++) {
    const ir_block_t *block = ir_block(graph, index);
    bool back_edge = false;
    for (uint32_t i = 0; i < block->predecessors.len; i++) back_edge |= block->predecessors.items[i] >= index;
    if (!back_edge || ra_state_predecessor(graph, index) != IR_NONE) continue;
    opt_bitset_for_each(&allocator.liveness.live_in[index], value) ra_ensure_spill_slot(&allocator, value);
  }

  uint32_t cycle_temp = IR_NONE;
  for (ir_block_id_t block = 0; block < block_count; block++) {
    ra_start_block(&allocator, block);
    const ir_node_vec_t *body = &ir_block(graph, block)->body;
    for (uint32_t i = 0; i < body->len; i++) ra_allocate_node(&allocator, body->items[i]);
    ra_phi_moves(&allocator, block, &cycle_temp, &allocation->block_end_moves[block]);
    ra_allocate_node(&allocator, ir_control_id(graph, block));
    allocator.end_contents[block] = opt_arena_dup(&scratch, allocator.contents, sizeof allocator.contents);
  }
  allocation->live_in = allocator.liveness.live_in;
  opt_arena_free(&scratch);
}

uint32_t ra_phi_spill_store(const ir_graph_t *graph, const ra_allocation_t *allocation, ir_node_id_t phi) {
  const ra_node_allocation_t *node = &allocation->nodes[phi];
  if (!node->has_output || node->output.kind != RA_LOC_REGISTER || node->spill_slot == IR_NONE) return IR_NONE;
  const ir_node_vec_t *inputs = &ir_node(graph, phi)->inputs;
  for (uint32_t i = 0; i < inputs->len; i++)
    if (allocation->nodes[inputs->items[i]].spill_slot != node->spill_slot) return node->spill_slot;
  return IR_NONE;
}

void ra_allocation_free(ra_allocation_t *allocation) {
  opt_arena_free(&allocation->arena);
  *allocation = (ra_allocation_t){0};
}

void ra_allocation_clone(ra_allocation_t *dst, const ra_allocation_t *src) {
  *dst = *src;
  opt_arena_init(&dst->arena);
  opt_arena_t *arena = &dst->arena;
  dst->nodes = opt_arena_dup(arena, src->nodes, (src->node_count + 1) * sizeof *src->nodes);
  for (uint32_t i = 0; i < src->node_count; i++) {
    ra_node_allocation_t *node = &dst->nodes[i];
    const ra_node_allocation_t *original = &src->nodes[i];
    opt_vec_clone(arena, &node->moves_before, &original->moves_before);
    opt_vec_clone(arena, &node->inputs, &original->inputs);
    opt_vec_clone(arena, &node->temps, &original->temps);
  }
  opt_exit_table_clone(&dst->exit_table, &src->exit_table, arena);
  dst->block_end_moves = opt_arena_dup(arena, src->block_end_moves, (src->block_count + 1) * sizeof *src->block_end_moves);
  for (uint32_t i = 0; i < src->block_count; i++) opt_vec_clone(arena, &dst->block_end_moves[i], &src->block_end_moves[i]);
  dst->live_in = opt_arena_new(arena, opt_bitset_t, src->block_count + 1);
  for (uint32_t i = 0; i < src->block_count; i++) dst->live_in[i] = opt_bitset_clone(arena, &src->live_in[i]);
}

void ra_exit_values(const ra_allocation_t *allocation, ir_node_id_t node, opt_arena_t *arena, ra_exit_values_t *out) {
  out->len = 0;
  opt_exit_cursor_t cursor = opt_exit_cursor(opt_exit_table_view(&allocation->exit_table), allocation->nodes[node].exits);
  for (const opt_exit_value_t *value; (value = opt_exit_next(&cursor));) {
    ra_location_t location = value->kind == RA_LOC_REGISTER ? ra_register(value->reg)
      : value->kind == RA_LOC_STACK ? ra_stack((uint32_t)value->bits) : ra_constant(value->bits);
    opt_vec_push(arena, out, ((ra_exit_value_t){.slot = value->slot, .location = location}));
  }
}

void ra_location_text(ra_location_t location, opt_text_t *out) {
  char text[64];
  switch (location.kind) {
    case RA_LOC_REGISTER:
      if (location.reg >= OPT_FPR_BASE) opt_text_appendf(out, "f%u", location.reg - OPT_FPR_BASE);
      else opt_text_appendf(out, "r%u", location.reg);
      break;
    case RA_LOC_STACK: opt_text_appendf(out, "s%u", location.slot); break;
    case RA_LOC_CONSTANT:
      ir_value_describe(location.bits, text, sizeof text);
      opt_text_appendf(out, "#%s", text);
      break;
  }
}

static void ra_moves_text(const ra_moves_t *moves, opt_text_t *out) {
  for (uint32_t i = 0; i < moves->len; i++) {
    if (i) opt_text_append(out, ", ");
    ra_location_text(moves->items[i].from, out);
    opt_text_append(out, " -> ");
    ra_location_text(moves->items[i].to, out);
  }
}

void ra_dump_allocation(const ir_graph_t *graph, const ra_allocation_t *allocation, opt_text_t *out) {
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    const ir_block_t *block = ir_block(graph, index);
    opt_text_appendf(out, "b%u:\n", index);
    for (uint32_t n = 0, count = ir_block_node_count(block); n < count; n++) {
      ir_node_id_t node_id = ir_block_node_at(block, n);
      const ra_node_allocation_t *node = &allocation->nodes[node_id];
      if (node_id == block->control && allocation->block_end_moves[index].len) {
        opt_text_append(out, "  moves ");
        ra_moves_text(&allocation->block_end_moves[index], out);
        opt_text_append(out, "\n");
      }
      if (node->moves_before.len) {
        opt_text_append(out, "  moves ");
        ra_moves_text(&node->moves_before, out);
        opt_text_append(out, "\n");
      }
      opt_text_appendf(out, "  v%u %s", node_id, ir_op_name(ir_node(graph, node_id)->op.code));
      if (node->inputs.len) {
        opt_text_append(out, " (");
        for (uint32_t i = 0; i < node->inputs.len; i++) {
          if (i) opt_text_append(out, ", ");
          ra_location_text(node->inputs.items[i], out);
        }
        opt_text_append(out, ")");
      }
      if (node->has_output) {
        opt_text_append(out, " -> ");
        ra_location_text(node->output, out);
      }
      if (node->temps.len) {
        opt_text_append(out, " temps ");
        for (uint32_t i = 0; i < node->temps.len; i++) opt_text_appendf(out, "%sr%u", i ? ", " : "", node->temps.items[i]);
      }
      if (node->spill_slot != IR_NONE && ir_node(graph, node_id)->op.code != IR_OP_PHI) opt_text_appendf(out, " spill s%u", node->spill_slot);
      if (node->exits.count) {
        opt_arena_t scratch;
        opt_arena_init(&scratch);
        ra_exit_values_t values = {0};
        ra_exit_values(allocation, node_id, &scratch, &values);
        opt_text_append(out, " exit {");
        for (uint32_t i = 0; i < values.len; i++) {
          opt_text_appendf(out, "%s%u=", i ? ", " : "", values.items[i].slot);
          ra_location_text(values.items[i].location, out);
        }
        opt_text_append(out, "}");
        opt_arena_free(&scratch);
      }
      opt_text_append(out, "\n");
    }
  }
  opt_text_appendf(out, "spill slots: %u\n", allocation->spill_slot_count);
}
