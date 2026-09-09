#include "jit_internal.h"

static const uint8_t forward_arguments_prefix[] = {
  OP_SPECIAL_OBJ,  // arguments object (operand 0)
  OP_PUT_LOCAL8,   // -> local 0
  OP_THIS,
  OP_GET_FIELD,    // this.method
  OP_DUP,
  OP_GET_FIELD,    // .apply
  OP_THIS,
  OP_GET_LOCAL8,   // arguments (local 0)
};
enum {
  FORWARD_PREFIX_OPS = sizeof forward_arguments_prefix,
  FORWARD_CALL_OP = FORWARD_PREFIX_OPS,
  FORWARD_RETURN_FORM_OPS = FORWARD_CALL_OP + 2,
  FORWARD_STATEMENT_FORM_OPS = FORWARD_CALL_OP + 3,
};
static const char forward_apply_name[] = "apply";

bool jit_can_forward_arguments(sv_func_t *func) {
  if (!func->is_strict || func->is_async || func->is_generator ||
      func->is_derived_ctor || func->param_count != 0 || func->max_locals != 1)
    return false;

  const uint8_t *ops[FORWARD_STATEMENT_FORM_OPS];
  int count = 0;
  for (const uint8_t *ip = func->code, *end = ip + func->code_len; ip < end;) {
    int size = sv_op_size[*ip];
    if (!size || ip + size > end || count == FORWARD_STATEMENT_FORM_OPS) return false;
    ops[count++] = ip;
    ip += size;
  }
  if (count != FORWARD_RETURN_FORM_OPS && count != FORWARD_STATEMENT_FORM_OPS) return false;

  for (int i = 0; i < FORWARD_PREFIX_OPS; i++)
    if (*ops[i] != forward_arguments_prefix[i]) return false;
  if (ops[0][1] != 0 || ops[1][1] != 0 || ops[7][1] != 0) return false;
  uint32_t atom = sv_get_u32(ops[5] + 1);
  if (atom >= (uint32_t)func->atom_count ||
      func->atoms[atom].len != sizeof forward_apply_name - 1 ||
      memcmp(func->atoms[atom].str, forward_apply_name, sizeof forward_apply_name - 1))
    return false;

  const uint8_t *call = ops[FORWARD_CALL_OP];
  if (sv_get_u16(call + 1) != 2) return false;  // apply(this, arguments)
  if (count == FORWARD_RETURN_FORM_OPS)
    return *call == OP_TAIL_CALL_METHOD && *ops[FORWARD_CALL_OP + 1] == OP_RETURN_UNDEF;
  return *call == OP_CALL_METHOD &&
    (*ops[FORWARD_CALL_OP + 1] == OP_POP || *ops[FORWARD_CALL_OP + 1] == OP_RETURN) &&
    *ops[FORWARD_CALL_OP + 2] == OP_RETURN_UNDEF;
}

bool scan_branch_targets(sv_func_t *func, jit_label_map_t *lm, MIR_context_t ctx) {
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_BRANCH32) != 0) {
      int off = (int)(ip - func->code) + sv_get_i32(ip + 1) + sz;
      if (!label_for_offset(ctx, lm, off)) return false;
    } else if ((flags & SV_OPF_JIT_BRANCH8) != 0) {
      int off = (int)(ip - func->code) + (int8_t)sv_get_i8(ip + 1) + sz;
      if (!label_for_offset(ctx, lm, off)) return false;
    }
    ip += sz;
  }
  return true;
}

static bool jit_local_has_numeric_hint(sv_func_t *func, int idx) {
  if (!func || idx < 0) return false;
  if (idx >= func->max_locals) return false;
  sv_type_info_t *local_types = sv_func_local_types(func);
  if (local_types && idx < func->local_type_count &&
      local_types[idx].type == SV_TI_NUM)
    return true;
  if (func->local_type_feedback) {
    uint8_t ltf = func->local_type_feedback[idx];
    if (ltf && !(ltf & ~SV_TFB_NUM)) return true;
  }
  return false;
}

