#include "compile.h"

static void jit_emit_forward_arguments_call(jit_compile_t *c, bool is_tail) {
  (void)vstack_pop(&c->vs); // Deferred arguments object.
  MIR_reg_t receiver = vstack_pop(&c->vs);
  MIR_reg_t apply = vstack_pop(&c->vs);
  MIR_reg_t target = vstack_pop(&c->vs);
  MIR_reg_t result = vstack_push(&c->vs);
  MIR_label_t slow = MIR_new_label(c->ctx), done = MIR_new_label(c->ctx);
  MIR_reg_t meta = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "forward_meta");
  MIR_reg_t closure = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "forward_closure");
  MIR_reg_t function = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "forward_function");
  MIR_reg_t code = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "forward_code");
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_URSH,
    MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_reg_op(c->ctx, apply),
    MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE,
    MIR_new_label_op(c->ctx, slow), MIR_new_reg_op(c->ctx, c->r_bool),
    MIR_new_uint_op(c->ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeBuiltin)));
  mir_emit_decode_ref(c->ctx, c->jit_func, meta, apply);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
    MIR_new_reg_op(c->ctx, c->r_bool),
    MIR_new_mem_op(c->ctx, MIR_T_P, offsetof(ant_cfunc_meta_t, fn), meta, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE,
    MIR_new_label_op(c->ctx, slow), MIR_new_reg_op(c->ctx, c->r_bool),
    MIR_new_uint_op(c->ctx, (uintptr_t)builtin_function_apply)));
  mir_emit_get_closure(c->ctx, c->jit_func, closure, target, c->r_bool, slow);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
    MIR_new_reg_op(c->ctx, c->r_bool),
    MIR_new_mem_op(c->ctx, MIR_T_U32, offsetof(sv_closure_t, call_flags), closure, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE,
    MIR_new_label_op(c->ctx, slow), MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
    MIR_new_reg_op(c->ctx, function),
    MIR_new_mem_op(c->ctx, MIR_T_P, offsetof(sv_closure_t, func), closure, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ,
    MIR_new_label_op(c->ctx, slow), MIR_new_reg_op(c->ctx, function), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
    MIR_new_reg_op(c->ctx, code),
    MIR_new_mem_op(c->ctx, MIR_T_P, offsetof(sv_func_t, jit_code), function, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ,
    MIR_new_label_op(c->ctx, slow), MIR_new_reg_op(c->ctx, code), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_call_insn(c->ctx, 10,
    MIR_new_ref_op(c->ctx, c->self_proto), MIR_new_reg_op(c->ctx, code),
    MIR_new_reg_op(c->ctx, result), MIR_new_reg_op(c->ctx, c->r_vm),
    MIR_new_reg_op(c->ctx, receiver), MIR_new_uint_op(c->ctx, js_mkundef()),
    MIR_new_uint_op(c->ctx, js_mkundef()), MIR_new_reg_op(c->ctx, c->r_args),
    MIR_new_reg_op(c->ctx, c->r_argc), MIR_new_reg_op(c->ctx, closure)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
  MIR_append_insn(c->ctx, c->jit_func, slow);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_call_insn(c->ctx, 10,
    MIR_new_ref_op(c->ctx, c->forward_arguments_proto),
    MIR_new_ref_op(c->ctx, c->imp_forward_arguments),
    MIR_new_reg_op(c->ctx, result), MIR_new_reg_op(c->ctx, c->r_vm),
    MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, apply),
    MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, receiver),
    MIR_new_reg_op(c->ctx, c->r_args), MIR_new_reg_op(c->ctx, c->r_argc)));
  MIR_append_insn(c->ctx, c->jit_func, done);
  jit_emit_throw_if_error(c, result);
  if (is_tail) jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, result));
}

