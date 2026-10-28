#include "ir.h"
#include "ir_value.h"
#include <assert.h>

static const char *const ir_op_names[IR_OP__COUNT] = {
#define IR_OP(name, text) [IR_OP_##name] = text,
#include "ir_ops.h"
};

static const char *const ir_bytecode_op_names[IR_BC__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [IR_BC_##name] = #name,
#include "silver/opcode.h"
};

const char *ir_op_name(ir_opcode_t code) {
  return code < IR_OP__COUNT ? ir_op_names[code] : "?";
}

const char *ir_bytecode_op_name(uint16_t opcode) {
  return opcode < IR_BC__COUNT ? ir_bytecode_op_names[opcode] : "?";
}

opt_repr_t ir_elements_loaded_repr(ir_elements_kind_t kind) {
  if (!ir_elements_is_typed(kind)) return OPT_REPR_TAGGED;
  return ir_typed_element_is_int32(ir_elements_element(kind)) ? OPT_REPR_INT32 : OPT_REPR_FLOAT64;
}

opt_repr_t ir_elements_stored_repr(ir_elements_kind_t kind) {
  if (!ir_elements_is_typed(kind)) return OPT_REPR_TAGGED;
  ir_typed_array_element_t element = ir_elements_element(kind);
  return element == IR_TYPED_FLOAT32 || element == IR_TYPED_FLOAT64 ? OPT_REPR_FLOAT64 : OPT_REPR_INT32;
}

bool ir_binary_int32_can_overflow(ir_binary_op_t op) {
  switch (op) {
    case IR_BINARY_ADD:
    case IR_BINARY_SUB:
    case IR_BINARY_MUL:
    case IR_BINARY_MOD:
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: return true;
    default: return false;
  }
}

bool ir_binary_is_bitwise(ir_binary_op_t op) {
  switch (op) {
    case IR_BINARY_BITWISE_AND:
    case IR_BINARY_BITWISE_OR:
    case IR_BINARY_BITWISE_XOR:
    case IR_BINARY_LEFT_SHIFT:
    case IR_BINARY_RIGHT_SHIFT:
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: return true;
    default: return false;
  }
}

int ir_comparison_equality(ir_comparison_t comparison) {
  switch (comparison) {
    case IR_CMP_STRICTLY_EQUALS:
    case IR_CMP_LOOSELY_EQUALS: return 1;
    case IR_CMP_STRICTLY_INEQUALS:
    case IR_CMP_LOOSELY_INEQUALS: return 0;
    default: return -1;
  }
}

bool ir_condition_equal(ir_condition_t a, ir_condition_t b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case IR_COND_ELEMENTS_KIND: return a.elements == b.elements;
    case IR_COND_SHAPE: return ir_shape_check_equal(a.shape, b.shape);
    case IR_COND_INT32:
    case IR_COND_FLOAT64: return a.comparison == b.comparison;
    case IR_COND_TAGGED_EQUALS: return a.equal == b.equal;
    case IR_COND_BUILTIN: return a.builtin == b.builtin;
    case IR_COND_BINDING_MUTABLE: return a.binding_index == b.binding_index;
    case IR_COND_NEXT_BINDING_OF_SHAPE:
      return a.next_binding.name == b.next_binding.name && a.next_binding.flags == b.next_binding.flags;
    default: return true;
  }
}

