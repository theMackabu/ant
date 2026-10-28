#include "ra_internal.h"
#include "../ir/ir_value.h"

#include <stdio.h>

typedef enum : uint8_t {
  RA_HOLDS_NOTHING,
  RA_HOLDS_VALUE,
  RA_HOLDS_CONSTANT,
} ra_content_kind_t;

typedef struct {
  ra_content_kind_t kind;
  uint64_t value;
} ra_content_t;

static inline bool ra_content_equal(ra_content_t a, ra_content_t b) {
  return a.kind == b.kind && (a.kind == RA_HOLDS_NOTHING || a.value == b.value);
}

typedef struct {
  ra_content_t registers[OPT_REGISTER_COUNT];
  ra_content_t *slots;
} ra_state_t;

typedef struct {
  const ir_graph_t *graph;
  const opt_register_set_t *registers;
  const ra_allocation_t *allocation;
  opt_arena_t *arena;
  uint32_t slot_count;
  bool checking;
  opt_text_t *out;
  uint32_t errors;
  ra_phi_inputs_t phi_inputs;
} ra_verifier_t;

[[gnu::format(printf, 2, 3)]] static void ra_error(ra_verifier_t *verifier, const char *format, ...) {
  if (!verifier->checking) return;
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  if (verifier->out) {
    if (verifier->errors) opt_text_append(verifier->out, "\n");
    opt_text_append(verifier->out, buffer);
  }
  verifier->errors++;
}

static ra_state_t *ra_state_new(ra_verifier_t *verifier) {
  ra_state_t *state = opt_arena_new(verifier->arena, ra_state_t, 1);
  state->slots = opt_arena_new(verifier->arena, ra_content_t, verifier->slot_count + 1);
  return state;
}

static ra_state_t *ra_state_clone(ra_verifier_t *verifier, const ra_state_t *state) {
  ra_state_t *copy = ra_state_new(verifier);
  memcpy(copy->registers, state->registers, sizeof copy->registers);
  memcpy(copy->slots, state->slots, (verifier->slot_count + 1) * sizeof *copy->slots);
  return copy;
}

static bool ra_state_equal(const ra_verifier_t *verifier, const ra_state_t *a, const ra_state_t *b) {
  for (uint32_t i = 0; i < OPT_REGISTER_COUNT; i++)
    if (!ra_content_equal(a->registers[i], b->registers[i])) return false;
  for (uint32_t i = 0; i < verifier->slot_count; i++)
    if (!ra_content_equal(a->slots[i], b->slots[i])) return false;
  return true;
}

static ra_content_t *ra_state_at(const ra_verifier_t *verifier, ra_state_t *state, ra_location_t location) {
  if (location.kind == RA_LOC_REGISTER) return location.reg < OPT_REGISTER_COUNT ? &state->registers[location.reg] : NULL;
  if (location.kind == RA_LOC_STACK) return location.slot < verifier->slot_count ? &state->slots[location.slot] : NULL;
  return NULL;
}

static ra_content_t ra_read(const ra_verifier_t *verifier, ra_state_t *state, ra_location_t location) {
  if (location.kind == RA_LOC_CONSTANT) return (ra_content_t){RA_HOLDS_CONSTANT, location.bits};
  ra_content_t *content = ra_state_at(verifier, state, location);
  return content ? *content : (ra_content_t){0};
}

static void ra_write(const ra_verifier_t *verifier, ra_state_t *state, ra_location_t location, ra_content_t content) {
  ra_content_t *slot = ra_state_at(verifier, state, location);
  if (slot) *slot = content;
}

static ra_content_t ra_content_of_value(const ra_verifier_t *verifier, ir_node_id_t value) {
  if (ir_node(verifier->graph, value)->op.code == IR_OP_VIRTUAL_OBJECT) return (ra_content_t){RA_HOLDS_CONSTANT, IR_VALUE_EMPTY};
  uint64_t bits;
  if (ir_constant_value(verifier->graph, value, &bits)) return (ra_content_t){RA_HOLDS_CONSTANT, bits};
  return (ra_content_t){RA_HOLDS_VALUE, value};
}

static void ra_content_text(ra_content_t content, opt_text_t *out) {
  char text[64];
  switch (content.kind) {
    case RA_HOLDS_NOTHING: opt_text_append(out, "nothing"); break;
    case RA_HOLDS_VALUE: opt_text_appendf(out, "v%u", (uint32_t)content.value); break;
    case RA_HOLDS_CONSTANT:
      ir_value_describe(content.value, text, sizeof text);
      opt_text_appendf(out, "#%s", text);
      break;
  }
}

