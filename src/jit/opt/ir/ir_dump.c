#include "ir.h"
#include "ir_value.h"
#include <inttypes.h>
#include <stdio.h>

void ir_slot_name(const ir_frame_layout_t *layout, uint32_t slot, char *buffer, size_t size) {
  if (slot == OPT_CALLEE_SLOT) snprintf(buffer, size, "callee");
  else if (slot < layout->number_of_registers) snprintf(buffer, size, "r%u", slot);
  else if (slot < layout->registers_and_locals_count) snprintf(buffer, size, "l%u", slot - layout->number_of_registers);
  else if (slot < ir_layout_arguments_base(layout)) snprintf(buffer, size, "c%u", slot - layout->registers_and_locals_count);
  else snprintf(buffer, size, "a%u", slot - ir_layout_arguments_base(layout));
}

static void ir_value_name(const ir_graph_t *graph, ir_node_id_t id, opt_text_t *out) {
  uint64_t bits;
  if (!ir_constant_value(graph, id, &bits)) {
    opt_text_appendf(out, "v%u", id);
    return;
  }
  switch (ir_node(graph, id)->repr) {
    case OPT_REPR_INT32: opt_text_appendf(out, "#i32:%d", (int32_t)(uint32_t)bits); return;
    case OPT_REPR_BOOL: opt_text_appendf(out, "#bool:%s", bits ? "true" : "false"); return;
    case OPT_REPR_POINTER: opt_text_appendf(out, "#pointer:0x%" PRIx64, bits); return;
    default: break;
  }
  char text[64];
  ir_value_describe(bits, text, sizeof text);
  opt_text_appendf(out, "#%s", text);
}

static const char *ir_repr_text(opt_repr_t repr) {
  switch (repr) {
    case OPT_REPR_TAGGED: return "tagged";
    case OPT_REPR_INT32: return "int32";
    case OPT_REPR_FLOAT64: return "float64";
    case OPT_REPR_BOOL: return "bool";
    case OPT_REPR_POINTER: return "pointer";
    default: return "none";
  }
}

static void ir_shape_text(ir_shape_check_t shape, opt_text_t *out) {
  if (shape.has_generation) opt_text_appendf(out, "0x%" PRIx64 "@%u", shape.shape, shape.generation);
  else opt_text_appendf(out, "0x%" PRIx64, shape.shape);
}

static const char *ir_typed_element_name(ir_typed_array_element_t element) {
  static const char *const names[] = {
    "Uint8", "Uint8Clamped", "Int8", "Uint16", "Int16", "Int32", "Uint32", "Float32", "Float64",
  };
  return element < sizeof names / sizeof *names ? names[element] : "?";
}

static void ir_elements_text(ir_elements_kind_t kind, opt_text_t *out) {
  if (kind == IR_ELEMENTS_PACKED) opt_text_append(out, "Packed");
  else if (kind == IR_ELEMENTS_HOLEY) opt_text_append(out, "Holey");
  else opt_text_appendf(out, "TypedArray(%s)", ir_typed_element_name(ir_elements_element(kind)));
}

static const char *ir_comparison_name(ir_comparison_t comparison) {
  static const char *const names[] = {
    "LessThan", "LessThanEquals", "GreaterThan", "GreaterThanEquals",
    "StrictlyEquals", "StrictlyInequals", "LooselyEquals", "LooselyInequals",
    "Below", "BelowOrEqual", "Above", "AboveOrEqual",
  };
  return comparison < sizeof names / sizeof *names ? names[comparison] : "?";
}

static const char *ir_binary_name(ir_binary_op_t op) {
  static const char *const names[] = {
    "Add", "Sub", "Mul", "Div", "Mod", "BitwiseAnd", "BitwiseOr", "BitwiseXor",
    "LeftShift", "RightShift", "UnsignedRightShift",
  };
  return op < sizeof names / sizeof *names ? names[op] : "?";
}

static const char *ir_float64_function_name(uint8_t function) {
  static const char *const names[] = {
#define ANT_MATH_DOUBLE(id, name, argc, expr) #name,
#include "modules/math_doubles.h"
  };
  return function < sizeof names / sizeof *names ? names[function] : "?";
}