jit_features_t jit_prescan_features(sv_func_t *func, int n_slots) {
  jit_features_t f = {
    .needs_new_target = func->is_derived_ctor,
    .needs_super = func->is_method || func->is_static || func->is_derived_ctor,
  };
  if (n_slots > 0)
    f.builder_target_slots = calloc((size_t)n_slots, sizeof(bool));
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if (op == OP_CHECK_CTOR) f.needs_new_target = true;
    if (op == OP_SPECIAL_OBJ) {
      uint8_t which = sv_get_u8(ip + 1);
      if (which == 1) f.needs_new_target = true;
      if (which == 2) f.needs_super = true;
    }
    if (op == OP_GET_GLOBAL) {
      const sv_atom_t *atom = &func->atoms[sv_get_u32(ip + 1)];
      if (atom->len == 5 && memcmp(atom->str, "super", 5) == 0) f.needs_super = true;
    }
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_NEEDS_BAILOUT) != 0) f.needs_bailout = true;
    if ((flags & SV_OPF_JIT_NEEDS_INC_LOCAL) != 0) f.needs_inc_local = true;
    if ((flags & SV_OPF_JIT_NEEDS_ARGS_BUF) != 0) f.needs_args_buf = true;
    if ((flags & SV_OPF_JIT_NEEDS_TCO_ARGS) != 0) f.needs_tco_args = true;
    if ((flags & SV_OPF_JIT_NEEDS_ITER_ROOTS) != 0) f.needs_iter_roots = true;
    if ((flags & SV_OPF_JIT_NEEDS_CLOSE_UPVAL) != 0) f.needs_close_upval = true;
    if ((flags & SV_OPF_JIT_NEEDS_IC_EPOCH) != 0) f.needs_ic_epoch = true;
    if (op == OP_THIS || op == OP_CLOSURE || op == OP_EVAL)
      f.needs_this = true;
    if ((flags & SV_OPF_BUILDER_TARGET) != 0 && f.builder_target_slots) {
      uint16_t slot = sv_get_u16(ip + 1);
      if ((int)slot < n_slots) f.builder_target_slots[slot] = true;
    }
    if ((flags & SV_OPF_JIT_LOCAL_NUMERIC_BAILOUT) != 0) {
      if (op == OP_PUT_LOCAL || op == OP_SET_LOCAL) {
        uint16_t idx = sv_get_u16(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      } else {
        uint8_t idx = sv_get_u8(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      }
    }
    ip += sz;
  }
  if (f.needs_bailout) f.needs_args_buf = true;
  return f;
}

static bool jit_value_is_closure(
    ant_value_t value, const sv_closure_t *expected) {
  if (vtype(value) != kTypeFunction) return false;
  return js_func_closure(value) == expected;
}

static bool jit_try_get_global_data(
    ant_t *js, sv_func_t *func, uint8_t *ip,
    const sv_atom_t *atom, ant_value_t *out) {
  if (!js || !func || !ip || !atom || !out) return false;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  return sv_global_ic_try_get_hit(js->global, ic, atom->str, out) ||
         sv_global_ic_try_fill(js->global, ic, atom->str, out);
}

bool jit_mark_self_binding_guards(
    ant_t *js, sv_func_t *func, sv_closure_t *hint_closure,
    uint8_t *guard_sites) {
  if (!js || !func || !hint_closure || !guard_sites) return false;

  bool found = false;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;

    if (op == OP_GET_UPVAL) {
      uint16_t idx = sv_get_u16(ip + 1);
      if (idx < (uint16_t)func->upvalue_count && func->upval_descs &&
          hint_closure->upvalues && hint_closure->upvalues[idx] &&
          jit_value_is_closure(
              *hint_closure->upvalues[idx]->location, hint_closure)) {
        guard_sites[ip - func->code] = 1;
        found = true;
      }
    } else if (op == OP_GET_GLOBAL || op == OP_GET_GLOBAL_UNDEF) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx < (uint32_t)func->atom_count) {
        sv_atom_t *atom = &func->atoms[idx];
        ant_value_t value;
        if (jit_try_get_global_data(js, func, ip, atom, &value) &&
            jit_value_is_closure(value, hint_closure)) {
          guard_sites[ip - func->code] = 1;
          found = true;
        }
      }
    }

    ip += sz;
  }
  return found;
}

bool jit_is_eligible(sv_func_t *func) {
  if (func->is_async || func->is_generator) return false;

  bool eligible = true;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    if ((sv_op_flags[op] & SV_OPF_JIT_ELIGIBLE) == 0) {
      if (op != OP_RE_LITERAL_EXEC &&
          op != OP_STR_RE_LITERAL_REPLACE &&
          op != OP_RE_EXEC_TRUTHY) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op %s in %s\n",
                  (op < OP__COUNT && sv_op_names[op]) ? sv_op_names[op] : "???",
                  func->debug->name ? func->debug->name : "<anonymous>");
      }
      eligible = false;
    } else if (op == OP_CLOSURE) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx >= (uint32_t)func->const_count) return false;
      ant_value_t cv = func->constants[idx];
      if (vtype(cv) != kTypeFunctionInfo) return false;
    } else if (op == OP_SPECIAL_OBJ) {
      if (sv_get_u8(ip + 1) == 0 && !func->is_strict) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op SPECIAL_OBJ(%d) in %s\n",
                  sv_get_u8(ip + 1),
                  func->debug->name ? func->debug->name : "<anonymous>");
        eligible = false;
      }
    }
    ip += sz;
  }
  return eligible;
}
