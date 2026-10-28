#ifndef ANT_JIT_OPT_IR_H
#define ANT_JIT_OPT_IR_H

#include "../arena.h"
#include "../code.h"
#include "types.h"

typedef uint32_t ir_node_id_t;
typedef uint32_t ir_block_id_t;
typedef uint32_t ir_frame_state_id_t;

typedef uint64_t ir_cell_t;

static constexpr uint32_t IR_NONE = UINT32_MAX;

static constexpr uint32_t IR_VIRTUAL_OBJECT_PROPERTY = UINT32_MAX;

typedef OPT_VEC(ir_node_id_t) ir_node_vec_t;
typedef OPT_VEC(ir_block_id_t) ir_block_vec_t;
typedef OPT_VEC(ir_cell_t) ir_cell_vec_t;

typedef uint16_t ir_locations_t;

static constexpr ir_locations_t IR_LOC_NONE = 0;
static constexpr ir_locations_t IR_LOC_FRAME = 1u << 0;
static constexpr ir_locations_t IR_LOC_NAMED_SLOTS = 1u << 1;
static constexpr ir_locations_t IR_LOC_SHAPES = 1u << 2;
static constexpr ir_locations_t IR_LOC_ELEMENTS = 1u << 3;
static constexpr ir_locations_t IR_LOC_ELEMENT_COUNTS = 1u << 4;
static constexpr ir_locations_t IR_LOC_GLOBAL_BINDINGS = 1u << 5;
static constexpr ir_locations_t IR_LOC_BINDINGS = 1u << 6;
static constexpr ir_locations_t IR_LOC_ITERATORS = 1u << 7;
static constexpr ir_locations_t IR_LOC_STRING_BUILDERS = 1u << 8;
static constexpr ir_locations_t IR_LOC_ALL = 0x1ff;
static constexpr ir_locations_t IR_LOC_NAMED = IR_LOC_NAMED_SLOTS | IR_LOC_SHAPES;
static constexpr ir_locations_t IR_LOC_ELEMENTS_AND_COUNTS = IR_LOC_ELEMENTS | IR_LOC_ELEMENT_COUNTS;

static inline bool ir_locations_intersect(ir_locations_t a, ir_locations_t b) {
  return (a & b) != 0;
}

typedef struct {
  uint32_t reserved_count;
  uint32_t number_of_registers;
  uint32_t registers_and_locals_count;
  uint32_t number_of_constants;
  uint32_t number_of_arguments;
} ir_frame_layout_t;

static inline uint32_t ir_layout_arguments_base(const ir_frame_layout_t *layout) {
  return layout->registers_and_locals_count + layout->number_of_constants;
}

typedef enum : uint8_t {
  IR_FIELD_LEXICAL_ENVIRONMENT,
  IR_FIELD_VARIABLE_ENVIRONMENT,
  IR_FIELD_PRIVATE_ENVIRONMENT,
  IR_FIELD_REALM,
  IR_FIELD_EXECUTABLE,
  IR_FIELD_SUPER_VALUE,
  IR_FIELD_CALLEE,
} ir_frame_field_t;

typedef enum : uint8_t {
  IR_TYPED_UINT8,
  IR_TYPED_UINT8_CLAMPED,
  IR_TYPED_INT8,
  IR_TYPED_UINT16,
  IR_TYPED_INT16,
  IR_TYPED_INT32,
  IR_TYPED_UINT32,
  IR_TYPED_FLOAT32,
  IR_TYPED_FLOAT64,
} ir_typed_array_element_t;

typedef uint8_t ir_elements_kind_t;

static constexpr ir_elements_kind_t IR_ELEMENTS_PACKED = 0;
static constexpr ir_elements_kind_t IR_ELEMENTS_HOLEY = 1;
static constexpr ir_elements_kind_t IR_ELEMENTS_TYPED_ARRAY = 2;

