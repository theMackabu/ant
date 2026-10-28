#include "ra_internal.h"

static constexpr uint32_t RA_MAX_CLASS_SIZE = 32;

typedef struct {
  ir_block_id_t block;
  uint32_t position;
  bool is_phi;
} ra_definition_t;

typedef struct {
  ir_block_id_t block;
  uint32_t position;
} ra_death_t;

typedef OPT_VEC(ra_death_t) ra_deaths_t;

typedef struct {
  const ir_graph_t *graph;
  const ra_liveness_t *liveness;
  ra_definition_t *definitions;
  ra_deaths_t *deaths;
  opt_arena_t *arena;
  ir_slot_values_t frame_values;
} ra_interference_t;

static bool ra_frame_state_reads(ra_interference_t *interference, ir_node_id_t node, ir_node_id_t value) {
  ir_frame_state_id_t frame_state = ir_node(interference->graph, node)->frame_state;
  if (frame_state == IR_NONE) return false;
  ir_frame_state_values(interference->graph, frame_state, interference->arena, &interference->frame_values);
  for (uint32_t i = 0; i < interference->frame_values.len; i++)
    if (interference->frame_values.items[i].value == value) return true;
  return false;
}

static bool ra_is_live_after_definition_of(ra_interference_t *interference, ir_node_id_t value, ir_node_id_t defined) {
  ra_definition_t at = interference->definitions[defined];
  ra_definition_t of_value = interference->definitions[value];
  bool available = opt_bitset_contains(&interference->liveness->live_in[at.block], value)
    || (of_value.block == at.block && (of_value.is_phi || (!at.is_phi && of_value.position < at.position)));
  if (at.is_phi || !available) return available;
  if (opt_bitset_contains(&interference->liveness->live_out[at.block], value)) return true;
  const ra_deaths_t *deaths = &interference->deaths[value];
  for (uint32_t i = 0; i < deaths->len; i++) {
    if (deaths->items[i].block != at.block) continue;
    if (deaths->items[i].position > at.position) return true;
    if (deaths->items[i].position == at.position && ra_frame_state_reads(interference, defined, value)) return true;
  }
  return false;
}

static bool ra_overlap(ra_interference_t *interference, ir_node_id_t a, ir_node_id_t b) {
  return ra_is_live_after_definition_of(interference, a, b) || ra_is_live_after_definition_of(interference, b, a);
}

uint32_t *ra_spill_slot_classes(const ir_graph_t *graph, const ra_liveness_t *liveness, opt_arena_t *arena) {
  uint32_t count = ir_node_count(graph);
  uint32_t block_count = ir_block_count(graph);
  uint32_t *classes = opt_arena_new(arena, uint32_t, count + 1);
  for (uint32_t i = 0; i < count; i++) classes[i] = i;
  bool has_phis = false;
  for (ir_block_id_t block = 0; block < block_count && !has_phis; block++) has_phis = ir_block(graph, block)->phis.len != 0;
  if (!has_phis) return classes;

  ra_interference_t interference = {
    .graph = graph,
    .liveness = liveness,
    .definitions = opt_arena_new(arena, ra_definition_t, count + 1),
    .deaths = opt_arena_new(arena, ra_deaths_t, count + 1),
    .arena = arena,
  };
  for (ir_block_id_t block = 0; block < block_count; block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->phis.len; i++)
      interference.definitions[data->phis.items[i]] = (ra_definition_t){.block = block, .is_phi = true};
    uint32_t nodes = data->body.len + (data->control != IR_NONE);
    for (uint32_t i = 0; i < nodes; i++) {
      ir_node_id_t node = i < data->body.len ? data->body.items[i] : data->control;
      uint32_t position = liveness->positions[node];
      interference.definitions[node] = (ra_definition_t){.block = block, .position = position};
      const ir_node_vec_t *dies = &liveness->dies_at[node];
      for (uint32_t j = 0; j < dies->len; j++) {
        ra_death_t death = {.block = block, .position = position};
        opt_vec_push(arena, &interference.deaths[dies->items[j]], death);
      }
    }
  }

  ir_node_vec_t *members = opt_arena_new(arena, ir_node_vec_t, count + 1);
  for (ir_block_id_t block = 0; block < block_count; block++) {
    const ir_node_vec_t *phis = &ir_block(graph, block)->phis;
    for (uint32_t i = 0; i < phis->len; i++) {
      ir_node_id_t phi = phis->items[i];
      const ir_node_vec_t *inputs = &ir_node(graph, phi)->inputs;
      for (uint32_t j = 0; j < inputs->len; j++) {
        ir_node_id_t input = inputs->items[j];
        if (input == phi || !ra_is_allocated_value(graph, input)) continue;
        uint32_t a = classes[phi], b = classes[input];
        if (a == b) continue;
        if (!members[a].len) opt_vec_push(arena, &members[a], (ir_node_id_t)a);
        if (!members[b].len) opt_vec_push(arena, &members[b], (ir_node_id_t)b);
        if (members[a].len + members[b].len > RA_MAX_CLASS_SIZE) continue;
        bool overlapping = false;
        for (uint32_t x = 0; x < members[a].len && !overlapping; x++)
          for (uint32_t y = 0; y < members[b].len && !overlapping; y++)
            overlapping = ra_overlap(&interference, members[a].items[x], members[b].items[y]);
        if (overlapping) continue;
        for (uint32_t k = 0; k < members[b].len; k++) {
          classes[members[b].items[k]] = a;
          opt_vec_push(arena, &members[a], members[b].items[k]);
        }
        members[b].len = 0;
      }
    }
  }
  return classes;
}
