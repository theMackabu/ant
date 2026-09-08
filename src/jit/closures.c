#include "jit_internal.h"
void mir_emit_call_stable_builtin(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    MIR_reg_t r_vm, MIR_reg_t r_js,
    int kind, MIR_reg_t call_func, MIR_reg_t call_this,
    MIR_reg_t arg0, MIR_reg_t args, int argc, MIR_reg_t dst,
    MIR_item_t call_proto, MIR_item_t imp_call,
    bool known_intrinsic, bool args_prepared, int site_id) {
  if (
      kind != SV_STABLE_BUILTIN_PROMISE_RESOLVE || argc != 1 || !arg0) {
    MIR_append_insn(ctx, fn,
                    MIR_new_call_insn(ctx, 10,
                                      MIR_new_ref_op(ctx, call_proto),
                                      MIR_new_ref_op(ctx, imp_call),
                                      MIR_new_reg_op(ctx, dst),
                                      MIR_new_reg_op(ctx, r_vm),
                                      MIR_new_reg_op(ctx, r_js),
                                      MIR_new_int_op(ctx, kind),
                                      MIR_new_reg_op(ctx, call_func),
                                      MIR_new_reg_op(ctx, call_this),
                                      MIR_new_reg_op(ctx, args),
                                      MIR_new_int_op(ctx, argc)));
    return;
  }

  char tag_name[40], ptr_name[40], guard_name[40], proto_name[40];
  snprintf(tag_name, sizeof(tag_name), "sb_tag_%d", site_id);
  snprintf(ptr_name, sizeof(ptr_name), "sb_ptr_%d", site_id);
  snprintf(guard_name, sizeof(guard_name), "sb_guard_%d", site_id);
  snprintf(proto_name, sizeof(proto_name), "sb_proto_%d", site_id);
  MIR_reg_t tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, tag_name);
  MIR_reg_t ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, ptr_name);
  MIR_reg_t guard = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, guard_name);
  MIR_reg_t proto = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, proto_name);
  MIR_label_t slow = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  if (!known_intrinsic) {
    mir_load_const_slot(ctx, fn, guard, &js->sym.promise_ctor);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, call_this),
                                 MIR_new_reg_op(ctx, guard)));
    mir_load_const_slot(ctx, fn, guard, &js->sym.promise_resolve);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, call_func),
                                 MIR_new_reg_op(ctx, guard)));
  }
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, arg0),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, NANBOX_TPROM_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, guard),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_t, promise_constructor_protector_invalid),
                                              r_js, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, guard),
                               MIR_new_int_op(ctx, 0)));
  mir_emit_decode_ref(ctx, fn, ptr, arg0);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, proto),
                               MIR_new_mem_op(ctx, MIR_JSVAL,
                                              (MIR_disp_t)offsetof(ant_object_t, proto), ptr, 0, 1)));
  mir_load_const_slot(ctx, fn, guard, &js->sym.promise_proto);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, proto),
                               MIR_new_reg_op(ctx, guard)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, arg0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, slow);
  if (!args_prepared)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_JSVAL, 0, args, 0, 1),
                                 MIR_new_reg_op(ctx, arg0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 10,
                                    MIR_new_ref_op(ctx, call_proto),
                                    MIR_new_ref_op(ctx, imp_call),
                                    MIR_new_reg_op(ctx, dst),
                                    MIR_new_reg_op(ctx, r_vm),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_int_op(ctx, kind),
                                    MIR_new_reg_op(ctx, call_func),
                                    MIR_new_reg_op(ctx, call_this),
                                    MIR_new_reg_op(ctx, args),
                                    MIR_new_int_op(ctx, argc)));
  MIR_append_insn(ctx, fn, done);
}

void mir_emit_load_stable_builtin(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    MIR_reg_t r_js, int kind,
    MIR_reg_t receiver, MIR_reg_t func, MIR_reg_t receiver_out,
    MIR_item_t load_proto, MIR_item_t imp_load, int site_id) {
  char guard_name[40];
  snprintf(guard_name, sizeof(guard_name), "stable_load_guard_%d", site_id);
  MIR_reg_t guard = MIR_new_func_reg(
      ctx, fn->u.func, MIR_T_I64, guard_name);
  MIR_label_t slow = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, guard),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(
                                                  ant_t, promise_resolve_lookup_protector_invalid),
                                              r_js, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, guard),
                               MIR_new_int_op(ctx, 0)));
  mir_load_const_slot(ctx, fn, receiver, &js->sym.promise_ctor);
  mir_load_const_slot(ctx, fn, func, &js->sym.promise_resolve);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 6,
                                    MIR_new_ref_op(ctx, load_proto),
                                    MIR_new_ref_op(ctx, imp_load),
                                    MIR_new_reg_op(ctx, func),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_int_op(ctx, kind),
                                    MIR_new_reg_op(ctx, receiver_out)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, receiver),
                               MIR_new_mem_op(ctx, MIR_JSVAL, 0, receiver_out, 0, 1)));
  MIR_append_insn(ctx, fn, done);
}