static inline ir_elements_kind_t ir_elements_typed(ir_typed_array_element_t element) {
  return (ir_elements_kind_t)(IR_ELEMENTS_TYPED_ARRAY + element);
}

static inline bool ir_elements_is_typed(ir_elements_kind_t kind) {
  return kind >= IR_ELEMENTS_TYPED_ARRAY;
}

static inline ir_typed_array_element_t ir_elements_element(ir_elements_kind_t kind) {
  return (ir_typed_array_element_t)(kind - IR_ELEMENTS_TYPED_ARRAY);
}

static inline bool ir_typed_element_is_int32(ir_typed_array_element_t element) {
  return element != IR_TYPED_UINT32 && element != IR_TYPED_FLOAT32 && element != IR_TYPED_FLOAT64;
}

opt_repr_t ir_elements_loaded_repr(ir_elements_kind_t kind);
opt_repr_t ir_elements_stored_repr(ir_elements_kind_t kind);

typedef enum : uint8_t {
  IR_BINARY_ADD,
  IR_BINARY_SUB,
  IR_BINARY_MUL,
  IR_BINARY_DIV,
  IR_BINARY_MOD,
  IR_BINARY_BITWISE_AND,
  IR_BINARY_BITWISE_OR,
  IR_BINARY_BITWISE_XOR,
  IR_BINARY_LEFT_SHIFT,
  IR_BINARY_RIGHT_SHIFT,
  IR_BINARY_UNSIGNED_RIGHT_SHIFT,
} ir_binary_op_t;

bool ir_binary_int32_can_overflow(ir_binary_op_t op);
bool ir_binary_is_bitwise(ir_binary_op_t op);

typedef enum : uint8_t {
  IR_CMP_LESS_THAN,
  IR_CMP_LESS_THAN_EQUALS,
  IR_CMP_GREATER_THAN,
  IR_CMP_GREATER_THAN_EQUALS,
  IR_CMP_STRICTLY_EQUALS,
  IR_CMP_STRICTLY_INEQUALS,
  IR_CMP_LOOSELY_EQUALS,
  IR_CMP_LOOSELY_INEQUALS,
  IR_CMP_BELOW,
  IR_CMP_BELOW_OR_EQUAL,
  IR_CMP_ABOVE,
  IR_CMP_ABOVE_OR_EQUAL,
} ir_comparison_t;

int ir_comparison_equality(ir_comparison_t comparison);

typedef enum : uint8_t {
  IR_UNARY_ABS,
  IR_UNARY_NEGATE,
  IR_UNARY_FLOOR,
  IR_UNARY_CEIL,
  IR_UNARY_ROUND,
  IR_UNARY_SQRT,
  IR_UNARY_TRUNC,
} ir_float64_unary_op_t;

typedef enum : uint8_t {
  IR_TYPEOF_NUMBER,
  IR_TYPEOF_UNDEFINED,
  IR_TYPEOF_OBJECT,
  IR_TYPEOF_STRING,
  IR_TYPEOF_SYMBOL,
  IR_TYPEOF_BOOLEAN,
  IR_TYPEOF_BIGINT,
  IR_TYPEOF_FUNCTION,
} ir_typeof_kind_t;

typedef enum : uint8_t {
  IR_ACCESSOR_GETTER,
  IR_ACCESSOR_SETTER,
} ir_accessor_part_t;

typedef struct {
  ir_cell_t shape;
  uint32_t generation;
  bool has_generation;
} ir_shape_check_t;

static inline bool ir_shape_check_equal(ir_shape_check_t a, ir_shape_check_t b) {
  return a.shape == b.shape && a.has_generation == b.has_generation
    && (!a.has_generation || a.generation == b.generation);
}

typedef struct {
  ir_shape_check_t check;
  ir_block_id_t block;
} ir_shape_case_t;

