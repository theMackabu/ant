#include "compile.h"
#include "silver/feedback.h"

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

void mir_emit_mod_numbers(
    const sv_asm_t *a, int *reg_site, MIR_item_t fmod_proto, MIR_item_t imp_fmod,
    MIR_reg_t l, MIR_reg_t r, MIR_reg_t dst) {
  const double exact_limit = 9007199254740992.0;
  int site = mir_next_reg_site(reg_site);
  MIR_reg_t integer = sv_asm_reg(a, MIR_T_I64, "mod_int", site);
  MIR_reg_t quot = sv_asm_reg(a, MIR_T_D, "mod_quot", site);
  MIR_reg_t rem = sv_asm_reg(a, MIR_T_D, "mod_rem", site);
  sv_asm_label_t nonzero = sv_asm_new_label(a);
  sv_asm_label_t slow = sv_asm_new_label(a);
  sv_asm_label_t done = sv_asm_new_label(a);

  sv_asm_d2i(a, integer, r);
  sv_asm_i64_beq_imm(a, slow, integer, 0);
  sv_asm_i2d(a, quot, integer);
  sv_asm_f64_bne(a, slow, r, quot);
  sv_asm_f64_bge_imm(a, slow, l, exact_limit);
  sv_asm_f64_ble_imm(a, slow, l, -exact_limit);
  sv_asm_f64_div(a, quot, l, r);
  sv_asm_d2i(a, integer, quot);
  sv_asm_i2d(a, quot, integer);
  sv_asm_f64_mul(a, quot, quot, r);
  sv_asm_f64_sub(a, rem, l, quot);
  sv_asm_f64_bne_imm(a, nonzero, rem, 0.0);
  sv_asm_f64_mul_imm(a, rem, l, 0.0);
  sv_asm_bind(a, nonzero);
  sv_asm_f64_mov(a, dst, rem);
  sv_asm_jump(a, done);
  sv_asm_bind(a, slow);
  sv_asm_append(a, MIR_new_call_insn(a->ctx, 5, MIR_new_ref_op(a->ctx, fmod_proto), MIR_new_ref_op(a->ctx, imp_fmod),
                                     MIR_new_reg_op(a->ctx, dst), MIR_new_reg_op(a->ctx, l), MIR_new_reg_op(a->ctx, r)));
  sv_asm_bind(a, done);
}

static void jit_emit_mod_numbers(jit_compile_t *c, MIR_reg_t l, MIR_reg_t r, MIR_reg_t dst) {
  mir_emit_mod_numbers(&c->as, &c->reg_site_n, c->fmod_proto, c->imp_fmod, l, r, dst);
}