bool ir_op_equal(const ir_op_t *a, const ir_op_t *b) {
  if (a->code != b->code) return false;
  switch (a->code) {
    case IR_OP_CONSTANT: return a->bits == b->bits;
    case IR_OP_LOAD_SLOT:
    case IR_OP_STORE_SLOT: return a->slot == b->slot;
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
    case IR_OP_STORE_ENVIRONMENT_BINDING:
    case IR_OP_INITIALIZE_ELEMENT: return a->index == b->index;
    case IR_OP_INITIALIZE_NAMED:
    case IR_OP_LOAD_NAMED:
    case IR_OP_STORE_NAMED: return a->offset == b->offset;
    case IR_OP_LOAD_ARGUMENT: return a->arguments_base == b->arguments_base;
    case IR_OP_ALLOCATE_ARRAY:
    case IR_OP_CONCATENATE_STRINGS:
    case IR_OP_LOAD_NAMED_STORAGE: return a->count == b->count;
    case IR_OP_ALLOCATE_FUNCTION: return a->shared_function_data_index == b->shared_function_data_index;
    case IR_OP_PROBE_GLOBAL_CACHE:
    case IR_OP_PROBE_GLOBAL_STORE: return a->cache == b->cache;
    case IR_OP_SLOW_PATH_OUTPUT: return a->output_index == b->output_index;
    case IR_OP_TAGGED_EQUALS: return a->equal == b->equal;
    case IR_OP_LOAD_FUNCTION_ENVIRONMENT: return a->is_private == b->is_private;
    case IR_OP_PROBE_HAS_PROPERTY: return a->own == b->own;
    case IR_OP_VIRTUAL_OBJECT: return a->shape == b->shape;
    case IR_OP_CHECK_CLOSURE: return a->shared_data == b->shared_data;
    case IR_OP_CHECK_PROTOTYPE_CHAIN_VALID: return a->validity == b->validity;
    case IR_OP_LOAD_FRAME_FIELD: return a->field == b->field;
    case IR_OP_CAN_ADD_ELEMENT:
    case IR_OP_HAS_IN_PROTOTYPE_CHAIN: return a->bits == b->bits;
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY:
    case IR_OP_FLOAT64_BINARY: return a->binary == b->binary;
    case IR_OP_INT32_COMPARE:
    case IR_OP_FLOAT64_COMPARE: return a->comparison == b->comparison;
    case IR_OP_FLOAT64_UNARY: return a->unary == b->unary;
    case IR_OP_CALL_FLOAT64_FUNCTION: return a->function == b->function;
    case IR_OP_CHECK_ELEMENTS:
    case IR_OP_LOAD_ELEMENT_AT:
    case IR_OP_STORE_ELEMENT_AT: return a->elements == b->elements;
    case IR_OP_EXIT: return a->exit_kind == b->exit_kind;
    case IR_OP_JUMP: return a->target == b->target;
    case IR_OP_GENERIC:
    case IR_OP_CALL_SLOW_PATH:
    case IR_OP_CALL_FORWARDING_ARGUMENTS:
      return a->slow.opcode == b->slow.opcode && a->slow.func == b->slow.func && a->slow.bc_off == b->slow.bc_off
        && a->slow.depth == b->slow.depth && a->slow.saves_registers == b->slow.saves_registers
        && a->slow.in_loop == b->slow.in_loop;
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_NATIVE:
    case IR_OP_CALL_CONSTRUCT:
      return a->call.func == b->call.func && a->call.bc_off == b->call.bc_off
        && a->call.inlined_frame_bytes == b->call.inlined_frame_bytes && a->call.stores_operands == b->call.stores_operands
        && a->call.target == b->call.target && a->call.argc == b->call.argc && a->call.helper == b->call.helper
        && a->call.extra == b->call.extra && a->call.allocates_only == b->call.allocates_only
        && a->call.computes_only == b->call.computes_only
        && a->call.needs_frame == b->call.needs_frame && a->call.slow_path == b->call.slow_path;
    case IR_OP_PROBE_PROPERTY_CACHE:
    case IR_OP_PROBE_KEYED_CACHE:
    case IR_OP_PROBE_PROPERTY_STORE:
    case IR_OP_PROBE_KEYED_STORE: return a->probe.func == b->probe.func && a->probe.cache == b->probe.cache;
    case IR_OP_ALLOCATE_OBJECT:
      return a->allocate_object.shape == b->allocate_object.shape
        && a->allocate_object.property_count == b->allocate_object.property_count
        && a->allocate_object.reserve == b->allocate_object.reserve;
    case IR_OP_ALLOCATE_ENVIRONMENT:
      return a->allocate_environment.shape_cache == b->allocate_environment.shape_cache
        && a->allocate_environment.capacity == b->allocate_environment.capacity;
    case IR_OP_CHECK_VALUE:
      return a->check_value.expected == b->check_value.expected && a->check_value.kind == b->check_value.kind;
    case IR_OP_TYPEOF_IS: return a->typeof_is.kind == b->typeof_is.kind && a->typeof_is.equal == b->typeof_is.equal;
    case IR_OP_CHECK_SHAPE:
      if (a->shapes.len != b->shapes.len) return false;
      for (uint32_t i = 0; i < a->shapes.len; i++)
        if (!ir_shape_check_equal(a->shapes.items[i], b->shapes.items[i])) return false;
      return true;
    case IR_OP_LOAD_GLOBAL_BINDING:
    case IR_OP_STORE_GLOBAL_BINDING:
      return a->global_binding.environment == b->global_binding.environment
        && a->global_binding.index == b->global_binding.index;
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
      return a->accessor.offset == b->accessor.offset && a->accessor.part == b->accessor.part
        && a->accessor.function == b->accessor.function;
    case IR_OP_LOAD_ACCESSOR_FUNCTION: return a->accessor.offset == b->accessor.offset && a->accessor.part == b->accessor.part;
    case IR_OP_ADD_NAMED:
    case IR_OP_CAN_ADD_NAMED:
      return a->add_named.offset == b->add_named.offset && a->add_named.shape == b->add_named.shape
        && a->add_named.property_count == b->add_named.property_count && a->add_named.proto == b->add_named.proto
        && a->add_named.cell == b->add_named.cell;
    case IR_OP_LOAD_UPVALUE_CELL: return a->index == b->index;
    case IR_OP_LOAD_UPVALUE: return a->initialized == b->initialized;
    case IR_OP_IS_NULLISH:
    case IR_OP_STRICT_EQUALS:
    case IR_OP_LOOSE_IDENTITY_EQUALS: return a->equal == b->equal;
    case IR_OP_CHECK_GLOBAL_LEXICALS: return a->count == b->count;
    case IR_OP_CHECK_IDENTITY_COMPARABLE: return a->bits == b->bits;
    case IR_OP_BRANCH:
      return ir_condition_equal(a->branch.condition, b->branch.condition) && a->branch.if_true == b->branch.if_true
        && a->branch.if_false == b->branch.if_false;
    case IR_OP_BRANCH_TRUTHY:
      return a->truthy.if_true == b->truthy.if_true && a->truthy.if_false == b->truthy.if_false
        && a->truthy.fallback == b->truthy.fallback;
    case IR_OP_BRANCH_ON_PC:
      return a->on_pc.bc_off == b->on_pc.bc_off && a->on_pc.if_true == b->on_pc.if_true
        && a->on_pc.if_false == b->on_pc.if_false;
    case IR_OP_SHAPE_SWITCH:
      if (a->cases.len != b->cases.len) return false;
      for (uint32_t i = 0; i < a->cases.len; i++) {
        if (!ir_shape_check_equal(a->cases.items[i].check, b->cases.items[i].check)) return false;
        if (a->cases.items[i].block != b->cases.items[i].block) return false;
      }
      return true;
    case IR_OP_REFINE:
      return ir_condition_equal(a->refine.condition, b->refine.condition) && a->refine.holds == b->refine.holds;
    default: return true;
  }
}

static constexpr ir_node_properties_t IR_PURE = {0};

static constexpr ir_node_properties_t IR_FULL_CALL = {
  .can_lazy_exit = true,
  .is_call = true,
  .reads = IR_LOC_ALL,
  .writes = IR_LOC_ALL,
  .runs_code = true,
  .allocates = true,
};

static inline ir_node_properties_t ir_reads(ir_locations_t reads) {
  return (ir_node_properties_t){.reads = reads};
}

static inline ir_node_properties_t ir_writes(ir_locations_t writes) {
  return (ir_node_properties_t){.writes = writes};
}

static inline ir_node_properties_t ir_reads_writes(ir_locations_t locations) {
  return (ir_node_properties_t){.reads = locations, .writes = locations};
}