typedef enum : uint8_t {
  IR_COND_BOOL,
  IR_COND_NULLISH,
  IR_COND_UNDEFINED,
  IR_COND_OBJECT,
  IR_COND_NON_NEGATIVE_INT32,
  IR_COND_ELEMENTS_KIND,
  IR_COND_INDEX_IN_BOUNDS,
  IR_COND_MAGICAL_LENGTH,
  IR_COND_EXTENSIBLE,
  IR_COND_ARRAY_ITERATOR,
  IR_COND_ITERABLE_ARRAY,
  IR_COND_ITERATOR_RETURN_INTACT,
  IR_COND_SHAPE,
  IR_COND_INT32,
  IR_COND_FLOAT64,
  IR_COND_TAGGED_EQUALS,
  IR_COND_INT32_VALUE,
  IR_COND_DOUBLE,
  IR_COND_STRING,
  IR_COND_RESOLVED_STRING,
  IR_COND_ASCII_STRING,
  IR_COND_BUILTIN,
  IR_COND_BINDING_MUTABLE,
  IR_COND_NEXT_BINDING_OF_SHAPE,
  IR_COND_PROPERTY_ITERATOR_CACHE_VALID,
  IR_COND_TYPED_ARRAY_LENGTH,
} ir_condition_kind_t;

typedef struct {
  ir_condition_kind_t kind;
  union {
    ir_elements_kind_t elements;
    ir_shape_check_t shape;
    ir_comparison_t comparison;
    bool equal;
    uint8_t builtin;
    uint32_t binding_index;
    struct {
      uint64_t name;
      uint8_t flags;
    } next_binding;
  };
} ir_condition_t;

static inline ir_condition_t ir_cond(ir_condition_kind_t kind) {
  return (ir_condition_t){.kind = kind};
}

bool ir_condition_equal(ir_condition_t a, ir_condition_t b);

typedef enum : uint16_t {
#define OP_DEF(name, size, n_pop, n_push, f) IR_BC_##name,
#include "silver/opcode.h"
  IR_BC__COUNT
} ir_bytecode_op_t;

const char *ir_bytecode_op_name(uint16_t opcode);

typedef enum : uint8_t {
#define IR_OP(name, text) IR_OP_##name,
#include "ir_ops.h"
  IR_OP__COUNT
} ir_opcode_t;

typedef struct {
  ir_opcode_t code;
  union {
    uint64_t bits;
    uint32_t slot;
    uint32_t index;
    uint32_t offset;
    uint32_t arguments_base;
    uint32_t count;
    uint32_t shared_function_data_index;
    uint32_t cache;
    uint8_t output_index;
    bool equal;
    bool is_private;
    bool own;
    bool initialized;
    ir_cell_t shape;
    ir_cell_t shared_data;
    ir_cell_t validity;
    ir_frame_field_t field;
    ir_binary_op_t binary;
    ir_comparison_t comparison;
    ir_float64_unary_op_t unary;
    uint8_t function;
    ir_elements_kind_t elements;
    opt_exit_kind_t exit_kind;
    ir_block_id_t target;
    struct {
      uint16_t opcode;
      uint16_t depth;
      sv_func_t *func;
      uint32_t bc_off;
      bool saves_registers;
      bool in_loop;
    } slow;
    struct {
      sv_func_t *func;
      uint32_t bc_off;
      uint32_t inlined_frame_bytes;
      bool stores_operands;
      sv_func_t *target;
      uint16_t argc;
      uintptr_t extra;
      uintptr_t helper;
      bool allocates_only;
      bool computes_only;
      bool needs_frame;
      bool slow_path;
    } call;
    struct {
      sv_func_t *func;
      uint32_t cache;
    } probe;
    struct {
      ir_cell_t shape;
      uint32_t property_count;
      uint32_t reserve;
    } allocate_object;
    struct {
      uint32_t shape_cache;
      uint32_t capacity;
    } allocate_environment;
    struct {
      uint64_t expected;
      opt_exit_kind_t kind;
    } check_value;
    struct {
      ir_typeof_kind_t kind;
      bool equal;
    } typeof_is;
    struct {
      ir_shape_check_t *items;
      uint32_t len;
    } shapes;
    struct {
      ir_cell_t environment;
      uint32_t index;
    } global_binding;
    struct {
      uint32_t offset;
      ir_accessor_part_t part;
      ir_cell_t function;
    } accessor;
    struct {
      uint32_t offset;
      ir_cell_t shape;
      uint32_t property_count;
      uint64_t proto;
      uint64_t cell;
    } add_named;
    struct {
      ir_condition_t condition;
      ir_block_id_t if_true;
      ir_block_id_t if_false;
    } branch;
    struct {
      ir_block_id_t if_true;
      ir_block_id_t if_false;
      ir_block_id_t fallback;
    } truthy;
    struct {
      uint32_t bc_off;
      ir_block_id_t if_true;
      ir_block_id_t if_false;
    } on_pc;
    struct {
      ir_shape_case_t *items;
      uint32_t len;
    } cases;
    struct {
      ir_condition_t condition;
      bool holds;
    } refine;
  };
} ir_op_t;