static uint32_t ra_phi_locations(const ra_verifier_t *verifier, ir_node_id_t phi, ra_location_t *locations) {
  const ra_node_allocation_t *allocation = &verifier->allocation->nodes[phi];
  locations[0] = allocation->output;
  if (allocation->has_output && allocation->output.kind == RA_LOC_REGISTER && allocation->spill_slot != IR_NONE
    && ra_phi_spill_store(verifier->graph, verifier->allocation, phi) == IR_NONE) {
    locations[1] = ra_stack(allocation->spill_slot);
    return 2;
  }
  return 1;
}

static ra_state_t *ra_entry_state(ra_verifier_t *verifier, ir_block_id_t block, ra_state_t *const *exit_states) {
  const ir_block_t *data = ir_block(verifier->graph, block);
  if (block == 0 || !data->predecessors.len) return ra_state_new(verifier);
  ra_state_t *result = NULL;
  for (uint32_t p = 0; p < data->predecessors.len; p++) {
    ir_block_id_t predecessor = data->predecessors.items[p];
    if (!exit_states[predecessor]) continue;
    ra_state_t *state = ra_state_clone(verifier, exit_states[predecessor]);
    ra_phi_inputs_from(verifier->graph, predecessor, block, verifier->arena, &verifier->phi_inputs);
    for (uint32_t i = 0; i < verifier->phi_inputs.len; i++) {
      ir_node_id_t phi = verifier->phi_inputs.items[i].phi;
      ra_location_t locations[2];
      uint32_t count = ra_phi_locations(verifier, phi, locations);
      for (uint32_t l = 0; l < count; l++) {
        if (ra_content_equal(ra_read(verifier, state, locations[l]), ra_content_of_value(verifier, verifier->phi_inputs.items[i].input)))
          ra_write(verifier, state, locations[l], (ra_content_t){RA_HOLDS_VALUE, phi});
        else
          ra_write(verifier, state, locations[l], (ra_content_t){0});
      }
    }
    if (!result) {
      result = state;
      continue;
    }
    for (uint32_t i = 0; i < OPT_REGISTER_COUNT; i++)
      if (!ra_content_equal(result->registers[i], state->registers[i])) result->registers[i] = (ra_content_t){0};
    for (uint32_t i = 0; i < verifier->slot_count; i++)
      if (!ra_content_equal(result->slots[i], state->slots[i])) result->slots[i] = (ra_content_t){0};
  }
  return result;
}

static void ra_check_phi_inputs(ra_verifier_t *verifier, ir_block_id_t block, ra_state_t *const *exit_states) {
  const ir_block_vec_t *predecessors = &ir_block(verifier->graph, block)->predecessors;
  for (uint32_t p = 0; p < predecessors->len; p++) {
    ir_block_id_t predecessor = predecessors->items[p];
    if (!exit_states[predecessor]) continue;
    ra_phi_inputs_from(verifier->graph, predecessor, block, verifier->arena, &verifier->phi_inputs);
    for (uint32_t i = 0; i < verifier->phi_inputs.len; i++) {
      ir_node_id_t phi = verifier->phi_inputs.items[i].phi;
      ra_location_t locations[2];
      uint32_t count = ra_phi_locations(verifier, phi, locations);
      for (uint32_t l = 0; l < count; l++) {
        ra_content_t found = ra_read(verifier, exit_states[predecessor], locations[l]);
        ra_content_t expected = ra_content_of_value(verifier, verifier->phi_inputs.items[i].input);
        if (ra_content_equal(found, expected)) continue;
        opt_text_t text = {0};
        ra_content_text(expected, &text);
        opt_text_append(&text, " in ");
        ra_location_text(locations[l], &text);
        opt_text_append(&text, ", found ");
        ra_content_text(found, &text);
        ra_error(verifier, "phi v%u in b%u: input from b%u should be %s", phi, block, predecessor, text.data);
        opt_text_free(&text);
      }
    }
  }
}

static void ra_check_location(ra_verifier_t *verifier, ra_location_t location, const char *context) {
  if (location.kind != RA_LOC_REGISTER) return;
  if (opt_mask_contains(opt_allocatable(verifier->registers, OPT_CLASS_GENERAL), location.reg)) return;
  if (opt_mask_contains(opt_allocatable(verifier->registers, OPT_CLASS_FLOAT), location.reg)) return;
  ra_error(verifier, "%s: r%u is not allocatable", context, location.reg);
}