void mir_emit_get_closure(MIR_context_t ctx, MIR_item_t fn,
                          MIR_reg_t dst, MIR_reg_t v,
                          MIR_reg_t r_tag, MIR_label_t fallback) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_reg_op(ctx, v),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, fallback),
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  mir_emit_decode_ref(ctx, fn, dst, v);
}

void mir_emit_resolve_call_this(MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t dst, MIR_reg_t r_closure,
                                MIR_reg_t fallback_this,
                                MIR_reg_t r_flags, MIR_reg_t r_bound) {
  MIR_label_t use_bound = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_flags),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                                              r_closure, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, r_flags),
                               MIR_new_reg_op(ctx, r_flags),
                               MIR_new_uint_op(ctx, SV_CALL_IS_ARROW | SV_CALL_HAS_BOUND_THIS)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, use_bound),
                               MIR_new_reg_op(ctx, r_flags),
                               MIR_new_uint_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, fallback_this)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, use_bound);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_bound),
                               MIR_new_mem_op(ctx, MIR_JSVAL,
                                              (MIR_disp_t)offsetof(sv_closure_t, bound_this),
                                              r_closure, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, r_bound)));
  MIR_append_insn(ctx, fn, done);
}

void scan_osr_entries(sv_func_t *func, osr_entry_map_t *osr) {
  osr->count = 0;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    int src = (int)(ip - func->code);
    int target = -1;
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_OSR_BACKEDGE) != 0) {
      if ((flags & SV_OPF_JIT_BRANCH32) != 0)
        target = src + sz + sv_get_i32(ip + 1);
      else if ((flags & SV_OPF_JIT_BRANCH8) != 0)
        target = src + sz + (int8_t)sv_get_i8(ip + 1);
    }
    if (target >= 0 && target <= src) {
      bool found = false;
      for (int i = 0; i < osr->count; i++)
        if (osr->offsets[i] == target) {
          found = true;
          break;
        }
      if (!found && osr->count < MAX_OSR_ENTRIES)
        osr->offsets[osr->count++] = target;
    }
    ip += sz;
  }
}

bool func_writes_params(sv_func_t *func) {
  if (!func || func->param_count <= 0) return false;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if (ip + sz > end) break;
    if (op == OP_PUT_ARG || op == OP_SET_ARG) return true;
    if ((sv_op_flags[op] & SV_OPF_BUILDER_TARGET) != 0) {
      if (sv_get_u16(ip + 1) < func->param_count) return true;
    }
    ip += sz;
  }

  return false;
}

static sv_func_t *scan_closure_child(sv_func_t *func, uint8_t *ip) {
  if ((sv_op_t)*ip != OP_CLOSURE) return NULL;

  uint32_t idx = sv_get_u32(ip + 1);
  if (idx >= (uint32_t)func->const_count) return NULL;

  ant_value_t cv = func->constants[idx];
  if (vtype(cv) != kTypeFunctionInfo) return NULL;

  return (sv_func_t *)vptr(cv);
}

jit_child_kind_t classify_child_closure_kind(sv_func_t *parent, sv_func_t *child) {
  if (!parent || !child || child->upvalue_count <= 0) return JIT_CHILD_PLAIN;

  bool has_inherited = false;
  bool has_param = false;
  bool has_local = false;
  for (int i = 0; i < child->upvalue_count; i++) {
    sv_upval_desc_t *desc = &child->upval_descs[i];
    if (!desc->is_local) {
      has_inherited = true;
      continue;
    }
    if (desc->index < (uint16_t)parent->param_count)
      has_param = true;
    else
      has_local = true;
  }

  if (!has_param && !has_local) return has_inherited ? JIT_CHILD_INHERITED_ONLY : JIT_CHILD_PLAIN;
  if (has_param && !has_local) return JIT_CHILD_PARAM_ONLY;
  if (has_local && !has_param) return JIT_CHILD_LOCAL_ONLY;

  return JIT_CHILD_MIXED;
}

bool *scan_captured_locals(sv_func_t *func, int n_locals) {
  if (n_locals <= 0) return NULL;
  bool *captured = calloc((size_t)n_locals, sizeof(bool));
  if (!captured) return NULL;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    sv_func_t *child = scan_closure_child(func, ip);
    if (!child) {
      ip += sz;
      continue;
    }

    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (!desc->is_local) continue;

      int li = (int)desc->index - func->param_count;
      if (li < 0 || li >= n_locals) continue;
      captured[li] = true;
    }
    ip += sz;
  }
  return captured;
}

bool *scan_captured_params(sv_func_t *func) {
  int param_count = func ? func->param_count : 0;
  if (param_count <= 0) return NULL;
  bool *captured = calloc((size_t)param_count, sizeof(bool));
  if (!captured) return NULL;

  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    sv_func_t *child = scan_closure_child(func, ip);
    if (!child) {
      ip += sz;
      continue;
    }

    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (!desc->is_local) continue;
      if (desc->index >= (uint16_t)param_count) continue;
      captured[desc->index] = true;
    }
    ip += sz;
  }

  return captured;
}