enum : uint32_t {
  IR_CONCAT_INT32_LHS = 1u << 0,
  IR_CONCAT_INT32_RHS = 1u << 1,
  IR_CONCAT_NUMBERS = 1u << 2,
};

static inline ir_op_t ir_op(ir_opcode_t code) {
  return (ir_op_t){.code = code};
}

const char *ir_op_name(ir_opcode_t code);
bool ir_op_equal(const ir_op_t *a, const ir_op_t *b);

typedef struct {
  bool can_lazy_exit;
  bool is_call;
  ir_locations_t reads;
  ir_locations_t writes;
  bool runs_code;
  bool allocates;
} ir_node_properties_t;

ir_node_properties_t ir_op_properties(const ir_op_t *op);

int ir_op_refined_input(const ir_op_t *op);
bool ir_op_is_control(const ir_op_t *op);
bool ir_op_exit_kind(const ir_op_t *op, opt_exit_kind_t *kind);

static inline bool ir_op_can_eager_exit(const ir_op_t *op) {
  return ir_op_exit_kind(op, NULL);
}

typedef struct {
  uint32_t len;
  ir_block_id_t blocks[3];
  const ir_shape_case_t *cases;
} ir_successors_t;

ir_successors_t ir_op_successors(const ir_op_t *op);

static inline ir_block_id_t ir_successor_at(const ir_successors_t *successors, uint32_t index) {
  return successors->cases ? successors->cases[index].block : successors->blocks[index];
}

bool ir_successors_contains(const ir_successors_t *successors, ir_block_id_t block);

typedef struct {
  ir_op_t op;
  ir_node_vec_t inputs;
  opt_repr_t repr;
  ir_frame_state_id_t frame_state;
  uint32_t bc_off;
  uint32_t handler;
} ir_node_t;

typedef struct {
  ir_block_vec_t predecessors;
  ir_node_vec_t phis;
  ir_node_vec_t body;
  ir_node_id_t control;
  uint32_t bytecode_start;
  sv_func_t *func;
  bool is_loop_header;
  bool is_cold;
  ir_frame_state_id_t start_frame_state;
} ir_block_t;

typedef struct {
  uint32_t slot;
  ir_node_id_t value;
} ir_slot_value_t;

typedef OPT_VEC(ir_slot_value_t) ir_slot_values_t;
typedef OPT_VEC(uint32_t) ir_slots_t;

typedef struct {
  sv_func_t *func;
  uint32_t bc_off;
  opt_resume_mode_t mode;
  uint32_t dst;
  ir_slot_values_t values;
  ir_slots_t in_frame;
  ir_frame_state_id_t parent;
  uint32_t passed_argument_count;
  uint32_t call_off;
  bool construct;
} ir_frame_state_t;

