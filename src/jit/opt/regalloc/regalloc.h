#ifndef ANT_JIT_OPT_REGALLOC_H
#define ANT_JIT_OPT_REGALLOC_H

#include "../bitset.h"
#include "../exits.h"
#include "../ir/ir.h"
#include "../target.h"

typedef enum : uint8_t {
  RA_LOC_REGISTER,
  RA_LOC_STACK,
  RA_LOC_CONSTANT,
} ra_location_kind_t;

typedef struct {
  ra_location_kind_t kind;
  uint8_t reg;
  uint32_t slot;
  uint64_t bits;
} ra_location_t;

static inline ra_location_t ra_register(uint8_t reg) {
  return (ra_location_t){.kind = RA_LOC_REGISTER, .reg = reg};
}

static inline ra_location_t ra_stack(uint32_t slot) {
  return (ra_location_t){.kind = RA_LOC_STACK, .slot = slot};
}

static inline ra_location_t ra_constant(uint64_t bits) {
  return (ra_location_t){.kind = RA_LOC_CONSTANT, .bits = bits};
}

static inline bool ra_location_equal(ra_location_t a, ra_location_t b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case RA_LOC_REGISTER: return a.reg == b.reg;
    case RA_LOC_STACK: return a.slot == b.slot;
    case RA_LOC_CONSTANT: return a.bits == b.bits;
  }
  return false;
}

typedef struct {
  ra_location_t from;
  ra_location_t to;
} ra_move_t;

typedef OPT_VEC(ra_move_t) ra_moves_t;
typedef OPT_VEC(ra_location_t) ra_locations_t;
typedef OPT_VEC(uint8_t) ra_temps_t;

typedef struct {
  uint32_t slot;
  ra_location_t location;
} ra_exit_value_t;

typedef OPT_VEC(ra_exit_value_t) ra_exit_values_t;

typedef struct {
  ra_moves_t moves_before;
  ra_locations_t inputs;
  ra_temps_t temps;
  bool has_output;
  ra_location_t output;
  uint32_t spill_slot;
  opt_exit_list_t exits;
} ra_node_allocation_t;

typedef struct {
  opt_arena_t arena;
  ra_node_allocation_t *nodes;
  uint32_t node_count;
  ra_moves_t *block_end_moves;
  uint32_t block_count;
  uint32_t spill_slot_count;
  opt_bitset_t *live_in;
  opt_exit_table_t exit_table;
} ra_allocation_t;

void ra_allocate(const ir_graph_t *graph, const opt_register_set_t *registers, ra_allocation_t *allocation);
void ra_allocation_free(ra_allocation_t *allocation);
void ra_allocation_clone(ra_allocation_t *dst, const ra_allocation_t *src);
void ra_exit_values(const ra_allocation_t *allocation, ir_node_id_t node, opt_arena_t *arena, ra_exit_values_t *out);

uint32_t ra_phi_spill_store(const ir_graph_t *graph, const ra_allocation_t *allocation, ir_node_id_t phi);
bool ra_verify(const ir_graph_t *graph, const opt_register_set_t *registers, const ra_allocation_t *allocation, opt_text_t *errors);
void ra_dump_allocation(const ir_graph_t *graph, const ra_allocation_t *allocation, opt_text_t *out);
void ra_location_text(ra_location_t location, opt_text_t *out);
void ra_resolve_parallel_moves(const ra_move_t *moves, uint32_t count, ra_location_t temp, opt_arena_t *arena, ra_moves_t *result);
bool ra_saves_only_gprs(const ir_op_t *op);

ir_block_id_t ra_state_predecessor(const ir_graph_t *graph, ir_block_id_t block);

#endif