ir_node_properties_t ir_op_properties(const ir_op_t *op) {
  switch (op->code) {
    case IR_OP_CONSTANT:
    case IR_OP_REFINE:
    case IR_OP_PHI: return IR_PURE;
    case IR_OP_LOAD_SLOT: return ir_reads(IR_LOC_FRAME);
    case IR_OP_STORE_SLOT:
    case IR_OP_INITIALIZE_FRAME:
    case IR_OP_ENSURE_FRAME_INITIALIZED:
    case IR_OP_PUBLISH_FRAME: return ir_writes(IR_LOC_FRAME);
    case IR_OP_BOX_CELL:
    case IR_OP_LOAD_OUTER_ENVIRONMENT: return IR_PURE;
    case IR_OP_LOAD_ENVIRONMENT_BINDING: return ir_reads(IR_LOC_BINDINGS | IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_STORE_ENVIRONMENT_BINDING:
    case IR_OP_APPEND_ENVIRONMENT_BINDING: return ir_writes(IR_LOC_BINDINGS | IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_SET_LEXICAL_ENVIRONMENT:
    case IR_OP_LEAVE_PRIVATE_ENVIRONMENT: return ir_reads_writes(IR_LOC_FRAME);
    case IR_OP_IS_CALLABLE: return IR_PURE;
    case IR_OP_TO_BOOLEAN: return (ir_node_properties_t){.is_call = true};
    case IR_OP_CALL_FLOAT64_FUNCTION: return (ir_node_properties_t){.is_call = true};
    case IR_OP_PRIMITIVE_TO_STRING:
    case IR_OP_TO_OBJECT:
    case IR_OP_ARRAY_CREATE: return (ir_node_properties_t){.is_call = true, .allocates = true};
    case IR_OP_CHECK_INT32:
    case IR_OP_CHECK_NUMBER:
    case IR_OP_CHECK_IDENTITY_COMPARABLE:
    case IR_OP_BOX_INT32:
    case IR_OP_BOX_BOOL:
    case IR_OP_BOOL_TO_INT32:
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY:
    case IR_OP_INT32_COMPARE:
    case IR_OP_TAGGED_EQUALS:
    case IR_OP_CELL_ADDRESS: return IR_PURE;
    case IR_OP_CHECK_ELEMENTS: return ir_elements_is_typed(op->elements) ? IR_PURE : ir_reads(IR_LOC_ELEMENT_COUNTS);
    case IR_OP_CHECK_TYPED_ARRAY: return IR_PURE;
    case IR_OP_LOAD_TYPED_ARRAY_LENGTH:
    case IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET: return ir_reads(IR_LOC_ELEMENT_COUNTS);
    case IR_OP_LOAD_ELEMENT_AT: return ir_reads(IR_LOC_ELEMENTS);
    case IR_OP_STORE_ELEMENT_AT: return ir_writes(IR_LOC_ELEMENTS);
    case IR_OP_CHECK_BOUNDS:
    case IR_OP_CHECK_NOT_HOLE:
    case IR_OP_CHECK_TYPED_ARRAY_STORAGE: return IR_PURE;
    case IR_OP_ALLOCATE_OBJECT: return (ir_node_properties_t){.allocates = true};
    case IR_OP_INITIALIZE_NAMED: return ir_writes(IR_LOC_NAMED_SLOTS);
    case IR_OP_SLICE_ARGUMENTS: return (ir_node_properties_t){.is_call = true, .reads = IR_LOC_FRAME, .allocates = true};
    case IR_OP_ALLOCATE_FUNCTION:
    case IR_OP_ALLOCATE_ENVIRONMENT:
    case IR_OP_ALLOCATE_ARRAY: return (ir_node_properties_t){.allocates = true};
    case IR_OP_VIRTUAL_OBJECT: return IR_PURE;
    case IR_OP_INITIALIZE_ELEMENT: return ir_writes(IR_LOC_ELEMENTS);
    case IR_OP_CHECK_OBJECT:
    case IR_OP_CHECK_STRING:
    case IR_OP_CHECK_VALUE:
    case IR_OP_CHECK_CLOSURE:
    case IR_OP_LOAD_FUNCTION_ENVIRONMENT:
    case IR_OP_EMPTY_TO_UNDEFINED:
    case IR_OP_TYPEOF:
    case IR_OP_TYPEOF_IS: return IR_PURE;
    case IR_OP_CHECK_SHAPE:
    case IR_OP_CHECK_PROTOTYPE_CHAIN_VALID: return ir_reads(IR_LOC_SHAPES);
    case IR_OP_ASSUME_VALID: return ir_reads(IR_LOC_ALL);
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
    case IR_OP_LOAD_ACCESSOR_FUNCTION:
    case IR_OP_LOAD_NAMED: return ir_reads(IR_LOC_NAMED_SLOTS);
    case IR_OP_LOAD_NAMED_STORAGE: return ir_reads(IR_LOC_SHAPES);
    case IR_OP_HAS_IN_PROTOTYPE_CHAIN: return ir_reads(IR_LOC_SHAPES);
    case IR_OP_LOAD_GLOBAL_BINDING: return ir_reads(IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_STORE_GLOBAL_BINDING: return ir_reads_writes(IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_STORE_NAMED: return ir_reads_writes(IR_LOC_NAMED_SLOTS);
    case IR_OP_ADD_NAMED: return ir_reads_writes(IR_LOC_NAMED);
    case IR_OP_ARGUMENT_COUNT:
    case IR_OP_LOAD_FRAME_FIELD:
    case IR_OP_LOAD_ARGUMENT: return ir_reads(IR_LOC_FRAME);
    case IR_OP_CALL_NATIVE:
      if (op->call.allocates_only) return (ir_node_properties_t){.allocates = true};
      if (op->call.computes_only) return (ir_node_properties_t){.is_call = true, .reads = IR_LOC_STRING_BUILDERS};
      return IR_FULL_CALL;
    case IR_OP_GENERIC:
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_CONSTRUCT:
    case IR_OP_CALL_FORWARDING_ARGUMENTS: return IR_FULL_CALL;
    case IR_OP_NEW_THIS: return (ir_node_properties_t){.is_call = true, .reads = IR_LOC_ALL, .allocates = true};
    case IR_OP_LOAD_PROTOTYPE: return ir_reads(IR_LOC_SHAPES);
    case IR_OP_THROW: return (ir_node_properties_t){.is_call = true};
    case IR_OP_JUMP:
    case IR_OP_BRANCH:
    case IR_OP_BRANCH_TRUTHY:
    case IR_OP_BRANCH_ON_PC:
    case IR_OP_RETURN:
    case IR_OP_UNREACHABLE:
    case IR_OP_EXIT: return IR_PURE;
    case IR_OP_SHAPE_SWITCH: return ir_reads(IR_LOC_SHAPES);
    case IR_OP_CALL_SLOW_PATH:
      if (!op->slow.saves_registers) return IR_FULL_CALL;
      return (ir_node_properties_t){.reads = IR_LOC_ALL, .writes = IR_LOC_ALL, .runs_code = true, .allocates = true};
    case IR_OP_SLOW_PATH_OUTPUT: return IR_PURE;
    case IR_OP_PROBE_PROPERTY_CACHE:
    case IR_OP_PROBE_KEYED_CACHE: return ir_reads(IR_LOC_NAMED);
    case IR_OP_PROBE_HAS_PROPERTY: return ir_reads(IR_LOC_ALL);
    case IR_OP_PROBE_GLOBAL_CACHE: return ir_reads(IR_LOC_ALL);
    case IR_OP_PROBE_GLOBAL_STORE:
      return (ir_node_properties_t){.reads = IR_LOC_ALL, .writes = IR_LOC_ALL, .runs_code = true};
    case IR_OP_PROBE_KEYED_STORE:
    case IR_OP_PROBE_PROPERTY_STORE:
      return (ir_node_properties_t){.reads = IR_LOC_NAMED, .writes = IR_LOC_NAMED, .allocates = true};
    case IR_OP_UNBOX_INT32:
    case IR_OP_UNBOX_DOUBLE:
    case IR_OP_BOX_FLOAT64:
    case IR_OP_FLOAT64_UNARY:
    case IR_OP_FLOAT64_BINARY:
    case IR_OP_FLOAT64_COMPARE:
    case IR_OP_FLOAT64_TO_INT32:
    case IR_OP_FLOAT64_TO_UINT8_CLAMPED:
    case IR_OP_UINT32_SHIFT_RIGHT:
    case IR_OP_UINT32_TO_FLOAT64:
    case IR_OP_INT32_ABS:
    case IR_OP_INT32_MIN:
    case IR_OP_INT32_MAX:
    case IR_OP_FLOAT64_MIN:
    case IR_OP_FLOAT64_MAX:
    case IR_OP_INT32_TO_FLOAT64:
    case IR_OP_STRING_ADDRESS: return IR_PURE;
    case IR_OP_STRINGS_EQUAL:
    case IR_OP_INTEGER_TO_STRING: return IR_PURE;
    case IR_OP_CONCATENATE_STRINGS: return (ir_node_properties_t){.allocates = true};
    case IR_OP_STRING_BUILDER_APPEND: return ir_reads_writes(IR_LOC_STRING_BUILDERS);
    case IR_OP_STRING_LENGTH:
    case IR_OP_LOAD_STRING_CODE_UNIT:
    case IR_OP_SINGLE_CHARACTER_STRING: return IR_PURE;
    case IR_OP_CHECK_APPENDABLE_ARRAY: return ir_reads(IR_LOC_ALL);
    case IR_OP_LOAD_ELEMENTS_LENGTH:
    case IR_OP_LOAD_ELEMENTS_CAPACITY: return ir_reads(IR_LOC_ELEMENT_COUNTS);
    case IR_OP_APPEND_ELEMENT:
    case IR_OP_POP_ELEMENT: return ir_reads_writes(IR_LOC_ELEMENTS_AND_COUNTS);
    case IR_OP_LOAD_PROPERTY_ITERATOR_KEY_COUNT:
    case IR_OP_LOAD_PROPERTY_ITERATOR_KEY: return IR_PURE;
    case IR_OP_CALL_ARRAY_PUSH:
      return (ir_node_properties_t){
        .reads = IR_LOC_ELEMENTS_AND_COUNTS, .writes = IR_LOC_ELEMENTS_AND_COUNTS, .allocates = true};
    case IR_OP_CREATE_ARRAY_ITERATOR: return (ir_node_properties_t){.reads = IR_LOC_ALL, .allocates = true};
    case IR_OP_LOAD_ARRAY_ITERATOR_ARRAY:
    case IR_OP_LOAD_ARRAY_ITERATOR_INDEX: return ir_reads(IR_LOC_ITERATORS);
    case IR_OP_STORE_ARRAY_ITERATOR_INDEX:
    case IR_OP_FINISH_ARRAY_ITERATOR: return ir_writes(IR_LOC_ITERATORS);
    case IR_OP_LOAD_UPVALUE_CELL:
    case IR_OP_CHECK_UPVALUE_INITIALIZED:
    case IR_OP_IS_NULLISH:
    case IR_OP_LOOSE_IDENTITY_EQUALS: return IR_PURE;
    case IR_OP_LOAD_UPVALUE: return ir_reads(IR_LOC_BINDINGS | IR_LOC_FRAME);
    case IR_OP_STORE_UPVALUE: return ir_writes(IR_LOC_BINDINGS | IR_LOC_FRAME);
    case IR_OP_CHECK_GLOBAL_LEXICALS: return ir_reads(IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_CAN_ADD_NAMED: return ir_reads(IR_LOC_NAMED | IR_LOC_GLOBAL_BINDINGS);
    case IR_OP_STRICT_EQUALS: return (ir_node_properties_t){.is_call = true};
    case IR_OP_OBJECT_ADDRESS: return IR_PURE;
    case IR_OP_CAN_ADD_ELEMENT: return ir_reads(IR_LOC_NAMED | IR_LOC_ELEMENTS_AND_COUNTS | IR_LOC_GLOBAL_BINDINGS);
    case IR_OP__COUNT: break;
  }
  return IR_PURE;
}

int ir_op_refined_input(const ir_op_t *op) {
  switch (op->code) {
    case IR_OP_REFINE:
    case IR_OP_CHECK_OBJECT:
    case IR_OP_CHECK_STRING:
    case IR_OP_CHECK_SHAPE:
    case IR_OP_CHECK_VALUE:
    case IR_OP_CHECK_CLOSURE:
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
    case IR_OP_CHECK_ELEMENTS:
    case IR_OP_CHECK_TYPED_ARRAY:
    case IR_OP_CHECK_IDENTITY_COMPARABLE:
    case IR_OP_CHECK_APPENDABLE_ARRAY:
    case IR_OP_CHECK_BOUNDS:
    case IR_OP_CHECK_TYPED_ARRAY_STORAGE:
    case IR_OP_CHECK_NOT_HOLE: return 0;
    default: return -1;
  }
}

bool ir_op_is_control(const ir_op_t *op) {
  switch (op->code) {
    case IR_OP_JUMP:
    case IR_OP_BRANCH:
    case IR_OP_BRANCH_TRUTHY:
    case IR_OP_BRANCH_ON_PC:
    case IR_OP_SHAPE_SWITCH:
    case IR_OP_RETURN:
    case IR_OP_THROW:
    case IR_OP_EXIT:
    case IR_OP_UNREACHABLE: return true;
    default: return false;
  }
}

bool ir_op_exit_kind(const ir_op_t *op, opt_exit_kind_t *kind) {
  opt_exit_kind_t result;
  switch (op->code) {
    case IR_OP_EXIT: result = op->exit_kind; break;
    case IR_OP_CHECK_OBJECT: result = OPT_EXIT_NOT_OBJECT; break;
    case IR_OP_CHECK_ELEMENTS:
    case IR_OP_CHECK_TYPED_ARRAY: result = OPT_EXIT_BAD_ELEMENTS; break;
    case IR_OP_CHECK_BOUNDS:
    case IR_OP_CHECK_NOT_HOLE:
    case IR_OP_CHECK_TYPED_ARRAY_STORAGE: result = OPT_EXIT_OUT_OF_BOUNDS; break;
    case IR_OP_LOAD_UPVALUE:
      if (op->initialized) return false;
      result = OPT_EXIT_UNEXPECTED_VALUE;
      break;
    case IR_OP_CHECK_UPVALUE_INITIALIZED: result = OPT_EXIT_UNEXPECTED_VALUE; break;
    case IR_OP_CHECK_INT32: result = OPT_EXIT_NOT_INT32; break;
    case IR_OP_CHECK_NUMBER:
    case IR_OP_CHECK_STRING:
    case IR_OP_CHECK_IDENTITY_COMPARABLE:
    case IR_OP_OBJECT_ADDRESS:
    case IR_OP_HAS_IN_PROTOTYPE_CHAIN: result = OPT_EXIT_BAD_TYPE; break;
    case IR_OP_INT32_BINARY:
      if (!ir_binary_int32_can_overflow(op->binary)) return false;
      result = OPT_EXIT_OVERFLOW;
      break;
    case IR_OP_CHECK_VALUE: result = op->check_value.kind; break;
    case IR_OP_ASSUME_VALID: result = OPT_EXIT_INVALIDATED; break;
    case IR_OP_LOAD_ARGUMENT:
    case IR_OP_SLICE_ARGUMENTS: result = OPT_EXIT_ARGUMENTS_INDEX; break;
    case IR_OP_CHECK_APPENDABLE_ARRAY: result = OPT_EXIT_SLOW_PATH; break;
    case IR_OP_LOAD_ACCESSOR_FUNCTION:
    case IR_OP_CHECK_CLOSURE: result = OPT_EXIT_BAD_CALL_TARGET; break;
    case IR_OP_CHECK_ACCESSOR_FUNCTION: result = OPT_EXIT_UNEXPECTED_VALUE; break;
    case IR_OP_CHECK_SHAPE:
    case IR_OP_CHECK_PROTOTYPE_CHAIN_VALID:
    case IR_OP_LOAD_GLOBAL_BINDING:
    case IR_OP_STORE_GLOBAL_BINDING:
    case IR_OP_LOAD_NAMED_STORAGE:
    case IR_OP_CHECK_GLOBAL_LEXICALS:
    case IR_OP_SHAPE_SWITCH: result = OPT_EXIT_BAD_SHAPE; break;
    default: return false;
  }
  if (kind) *kind = result;
  return true;
}

ir_successors_t ir_op_successors(const ir_op_t *op) {
  ir_successors_t successors = {0};
  switch (op->code) {
    case IR_OP_JUMP:
      successors.len = 1;
      successors.blocks[0] = op->target;
      break;
    case IR_OP_BRANCH:
      successors.len = 2;
      successors.blocks[0] = op->branch.if_true;
      successors.blocks[1] = op->branch.if_false;
      break;
    case IR_OP_BRANCH_ON_PC:
      successors.len = 2;
      successors.blocks[0] = op->on_pc.if_true;
      successors.blocks[1] = op->on_pc.if_false;
      break;
    case IR_OP_BRANCH_TRUTHY:
      successors.len = 3;
      successors.blocks[0] = op->truthy.if_true;
      successors.blocks[1] = op->truthy.if_false;
      successors.blocks[2] = op->truthy.fallback;
      break;
    case IR_OP_SHAPE_SWITCH:
      successors.len = op->cases.len;
      successors.cases = op->cases.items;
      break;
    default: break;
  }
  return successors;
}

bool ir_successors_contains(const ir_successors_t *successors, ir_block_id_t block) {
  for (uint32_t i = 0; i < successors->len; i++)
    if (ir_successor_at(successors, i) == block) return true;
  return false;
}

static uint32_t ir_op_successor_slots(ir_op_t *op, ir_block_id_t **slots, uint32_t max) {
  switch (op->code) {
    case IR_OP_JUMP:
      slots[0] = &op->target;
      return 1;
    case IR_OP_BRANCH:
      slots[0] = &op->branch.if_true;
      slots[1] = &op->branch.if_false;
      return 2;
    case IR_OP_BRANCH_ON_PC:
      slots[0] = &op->on_pc.if_true;
      slots[1] = &op->on_pc.if_false;
      return 2;
    case IR_OP_BRANCH_TRUTHY:
      slots[0] = &op->truthy.if_true;
      slots[1] = &op->truthy.if_false;
      slots[2] = &op->truthy.fallback;
      return 3;
    case IR_OP_SHAPE_SWITCH: {
      uint32_t count = op->cases.len < max ? op->cases.len : max;
      for (uint32_t i = 0; i < count; i++) slots[i] = &op->cases.items[i].block;
      return count;
    }
    default: return 0;
  }
}

static void ir_remap_successors(ir_op_t *op, const ir_block_id_t *new_index) {
  if (op->code == IR_OP_SHAPE_SWITCH) {
    for (uint32_t i = 0; i < op->cases.len; i++) {
      assert(new_index[op->cases.items[i].block] != IR_NONE);
      op->cases.items[i].block = new_index[op->cases.items[i].block];
    }
    return;
  }
  ir_block_id_t *slots[3];
  uint32_t count = ir_op_successor_slots(op, slots, 3);
  for (uint32_t i = 0; i < count; i++) {
    assert(new_index[*slots[i]] != IR_NONE);
    *slots[i] = new_index[*slots[i]];
  }
}

void ir_graph_init(ir_graph_t *graph) {
  memset(graph, 0, sizeof *graph);
  opt_arena_init(&graph->arena);
}

void ir_graph_free(ir_graph_t *graph) {
  opt_arena_free(&graph->arena);
  memset(graph, 0, sizeof *graph);
}

static void ir_op_clone_payload(opt_arena_t *arena, ir_op_t *op) {
  if (op->code == IR_OP_CHECK_SHAPE)
    op->shapes.items = opt_arena_dup(arena, op->shapes.items, op->shapes.len * sizeof *op->shapes.items);
  else if (op->code == IR_OP_SHAPE_SWITCH)
    op->cases.items = opt_arena_dup(arena, op->cases.items, op->cases.len * sizeof *op->cases.items);
}

void ir_graph_clone(ir_graph_t *dst, const ir_graph_t *src) {
  ir_graph_init(dst);
  opt_arena_t *arena = &dst->arena;
  dst->func = src->func;
  dst->materialized_frame_bytes = src->materialized_frame_bytes;
  dst->uses_frame = src->uses_frame;
  opt_vec_clone(arena, &dst->nodes, &src->nodes);
  for (uint32_t i = 0; i < dst->nodes.len; i++) {
    ir_node_t *node = &dst->nodes.items[i];
    ir_node_vec_t inputs = node->inputs;
    opt_vec_clone(arena, &node->inputs, &inputs);
    ir_op_clone_payload(arena, &node->op);
  }
  opt_vec_clone(arena, &dst->blocks, &src->blocks);
  for (uint32_t i = 0; i < dst->blocks.len; i++) {
    ir_block_t *block = &dst->blocks.items[i];
    ir_block_vec_t predecessors = block->predecessors;
    ir_node_vec_t phis = block->phis, body = block->body;
    opt_vec_clone(arena, &block->predecessors, &predecessors);
    opt_vec_clone(arena, &block->phis, &phis);
    opt_vec_clone(arena, &block->body, &body);
  }
  opt_vec_clone(arena, &dst->frame_states, &src->frame_states);
  for (uint32_t i = 0; i < dst->frame_states.len; i++) {
    ir_frame_state_t *state = &dst->frame_states.items[i];
    ir_slot_values_t values = state->values;
    ir_slots_t in_frame = state->in_frame;
    opt_vec_clone(arena, &state->values, &values);
    opt_vec_clone(arena, &state->in_frame, &in_frame);
  }
  opt_vec_clone(arena, &dst->embedded_cells, &src->embedded_cells);
  opt_vec_clone(arena, &dst->typed_array_constants, &src->typed_array_constants);
  opt_vec_clone(arena, &dst->osr_entries, &src->osr_entries);
  opt_vec_clone(arena, &dst->handlers, &src->handlers);
  dst->handler_at_len = src->handler_at_len;
  if (src->handler_at) dst->handler_at = opt_arena_dup(arena, src->handler_at, src->handler_at_len * sizeof *src->handler_at);
  opt_vec_clone(arena, &dst->slow_paths_initializing_frame, &src->slow_paths_initializing_frame);
  opt_vec_clone(arena, &dst->exit_sites, &src->exit_sites);
  opt_vec_clone(arena, &dst->speculatively_hoisted, &src->speculatively_hoisted);
  opt_vec_clone(arena, &dst->retained, &src->retained);
  opt_vec_clone(arena, &dst->retained_funcs, &src->retained_funcs);
  opt_vec_clone(arena, &dst->dependencies, &src->dependencies);
}

ir_node_id_t ir_add_node(ir_graph_t *graph, ir_op_t op, const ir_node_id_t *inputs, uint32_t input_count, opt_repr_t repr, uint32_t bc_off) {
  ir_node_id_t id = graph->nodes.len;
  ir_node_t node = {.op = op, .repr = repr, .frame_state = IR_NONE, .bc_off = bc_off, .handler = IR_NONE};
  if (input_count) {
    opt_vec_reserve(&graph->arena, &node.inputs, input_count);
    memcpy(node.inputs.items, inputs, input_count * sizeof *inputs);
    node.inputs.len = input_count;
  }
  opt_vec_push(&graph->arena, &graph->nodes, node);
  return id;
}

ir_block_id_t ir_add_block(ir_graph_t *graph) {
  ir_block_id_t id = graph->blocks.len;
  ir_block_t block = {.control = IR_NONE, .bytecode_start = IR_NONE, .start_frame_state = IR_NONE};
  opt_vec_push(&graph->arena, &graph->blocks, block);
  return id;
}

ir_frame_state_id_t ir_add_frame_state(ir_graph_t *graph, const ir_frame_state_t *frame_state) {
  ir_frame_state_id_t id = graph->frame_states.len;
  ir_frame_state_t copy = *frame_state;
  opt_vec_clone(&graph->arena, &copy.values, &frame_state->values);
  opt_vec_clone(&graph->arena, &copy.in_frame, &frame_state->in_frame);
  opt_vec_push(&graph->arena, &graph->frame_states, copy);
  return id;
}

ir_frame_state_t ir_frame_state_copy(ir_graph_t *graph, ir_frame_state_id_t id) {
  ir_frame_state_t copy = *ir_frame_state(graph, id);
  ir_frame_state_t *original = ir_frame_state(graph, id);
  opt_vec_clone(&graph->arena, &copy.values, &original->values);
  opt_vec_clone(&graph->arena, &copy.in_frame, &original->in_frame);
  return copy;
}

void ir_set_inputs(ir_graph_t *graph, ir_node_id_t node, const ir_node_id_t *inputs, uint32_t count) {
  ir_node_vec_t *vec = &ir_node(graph, node)->inputs;
  if (count > vec->cap) {
    ir_node_vec_t fresh = {0};
    opt_vec_reserve(&graph->arena, &fresh, count);
    *vec = fresh;
  }
  if (count) memmove(vec->items, inputs, count * sizeof *inputs);
  vec->len = count;
}

void ir_push_input(ir_graph_t *graph, ir_node_id_t node, ir_node_id_t input) {
  opt_vec_push(&graph->arena, &ir_node(graph, node)->inputs, input);
}

ir_shape_check_t *ir_dup_shapes(ir_graph_t *graph, const ir_shape_check_t *shapes, uint32_t count) {
  return opt_arena_dup(&graph->arena, shapes, count * sizeof *shapes);
}

ir_shape_case_t *ir_dup_cases(ir_graph_t *graph, const ir_shape_case_t *cases, uint32_t count) {
  return opt_arena_dup(&graph->arena, cases, count * sizeof *cases);
}

void ir_depend_on(ir_graph_t *graph, opt_dependency_t dependency) {
  for (uint32_t i = 0; i < graph->dependencies.len; i++)
    if (opt_dependency_equal(graph->dependencies.items[i], dependency)) return;
  opt_vec_push(&graph->arena, &graph->dependencies, dependency);
}

bool ir_is_entry(const ir_graph_t *graph, ir_block_id_t block) {
  for (uint32_t i = 0; i < ir_entry_count(graph); i++)
    if (ir_entry_at(graph, i) == block) return true;
  return false;
}

bool ir_is_known_non_empty(const ir_graph_t *graph, ir_node_id_t node, uint32_t arguments_start, uint32_t arguments_end) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  ir_node_vec_t pending = {0}, seen = {0};
  opt_vec_push(&scratch, &pending, node);
  bool result = true;
  while (pending.len && result) {
    ir_node_id_t current = pending.items[--pending.len];
    if (ir_node_vec_contains(&seen, current)) continue;
    opt_vec_push(&scratch, &seen, current);
    const ir_node_t *data = ir_node(graph, current);
    if (data->repr != OPT_REPR_NONE && data->repr != OPT_REPR_TAGGED) continue;
    switch (data->op.code) {
      case IR_OP_CONSTANT:
        if (data->op.bits == IR_VALUE_EMPTY) result = false;
        break;
      case IR_OP_LOAD_SLOT:
        if (data->op.slot < arguments_start || data->op.slot >= arguments_end) result = false;
        break;
      case IR_OP_CALL_DIRECT:
      case IR_OP_CALL_NATIVE:
      case IR_OP_CALL_CONSTRUCT:
      case IR_OP_ARGUMENT_COUNT:
      case IR_OP_LOAD_ARGUMENT:
      case IR_OP_IS_CALLABLE:
      case IR_OP_EMPTY_TO_UNDEFINED:
      case IR_OP_TYPEOF:
      case IR_OP_ALLOCATE_OBJECT:
      case IR_OP_ALLOCATE_ARRAY:
      case IR_OP_CONCATENATE_STRINGS:
      case IR_OP_ALLOCATE_FUNCTION:
      case IR_OP_ALLOCATE_ENVIRONMENT:
      case IR_OP_SLICE_ARGUMENTS:
      case IR_OP_BOX_INT32:
      case IR_OP_BOX_BOOL:
      case IR_OP_BOX_FLOAT64: break;
      case IR_OP_PHI:
        for (uint32_t i = 0; i < data->inputs.len; i++) opt_vec_push(&scratch, &pending, data->inputs.items[i]);
        break;
      default: result = false; break;
    }
  }
  opt_arena_free(&scratch);
  return result;
}

void ir_frame_state_chain(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_node_vec_t *chain) {
  chain->len = 0;
  for (ir_frame_state_id_t current = id; current != IR_NONE; current = ir_frame_state(graph, current)->parent)
    opt_vec_push(arena, chain, current);
}

static void ir_chain_values(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_slot_values_t *values) {
  values->len = 0;
  for (ir_frame_state_id_t current = id; current != IR_NONE; current = ir_frame_state(graph, current)->parent) {
    const ir_frame_state_t *state = ir_frame_state(graph, current);
    opt_vec_reserve(arena, values, values->len + state->values.len);
    if (state->values.len)
      memcpy(&values->items[values->len], state->values.items, state->values.len * sizeof *state->values.items);
    values->len += state->values.len;
  }
}

static inline bool ir_is_virtual_object(const ir_graph_t *graph, ir_node_id_t value) {
  return ir_node(graph, value)->op.code == IR_OP_VIRTUAL_OBJECT;
}

static void ir_virtual_objects_among(const ir_graph_t *graph, const ir_slot_values_t *values, opt_arena_t *arena, ir_node_vec_t *objects) {
  objects->len = 0;
  bool any = false;
  for (uint32_t i = 0; i < values->len && !any; i++) any = ir_is_virtual_object(graph, values->items[i].value);
  if (!any) return;
  ir_node_vec_t pending = {0};
  for (uint32_t i = values->len; i-- > 0;) opt_vec_push(arena, &pending, values->items[i].value);
  while (pending.len) {
    ir_node_id_t value = pending.items[--pending.len];
    if (!ir_is_virtual_object(graph, value) || ir_node_vec_contains(objects, value)) continue;
    opt_vec_push(arena, objects, value);
    const ir_node_t *node = ir_node(graph, value);
    for (uint32_t i = node->inputs.len; i-- > 0;) opt_vec_push(arena, &pending, node->inputs.items[i]);
  }
}

void ir_frame_state_values(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_slot_values_t *values) {
  ir_chain_values(graph, id, arena, values);
  ir_node_vec_t objects = {0};
  ir_virtual_objects_among(graph, values, arena, &objects);
  for (uint32_t i = 0; i < objects.len; i++) {
    const ir_node_t *object = ir_node(graph, objects.items[i]);
    for (uint32_t j = 0; j < object->inputs.len; j++) {
      ir_slot_value_t value = {.slot = IR_VIRTUAL_OBJECT_PROPERTY, .value = object->inputs.items[j]};
      opt_vec_push(arena, values, value);
    }
  }
}

void ir_virtual_objects(const ir_graph_t *graph, ir_frame_state_id_t id, opt_arena_t *arena, ir_node_vec_t *objects) {
  ir_slot_values_t values = {0};
  ir_chain_values(graph, id, arena, &values);
  ir_virtual_objects_among(graph, &values, arena, objects);
}

void ir_used_values(const ir_graph_t *graph, ir_node_id_t node, opt_arena_t *arena, ir_node_vec_t *values) {
  const ir_node_t *data = ir_node(graph, node);
  values->len = 0;
  opt_vec_reserve(arena, values, data->inputs.len);
  if (data->inputs.len) memcpy(values->items, data->inputs.items, data->inputs.len * sizeof *data->inputs.items);
  values->len = data->inputs.len;
  if (data->frame_state == IR_NONE) return;
  bool virtual_objects = false;
  for (ir_frame_state_id_t current = data->frame_state; current != IR_NONE; current = ir_frame_state(graph, current)->parent) {
    const ir_frame_state_t *state = ir_frame_state(graph, current);
    opt_vec_reserve(arena, values, values->len + state->values.len);
    for (uint32_t i = 0; i < state->values.len; i++) {
      ir_node_id_t value = state->values.items[i].value;
      virtual_objects |= ir_is_virtual_object(graph, value);
      values->items[values->len++] = value;
    }
  }
  if (!virtual_objects) return;
  ir_slot_values_t state_values = {0};
  ir_frame_state_values(graph, data->frame_state, arena, &state_values);
  values->len = data->inputs.len;
  for (uint32_t i = 0; i < state_values.len; i++) opt_vec_push(arena, values, state_values.items[i].value);
}

void ir_replace_successor(ir_graph_t *graph, ir_block_id_t block, ir_block_id_t from, ir_block_id_t to) {
  ir_op_t *op = &ir_control(graph, block)->op;
  if (op->code == IR_OP_SHAPE_SWITCH) {
    for (uint32_t i = 0; i < op->cases.len; i++)
      if (op->cases.items[i].block == from) op->cases.items[i].block = to;
    return;
  }
  ir_block_id_t *slots[3];
  uint32_t count = ir_op_successor_slots(op, slots, 3);
  for (uint32_t i = 0; i < count; i++)
    if (*slots[i] == from) *slots[i] = to;
}

void ir_reorder_blocks(ir_graph_t *graph, const ir_block_id_t *order, uint32_t count) {
  uint32_t old_count = graph->blocks.len;
  ir_block_id_t *new_index = opt_arena_alloc(&graph->arena, old_count * sizeof *new_index);
  for (uint32_t i = 0; i < old_count; i++) new_index[i] = IR_NONE;
  for (uint32_t i = 0; i < count; i++) new_index[order[i]] = i;
  ir_block_t *old_blocks = graph->blocks.items;
  ir_block_t *blocks = opt_arena_alloc(&graph->arena, (count ? count : 1) * sizeof *blocks);
  for (uint32_t i = 0; i < count; i++) blocks[i] = old_blocks[order[i]];
  graph->blocks.items = blocks;
  graph->blocks.len = count;
  graph->blocks.cap = count;
  for (uint32_t i = 0; i < count; i++) {
    ir_block_t *block = &blocks[i];
    for (uint32_t j = 0; j < block->predecessors.len; j++) {
      assert(new_index[block->predecessors.items[j]] != IR_NONE);
      block->predecessors.items[j] = new_index[block->predecessors.items[j]];
    }
    if (block->control != IR_NONE) ir_remap_successors(&ir_node(graph, block->control)->op, new_index);
  }
  for (uint32_t i = 0; i < graph->osr_entries.len; i++) {
    assert(new_index[graph->osr_entries.items[i].block] != IR_NONE);
    graph->osr_entries.items[i].block = new_index[graph->osr_entries.items[i].block];
  }
}

ir_node_id_t ir_unrefined(const ir_graph_t *graph, ir_node_id_t id) {
  for (;;) {
    const ir_node_t *node = ir_node(graph, id);
    int index = ir_op_refined_input(&node->op);
    if (index < 0) return id;
    id = node->inputs.items[index];
  }
}

bool ir_constant_value(const ir_graph_t *graph, ir_node_id_t id, uint64_t *bits) {
  const ir_node_t *node = ir_node(graph, ir_unrefined(graph, id));
  if (node->op.code != IR_OP_CONSTANT) return false;
  if (bits) *bits = node->op.bits;
  return true;
}