typedef struct {
  uint64_t bits;
  ir_typed_array_element_t element;
  bool has_fixed_length;
  uint32_t fixed_length;
} ir_typed_array_constant_t;

typedef struct {
  uint32_t bc_off;
  ir_block_id_t block;
} ir_osr_entry_t;

typedef struct {
  uint32_t bc_off;
  opt_exit_kind_t kind;
  uint32_t slot;
  sv_func_t *func;
} ir_exit_site_t;

typedef struct {
  uint32_t try_off;
  uint32_t catch_off;
  uint32_t depth;
  bool has_entry;
  bool learnable;
  bool finally;
} ir_handler_t;

typedef struct {
  opt_arena_t arena;
  sv_func_t *func;
  OPT_VEC(ir_node_t) nodes;
  OPT_VEC(ir_block_t) blocks;
  OPT_VEC(ir_frame_state_t) frame_states;
  ir_cell_vec_t embedded_cells;
  OPT_VEC(ir_typed_array_constant_t) typed_array_constants;
  uint64_t materialized_frame_bytes;
  OPT_VEC(ir_osr_entry_t) osr_entries;
  OPT_VEC(ir_handler_t) handlers;
  int32_t *handler_at;
  uint32_t handler_at_len;
  ir_node_vec_t slow_paths_initializing_frame;
  OPT_VEC(ir_exit_site_t) exit_sites;
  ir_node_vec_t speculatively_hoisted;
  OPT_VEC(uint64_t) retained;
  OPT_VEC(sv_func_t *) retained_funcs;
  OPT_VEC(opt_dependency_t) dependencies;
  bool uses_frame;
} ir_graph_t;

void ir_graph_init(ir_graph_t *graph);
void ir_graph_free(ir_graph_t *graph);
void ir_graph_clone(ir_graph_t *dst, const ir_graph_t *src);

static inline ir_node_t *ir_node(const ir_graph_t *graph, ir_node_id_t id) {
  return &graph->nodes.items[id];
}

static inline ir_block_t *ir_block(const ir_graph_t *graph, ir_block_id_t id) {
  return &graph->blocks.items[id];
}

static inline ir_frame_state_t *ir_frame_state(const ir_graph_t *graph, ir_frame_state_id_t id) {
  return &graph->frame_states.items[id];
}

static inline uint32_t ir_node_count(const ir_graph_t *graph) {
  return graph->nodes.len;
}

static inline uint32_t ir_block_count(const ir_graph_t *graph) {
  return graph->blocks.len;
}

ir_node_id_t ir_add_node(ir_graph_t *graph, ir_op_t op, const ir_node_id_t *inputs, uint32_t input_count, opt_repr_t repr, uint32_t bc_off);
ir_block_id_t ir_add_block(ir_graph_t *graph);
ir_frame_state_id_t ir_add_frame_state(ir_graph_t *graph, const ir_frame_state_t *frame_state);
ir_frame_state_t ir_frame_state_copy(ir_graph_t *graph, ir_frame_state_id_t id);

void ir_set_inputs(ir_graph_t *graph, ir_node_id_t node, const ir_node_id_t *inputs, uint32_t count);
void ir_push_input(ir_graph_t *graph, ir_node_id_t node, ir_node_id_t input);
void ir_depend_on(ir_graph_t *graph, opt_dependency_t dependency);
ir_shape_check_t *ir_dup_shapes(ir_graph_t *graph, const ir_shape_check_t *shapes, uint32_t count);
ir_shape_case_t *ir_dup_cases(ir_graph_t *graph, const ir_shape_case_t *cases, uint32_t count);

static inline uint32_t ir_block_node_count(const ir_block_t *block) {
  return block->phis.len + block->body.len + (block->control != IR_NONE);
}

static inline ir_node_id_t ir_block_node_at(const ir_block_t *block, uint32_t index) {
  if (index < block->phis.len) return block->phis.items[index];
  index -= block->phis.len;
  if (index < block->body.len) return block->body.items[index];
  return block->control;
}