static void ra_apply_moves(ra_verifier_t *verifier, ra_state_t *state, const ra_moves_t *moves, const char *context) {
  for (uint32_t i = 0; i < moves->len; i++) {
    ra_move_t move = moves->items[i];
    if (move.to.kind == RA_LOC_CONSTANT) {
      ra_error(verifier, "%s: move into a constant", context);
      continue;
    }
    ra_check_location(verifier, move.to, context);
    if (move.to.kind == RA_LOC_STACK && move.to.slot >= verifier->slot_count)
      ra_error(verifier, "%s: s%u is past the spill slots", context, move.to.slot);
    ra_content_t content = ra_read(verifier, state, move.from);
    if (content.kind == RA_HOLDS_NOTHING) {
      opt_text_t text = {0};
      ra_location_text(move.from, &text);
      ra_error(verifier, "%s: move reads %s, which holds nothing", context, text.data);
      opt_text_free(&text);
    }
    ra_write(verifier, state, move.to, content);
  }
}

static void ra_expect(ra_verifier_t *verifier, ra_state_t *state, ra_location_t location, ir_node_id_t value, const char *context) {
  ra_content_t expected = ra_content_of_value(verifier, value);
  ra_content_t found = ra_read(verifier, state, location);
  if (ra_content_equal(found, expected)) return;
  opt_text_t text = {0};
  ra_content_text(expected, &text);
  opt_text_append(&text, " in ");
  ra_location_text(location, &text);
  opt_text_append(&text, ", found ");
  ra_content_text(found, &text);
  ra_error(verifier, "%s: expected %s", context, text.data);
  opt_text_free(&text);
}

static void ra_check_exit_values(ra_verifier_t *verifier, ra_state_t *state, ir_node_id_t node, const char *context) {
  ir_frame_state_id_t frame_state = ir_node(verifier->graph, node)->frame_state;
  if (frame_state == IR_NONE) return;
  ir_slot_values_t expected = {0};
  ir_frame_state_values(verifier->graph, frame_state, verifier->arena, &expected);
  ra_exit_values_t decoded = {0};
  ra_exit_values(verifier->allocation, node, verifier->arena, &decoded);
  const ra_exit_values_t *exit_values = &decoded;
  if (expected.len != exit_values->len) {
    ra_error(verifier, "%s: exit has %u values, frame state %u", context, exit_values->len, expected.len);
    return;
  }
  char exit_context[256];
  for (uint32_t i = 0; i < expected.len; i++) {
    uint32_t slot = expected.items[i].slot;
    if (slot != exit_values->items[i].slot)
      ra_error(verifier, "%s: exit value for slot %u, frame state slot %u", context, exit_values->items[i].slot, slot);
    snprintf(exit_context, sizeof exit_context, "%s, exit value for slot %u", context, slot);
    ra_expect(verifier, state, exit_values->items[i].location, expected.items[i].value, exit_context);
  }
}

static void ra_simulate_node(ra_verifier_t *verifier, ra_state_t *state, ir_node_id_t node_id) {
  const ir_graph_t *graph = verifier->graph;
  const ir_node_t *node = ir_node(graph, node_id);
  const ra_node_allocation_t *allocation = &verifier->allocation->nodes[node_id];
  char context[32];
  snprintf(context, sizeof context, "v%u", node_id);

  ra_apply_moves(verifier, state, &allocation->moves_before, context);
  if (allocation->inputs.len != node->inputs.len) {
    ra_error(verifier, "%s: wrong number of input locations", context);
    return;
  }
  bool reads_inputs_anywhere = node->op.code == IR_OP_CALL_SLOW_PATH || node->op.code == IR_OP_SLOW_PATH_OUTPUT
    || ((node->op.code == IR_OP_CALL_DIRECT || node->op.code == IR_OP_CALL_NATIVE || node->op.code == IR_OP_CALL_CONSTRUCT)
      && node->op.call.stores_operands);
  char input_context[64];
  for (uint32_t index = 0; index < node->inputs.len; index++) {
    ir_node_id_t input = node->inputs.items[index];
    ra_location_t location = allocation->inputs.items[index];
    if (location.kind == RA_LOC_CONSTANT) {
      uint64_t bits;
      if (!ir_constant_value(graph, input, &bits) || bits != location.bits)
        ra_error(verifier, "%s: input %u is not the constant 0x%llx", context, index, (unsigned long long)location.bits);
      continue;
    }
    if (location.kind != RA_LOC_REGISTER && !reads_inputs_anywhere) ra_error(verifier, "%s: input %u is not in a register", context, index);
    ra_check_location(verifier, location, context);
    snprintf(input_context, sizeof input_context, "%s, input %u", context, index);
    ra_expect(verifier, state, location, input, input_context);
  }

  for (uint32_t i = 0; i < allocation->temps.len; i++) {
    uint8_t temp = allocation->temps.items[i];
    ra_check_location(verifier, ra_register(temp), context);
    for (uint32_t j = 0; j < allocation->inputs.len; j++)
      if (ra_location_equal(allocation->inputs.items[j], ra_register(temp))) {
        ra_error(verifier, "%s: temp r%u is also an input", context, temp);
        break;
      }
    state->registers[temp] = (ra_content_t){0};
  }
  opt_register_mask_t clobbered = 0;
  if (ra_clobbers_caller_saved(&node->op)) clobbered = opt_caller_saved(verifier->registers);
  else if (ra_saves_only_gprs(&node->op)) clobbered = opt_allocatable(verifier->registers, OPT_CLASS_FLOAT);
  for (uint8_t reg = 0; reg < OPT_REGISTER_COUNT; reg++)
    if (opt_mask_contains(clobbered, reg)) state->registers[reg] = (ra_content_t){0};
  if (allocation->has_output && allocation->output.kind == RA_LOC_REGISTER) {
    ra_state_t *without_output = ra_state_clone(verifier, state);
    without_output->registers[allocation->output.reg] = (ra_content_t){0};
    ra_check_exit_values(verifier, without_output, node_id, context);
  } else {
    ra_check_exit_values(verifier, state, node_id, context);
  }

  bool has_value = node->repr != OPT_REPR_NONE;
  if (has_value && allocation->has_output && allocation->output.kind == RA_LOC_REGISTER) {
    uint8_t reg = allocation->output.reg;
    ra_check_location(verifier, allocation->output, context);
    if (opt_register_class_of_register(reg) != opt_register_class_of(node->repr))
      ra_error(verifier, "%s: the output is in a register of the other class", context);
    state->registers[reg] = (ra_content_t){RA_HOLDS_VALUE, node_id};
    if (allocation->spill_slot != IR_NONE) ra_write(verifier, state, ra_stack(allocation->spill_slot), (ra_content_t){RA_HOLDS_VALUE, node_id});
  } else if (has_value || allocation->has_output) {
    opt_text_t text = {0};
    if (allocation->has_output) ra_location_text(allocation->output, &text);
    ra_error(verifier, "%s: unexpected output location %s", context, allocation->has_output ? text.data : "None");
    opt_text_free(&text);
  }
}