void jit_emit_methods(jit_compile_t *c) {
  switch (c->op) {
    case OP_CALL_SUPER: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint16_t call_argc = sv_get_u16(c->ip + 1);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 3) {
        c->ok = false;
        break;
      }

      for (int i = (int)call_argc - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    c->r_args_buf, 0, 1),
                                     MIR_new_reg_op(c->ctx, areg)));
      }

      MIR_reg_t r_super_nt = vstack_pop(&c->vs);
      MIR_reg_t r_super_fn = vstack_pop(&c->vs);
      MIR_reg_t r_super_this = vstack_pop(&c->vs);
      MIR_reg_t r_super_res = vstack_push(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_call_out_this, 0, 1),
                                   MIR_new_reg_op(c->ctx, r_super_this)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 12,
                                        MIR_new_ref_op(c->ctx, c->call_method_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_method),
                                        MIR_new_reg_op(c->ctx, r_super_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_super_fn),
                                        MIR_new_reg_op(c->ctx, r_super_this),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc),
                                        MIR_new_reg_op(c->ctx, r_super_fn),
                                        MIR_new_reg_op(c->ctx, r_super_nt),
                                        MIR_new_reg_op(c->ctx, c->r_call_out_this)));

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_super_res);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_this_curr),
                                   MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_call_out_this, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_super_res),
                                   MIR_new_reg_op(c->ctx, c->r_this_curr)));
      break;
    }

    case OP_TAIL_CALL_METHOD:
    case OP_CALL_METHOD: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      bool is_tail = (c->op == OP_TAIL_CALL_METHOD);
      uint16_t call_argc = sv_get_u16(c->ip + 1);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 2) {
        c->ok = false;
        break;
      }

      if (c->forward_arguments) {
        jit_emit_forward_arguments_call(c, is_tail);
        break;
      }

      MIR_label_t cm_devirt_slow = NULL;
      MIR_label_t cm_devirt_join = NULL;

      if (!is_tail) {
        sv_func_t *inline_callee = sv_tfb_get_call_target(c->func, c->bc_off);
        if (inline_callee && jit_inlineable(inline_callee)) {
          int mcn = c->call_n++;
          cm_devirt_slow = MIR_new_label(c->ctx);
          cm_devirt_join = MIR_new_label(c->ctx);

          MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
          for (int i = 0; i < (int)call_argc; i++)
            inl_arg_regs[i] = c->vs.regs[c->vs.sp - call_argc + i];
          MIR_reg_t r_inl_callee = c->vs.regs[c->vs.sp - call_argc - 1];
          MIR_reg_t r_inl_recv = c->vs.regs[c->vs.sp - call_argc - 2];
          MIR_reg_t r_inl_res = c->vs.regs[c->vs.sp - call_argc - 2];

          char micl_rn[32], mithis_rn[32], miflags_rn[32], mibound_rn[32];
          char mint_rn[32], misup_rn[32], mitag_rn[32], mifn_rn[32];
          snprintf(micl_rn, sizeof(micl_rn), "mi%d_cl", mcn);
          snprintf(mithis_rn, sizeof(mithis_rn), "mi%d_this", mcn);
          snprintf(miflags_rn, sizeof(miflags_rn), "mi%d_flags", mcn);
          snprintf(mibound_rn, sizeof(mibound_rn), "mi%d_bound", mcn);
          snprintf(mint_rn, sizeof(mint_rn), "mi%d_nt", mcn);
          snprintf(misup_rn, sizeof(misup_rn), "mi%d_sup", mcn);
          snprintf(mitag_rn, sizeof(mitag_rn), "mi%d_tag", mcn);
          snprintf(mifn_rn, sizeof(mifn_rn), "mi%d_fn", mcn);

          MIR_reg_t r_inl_cl = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, micl_rn);
          MIR_reg_t r_inl_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, mithis_rn);
          MIR_reg_t r_inl_flags = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, miflags_rn);
          MIR_reg_t r_inl_bound = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, mibound_rn);
          MIR_reg_t r_inl_nt = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, mint_rn);
          MIR_reg_t r_inl_sup = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, misup_rn);
          MIR_reg_t r_inl_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, mitag_rn);
          MIR_reg_t r_inl_gfn = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, mifn_rn);

          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BEQ,
                                       MIR_new_label_op(c->ctx, cm_devirt_slow),
                                       MIR_new_reg_op(c->ctx, r_inl_callee),
                                       MIR_new_reg_op(c->ctx, c->r_super_val)));

          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_URSH,
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_reg_op(c->ctx, r_inl_callee),
                                       MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, cm_devirt_slow),
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_uint_op(c->ctx, NANBOX_TFUNC_TAG)));

          mir_emit_decode_ref(c->ctx, c->jit_func, r_inl_cl, r_inl_callee);

          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_inl_gfn),
                                       MIR_new_mem_op(c->ctx, MIR_T_P,
                                                      (MIR_disp_t)offsetof(sv_closure_t, func),
                                                      r_inl_cl, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, cm_devirt_slow),
                                       MIR_new_reg_op(c->ctx, r_inl_gfn),
                                       MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)inline_callee)));

          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                      (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                                                      r_inl_cl, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_AND,
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_uint_op(c->ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, cm_devirt_slow),
                                       MIR_new_reg_op(c->ctx, r_inl_tag),
                                       MIR_new_uint_op(c->ctx, 0)));

          mir_load_imm(c->ctx, c->jit_func, r_inl_nt, mkval(kTypeUndefined, 0));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_inl_sup),
                                       MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                      (MIR_disp_t)offsetof(sv_closure_t, super_val),
                                                      r_inl_cl, 0, 1)));

          mir_emit_resolve_call_this(
              c->ctx, c->jit_func, r_inl_this, r_inl_cl,
              r_inl_recv, r_inl_flags, r_inl_bound);

          jit_emit_inline_body(
              c->ctx, c->jit_func, c->js, inline_callee,
              inl_arg_regs, (int)call_argc,
              NULL, NULL,
              r_inl_res, cm_devirt_slow, cm_devirt_join,
              c->r_bool, &c->r_d_slot, mcn, &c->reg_site_n,
              r_inl_cl, r_inl_this, r_inl_nt, r_inl_sup,
              c->r_vm, c->r_js, c->r_ic_epoch_val,
              c->helper2_proto, c->imp_seq, c->imp_sne, c->imp_eq, c->imp_ne,
              c->gf_proto, c->imp_get_field_inline,
              c->special_obj_proto, c->imp_special_obj,
              &c->inline_ext);

          MIR_append_insn(c->ctx, c->jit_func, cm_devirt_slow);
        }
      }

      int cn = c->call_n++;

      char rn_arr[32], rn_ccl[32], rn_cfn[32], rn_jptr[32], rn_sup[32];
      snprintf(rn_arr, sizeof(rn_arr), "cm_arr%d", cn);
      snprintf(rn_ccl, sizeof(rn_ccl), "cm_cl%d", cn);
      snprintf(rn_cfn, sizeof(rn_cfn), "cm_fn%d", cn);
      snprintf(rn_jptr, sizeof(rn_jptr), "cm_jptr%d", cn);
      snprintf(rn_sup, sizeof(rn_sup), "cm_sup%d", cn);

      MIR_reg_t r_arg_arr = c->r_args_buf;

      for (int i = (int)call_argc - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    r_arg_arr, 0, 1),
                                     MIR_new_reg_op(c->ctx, areg)));
      }

      MIR_reg_t r_call_func = vstack_pop(&c->vs);
      MIR_reg_t r_call_this = vstack_pop(&c->vs);
      MIR_reg_t r_call_res = vstack_push(&c->vs);
      MIR_reg_t r_callee_super = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, rn_sup);

      MIR_label_t lbl_cm_self = MIR_new_label(c->ctx);
      MIR_label_t lbl_cm_super = MIR_new_label(c->ctx);
      MIR_label_t lbl_cm_interp = MIR_new_label(c->ctx);
      MIR_label_t lbl_cm_done = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_cm_super),
                                   MIR_new_reg_op(c->ctx, r_call_func),
                                   MIR_new_reg_op(c->ctx, c->r_super_val)));

      MIR_reg_t r_callee_cl = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_ccl);
      mir_emit_get_closure(c->ctx, c->jit_func, r_callee_cl, r_call_func,
                           c->r_bool, lbl_cm_interp);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                  (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                                                  r_callee_cl, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_AND,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, lbl_cm_interp),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, 0)));

      MIR_reg_t r_callee_fn = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_cfn);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_callee_fn),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_closure_t, func),
                                                  r_callee_cl, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_callee_super),
                                   MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                  (MIR_disp_t)offsetof(sv_closure_t, super_val),
                                                  r_callee_cl, 0, 1)));
      mir_emit_resolve_call_this(c->ctx, c->jit_func, r_call_this, r_callee_cl,
                                 r_call_this, c->r_bool, c->r_tmp2);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_cm_interp),
                                   MIR_new_reg_op(c->ctx, r_callee_fn),
                                   MIR_new_int_op(c->ctx, 0)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_cm_self),
                                   MIR_new_reg_op(c->ctx, r_callee_cl),
                                   MIR_new_reg_op(c->ctx, c->r_closure)));

      MIR_reg_t r_jit_ptr = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_jptr);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_jit_ptr),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_func_t, jit_code),
                                                  r_callee_fn, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_cm_interp),
                                   MIR_new_reg_op(c->ctx, r_jit_ptr),
                                   MIR_new_int_op(c->ctx, 0)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->self_proto),
                                        MIR_new_reg_op(c->ctx, r_jit_ptr),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)),
                                        MIR_new_reg_op(c->ctx, r_callee_super),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc),
                                        MIR_new_reg_op(c->ctx, r_callee_cl)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_cm_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_cm_self);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->self_proto),
                                        MIR_new_ref_op(c->ctx, c->jit_func),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)),
                                        MIR_new_reg_op(c->ctx, c->r_super_val),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc),
                                        MIR_new_reg_op(c->ctx, c->r_closure)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_cm_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_cm_super);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_call_out_this, 0, 1),
                                   MIR_new_reg_op(c->ctx, r_call_this)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 12,
                                        MIR_new_ref_op(c->ctx, c->call_method_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_method),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc),
                                        MIR_new_reg_op(c->ctx, c->r_super_val),
                                        MIR_new_reg_op(c->ctx, c->r_new_target),
                                        MIR_new_reg_op(c->ctx, c->r_call_out_this)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_this_curr),
                                   MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_call_out_this, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_cm_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_cm_interp);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->call_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc)));
      MIR_append_insn(c->ctx, c->jit_func, lbl_cm_done);
      if (is_tail && c->jit_try_depth == 0) {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_call_res));
      } else {
        if (c->has_captures) {
          for (int i = 0; i < c->n_locals; i++)
            if (c->captured_locals[i])
              MIR_append_insn(c->ctx, c->jit_func,
                              MIR_new_insn(c->ctx, MIR_MOV,
                                           MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                           MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        }
        if (c->jit_try_depth > 0) {
          jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
          MIR_label_t no_err = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_URSH,
                                       MIR_new_reg_op(c->ctx, c->r_bool),
                                       MIR_new_reg_op(c->ctx, r_call_res),
                                       MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, no_err),
                                       MIR_new_reg_op(c->ctx, c->r_bool),
                                       MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                       MIR_new_reg_op(c->ctx, r_call_res)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP,
                                       MIR_new_label_op(c->ctx, h->catch_label)));
          MIR_append_insn(c->ctx, c->jit_func, no_err);
        } else {
          MIR_label_t no_err = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_URSH,
                                       MIR_new_reg_op(c->ctx, c->r_bool),
                                       MIR_new_reg_op(c->ctx, r_call_res),
                                       MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, no_err),
                                       MIR_new_reg_op(c->ctx, c->r_bool),
                                       MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
          jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_call_res));
          MIR_append_insn(c->ctx, c->jit_func, no_err);
        }
        if (is_tail) {
          jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_call_res));
        }
      }
      if (cm_devirt_join) {
        MIR_append_insn(c->ctx, c->jit_func, cm_devirt_join);
        MIR_reg_t r_join_res = c->vs.regs[c->vs.sp - 1];
        jit_emit_throw_if_error(c, r_join_res);
      }
      break;
    }

    case OP_CALL_CALL: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint8_t cc_n1 = c->ip[1];
      uint8_t cc_n2 = c->ip[2];
      int cc_total = 1 + (int)cc_n1 + (int)cc_n2;
      if (cc_total > SV_JIT_ARGS_BUF_CAP || c->vs.sp < cc_total) {
        c->ok = false;
        break;
      }

      for (int i = cc_total - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    c->r_args_buf, 0, 1),
                                     MIR_new_reg_op(c->ctx, areg)));
      }

      MIR_reg_t r_cc_res = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->call_call_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_call),
                                        MIR_new_reg_op(c->ctx, r_cc_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf),
                                        MIR_new_int_op(c->ctx, (int64_t)cc_n1),
                                        MIR_new_int_op(c->ctx, (int64_t)cc_n2)));

      jit_emit_throw_if_error(c, r_cc_res);
      break;
    }

    case OP_CALL_CALL_SLOT: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint16_t cc_slot_idx = sv_get_u16(c->ip + 1);
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }

      MIR_reg_t r_cc_arg1 = vstack_pop(&c->vs);
      MIR_reg_t r_cc_func = vstack_pop(&c->vs);
      char cc_slot_name[32];
      snprintf(cc_slot_name, sizeof(cc_slot_name), "cc_slot_%d", c->bc_off);
      MIR_reg_t r_cc_slot = MIR_new_func_reg(
          c->ctx, c->jit_func->u.func, MIR_T_I64, cc_slot_name);

      if ((int)cc_slot_idx < c->param_count) {
        uint16_t idx = cc_slot_idx;
        bool slot_backed = c->writes_params ||
                           (c->has_captured_params && c->captured_params && c->captured_params[idx]);
        if (slot_backed) {
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_ADD,
                                       MIR_new_reg_op(c->ctx, r_cc_slot),
                                       MIR_new_reg_op(c->ctx, c->r_slotbuf),
                                       MIR_new_int_op(c->ctx,
                                                      (int64_t)idx * (int64_t)sizeof(ant_value_t))));
        } else {
          char cc_value_name[32];
          snprintf(cc_value_name, sizeof(cc_value_name), "cc_value_%d", c->bc_off);
          MIR_reg_t r_cc_value = MIR_new_func_reg(
              c->ctx, c->jit_func->u.func, MIR_JSVAL, cc_value_name);
          MIR_label_t arg_in_range = MIR_new_label(c->ctx);
          MIR_label_t arg_done = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, arg_in_range),
                                       MIR_new_reg_op(c->ctx, c->r_argc),
                                       MIR_new_int_op(c->ctx, (int64_t)idx)));
          mir_load_imm(c->ctx, c->jit_func, r_cc_value, mkval(kTypeUndefined, 0));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, arg_done)));
          MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_cc_value),
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                      (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                      c->r_args, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func, arg_done);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, c->r_args_buf, 0, 1),
                                       MIR_new_reg_op(c->ctx, r_cc_value)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_cc_slot),
                                       MIR_new_reg_op(c->ctx, c->r_args_buf)));
        }
      } else {
        uint16_t idx = (uint16_t)(cc_slot_idx - (uint16_t)c->param_count);
        if (idx >= (uint16_t)c->n_locals) {
          c->ok = false;
          break;
        }
        bool slot_backed = c->has_captures && c->captured_locals && c->captured_locals[idx];
        if (slot_backed) {
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_ADD,
                                       MIR_new_reg_op(c->ctx, r_cc_slot),
                                       MIR_new_reg_op(c->ctx, c->r_lbuf),
                                       MIR_new_int_op(c->ctx,
                                                      (int64_t)idx * (int64_t)sizeof(ant_value_t))));
        } else {
          if (c->dnum_locals && c->dnum_locals[idx])
            mir_d_to_i64(
                c->ctx, c->jit_func, c->local_regs[idx], c->local_d_regs[idx], c->r_d_slot);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, c->r_args_buf, 0, 1),
                                       MIR_new_reg_op(c->ctx, c->local_regs[idx])));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_cc_slot),
                                       MIR_new_reg_op(c->ctx, c->r_args_buf)));
        }
      }

      MIR_reg_t r_cc_res = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->call_call_slot_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_call_slot),
                                        MIR_new_reg_op(c->ctx, r_cc_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_cc_func),
                                        MIR_new_reg_op(c->ctx, r_cc_arg1),
                                        MIR_new_reg_op(c->ctx, r_cc_slot)));

      jit_emit_throw_if_error(c, r_cc_res);
      break;
    }

    case OP_APPLY: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint16_t apply_argc = sv_get_u16(c->ip + 1);
      if (apply_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)apply_argc + 2) {
        c->ok = false;
        break;
      }

      int cn = c->call_n++;
      MIR_reg_t r_arg_arr = c->r_args_buf;

      for (int i = (int)apply_argc - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    r_arg_arr, 0, 1),
                                     MIR_new_reg_op(c->ctx, areg)));
      }

      MIR_reg_t r_apply_this = vstack_pop(&c->vs);
      MIR_reg_t r_apply_func = vstack_pop(&c->vs);
      MIR_reg_t r_apply_res = vstack_push(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->call_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_apply),
                                        MIR_new_reg_op(c->ctx, r_apply_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_apply_func),
                                        MIR_new_reg_op(c->ctx, r_apply_this),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)apply_argc)));

      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_label_t no_err = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_URSH,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, r_apply_res),
                                     MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, no_err),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, r_apply_res)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
        MIR_append_insn(c->ctx, c->jit_func, no_err);
      } else {
        MIR_label_t no_err = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_URSH,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, r_apply_res),
                                     MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, no_err),
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_apply_res));
        MIR_append_insn(c->ctx, c->jit_func, no_err);
      }
      (void)cn;
      break;
    }

    default:
      __builtin_unreachable();
  }
}