static inline bool ir_node_vec_contains(const ir_node_vec_t *vec, ir_node_id_t node) {
  for (uint32_t i = 0; i < vec->len; i++)
    if (vec->items[i] == node) return true;
  return false;
}

static inline int ir_node_vec_position(const ir_node_vec_t *vec, ir_node_id_t node) {
  for (uint32_t i = 0; i < vec->len; i++)
    if (vec->items[i] == node) return (int)i;
  return -1;
}

static inline int ir_block_vec_position(const ir_block_vec_t *vec, ir_block_id_t block) {
  for (uint32_t i = 0; i < vec->len; i++)
    if (vec->items[i] == block) return (int)i;
  return -1;
}

static inline ir_node_id_t ir_control_id(const ir_graph_t *graph, ir_block_id_t block) {
  return ir_block(graph, block)->control;
}

static inline ir_node_t *ir_control(const ir_graph_t *graph, ir_block_id_t block) {
  return ir_node(graph, ir_control_id(graph, block));
}

static inline ir_successors_t ir_block_successors(const ir_graph_t *graph, ir_block_id_t block) {
  return ir_op_successors(&ir_control(graph, block)->op);
}

static inline uint32_t ir_entry_count(const ir_graph_t *graph) {
  return 1 + graph->osr_entries.len;
}

static inline ir_block_id_t ir_entry_at(const ir_graph_t *graph, uint32_t index) {
  return index == 0 ? 0 : graph->osr_entries.items[index - 1].block;
}

bool ir_is_entry(const ir_graph_t *graph, ir_block_id_t block);

bool ir_is_known_non_empty(const ir_graph_t *graph, ir_node_id_t node, uint32_t arguments_start, uint32_t arguments_end);

void ir_frame_state_chain(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_node_vec_t *chain);

void ir_frame_state_values(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_slot_values_t *values);

void ir_virtual_objects(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_node_vec_t *objects);

void ir_used_values(const ir_graph_t *graph, ir_node_id_t node, opt_arena_t *arena, ir_node_vec_t *values);

void ir_replace_successor(ir_graph_t *graph, ir_block_id_t block, ir_block_id_t from, ir_block_id_t to);

void ir_reorder_blocks(ir_graph_t *graph, const ir_block_id_t *order, uint32_t count);

ir_node_id_t ir_unrefined(const ir_graph_t *graph, ir_node_id_t id);

bool ir_constant_value(const ir_graph_t *graph, ir_node_id_t id, uint64_t *bits);

static inline bool ir_is_constant(const ir_graph_t *graph, ir_node_id_t id) {
  return ir_constant_value(graph, id, NULL);
}

static inline bool ir_exit_sites_contain_slot(const ir_graph_t *graph, sv_func_t *func, uint32_t bc_off, opt_exit_kind_t kind, uint32_t slot) {
  sv_func_t *in = func ? func : graph->func;
  for (uint32_t i = 0; i < graph->exit_sites.len; i++) {
    const ir_exit_site_t *site = &graph->exit_sites.items[i];
    if ((site->func ? site->func : graph->func) != in) continue;
    if (site->bc_off == bc_off && site->kind == kind && (site->slot == 0 || site->slot == slot || kind == OPT_EXIT_BAD_SHAPE)) return true;
  }
  return false;
}

static inline bool ir_exit_sites_contain(const ir_graph_t *graph, sv_func_t *func, uint32_t bc_off, opt_exit_kind_t kind) {
  return ir_exit_sites_contain_slot(graph, func, bc_off, kind, 0);
}

void ir_dump(const ir_graph_t *graph, const ir_frame_layout_t *layout, opt_text_t *out);
void ir_node_text(const ir_graph_t *graph, const ir_frame_layout_t *layout, ir_node_id_t node, opt_text_t *out);
void ir_slot_name(const ir_frame_layout_t *layout, uint32_t slot, char *buffer, size_t size);

#endif