void jit_emit_arithmetic(jit_compile_t *c) {
  const sv_asm_t *a = &c->as;
  switch (c->op) {
    case OP_ADD:
    case OP_ADD_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_ADD_NUM);
      bool fb_num_only = force_num_only || jit_speculate_unseen_numeric(c, fb) ||
                         (fb && !(fb & ~(SV_TFB_NUM | SV_TFB_DOUBLE)));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);
      bool fb_str_only = !force_num_only && fb && !(fb & ~SV_TFB_STR) && jit_site_runs_hot(c);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_str_only) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_string_concat_fastpath(c->ctx, c->jit_func, c->r_js, rl, rr, rd, slow, -1, c->bc_off, false);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, done);
      } else if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_add(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_add(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else if (fb_num_only) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int an = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "add_d1", an);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "add_d2", an);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "add_d3", an);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_add(a, fd3, fd1, fd2);
        sv_asm_f64_mov(a, c->vs.d_regs[c->vs.sp - 1], fd3);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int an = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "add_d1", an);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "add_d2", an);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "add_d3", an);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_add(a, fd3, fd1, fd2);
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_SUB:
    case OP_SUB_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_SUB_NUM);
      bool fb_num_only = force_num_only || jit_speculate_unseen_numeric(c, fb) ||
                         (fb && !(fb & ~(SV_TFB_NUM | SV_TFB_DOUBLE)));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_sub,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_sub(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_sub(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else if (fb_num_only) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int sn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "sub_d1", sn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "sub_d2", sn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "sub_d3", sn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_sub(a, fd3, fd1, fd2);
        sv_asm_f64_mov(a, c->vs.d_regs[c->vs.sp - 1], fd3);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int sn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "sub_d1", sn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "sub_d2", sn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "sub_d3", sn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_sub(a, fd3, fd1, fd2);
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_sub, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_MUL:
    case OP_MUL_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_MUL_NUM);
      bool fb_num_only = force_num_only || jit_speculate_unseen_numeric(c, fb) ||
                         (fb && !(fb & ~(SV_TFB_NUM | SV_TFB_DOUBLE)));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mul,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_mul(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_mul(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else if (fb_num_only) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int mn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "mul_d1", mn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "mul_d2", mn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "mul_d3", mn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_mul(a, fd3, fd1, fd2);
        sv_asm_f64_mov(a, c->vs.d_regs[c->vs.sp - 1], fd3);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int mn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "mul_d1", mn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "mul_d2", mn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "mul_d3", mn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_mul(a, fd3, fd1, fd2);
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mul, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_DIV:
    case OP_DIV_NUM: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_DIV_NUM);
      bool fb_num_only = force_num_only || jit_speculate_unseen_numeric(c, fb) ||
                         (fb && !(fb & ~(SV_TFB_NUM | SV_TFB_DOUBLE)));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_div,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_div(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        sv_asm_f64_div(a, fd_dst, fd_dst, fd_r);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else if (fb_num_only) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int dn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "dv_d1", dn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "dv_d2", dn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "dv_d3", dn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_div(a, fd3, fd1, fd2);
        sv_asm_f64_mov(a, c->vs.d_regs[c->vs.sp - 1], fd3);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int dn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "dv_d1", dn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "dv_d2", dn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "dv_d3", dn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        sv_asm_f64_div(a, fd3, fd1, fd2);
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_div, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_MOD: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool fb_num_only = fb && !(fb & ~(SV_TFB_NUM | SV_TFB_DOUBLE));
      bool fb_never_num = fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mod, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
      } else if (fb_num_only) {
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        sv_asm_label_t skip_bail = sv_asm_new_label(a);
        int l_idx = c->vs.sp - 1, r_idx = c->vs.sp;
        if (!l_is_num) {
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
          mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[l_idx], rl, c->r_d_slot);
        }
        if (!r_is_num) {
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
          mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[r_idx], rr, c->r_d_slot);
        }
        jit_emit_mod_numbers(c, c->vs.d_regs[l_idx], c->vs.d_regs[r_idx], c->vs.d_regs[l_idx]);
        c->vs.slot_type[l_idx] = SLOT_NUM;
        sv_asm_jump(a, skip_bail);
        sv_asm_bind(a, bail_direct);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp + 1,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        sv_asm_bind(a, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        sv_asm_label_t slow = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int mn = c->arith_n++;
        MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "mod_d1", mn);
        MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "mod_d2", mn);
        MIR_reg_t fd3 = sv_asm_reg(a, MIR_T_D, "mod_d3", mn);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        jit_emit_mod_numbers(c, fd1, fd2, fd3);
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        sv_asm_jump(a, done);
        sv_asm_bind(a, slow);
        sv_asm_mov(a, c->r_bailout_val, rl);
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mod, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        jit_emit_throw_if_error(c, rd);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_NEG: {
      int top_idx = c->vs.sp - 1;
      bool input_is_num = vstack_prepare_num(
          &c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      vstack_clear_value_info(&c->vs, top_idx);

      if (input_is_num) {
        sv_asm_f64_neg(a, c->vs.d_regs[top_idx], c->vs.d_regs[top_idx]);
        break;
      }

      MIR_reg_t rs = vstack_top(&c->vs);
      sv_asm_label_t slow = sv_asm_new_label(a);
      sv_asm_label_t done = sv_asm_new_label(a);

      mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rs, slow);

      int nn = c->arith_n++;
      MIR_reg_t fd1 = sv_asm_reg(a, MIR_T_D, "neg_d1", nn);
      MIR_reg_t fd2 = sv_asm_reg(a, MIR_T_D, "neg_d2", nn);
      mir_i64_to_d(c->ctx, c->jit_func, fd1, rs, c->r_d_slot);
      sv_asm_f64_neg(a, fd2, fd1);
      mir_d_to_i64(c->ctx, c->jit_func, rs, fd2, c->r_d_slot);
      sv_asm_jump(a, done);

      sv_asm_bind(a, slow);
      mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp, &c->bailout_ctx, -1, SLOT_BOXED, -1, SLOT_BOXED);

      sv_asm_bind(a, done);
      break;
    }

    case OP_UPLUS: {
      int top_idx = c->vs.sp - 1;
      uint8_t input_type = c->vs.slot_type ? c->vs.slot_type[top_idx] : SLOT_BOXED;
      if (input_type == SLOT_NUM || input_type == SLOT_I32) break;

      vstack_ensure_boxed(&c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      vstack_clear_value_info(&c->vs, top_idx);
      MIR_reg_t rs = vstack_top(&c->vs);
      sv_asm_label_t slow = sv_asm_new_label(a);
      sv_asm_label_t done = sv_asm_new_label(a);
      mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rs, slow);
      sv_asm_jump(a, done);
      sv_asm_bind(a, slow);
      sv_asm_mov(a, c->r_bailout_val, rs);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->helper1_proto), MIR_new_ref_op(c->ctx, c->imp_uplus), MIR_new_reg_op(c->ctx, rs), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, rs)));
      mir_emit_bailout_check(c->ctx, c->jit_func, rs, c->r_bailout_val, c->bc_off, c->vs.sp, &c->bailout_ctx);
      sv_asm_bind(a, done);
      break;
    }

    case OP_EXP: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);
      sv_asm_mov(a, c->r_bailout_val, rl);
      mir_call_helper2(c->ctx, c->jit_func, rd, c->helper2_proto, c->imp_exp, c->r_vm, c->r_js, rl, rr);
      mir_emit_bailout_check(c->ctx, c->jit_func, rd, c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx);
      jit_emit_throw_if_error(c, rd);
      break;
    }

    case OP_INC:
    case OP_DEC: {
      int top_idx = c->vs.sp - 1;
      if (c->op == OP_INC && c->vs.slot_type[top_idx] == SLOT_I32) {
        jit_integer_range_t range = c->vs.integer_range[top_idx];
        if (range.known && range.min >= 0 && range.max < UINT32_MAX) {
          MIR_reg_t value = vstack_top(&c->vs);
          sv_asm_i64_add_imm(a, value, value, 1);
          vstack_clear_value_info(&c->vs, top_idx);
          c->vs.integer_range[top_idx] = (jit_integer_range_t){
              .min = range.min + 1, .max = range.max + 1, .known = true};
          break;
        }
      }
      bool input_is_num = vstack_prepare_num(
          &c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);

      if (c->vs.known_func) c->vs.known_func[top_idx] = NULL;
      if (c->vs.has_const) c->vs.has_const[top_idx] = false;

      sv_asm_label_t bailout = input_is_num ? NULL : sv_asm_new_label(a);
      sv_asm_label_t done = input_is_num ? NULL : sv_asm_new_label(a);
      if (!input_is_num)
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rs, bailout);

      vstack_ensure_num(&c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      (c->op == OP_INC ? sv_asm_f64_add : sv_asm_f64_sub)(a, c->vs.d_regs[top_idx], c->vs.d_regs[top_idx], c->r_d_one);

      if (!input_is_num) {
        sv_asm_jump(a, done);
        sv_asm_bind(a, bailout);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp,
                                    &c->bailout_ctx, top_idx, false, -1, false);
        sv_asm_bind(a, done);
      }
      break;
    }

    case OP_POST_INC:
    case OP_POST_DEC: {
      int old_idx = c->vs.sp - 1;
      if (c->vs.slot_type[old_idx] == SLOT_I32) {
        // the old value stays an integer, new one is a double, so no overflow check
        MIR_reg_t rold = vstack_top(&c->vs);
        vstack_push(&c->vs);
        int new_idx = c->vs.sp - 1;
        sv_asm_i2d(a, c->vs.d_regs[new_idx], rold);
        (c->op == OP_POST_INC ? sv_asm_f64_add : sv_asm_f64_sub)(a, c->vs.d_regs[new_idx], c->vs.d_regs[new_idx], c->r_d_one);
        c->vs.slot_type[new_idx] = SLOT_NUM;
        break;
      }
      bool input_is_num = vstack_prepare_num(
          &c->vs, old_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rold = vstack_top(&c->vs);
      vstack_push(&c->vs);
      int new_idx = c->vs.sp - 1;

      vstack_clear_value_info(&c->vs, old_idx);
      vstack_clear_value_info(&c->vs, new_idx);

      sv_asm_label_t bailout = input_is_num ? NULL : sv_asm_new_label(a);
      sv_asm_label_t done = input_is_num ? NULL : sv_asm_new_label(a);
      if (!input_is_num)
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rold, bailout);

      if (!input_is_num)
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[old_idx], rold, c->r_d_slot);
      (c->op == OP_POST_INC ? sv_asm_f64_add : sv_asm_f64_sub)(a, c->vs.d_regs[new_idx], c->vs.d_regs[old_idx], c->r_d_one);
      c->vs.slot_type[new_idx] = SLOT_NUM;

      if (!input_is_num) {
        sv_asm_jump(a, done);
        sv_asm_bind(a, bailout);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp - 1,
                                    &c->bailout_ctx, old_idx, false, -1, false);
        sv_asm_bind(a, done);
      }
      break;
    }

    default:
      __builtin_unreachable();
  }
}

#pragma clang attribute pop