static ra_state_t *ra_simulate_block(ra_verifier_t *verifier, ir_block_id_t block, const ra_state_t *entry) {
  ra_state_t *state = ra_state_clone(verifier, entry);
  const ir_block_t *data = ir_block(verifier->graph, block);
  for (uint32_t i = 0; i < data->phis.len; i++) {
    uint32_t slot = ra_phi_spill_store(verifier->graph, verifier->allocation, data->phis.items[i]);
    if (slot != IR_NONE) ra_write(verifier, state, ra_stack(slot), (ra_content_t){RA_HOLDS_VALUE, data->phis.items[i]});
  }
  for (uint32_t i = 0; i < data->body.len; i++) ra_simulate_node(verifier, state, data->body.items[i]);
  char context[32];
  snprintf(context, sizeof context, "end of b%u", block);
  ra_apply_moves(verifier, state, &verifier->allocation->block_end_moves[block], context);
  ra_simulate_node(verifier, state, data->control);
  return state;
}

bool ra_verify(const ir_graph_t *graph, const opt_register_set_t *registers, const ra_allocation_t *allocation, opt_text_t *errors) {
  opt_arena_t arena;
  opt_arena_init(&arena);
  ra_verifier_t verifier = {
    .graph = graph,
    .registers = registers,
    .allocation = allocation,
    .arena = &arena,
    .slot_count = allocation->spill_slot_count,
    .out = errors,
  };
  uint32_t block_count = ir_block_count(graph);
  ra_state_t **entry_states = opt_arena_new(&arena, ra_state_t *, block_count + 1);
  ra_state_t **exit_states = opt_arena_new(&arena, ra_state_t *, block_count + 1);
  bool changed = true;
  while (changed) {
    changed = false;
    for (ir_block_id_t block = 0; block < block_count; block++) {
      ra_state_t *entry = ra_entry_state(&verifier, block, exit_states);
      if (!entry) continue;
      if (entry_states[block] && ra_state_equal(&verifier, entry_states[block], entry) && exit_states[block]) continue;
      exit_states[block] = ra_simulate_block(&verifier, block, entry);
      entry_states[block] = entry;
      changed = true;
    }
  }

  verifier.checking = true;
  for (ir_block_id_t block = 0; block < block_count; block++) {
    if (!entry_states[block]) {
      ra_error(&verifier, "b%u is unreachable", block);
      continue;
    }
    ra_check_phi_inputs(&verifier, block, exit_states);
    ra_simulate_block(&verifier, block, entry_states[block]);
  }
  opt_arena_free(&arena);
  return verifier.errors == 0;
}