static const char *ir_unary_name(ir_float64_unary_op_t op) {
  static const char *const names[] = {"Abs", "Negate", "Floor", "Ceil", "Round", "Sqrt", "Trunc"};
  return op < sizeof names / sizeof *names ? names[op] : "?";
}

static const char *ir_field_name(ir_frame_field_t field) {
  static const char *const names[] = {
    "LexicalEnvironment", "VariableEnvironment", "PrivateEnvironment", "Realm", "Executable", "SuperValue", "Callee",
  };
  return field < sizeof names / sizeof *names ? names[field] : "?";
}

static const char *ir_typeof_name(ir_typeof_kind_t kind) {
  static const char *const names[] = {
    "Number", "Undefined", "Object", "String", "Symbol", "Boolean", "Bigint", "Function",
  };
  return kind < sizeof names / sizeof *names ? names[kind] : "?";
}

static void ir_condition_text(ir_condition_t condition, opt_text_t *out) {
  switch (condition.kind) {
    case IR_COND_BOOL: opt_text_append(out, "Bool"); break;
    case IR_COND_NULLISH: opt_text_append(out, "Nullish"); break;
    case IR_COND_UNDEFINED: opt_text_append(out, "Undefined"); break;
    case IR_COND_OBJECT: opt_text_append(out, "Object"); break;
    case IR_COND_ELEMENTS_KIND:
      opt_text_append(out, "ElementsKind ");
      ir_elements_text(condition.elements, out);
      break;
    case IR_COND_INDEX_IN_BOUNDS: opt_text_append(out, "IndexInBounds"); break;
    case IR_COND_MAGICAL_LENGTH: opt_text_append(out, "MagicalLength"); break;
    case IR_COND_EXTENSIBLE: opt_text_append(out, "Extensible"); break;
    case IR_COND_ARRAY_ITERATOR: opt_text_append(out, "ArrayIterator"); break;
    case IR_COND_ITERABLE_ARRAY: opt_text_append(out, "IterableArray"); break;
    case IR_COND_TYPED_ARRAY_LENGTH: opt_text_append(out, "TypedArrayLength"); break;
    case IR_COND_ITERATOR_RETURN_INTACT: opt_text_append(out, "IteratorReturnIntact"); break;
    case IR_COND_SHAPE:
      opt_text_append(out, "Shape ");
      ir_shape_text(condition.shape, out);
      break;
    case IR_COND_NON_NEGATIVE_INT32: opt_text_append(out, "NonNegativeInt32"); break;
    case IR_COND_INT32: opt_text_appendf(out, "Int32%s", ir_comparison_name(condition.comparison)); break;
    case IR_COND_FLOAT64: opt_text_appendf(out, "Float64%s", ir_comparison_name(condition.comparison)); break;
    case IR_COND_TAGGED_EQUALS: opt_text_append(out, condition.equal ? "TaggedEquals" : "TaggedNotEquals"); break;
    case IR_COND_INT32_VALUE: opt_text_append(out, "Int32Value"); break;
    case IR_COND_DOUBLE: opt_text_append(out, "Double"); break;
    case IR_COND_STRING: opt_text_append(out, "String"); break;
    case IR_COND_RESOLVED_STRING: opt_text_append(out, "ResolvedString"); break;
    case IR_COND_ASCII_STRING: opt_text_append(out, "AsciiString"); break;
    case IR_COND_BUILTIN: opt_text_appendf(out, "Builtin %u", condition.builtin); break;
    case IR_COND_PROPERTY_ITERATOR_CACHE_VALID: opt_text_append(out, "PropertyIteratorCacheValid"); break;
    case IR_COND_BINDING_MUTABLE: opt_text_appendf(out, "BindingMutable %u", condition.binding_index); break;
    case IR_COND_NEXT_BINDING_OF_SHAPE:
      opt_text_appendf(out, "NextBindingOfShape 0x%" PRIx64 " %u", condition.next_binding.name, condition.next_binding.flags);
      break;
  }
}

static void ir_location_text(const ir_graph_t *graph, sv_func_t *func, uint32_t bc_off, opt_text_t *out) {
  if (!func || func == graph->func) opt_text_appendf(out, "@%u", bc_off);
  else opt_text_appendf(out, "@e%p:%u", (void *)func, bc_off);
}

