#include "compile.h"

void jit_emit_properties(jit_compile_t *c) {
  switch (c->op) {
    case OP_TO_PROPKEY: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t is_key = MIR_new_label(c->ctx);
      MIR_label_t pk_done = MIR_new_label(c->ctx);
      MIR_label_t pk_helper = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBLE,
                                   MIR_new_label_op(c->ctx, pk_helper),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_AND,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_MASK)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_key),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeString)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_key),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeSymbol)));
      MIR_append_insn(c->ctx, c->jit_func, pk_helper);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->helper1_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_to_propkey),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, pk_done)));
      MIR_append_insn(c->ctx, c->jit_func, is_key);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func, pk_done);
      break;
    }

    case OP_GET_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_FIELD2: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_FIELD_OPT: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t nullish = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_LENGTH: {
      bool builder_length = false;
      int raw_slot_size = sv_op_size[OP_GET_SLOT_RAW];
      if (c->builder_target_slots && c->bc_off >= raw_slot_size) {
        uint8_t *prev_ip = c->ip - raw_slot_size;
        if (*prev_ip == OP_GET_SLOT_RAW) {
          uint16_t slot_idx = sv_get_u16(prev_ip + 1);
          int slot_count = c->param_count + c->n_locals;
          builder_length = (int)slot_idx < slot_count &&
                           c->builder_target_slots[slot_idx];
        }
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_emit_get_length(
          c->ctx, c->jit_func, obj, dst,
          c->r_vm, c->r_js, c->r_d_slot,
          c->helper1_proto, c->imp_get_length,
          builder_length,
          -1, c->bc_off);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_DELETE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rk = vstack_pop(&c->vs);
      MIR_reg_t ro = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_delete,
                       c->r_vm, c->r_js, ro, rk);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      sv_ic_entry_t *ic_slot = NULL;
      uint16_t pf_ic_idx = sv_get_u16(c->ip + 5);
      if (c->func->ic_slots && pf_ic_idx != UINT16_MAX && pf_ic_idx < c->func->ic_count)
        ic_slot = &c->func->ic_slots[pf_ic_idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      bool fast = mir_emit_put_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, pf_ic_idx, atom,
          c->r_js, obj, val, slow, c->r_ic_epoch_val,
          c->shape_transition_proto, c->imp_shape_transition,
          c->remember_obj_proto, c->imp_remember_obj);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->put_field_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_field),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ic_slot)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_DEFINE_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->define_field_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_field),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len)));
      break;
    }

    case OP_DEFINE_SLOT: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      uint16_t slot = sv_get_u16(c->ip + 5);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->define_slot_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_slot),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, (int64_t)slot)));
      break;
    }

    case OP_DEFINE_METHOD_COMP: {
      uint8_t flags = sv_get_u8(c->ip + 1);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t fn_val = vstack_pop(&c->vs);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->define_method_comp_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_method_comp),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_reg_op(c->ctx, fn_val),
                                        MIR_new_int_op(c->ctx, (int64_t)flags)));
      break;
    }

    case OP_GET_ELEM: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 1] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, integer_index), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1])));
      }
      bool key_is_num = !integer_index && vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool obj_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);

      if (specialize && !obj_is_num) {
        c->element_available = false;
        MIR_reg_t key = vstack_pop(&c->vs);
        MIR_reg_t obj = vstack_pop(&c->vs);
        (void)vstack_push(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int index_site = mir_next_reg_site(&c->reg_site_n);
        int element_site = mir_next_reg_site(&c->reg_site_n);
        MIR_reg_t index = integer_index
            ? mir_emit_known_array_index_guard(
                c->ctx, c->jit_func, key, integer_index, bail_direct,
                c->r_d_slot, index_site, c->cached_index_key, c->cached_index_value)
            : mir_emit_array_index_guard(
                c->ctx, c->jit_func, key, c->vs.d_regs[c->vs.sp], key_is_num,
                c->r_d_slot, bail_direct, index_site);
        MIR_reg_t loaded = c->r_err_tmp;
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, loaded, JIT_ELEMENT_NUMERIC_READ, bail_direct, element_site);
        mir_i64_to_d(
            c->ctx, c->jit_func, c->vs.d_regs[c->vs.sp - 1], loaded, c->r_d_slot);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
            c->vs.sp - 1, SLOT_BOXED, c->vs.sp,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t element_done = NULL;
      if (!obj_is_num) {
        MIR_label_t slow = MIR_new_label(c->ctx);
        element_done = MIR_new_label(c->ctx);
        MIR_label_t loaded = MIR_new_label(c->ctx);
        if (c->element_available) {
          MIR_label_t miss = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, c->cached_element_valid), MIR_new_int_op(c->ctx, 0)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, c->cached_element_object)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, key), MIR_new_reg_op(c->ctx, c->cached_element_key)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->cached_element_value)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loaded)));
          MIR_append_insn(c->ctx, c->jit_func, miss);
        }
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_READ, slow,
            mir_next_reg_site(&c->reg_site_n));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_object), MIR_new_reg_op(c->ctx, obj)));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_key), MIR_new_reg_op(c->ctx, key)));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_value), MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 1);
        MIR_append_insn(c->ctx, c->jit_func, loaded);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, element_done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 0);
      }
      c->element_available = !obj_is_num;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->ge_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_elem),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      jit_emit_throw_if_error(c, dst);
      if (element_done) MIR_append_insn(c->ctx, c->jit_func, element_done);
      break;
    }

    case OP_GET_ELEM_OPT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t nullish = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->ge_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_elem),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_ELEM2: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_get_elem2,
                       c->r_vm, c->r_js, obj, key);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_ELEM: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 2] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, integer_index), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 2])));
      }
      bool tagged_old_possible =
          sv_tfb_put_needs_tagged_old_guard(feedback);
      bool val_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool key_is_num = !integer_index && vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool obj_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);

      if (specialize && !obj_is_num) {
        MIR_reg_t val = vstack_pop(&c->vs);
        MIR_reg_t key = vstack_pop(&c->vs);
        MIR_reg_t obj = vstack_pop(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int index_site = mir_next_reg_site(&c->reg_site_n);
        int element_site = mir_next_reg_site(&c->reg_site_n);

        MIR_reg_t index = integer_index
            ? mir_emit_known_array_index_guard(
                c->ctx, c->jit_func, key, integer_index, bail_direct,
                c->r_d_slot, index_site, c->cached_index_key, c->cached_index_value)
            : mir_emit_array_index_guard(
                c->ctx, c->jit_func, key, c->vs.d_regs[c->vs.sp + 1], key_is_num,
                c->r_d_slot, bail_direct, index_site);
        if (val_is_num)
          mir_d_to_i64(
              c->ctx, c->jit_func, val, c->vs.d_regs[c->vs.sp + 2], c->r_d_slot);
        else
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, val, bail_direct);

        MIR_reg_t old_value = c->r_err_tmp;
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, old_value,
            JIT_ELEMENT_WRITE, bail_direct, element_site);
        MIR_label_t tagged_old_value = NULL;
        MIR_label_t store = NULL;
        if (tagged_old_possible) {
          tagged_old_value = MIR_new_label(c->ctx);
          store = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, tagged_old_value),
                                       MIR_new_reg_op(c->ctx, old_value),
                                       MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
          MIR_append_insn(c->ctx, c->jit_func, store);
        } else {
          mir_emit_is_num_guard(
              c->ctx, c->jit_func, c->r_bool, old_value, bail_direct);
        }
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, data, index,
                                                    sizeof(ant_value_t)),
                                     MIR_new_reg_op(c->ctx, val)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        if (tagged_old_possible) {
          MIR_append_insn(c->ctx, c->jit_func, tagged_old_value);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGE,
                                       MIR_new_label_op(c->ctx, bail_direct),
                                       MIR_new_reg_op(c->ctx, old_value),
                                       MIR_new_uint_op(c->ctx, ANT_SENTINEL_TAG)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, store)));
        }
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 3, &c->bailout_ctx,
            -1, SLOT_BOXED, c->vs.sp + 1,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_label_t element_done = NULL;
      if (!obj_is_num) {
        MIR_label_t slow = MIR_new_label(c->ctx);
        element_done = MIR_new_label(c->ctx);
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, val, slow);
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_WRITE, slow,
            mir_next_reg_site(&c->reg_site_n));
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, c->r_err_tmp, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, data, index, sizeof(ant_value_t)),
                                     MIR_new_reg_op(c->ctx, val)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, element_done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->put_elem_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_elem),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_reg_op(c->ctx, val)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      if (element_done) MIR_append_insn(c->ctx, c->jit_func, element_done);
      break;
    }

    case OP_GET_PRIVATE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t token = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_get_private,
                       c->r_vm, c->r_js, obj, token);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_PRIVATE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t token = vstack_pop(&c->vs);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->private_put_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_private),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_reg_op(c->ctx, token)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_SET_PROTO: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t proto = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, c->r_err_tmp,
                       c->helper2_proto, c->imp_set_proto,
                       c->r_vm, c->r_js, obj, proto);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
