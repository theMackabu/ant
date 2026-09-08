#include "compile.h"

void jit_emit_calls(jit_compile_t *c) {
  switch (c->op) {
    case OP_TAIL_CALL:
    case OP_CALL: {
      bool is_tail = (c->op == OP_TAIL_CALL);
      uint16_t call_argc = sv_get_u16(c->ip + 1);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 1) {
        c->ok = false;
        break;
      }

      if (!is_tail) {
        sv_func_t *inline_callee = c->vs.known_func[c->vs.sp - call_argc - 1];
        if (!inline_callee)
          inline_callee = sv_tfb_get_call_target(c->func, c->bc_off);
        bool speculative = (inline_callee && !c->vs.known_func[c->vs.sp - call_argc - 1]);
        if (inline_callee && jit_inlineable(inline_callee)) {
          int cn = c->call_n++;
          int inl_arg_base = c->vs.sp - (int)call_argc;
          if (c->jit_try_depth > 0) {
            for (int k = 0; k < inl_arg_base; k++)
              vstack_ensure_boxed(&c->vs, k, c->ctx, c->jit_func, c->r_d_slot);
          } else if (inl_arg_base > 0) {
            vstack_ensure_boxed(&c->vs, inl_arg_base - 1, c->ctx, c->jit_func, c->r_d_slot);
          }
          uint8_t inl_arg_num[SV_JIT_ARGS_BUF_CAP] = {0};
          MIR_reg_t inl_arg_d[SV_JIT_ARGS_BUF_CAP] = {0};
          for (int i = 0; i < (int)call_argc; i++) {
            inl_arg_num[i] = vstack_prepare_num(
                &c->vs, inl_arg_base + i, c->ctx, c->jit_func, c->r_d_slot);
            inl_arg_d[i] = c->vs.d_regs[inl_arg_base + i];
          }

          MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
          for (int i = (int)call_argc - 1; i >= 0; i--)
            inl_arg_regs[i] = vstack_pop(&c->vs);
          MIR_reg_t r_inl_callee = vstack_pop(&c->vs);

          MIR_reg_t r_call_res = vstack_push(&c->vs);

          MIR_label_t inl_slow = MIR_new_label(c->ctx);
          MIR_label_t inl_join = MIR_new_label(c->ctx);

          MIR_reg_t r_inl_cl = 0;
          MIR_reg_t r_inl_new_target = 0;
          MIR_reg_t r_inl_super = 0;
          MIR_reg_t r_spec_guard = 0;
          char inl_this_rn[32], inl_flags_rn[32], inl_bound_rn[32];
          char inl_guard_rn[32];
          snprintf(inl_this_rn, sizeof(inl_this_rn), "inl%d_this", cn);
          snprintf(inl_flags_rn, sizeof(inl_flags_rn), "inl%d_flags", cn);
          snprintf(inl_bound_rn, sizeof(inl_bound_rn), "inl%d_bound", cn);
          snprintf(inl_guard_rn, sizeof(inl_guard_rn), "inl%d_guard", cn);
          MIR_reg_t r_inl_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                  MIR_JSVAL, inl_this_rn);
          MIR_reg_t r_inl_flags = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                   MIR_T_I64, inl_flags_rn);
          MIR_reg_t r_inl_bound = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                   MIR_JSVAL, inl_bound_rn);
          if (speculative) {
            r_spec_guard = MIR_new_func_reg(
                c->ctx, c->jit_func->u.func, MIR_T_I64, inl_guard_rn);
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_URSH,
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_reg_op(c->ctx, r_inl_callee),
                                         MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_BNE,
                                         MIR_new_label_op(c->ctx, inl_slow),
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_uint_op(c->ctx, NANBOX_TFUNC_TAG)));
          }
          {
            char cl_rn[32];
            snprintf(cl_rn, sizeof(cl_rn), "inl%d_cl", cn);
            r_inl_cl = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cl_rn);
            mir_emit_decode_ref(c->ctx, c->jit_func, r_inl_cl, r_inl_callee);
          }
          {
            char nt_rn[32], sup_rn[32];
            snprintf(nt_rn, sizeof(nt_rn), "inl%d_nt", cn);
            snprintf(sup_rn, sizeof(sup_rn), "inl%d_sup", cn);
            r_inl_new_target = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                MIR_JSVAL, nt_rn);
            r_inl_super = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                           MIR_JSVAL, sup_rn);
            mir_load_imm(c->ctx, c->jit_func, r_inl_new_target, mkval(kTypeUndefined, 0));
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, r_inl_super),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)offsetof(sv_closure_t, super_val),
                                                        r_inl_cl, 0, 1)));
          }

          if (speculative) {
            char gf_rn[32];
            snprintf(gf_rn, sizeof(gf_rn), "inl%d_gf", cn);
            MIR_reg_t r_guard_fn = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                    MIR_T_I64, gf_rn);
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, r_guard_fn),
                                         MIR_new_mem_op(c->ctx, MIR_T_P,
                                                        (MIR_disp_t)offsetof(sv_closure_t, func),
                                                        r_inl_cl, 0, 1)));
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_BNE,
                                         MIR_new_label_op(c->ctx, inl_slow),
                                         MIR_new_reg_op(c->ctx, r_guard_fn),
                                         MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)inline_callee)));

            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                        (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                                                        r_inl_cl, 0, 1)));
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_AND,
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_uint_op(c->ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_BNE,
                                         MIR_new_label_op(c->ctx, inl_slow),
                                         MIR_new_reg_op(c->ctx, r_spec_guard),
                                         MIR_new_uint_op(c->ctx, 0)));
          }

          bool inl_uses_this = false;
          {
            uint8_t *tscan = inline_callee->code;
            uint8_t *tend = inline_callee->code + inline_callee->code_len;
            while (tscan < tend) {
              sv_op_t top_ = (sv_op_t)*tscan;
              int tsz = sv_op_size[top_];
              if (tsz == 0) {
                inl_uses_this = true;
                break;
              }
              if (top_ == OP_THIS) {
                inl_uses_this = true;
                break;
              }
              tscan += tsz;
            }
          }
          if (inl_uses_this)
            mir_emit_resolve_call_this(c->ctx, c->jit_func, r_inl_this, r_inl_cl,
                                       c->r_this_curr, r_inl_flags, r_inl_bound);
          else
            mir_load_imm(c->ctx, c->jit_func, r_inl_this, mkval(kTypeUndefined, 0));

          jit_emit_inline_body(
              c->ctx, c->jit_func, c->js, inline_callee,
              inl_arg_regs, (int)call_argc,
              inl_arg_num, inl_arg_d,
              r_call_res, inl_slow, inl_join,
              c->r_bool, &c->r_d_slot, cn, &c->reg_site_n,
              r_inl_cl, r_inl_this, r_inl_new_target, r_inl_super,
              c->r_vm, c->r_js, c->r_ic_epoch_val,
              c->helper2_proto, c->imp_seq, c->imp_sne, c->imp_eq, c->imp_ne,
              c->gf_proto, c->imp_get_field_inline,
              c->special_obj_proto, c->imp_special_obj,
              &c->inline_ext);

          MIR_append_insn(c->ctx, c->jit_func, inl_slow);
          for (int i = 0; i < (int)call_argc; i++)
            if (inl_arg_num[i])
              mir_d_to_i64(c->ctx, c->jit_func, inl_arg_regs[i],
                           inl_arg_d[i], c->r_d_slot);
          for (int i = 0; i < (int)call_argc; i++)
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                        c->r_args_buf, 0, 1),
                                         MIR_new_reg_op(c->ctx, inl_arg_regs[i])));

          char rn_sl_this[32];
          snprintf(rn_sl_this, sizeof(rn_sl_this), "inl%d_slow_t", cn);
          MIR_reg_t r_slow_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                                   MIR_JSVAL, rn_sl_this);
          mir_load_imm(c->ctx, c->jit_func, r_slow_this, mkval(kTypeUndefined, 0));

          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_call_insn(c->ctx, 9,
                                            MIR_new_ref_op(c->ctx, c->call_proto),
                                            MIR_new_ref_op(c->ctx, c->imp_call),
                                            MIR_new_reg_op(c->ctx, r_call_res),
                                            MIR_new_reg_op(c->ctx, c->r_vm),
                                            MIR_new_reg_op(c->ctx, c->r_js),
                                            MIR_new_reg_op(c->ctx, r_inl_callee),
                                            MIR_new_reg_op(c->ctx, r_slow_this),
                                            MIR_new_reg_op(c->ctx, c->r_args_buf),
                                            MIR_new_int_op(c->ctx, (int64_t)call_argc)));

          MIR_append_insn(c->ctx, c->jit_func, inl_join);
          jit_emit_throw_if_error(c, r_call_res);
          break;
        }
      }

      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);

      int cn = c->call_n++;

      char rn_arr[32], rn_this[32], rn_ccl[32], rn_cfn[32], rn_jptr[32], rn_csup[32];
      snprintf(rn_arr, sizeof(rn_arr), "arg_arr%d", cn);
      snprintf(rn_this, sizeof(rn_this), "call_this%d", cn);
      snprintf(rn_ccl, sizeof(rn_ccl), "callee_cl%d", cn);
      snprintf(rn_cfn, sizeof(rn_cfn), "callee_func%d", cn);
      snprintf(rn_jptr, sizeof(rn_jptr), "jit_ptr%d", cn);
      snprintf(rn_csup, sizeof(rn_csup), "callee_super%d", cn);

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

      sv_func_t *call_known = c->vs.known_func
                                  ? c->vs.known_func[c->vs.sp - 1]
                                  : NULL;

      MIR_reg_t r_call_func = vstack_pop(&c->vs);
      MIR_reg_t r_call_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, rn_this);
      mir_load_imm(c->ctx, c->jit_func, r_call_this, mkval(kTypeUndefined, 0));

      MIR_reg_t r_call_res = vstack_push(&c->vs);

      if (call_known == c->func) {
        if (is_tail && c->jit_try_depth == 0) {
          mir_emit_self_tail(c->ctx, c->jit_func, (int)call_argc, c->param_count,
                             c->r_tco_args, r_arg_arr, c->r_args, c->r_argc,
                             c->local_regs, c->n_locals, c->has_captured_slots, c->r_slotbuf, c->captured_params,
                             c->writes_params,
                             c->has_captures,
                             c->captured_locals, c->r_lbuf, c->self_tail_entry);
          break;
        }
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
                                       MIR_new_reg_op(c->ctx, c->r_result),
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
        break;
      }

      MIR_label_t lbl_self_call = MIR_new_label(c->ctx);
      MIR_label_t lbl_super_call = MIR_new_label(c->ctx);
      MIR_label_t lbl_interp_call = MIR_new_label(c->ctx);
      MIR_label_t lbl_call_done = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_super_call),
                                   MIR_new_reg_op(c->ctx, r_call_func),
                                   MIR_new_reg_op(c->ctx, c->r_super_val)));

      MIR_reg_t r_callee_cl = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_ccl);
      mir_emit_get_closure(c->ctx, c->jit_func, r_callee_cl, r_call_func,
                           c->r_bool, lbl_interp_call);

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
                                   MIR_new_label_op(c->ctx, lbl_interp_call),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, 0)));

      MIR_reg_t r_callee_fn = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_cfn);
      MIR_reg_t r_callee_super = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, rn_csup);
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
                                   MIR_new_label_op(c->ctx, lbl_interp_call),
                                   MIR_new_reg_op(c->ctx, r_callee_fn),
                                   MIR_new_int_op(c->ctx, 0)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, lbl_self_call),
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
                                   MIR_new_label_op(c->ctx, lbl_interp_call),
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
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_call_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_self_call);
      if (is_tail && c->jit_try_depth == 0) {
        mir_emit_self_tail(c->ctx, c->jit_func, (int)call_argc, c->param_count,
                           c->r_tco_args, r_arg_arr, c->r_args, c->r_argc,
                           c->local_regs, c->n_locals, c->has_captured_slots, c->r_slotbuf, c->captured_params,
                           c->writes_params,
                           c->has_captures,
                           c->captured_locals, c->r_lbuf, c->self_tail_entry);
      } else {
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
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_call_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_super_call);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_call_out_this, 0, 1),
                                   MIR_new_reg_op(c->ctx, c->r_this_curr)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 12,
                                        MIR_new_ref_op(c->ctx, c->call_method_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_method),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, c->r_this_curr),
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
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl_call_done)));

      MIR_append_insn(c->ctx, c->jit_func, lbl_interp_call);
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

      MIR_append_insn(c->ctx, c->jit_func, lbl_call_done);
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
      break;
    }

    case OP_CHECK_CTOR: {
      MIR_label_t has_new_target = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, has_new_target),
                                   MIR_new_reg_op(c->ctx, c->r_new_target),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->throw_error_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_throw_error),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_uint_op(c->ctx,
                                                        (uint64_t)(uintptr_t)SV_CLASS_CTOR_CALL_ERROR),
                                        MIR_new_uint_op(c->ctx, sizeof(SV_CLASS_CTOR_CALL_ERROR) - 1),
                                        MIR_new_int_op(c->ctx, JS_ERR_TYPE)));
      jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      MIR_append_insn(c->ctx, c->jit_func, has_new_target);
      break;
    }

    case OP_NEW: {
      uint16_t new_argc = sv_get_u16(c->ip + 1);
      if (new_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)new_argc + 2) {
        c->ok = false;
        break;
      }

      for (int i = 0; i < (int)new_argc + 2; i++)
        vstack_ensure_boxed(&c->vs, c->vs.sp - 1 - i, c->ctx, c->jit_func, c->r_d_slot);
      for (int i = (int)new_argc - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    c->r_args_buf, 0, 1),
                                     MIR_new_reg_op(c->ctx, areg)));
      }

      MIR_reg_t r_ctor_target = vstack_pop(&c->vs);
      MIR_reg_t r_new_func = vstack_pop(&c->vs);
      MIR_reg_t r_new_res = vstack_push(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->new_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_new),
                                        MIR_new_reg_op(c->ctx, r_new_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_new_func),
                                        MIR_new_reg_op(c->ctx, r_ctor_target),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf),
                                        MIR_new_int_op(c->ctx, (int64_t)new_argc)));

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_new_res);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