static void ir_op_text(const ir_graph_t *graph, const ir_frame_layout_t *layout, const ir_op_t *op, const char *inputs, opt_text_t *out) {
  char slot[32];
  const char *name = ir_op_name(op->code);
  opt_text_t head = {0};
  bool with_inputs = true;
  switch (op->code) {
    case IR_OP_CONSTANT: {
      char text[64];
      ir_value_describe(op->bits, text, sizeof text);
      opt_text_appendf(&head, "Constant %s", text);
      with_inputs = false;
      break;
    }
    case IR_OP_LOAD_SLOT:
    case IR_OP_STORE_SLOT:
      ir_slot_name(layout, op->slot, slot, sizeof slot);
      opt_text_appendf(&head, "%s %s", name, slot);
      break;
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
    case IR_OP_STORE_ENVIRONMENT_BINDING: opt_text_appendf(&head, "%s %u", name, op->index); break;
    case IR_OP_INT32_BINARY: opt_text_appendf(&head, "Int32%s", ir_binary_name(op->binary)); break;
    case IR_OP_INT32_WRAPPING_BINARY: opt_text_appendf(&head, "Int32Wrapping%s", ir_binary_name(op->binary)); break;
    case IR_OP_INT32_COMPARE: opt_text_appendf(&head, "Int32%s", ir_comparison_name(op->comparison)); break;
    case IR_OP_CHECK_ELEMENTS:
    case IR_OP_LOAD_ELEMENT_AT:
    case IR_OP_STORE_ELEMENT_AT:
      opt_text_appendf(&head, "%s ", name);
      ir_elements_text(op->elements, &head);
      break;
    case IR_OP_TAGGED_EQUALS: opt_text_append(&head, op->equal ? "TaggedEquals" : "TaggedNotEquals"); break;
    case IR_OP_LOOSE_IDENTITY_EQUALS: opt_text_append(&head, op->equal ? "LooseIdentityEquals" : "LooseIdentityNotEquals"); break;
    case IR_OP_IS_NULLISH: opt_text_append(&head, op->equal ? "IsNullish" : "IsNotNullish"); break;
    case IR_OP_STRICT_EQUALS: opt_text_append(&head, op->equal ? "StrictEquals" : "StrictNotEquals"); break;
    case IR_OP_LOAD_UPVALUE_CELL: opt_text_appendf(&head, "%s %u", name, op->index); break;
    case IR_OP_CHECK_GLOBAL_LEXICALS: opt_text_appendf(&head, "%s %u", name, op->count); break;
    case IR_OP_GENERIC:
      opt_text_appendf(&head, "Generic %s ", ir_bytecode_op_name(op->slow.opcode));
      ir_location_text(graph, op->slow.func, op->slow.bc_off, &head);
      break;
    case IR_OP_LOAD_FRAME_FIELD: opt_text_appendf(&head, "LoadFrameField %s", ir_field_name(op->field)); break;
    case IR_OP_CALL_FORWARDING_ARGUMENTS:
      opt_text_append(&head, "CallForwardingArguments ");
      ir_location_text(graph, op->slow.func, op->slow.bc_off, &head);
      break;
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_NATIVE:
    case IR_OP_CALL_CONSTRUCT:
      opt_text_appendf(&head, "%s ", name);
      ir_location_text(graph, op->call.func, op->call.bc_off, &head);
      if (op->call.target) opt_text_appendf(&head, " to e%p", (void *)op->call.target);
      if (op->call.inlined_frame_bytes)
        opt_text_appendf(&head, " without inlined frames (%u bytes)", op->call.inlined_frame_bytes);
      if (op->call.stores_operands) opt_text_append(&head, " storing operands");
      break;
    case IR_OP_ALLOCATE_OBJECT:
      opt_text_appendf(&head, "AllocateObject 0x%" PRIx64 " with %u properties, room for %u",
        op->allocate_object.shape, op->allocate_object.property_count, op->allocate_object.reserve);
      with_inputs = false;
      break;
    case IR_OP_INITIALIZE_NAMED: opt_text_appendf(&head, "InitializeNamed +%u", op->offset); break;
    case IR_OP_ALLOCATE_ARRAY:
      opt_text_appendf(&head, "AllocateArray with %u elements", op->count);
      with_inputs = false;
      break;
    case IR_OP_VIRTUAL_OBJECT: opt_text_appendf(&head, "VirtualObject 0x%" PRIx64, op->shape); break;
    case IR_OP_ALLOCATE_ENVIRONMENT:
      opt_text_appendf(&head, "AllocateEnvironment %u capacity %u",
        op->allocate_environment.shape_cache, op->allocate_environment.capacity);
      break;
    case IR_OP_ALLOCATE_FUNCTION: opt_text_appendf(&head, "AllocateFunction %u", op->shared_function_data_index); break;
    case IR_OP_INITIALIZE_ELEMENT: opt_text_appendf(&head, "InitializeElement [%u]", op->index); break;
    case IR_OP_CHECK_VALUE: opt_text_appendf(&head, "CheckValue 0x%" PRIx64, op->check_value.expected); break;
    case IR_OP_CHECK_CLOSURE: opt_text_appendf(&head, "CheckClosure 0x%" PRIx64, op->shared_data); break;
    case IR_OP_LOAD_FUNCTION_ENVIRONMENT:
      opt_text_append(&head, op->is_private ? "LoadFunctionEnvironment private" : "LoadFunctionEnvironment");
      break;
    case IR_OP_TYPEOF_IS:
      opt_text_appendf(&head, "TypeofIs%s %s", op->typeof_is.equal ? "" : "Not", ir_typeof_name(op->typeof_is.kind));
      break;
    case IR_OP_CHECK_SHAPE:
      opt_text_append(&head, "CheckShape [");
      for (uint32_t i = 0; i < op->shapes.len; i++) {
        if (i) opt_text_append(&head, ", ");
        ir_shape_text(op->shapes.items[i], &head);
      }
      opt_text_append(&head, "]");
      break;
    case IR_OP_ASSUME_VALID: with_inputs = false; opt_text_append(&head, name); break;
    case IR_OP_CHECK_PROTOTYPE_CHAIN_VALID:
      opt_text_appendf(&head, "CheckPrototypeChainValid 0x%" PRIx64, op->validity);
      with_inputs = false;
      break;
    case IR_OP_LOAD_GLOBAL_BINDING:
      opt_text_appendf(&head, "LoadGlobalBinding 0x%" PRIx64 "[%u]", op->global_binding.environment, op->global_binding.index);
      with_inputs = false;
      break;
    case IR_OP_STORE_GLOBAL_BINDING:
      opt_text_appendf(&head, "StoreGlobalBinding 0x%" PRIx64 "[%u]", op->global_binding.environment, op->global_binding.index);
      break;
    case IR_OP_LOAD_ACCESSOR_FUNCTION:
      opt_text_appendf(&head, "LoadAccessorFunction +%u %s", op->accessor.offset,
        op->accessor.part == IR_ACCESSOR_GETTER ? "Getter" : "Setter");
      break;
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
      opt_text_appendf(&head, "CheckAccessorFunction +%u %s 0x%" PRIx64, op->accessor.offset,
        op->accessor.part == IR_ACCESSOR_GETTER ? "Getter" : "Setter", op->accessor.function);
      break;
    case IR_OP_LOAD_NAMED: opt_text_appendf(&head, "LoadNamed +%u", op->offset); break;
    case IR_OP_LOAD_NAMED_STORAGE: opt_text_appendf(&head, "LoadNamedStorage %u", op->count); break;
    case IR_OP_STORE_NAMED: opt_text_appendf(&head, "StoreNamed +%u", op->offset); break;
    case IR_OP_ADD_NAMED: opt_text_appendf(&head, "AddNamed +%u to 0x%" PRIx64, op->add_named.offset, op->add_named.shape); break;
    case IR_OP_CALL_SLOW_PATH:
      opt_text_appendf(&head, "%s %s ", name, ir_bytecode_op_name(op->slow.opcode));
      ir_location_text(graph, op->slow.func, op->slow.bc_off, &head);
      break;
    case IR_OP_FLOAT64_UNARY: opt_text_appendf(&head, "Float64%s", ir_unary_name(op->unary)); break;
    case IR_OP_CALL_FLOAT64_FUNCTION:
      opt_text_appendf(&head, "CallFloat64Function %s", ir_float64_function_name(op->function));
      break;
    case IR_OP_FLOAT64_BINARY: opt_text_appendf(&head, "Float64%s", ir_binary_name(op->binary)); break;
    case IR_OP_FLOAT64_COMPARE: opt_text_appendf(&head, "Float64%s", ir_comparison_name(op->comparison)); break;
    case IR_OP_REFINE:
      opt_text_appendf(&head, "Refine%s", op->refine.holds ? "" : "Not");
      ir_condition_text(op->refine.condition, &head);
      break;
    case IR_OP_SLOW_PATH_OUTPUT: opt_text_appendf(&head, "SlowPathOutput %u", op->output_index); break;
    case IR_OP_PROBE_HAS_PROPERTY: opt_text_append(&head, op->own ? "ProbeHasOwnProperty" : "ProbeHasProperty"); break;
    case IR_OP_PROBE_GLOBAL_CACHE:
    case IR_OP_PROBE_GLOBAL_STORE: opt_text_appendf(&head, "%s cache %u", name, op->cache); break;
    case IR_OP_PROBE_PROPERTY_CACHE:
    case IR_OP_PROBE_KEYED_CACHE:
    case IR_OP_PROBE_KEYED_STORE:
    case IR_OP_PROBE_PROPERTY_STORE:
      if (!op->probe.func || op->probe.func == graph->func) opt_text_appendf(&head, "%s cache %u", name, op->probe.cache);
      else opt_text_appendf(&head, "%s cache e%p:%u", name, (void *)op->probe.func, op->probe.cache);
      break;
    case IR_OP_JUMP:
      opt_text_appendf(out, "Jump b%u", op->target);
      return;
    case IR_OP_BRANCH:
      opt_text_append(&head, "Branch");
      ir_condition_text(op->branch.condition, &head);
      break;
    case IR_OP_BRANCH_TRUTHY:
    case IR_OP_BRANCH_ON_PC:
    case IR_OP_SHAPE_SWITCH:
      if (op->code == IR_OP_BRANCH_ON_PC) opt_text_appendf(&head, "BranchOnPc @%u", op->on_pc.bc_off);
      else opt_text_append(&head, name);
      break;
    case IR_OP_EXIT: opt_text_appendf(&head, "Exit %s", opt_exit_kind_name(op->exit_kind)); break;
    default: opt_text_append(&head, name); break;
  }
  opt_text_append(out, head.data ? head.data : "");
  opt_text_free(&head);
  if (with_inputs && inputs[0]) opt_text_appendf(out, " %s", inputs);
  switch (op->code) {
    case IR_OP_BRANCH: opt_text_appendf(out, " -> b%u, b%u", op->branch.if_true, op->branch.if_false); break;
    case IR_OP_BRANCH_TRUTHY:
      opt_text_appendf(out, " -> b%u, b%u, fallback b%u", op->truthy.if_true, op->truthy.if_false, op->truthy.fallback);
      break;
    case IR_OP_BRANCH_ON_PC: opt_text_appendf(out, " -> b%u, b%u", op->on_pc.if_true, op->on_pc.if_false); break;
    case IR_OP_SHAPE_SWITCH:
      opt_text_append(out, " [");
      for (uint32_t i = 0; i < op->cases.len; i++) {
        if (i) opt_text_append(out, ", ");
        ir_shape_text(op->cases.items[i].check, out);
        opt_text_appendf(out, " -> b%u", op->cases.items[i].block);
      }
      opt_text_append(out, "]");
      break;
    default: break;
  }
}

