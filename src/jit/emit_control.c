#include "compile.h"

static void jit_emit_promote_check(jit_compile_t *c, MIR_label_t loop, int64_t every) {
  MIR_reg_t r_due = c->r_tmp2;
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, c->r_promote),
                               MIR_new_reg_op(c->ctx, c->r_promote), MIR_new_int_op(c->ctx, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, loop),
                               MIR_new_reg_op(c->ctx, c->r_promote), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 4,
                                    MIR_new_ref_op(c->ctx, c->promote_due_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_promote_due),
                                    MIR_new_reg_op(c->ctx, r_due),
                                    MIR_new_ref_op(c->ctx, c->cold_ns_item)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_promote),
                               MIR_new_int_op(c->ctx, every)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, loop),
                               MIR_new_reg_op(c->ctx, r_due), MIR_new_int_op(c->ctx, 0)));
  mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp, &c->promote_ctx,
                              -1, SLOT_BOXED, -1, SLOT_BOXED);
  c->needs_promote = true;

}

void jit_emit_control(jit_compile_t *c) {
  switch (c->op) {
    case OP_JMP:
    case OP_JMP8: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      bool short_op = (c->op == OP_JMP8);
      int target = c->bc_off + c->sz + (short_op ? (int8_t)sv_get_i8(c->ip + 1) : sv_get_i32(c->ip + 1));
      MIR_label_t lbl = label_for_branch(c->ctx, &c->lm, target, c->vs.sp);
      if (c->cold_tier && target <= c->bc_off && c->promote_sites && c->promote_sites[c->bc_off])
        jit_emit_promote_check(c, lbl, c->promote_sites[c->bc_off] == 2 ? 1 : JIT_COLD_PROMOTE_CHECK_EVERY);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl)));
      break;
    }

    case OP_JMP_NOT_NULLISH: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t cond = vstack_top(&c->vs);
      int target = c->bc_off + c->sz + sv_get_i32(c->ip + 1);
      MIR_label_t lbl = label_for_branch(c->ctx, &c->lm, target, c->vs.sp);
      MIR_label_t done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, done),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, lbl),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      MIR_append_insn(c->ctx, c->jit_func, done);
      break;
    }

    case OP_JMP_FALSE_PEEK:
    case OP_JMP_TRUE_PEEK:
    case OP_JMP_FALSE:
    case OP_JMP_FALSE8:
    case OP_JMP_TRUE:
    case OP_JMP_TRUE8: {
      bool cond_known_bool = c->vs.known_bool && c->vs.sp > 0 &&
                             c->vs.known_bool[c->vs.sp - 1];
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      bool is_peek = (c->op == OP_JMP_FALSE_PEEK || c->op == OP_JMP_TRUE_PEEK);
      MIR_reg_t cond = is_peek ? vstack_top(&c->vs) : vstack_pop(&c->vs);
      bool short_op = (c->op == OP_JMP_FALSE8 || c->op == OP_JMP_TRUE8);
      bool is_false_branch = (c->op == OP_JMP_FALSE || c->op == OP_JMP_FALSE8 || c->op == OP_JMP_FALSE_PEEK);
      int target = c->bc_off + c->sz + (short_op ? (int8_t)sv_get_i8(c->ip + 1) : sv_get_i32(c->ip + 1));
      MIR_label_t lbl = label_for_branch(c->ctx, &c->lm, target, c->vs.sp);
      if (cond_known_bool) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, cond),
                                     MIR_new_uint_op(c->ctx, is_false_branch ? js_false : js_true)));
        break;
      }
      MIR_label_t lbl_not_bool = MIR_new_label(c->ctx);
      MIR_label_t lbl_not_num = MIR_new_label(c->ctx);
      MIR_label_t lbl_done = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, lbl_not_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, js_false >> NANBOX_TYPE_SHIFT)));
      uint64_t cmp_bool = is_false_branch ? js_false : js_true;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, cmp_bool)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_not_bool);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBGT,
                                   MIR_new_label_op(c->ctx, lbl_not_num),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
      if (!c->r_d_slot) {
        c->r_d_slot = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "d_slot_cond");
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_ALLOCA,
                                     MIR_new_reg_op(c->ctx, c->r_d_slot),
                                     MIR_new_uint_op(c->ctx, 8)));
      }
      if (!c->r_cond_d) {
        c->r_cond_d = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, "cond_d");
        c->r_cond_nan = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cond_nan");
        c->r_cond_zd = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, "zero_d");
        c->r_cond_zero = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cond_zero");
      }
      mir_i64_to_d(c->ctx, c->jit_func, c->r_cond_d, cond, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DNE,
                                   MIR_new_reg_op(c->ctx, c->r_cond_nan),
                                   MIR_new_reg_op(c->ctx, c->r_cond_d),
                                   MIR_new_reg_op(c->ctx, c->r_cond_d)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DMOV,
                                   MIR_new_reg_op(c->ctx, c->r_cond_zd),
                                   MIR_new_double_op(c->ctx, 0.0)));
      MIR_reg_t r_is_zero = c->r_cond_zero;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DEQ,
                                   MIR_new_reg_op(c->ctx, r_is_zero),
                                   MIR_new_reg_op(c->ctx, c->r_cond_d),
                                   MIR_new_reg_op(c->ctx, c->r_cond_zd)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_OR,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, r_is_zero),
                                   MIR_new_reg_op(c->ctx, c->r_cond_nan)));
      if (is_false_branch) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, 0)));
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, 0)));
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_not_num);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->truthy_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_is_truthy),
                                        MIR_new_reg_op(c->ctx, c->r_bool),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, cond)));
      if (is_false_branch) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, 0)));
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, 0)));
      }
      MIR_append_insn(c->ctx, c->jit_func, lbl_done);
      break;
    }

    case OP_RETURN: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t ret_val = vstack_pop(&c->vs);
      if (c->has_captured_slots)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_slotbuf, c->r_jit_open_upvalues, c->captured_params, 0, c->param_count,
                                    mir_next_reg_site(&c->reg_site_n));
      if (c->has_captures)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_lbuf, c->r_jit_open_upvalues, c->captured_locals, 0, c->n_locals,
                                    mir_next_reg_site(&c->reg_site_n));
      if (c->func->is_derived_ctor) {
        MIR_label_t dctor_replace = MIR_new_label(c->ctx);
        MIR_label_t dctor_keep = MIR_new_label(c->ctx);
        char rn_dt[32];
        snprintf(rn_dt, sizeof(rn_dt), "dctor%d", c->upval_n++);
        MIR_reg_t r_dt = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_dt);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBLE,
                                     MIR_new_label_op(c->ctx, dctor_replace),
                                     MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_URSH,
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_AND,
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_int_op(c->ctx, NANBOX_TYPE_MASK)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_LSH,
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_int_op(c->ctx, 1),
                                     MIR_new_reg_op(c->ctx, r_dt)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_AND,
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_reg_op(c->ctx, r_dt),
                                     MIR_new_int_op(c->ctx, (int64_t)T_OBJECT_MASK)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BT,
                                     MIR_new_label_op(c->ctx, dctor_keep),
                                     MIR_new_reg_op(c->ctx, r_dt)));
        MIR_append_insn(c->ctx, c->jit_func, dctor_replace);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_reg_op(c->ctx, c->r_this_curr)));
        MIR_append_insn(c->ctx, c->jit_func, dctor_keep);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_val)));
      break;
    }

    case OP_RETURN_UNDEF: {
      if (c->has_captured_slots)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_slotbuf, c->r_jit_open_upvalues, c->captured_params, 0, c->param_count,
                                    mir_next_reg_site(&c->reg_site_n));
      if (c->has_captures)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_lbuf, c->r_jit_open_upvalues, c->captured_locals, 0, c->n_locals,
                                    mir_next_reg_site(&c->reg_site_n));
      if (c->func->is_derived_ctor) {
        // implicit ctor return: hand the (possibly super-rebound) this
        // back to the caller, which has no other channel to receive it
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, c->r_this_curr)));
        break;
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_ret_insn(c->ctx, 1,
                                       MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      break;
    }

    case OP_NOP:
    case OP_HALT:
    case OP_LINE_NUM:
    case OP_COL_NUM:
    case OP_LABEL:
      break;
    case OP_TRY_PUSH: {
      if (c->jit_try_depth >= JIT_TRY_MAX || c->catch_sp_count >= JIT_TRY_MAX) {
        c->ok = false;
        break;
      }
      int32_t off = sv_get_i32(c->ip + 1);
      int catch_off = c->bc_off + c->sz + off;
      MIR_label_t catch_lbl = label_for_branch(c->ctx, &c->lm, catch_off, c->vs.sp);
      c->jit_try_stack[c->jit_try_depth].catch_label = catch_lbl;
      c->jit_try_stack[c->jit_try_depth].catch_bc_off = catch_off;
      c->jit_try_stack[c->jit_try_depth].saved_sp = c->vs.sp;
      c->jit_try_depth++;
      c->catch_sp_map[c->catch_sp_count].bc_off = catch_off;
      c->catch_sp_map[c->catch_sp_count].saved_sp = c->vs.sp;
      c->catch_sp_count++;
      break;
    }

    case OP_TRY_POP:
      if (c->jit_try_depth > 0) c->jit_try_depth--;
      break;

    case OP_THROW: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t thrown = vstack_pop(&c->vs);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, thrown)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 6,
                                          MIR_new_ref_op(c->ctx, c->helper1_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_throw),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, thrown)));
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      }
      break;
    }

    case OP_THROW_ERROR: {
      uint32_t atom_idx = sv_get_u32(c->ip + 1);
      uint8_t err_type = sv_get_u8(c->ip + 5);
      if (atom_idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[atom_idx];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->throw_error_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_throw_error),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, (int64_t)err_type)));
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
      break;
    }

    case OP_CATCH: {
      int catch_saved_sp = -1;
      for (int i = 0; i < c->catch_sp_count; i++) {
        if (c->catch_sp_map[i].bc_off == c->bc_off) {
          catch_saved_sp = c->catch_sp_map[i].saved_sp;
          break;
        }
      }
      if (catch_saved_sp >= 0) {
        c->vs.sp = catch_saved_sp + 1;
        vstack_clear_value_info(&c->vs, catch_saved_sp);
        c->vs.slot_type[catch_saved_sp] = SLOT_BOXED;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 6,
                                          MIR_new_ref_op(c->ctx, c->helper1_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_catch_value),
                                          MIR_new_reg_op(c->ctx, c->vs.regs[catch_saved_sp]),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, c->vs.regs[catch_saved_sp])));
      } else {
        c->ok = false;
      }
      break;
    }

    case OP_NIP_CATCH: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t info = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t a = c->vs.regs[c->vs.sp - 1];
      MIR_reg_t below = c->vs.regs[c->vs.sp - 2];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, below),
                                   MIR_new_reg_op(c->ctx, a)));
      c->vs.sp--;
      c->vs.slot_type[c->vs.sp - 1] = SLOT_BOXED;
      vstack_set_value_info(&c->vs, c->vs.sp - 1, info);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
