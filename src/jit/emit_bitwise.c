#include "compile.h"
#include "silver/feedback.h"

void jit_emit_bitwise(jit_compile_t *c) {
  switch (c->op) {
    case OP_IS_UNDEF:
    case OP_IS_NULL: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);
      uint64_t cmp_val = (c->op == OP_IS_UNDEF) ? mkval(kTypeUndefined, 0) : mkval(kTypeNull, 0);
      MIR_label_t is_true = MIR_new_label(c->ctx);
      MIR_label_t is_done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_true),
                                   MIR_new_reg_op(c->ctx, rs),
                                   MIR_new_uint_op(c->ctx, cmp_val)));
      mir_load_imm(c->ctx, c->jit_func, rs, js_false);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, is_done)));
      MIR_append_insn(c->ctx, c->jit_func, is_true);
      mir_load_imm(c->ctx, c->jit_func, rs, js_true);
      MIR_append_insn(c->ctx, c->jit_func, is_done);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;
      break;
    }

    case OP_IS_UNDEF_OR_NULL: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);
      MIR_label_t is_true = MIR_new_label(c->ctx);
      MIR_label_t is_done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_true),
                                   MIR_new_reg_op(c->ctx, rs),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_true),
                                   MIR_new_reg_op(c->ctx, rs),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      mir_load_imm(c->ctx, c->jit_func, rs, js_false);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, is_done)));
      MIR_append_insn(c->ctx, c->jit_func, is_true);
      mir_load_imm(c->ctx, c->jit_func, rs, js_true);
      MIR_append_insn(c->ctx, c->jit_func, is_done);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;
      break;
    }

    case OP_BAND:
    case OP_BOR:
    case OP_BXOR:
    case OP_SHL:
    case OP_SHR:
    case OP_USHR: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      int rr_idx = c->vs.sp - 1;
      int rl_idx = c->vs.sp - 2;
      uint8_t l_type = c->vs.slot_type ? c->vs.slot_type[rl_idx] : SLOT_BOXED;
      uint8_t r_type = c->vs.slot_type ? c->vs.slot_type[rr_idx] : SLOT_BOXED;
      bool l_is_num = l_type == SLOT_NUM;
      bool r_is_num = r_type == SLOT_NUM;
      bool l_is_i32 = l_type == SLOT_I32;
      bool r_is_i32 = r_type == SLOT_I32;

      jit_integer_range_t result_range = jit_word_range(
          c->op, l_is_i32 ? c->vs.integer_range[rl_idx] : (jit_integer_range_t){0},
          r_is_i32 ? c->vs.integer_range[rr_idx] : (jit_integer_range_t){0});
      if (specialize || (l_is_i32 && r_is_i32)) {
        MIR_reg_t rr = vstack_pop(&c->vs);
        MIR_reg_t rl = vstack_pop(&c->vs);
        MIR_reg_t rd = vstack_push(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int left_site = mir_next_reg_site(&c->reg_site_n);
        int right_site = mir_next_reg_site(&c->reg_site_n);
        int result_site = mir_next_reg_site(&c->reg_site_n);
        MIR_reg_t left = mir_emit_word32_guard(
            c->ctx, c->jit_func, rl, c->vs.d_regs[c->vs.sp - 1], l_is_num, l_is_i32,
            c->r_d_slot, bail_direct, left_site);
        MIR_reg_t right = mir_emit_word32_guard(
            c->ctx, c->jit_func, rr, c->vs.d_regs[c->vs.sp], r_is_num, r_is_i32,
            c->r_d_slot, bail_direct, right_site);
        mir_emit_word32_binary(
            c->ctx, c->jit_func, c->op, left, right, rd, 0, false, result_site);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_I32;
        c->vs.integer_range[c->vs.sp - 1] = result_range;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
            c->vs.sp - 1, l_type, c->vs.sp, r_type);
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, rl_idx, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, rr_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);
      MIR_item_t imp;
      switch (c->op) {
        case OP_BAND:
          imp = c->imp_band;
          break;
        case OP_BOR:
          imp = c->imp_bor;
          break;
        case OP_BXOR:
          imp = c->imp_bxor;
          break;
        case OP_SHL:
          imp = c->imp_shl;
          break;
        case OP_SHR:
          imp = c->imp_shr;
          break;
        default:
          imp = c->imp_ushr;
          break;
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                   MIR_new_reg_op(c->ctx, rl)));
      mir_call_helper2(c->ctx, c->jit_func, rd,
                       c->helper2_proto, imp, c->r_vm, c->r_js, rl, rr);
      mir_emit_bailout_check(c->ctx, c->jit_func, rd,
                             c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx);
      break;
    }

    case OP_BNOT: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      uint8_t input_type = c->vs.slot_type
                               ? c->vs.slot_type[c->vs.sp - 1]
                               : SLOT_BOXED;
      bool input_is_num = input_type == SLOT_NUM;
      bool input_is_i32 = input_type == SLOT_I32;
      vstack_clear_value_info(&c->vs, c->vs.sp - 1);

      if (specialize) {
        MIR_reg_t value = vstack_top(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int input_site = mir_next_reg_site(&c->reg_site_n);
        MIR_reg_t integer = mir_emit_word32_guard(
            c->ctx, c->jit_func, value, c->vs.d_regs[c->vs.sp - 1],
            input_is_num, input_is_i32,
            c->r_d_slot, bail_direct, input_site);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_XORS,
                                     MIR_new_reg_op(c->ctx, value),
                                     MIR_new_reg_op(c->ctx, integer),
                                     MIR_new_int_op(c->ctx, -1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_EXT32,
                                     MIR_new_reg_op(c->ctx, value),
                                     MIR_new_reg_op(c->ctx, value)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_I32;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp, &c->bailout_ctx,
            c->vs.sp - 1, input_type, -1, SLOT_BOXED);
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                   MIR_new_reg_op(c->ctx, rs)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->helper1_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_bnot),
                                        MIR_new_reg_op(c->ctx, rs),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, rs)));
      mir_emit_bailout_check(c->ctx, c->jit_func, rs,
                             c->r_bailout_val, c->bc_off, c->vs.sp,
                             &c->bailout_ctx);
      break;
    }

    case OP_NOT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->helper1_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_not),
                                        MIR_new_reg_op(c->ctx, rs),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, rs)));
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;
      break;
    }

    case OP_TYPEOF: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->helper1_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_typeof),
                                        MIR_new_reg_op(c->ctx, rs),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, rs)));
      vstack_clear_value_info(&c->vs, c->vs.sp - 1);
      break;
    }

    case OP_IS_PRIMITIVE_TYPE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      mir_emit_primitive_type_test(c->ctx, c->jit_func, vstack_top(&c->vs), c->r_bool, c->ip[1]);
      vstack_clear_value_info(&c->vs, c->vs.sp - 1);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;
      break;
    }

    case OP_VOID: {
      MIR_reg_t rs = vstack_top(&c->vs);
      mir_load_imm(c->ctx, c->jit_func, rs, mkval(kTypeUndefined, 0));
      if (c->vs.slot_type) c->vs.slot_type[c->vs.sp - 1] = SLOT_BOXED;
      vstack_clear_value_info(&c->vs, c->vs.sp - 1);
      if (c->vs.has_const) {
        c->vs.has_const[c->vs.sp - 1] = true;
        c->vs.known_const[c->vs.sp - 1] = mkval(kTypeUndefined, 0);
      }
      break;
    }

    default:
      __builtin_unreachable();
  }
}