void ir_node_text(const ir_graph_t *graph, const ir_frame_layout_t *layout, ir_node_id_t node_id, opt_text_t *out) {
  const ir_node_t *node = ir_node(graph, node_id);
  if (node->repr != OPT_REPR_NONE) {
    opt_text_appendf(out, "v%u", node_id);
    if (node->repr != OPT_REPR_TAGGED) opt_text_appendf(out, ":%s", ir_repr_text(node->repr));
    opt_text_append(out, " = ");
  }
  opt_text_t inputs = {0};
  if (node->inputs.len) {
    opt_text_append(&inputs, "(");
    for (uint32_t i = 0; i < node->inputs.len; i++) {
      if (i) opt_text_append(&inputs, ", ");
      ir_value_name(graph, node->inputs.items[i], &inputs);
    }
    opt_text_append(&inputs, ")");
  }
  ir_op_text(graph, layout, &node->op, inputs.data ? inputs.data : "", out);
  opt_text_free(&inputs);
  if (node->frame_state != IR_NONE) opt_text_appendf(out, " [fs%u]", node->frame_state);
  if (node->handler != IR_NONE) opt_text_appendf(out, " catch h%u", node->handler);
}

void ir_dump(const ir_graph_t *graph, const ir_frame_layout_t *layout, opt_text_t *out) {
  char slot[32];
  for (ir_block_id_t id = 0; id < graph->blocks.len; id++) {
    const ir_block_t *block = ir_block(graph, id);
    opt_text_appendf(out, "b%u", id);
    if (block->bytecode_start != IR_NONE) opt_text_appendf(out, " (pc %u)", block->bytecode_start);
    for (uint32_t i = 0; i < block->predecessors.len; i++)
      opt_text_appendf(out, "%sb%u", i ? ", " : " <- ", block->predecessors.items[i]);
    if (block->is_loop_header) opt_text_append(out, " loop");
    if (block->is_cold) opt_text_append(out, " cold");
    opt_text_append(out, ":\n");
    for (uint32_t i = 0, count = ir_block_node_count(block); i < count; i++) {
      opt_text_append(out, "  ");
      ir_node_text(graph, layout, ir_block_node_at(block, i), out);
      opt_text_append(out, "\n");
    }
  }
  for (uint32_t i = 0; i < graph->osr_entries.len; i++)
    opt_text_appendf(out, "osr entry at pc %u: b%u\n", graph->osr_entries.items[i].bc_off, graph->osr_entries.items[i].block);
  for (uint32_t i = 0; i < graph->handlers.len; i++) {
    const ir_handler_t *handler = &graph->handlers.items[i];
    opt_text_appendf(out, "handler h%u: try pc %u catch pc %u depth %u%s\n", i, handler->try_off, handler->catch_off, handler->depth,
      handler->has_entry ? " entry" : "");
  }
  if (graph->frame_states.len) opt_text_append(out, "frame states:\n");
  for (uint32_t index = 0; index < graph->frame_states.len; index++) {
    const ir_frame_state_t *state = ir_frame_state(graph, index);
    opt_text_appendf(out, "  fs%u: ", index);
    if (state->func && state->func != graph->func) opt_text_appendf(out, "e%p ", (void *)state->func);
    opt_text_appendf(out, "pc %u", state->bc_off);
    if (state->mode == OPT_RESUME_AT) opt_text_append(out, " at");
    else if (state->mode == OPT_RESUME_RETURN) opt_text_append(out, " returning");
    else if (state->dst == OPT_INVALID_SLOT) opt_text_append(out, " after");
    else {
      ir_slot_name(layout, state->dst, slot, sizeof slot);
      opt_text_appendf(out, " after -> %s", slot);
    }
    opt_text_append(out, " {");
    for (uint32_t i = 0; i < state->values.len; i++) {
      ir_slot_name(layout, state->values.items[i].slot, slot, sizeof slot);
      opt_text_appendf(out, "%s%s=", i ? ", " : "", slot);
      ir_value_name(graph, state->values.items[i].value, out);
    }
    opt_text_append(out, "}");
    if (state->in_frame.len) {
      opt_text_append(out, " frame [");
      for (uint32_t i = 0; i < state->in_frame.len; i++) {
        ir_slot_name(layout, state->in_frame.items[i], slot, sizeof slot);
        opt_text_appendf(out, "%s%s", i ? ", " : "", slot);
      }
      opt_text_append(out, "]");
    }
    if (state->construct) opt_text_append(out, " construct");
    if (state->parent != IR_NONE) opt_text_appendf(out, " in fs%u", state->parent);
    opt_text_append(out, "\n");
  }
}
