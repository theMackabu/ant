#include "compile.h"

void jit_emit_intrinsics(jit_compile_t *c) {
  switch (c->op) {
    case OP_LOAD_STABLE_BUILTIN: {
      uint8_t stable_kind = sv_get_u8(c->ip + 1);
      if (stable_kind != SV_STABLE_BUILTIN_PROMISE_RESOLVE) {
        c->ok = false;
        break;
      }

      MIR_reg_t stable_this = vstack_push(&c->vs);
      MIR_reg_t stable_fn = vstack_push(&c->vs);
      mir_emit_load_stable_builtin(
          c->ctx, c->jit_func, c->js, c->r_js, stable_kind,
          stable_this, stable_fn, c->r_args_buf,
          c->stable_load_proto, c->imp_load_stable_builtin, c->bc_off);
      jit_emit_throw_if_error(c, stable_fn);
      break;
    }

    case OP_CALL_CHAR_CODE_AT:
    case OP_CALL_ARRAY_INCLUDES: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint16_t call_argc = sv_get_u16(c->ip + 1);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 2) {
        c->ok = false;
        break;
      }

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

      MIR_label_t char_done = NULL;
      if (c->op == OP_CALL_CHAR_CODE_AT && call_argc >= 2) {
        MIR_label_t char_slow = MIR_new_label(c->ctx);
        char_done = MIR_new_label(c->ctx);
        mir_emit_uncurried_char_code_at(c->ctx, c->jit_func, r_call_func, r_arg_arr,
                                        r_call_res, c->r_js, c->r_d_slot, char_slow, char_done, mir_next_reg_site(&c->reg_site_n));
        MIR_append_insn(c->ctx, c->jit_func, char_slow);
      }

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->call_proto),
                                        MIR_new_ref_op(c->ctx, c->op == OP_CALL_CHAR_CODE_AT
                                                                   ? c->imp_call_char_code_at
                                                                   : c->imp_call_array_includes),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc)));

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_call_res);
      if (char_done) MIR_append_insn(c->ctx, c->jit_func, char_done);
      break;
    }

    case OP_CALL_STRING_INTRINSIC: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      ant_string_intrinsic_kind_t kind =
          (ant_string_intrinsic_kind_t)c->ip[1];
      uint16_t call_argc = sv_get_u16(c->ip + 2);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 2) {
        c->ok = false;
        break;
      }

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

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->call_string_intrinsic_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_string_intrinsic),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_int_op(c->ctx, (int64_t)kind),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_reg_op(c->ctx, r_arg_arr),
                                        MIR_new_int_op(c->ctx, (int64_t)call_argc)));

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                        c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_call_res);
      break;
    }

    case OP_CALL_MAP_TEMPLATE: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint32_t desc_idx = sv_get_u32(c->ip + 1);
      const sv_map_template_desc_t *desc =
          sv_map_template_desc_at(c->func, desc_idx);
      if (!desc || c->vs.sp < desc->substitution_count + 2) {
        c->ok = false;
        break;
      }

      MIR_reg_t r_values[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {0};
      for (int i = desc->substitution_count - 1; i >= 0; i--)
        r_values[i] = vstack_pop(&c->vs);
      MIR_reg_t r_call_func = vstack_pop(&c->vs);
      MIR_reg_t r_call_this = vstack_pop(&c->vs);
      MIR_reg_t r_call_res = vstack_push(&c->vs);
      MIR_op_t value_ops[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {
          MIR_new_uint_op(c->ctx, 0),
          MIR_new_uint_op(c->ctx, 0),
          MIR_new_uint_op(c->ctx, 0),
      };
      for (uint8_t i = 0; i < desc->substitution_count; i++)
        value_ops[i] = MIR_new_reg_op(c->ctx, r_values[i]);

      if (sv_map_template_is_canonical_pair_get(desc)) {
        const sv_atom_t *separator = &desc->segments[1];
        MIR_label_t fast_done = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, r_call_this)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 10,
                                          MIR_new_ref_op(c->ctx, c->map_numeric_pair_fast_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_map_numeric_pair_fast),
                                          MIR_new_reg_op(c->ctx, r_call_res),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, r_call_func),
                                          MIR_new_reg_op(c->ctx, r_call_this),
                                          value_ops[0], value_ops[1],
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)separator->str),
                                          MIR_new_uint_op(c->ctx, (uint64_t)separator->len)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, fast_done),
                                     MIR_new_reg_op(c->ctx, r_call_res),
                                     MIR_new_uint_op(c->ctx, (uint64_t)SV_JIT_BAILOUT)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 11,
                                          MIR_new_ref_op(c->ctx, c->call_map_template_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_call_map_template),
                                          MIR_new_reg_op(c->ctx, r_call_res),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, r_call_func),
                                          MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                          value_ops[0], value_ops[1], value_ops[2],
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)desc)));
        MIR_append_insn(c->ctx, c->jit_func, fast_done);
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 11,
                                          MIR_new_ref_op(c->ctx, c->call_map_template_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_call_map_template),
                                          MIR_new_reg_op(c->ctx, r_call_res),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, r_call_func),
                                          MIR_new_reg_op(c->ctx, r_call_this),
                                          value_ops[0], value_ops[1], value_ops[2],
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)desc)));
      }

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_call_res);
      break;
    }

    case OP_TAIL_MAP_TEMPLATE: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      uint32_t desc_idx = sv_get_u32(c->ip + 1);
      int pre_op_sp = c->vs.sp;
      const sv_map_template_desc_t *desc =
          sv_map_template_desc_at(c->func, desc_idx);
      if (!desc || c->vs.sp < desc->substitution_count + 2) {
        c->ok = false;
        break;
      }

      MIR_reg_t r_values[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {0};
      for (int i = desc->substitution_count - 1; i >= 0; i--)
        r_values[i] = vstack_pop(&c->vs);
      MIR_reg_t r_call_func = vstack_pop(&c->vs);
      MIR_reg_t r_call_this = vstack_pop(&c->vs);
      MIR_op_t value_ops[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {
          MIR_new_uint_op(c->ctx, 0),
          MIR_new_uint_op(c->ctx, 0),
          MIR_new_uint_op(c->ctx, 0),
      };
      for (uint8_t i = 0; i < desc->substitution_count; i++)
        value_ops[i] = MIR_new_reg_op(c->ctx, r_values[i]);

      if (sv_map_template_is_canonical_pair_get(desc)) {
        const sv_atom_t *separator = &desc->segments[1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 10,
                                          MIR_new_ref_op(c->ctx, c->map_numeric_pair_fast_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_map_numeric_pair_fast),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, r_call_func),
                                          MIR_new_reg_op(c->ctx, r_call_this),
                                          value_ops[0], value_ops[1],
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)separator->str),
                                          MIR_new_uint_op(c->ctx, (uint64_t)separator->len)));
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 10,
                                          MIR_new_ref_op(c->ctx, c->map_template_fast_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_map_template_fast),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, r_call_func),
                                          MIR_new_reg_op(c->ctx, r_call_this),
                                          value_ops[0], value_ops[1], value_ops[2],
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)desc)));
      }

      mir_emit_bailout_check(
          c->ctx, c->jit_func, c->r_err_tmp, 0,
          c->bc_off, pre_op_sp, &c->bailout_ctx);
      jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      break;
    }

    case OP_RE_EXEC_TRUTHY:
    case OP_RE_EXEC_DISCARD: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      if (c->vs.sp < 3) {
        c->ok = false;
        break;
      }

      MIR_reg_t r_arg = vstack_pop(&c->vs);
      MIR_reg_t r_call_func = vstack_pop(&c->vs);
      MIR_reg_t r_call_this = vstack_pop(&c->vs);
      MIR_reg_t r_call_res = c->op == OP_RE_EXEC_TRUTHY
                                 ? vstack_push(&c->vs)
                                 : c->r_err_tmp;

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->regexp_exec_truthy_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_regexp_exec_truthy),
                                        MIR_new_reg_op(c->ctx, r_call_res),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, r_call_func),
                                        MIR_new_reg_op(c->ctx, r_call_this),
                                        MIR_new_reg_op(c->ctx, r_arg)));

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_call_res);
      break;
    }

    case OP_CALL_STABLE_BUILTIN: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      int stable_kind = c->ip[1];
      uint16_t call_argc = sv_get_u16(c->ip + 2);
      if (call_argc > SV_JIT_ARGS_BUF_CAP ||
          c->vs.sp < (int)call_argc + 2) {
        c->ok = false;
        break;
      }

      MIR_reg_t r_arg_arr = c->r_args_buf;
      MIR_reg_t r_arg0 = 0;
      int call_base = c->vs.sp - (int)call_argc - 2;
      bool known_intrinsic =
          call_base >= 0 && c->vs.has_const &&
          c->vs.has_const[call_base] &&
          c->vs.known_const[call_base] == c->js->sym.promise_ctor &&
          c->vs.has_const[call_base + 1] &&
          c->vs.known_const[call_base + 1] == c->js->sym.promise_resolve;
      bool args_prepared = !(
          stable_kind == SV_STABLE_BUILTIN_PROMISE_RESOLVE &&
          call_argc == 1);
      for (int i = (int)call_argc - 1; i >= 0; i--) {
        MIR_reg_t areg = vstack_pop(&c->vs);
        if (i == 0) r_arg0 = areg;
        if (args_prepared)
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

      mir_emit_call_stable_builtin(
          c->ctx, c->jit_func, c->js, c->r_vm, c->r_js,
          stable_kind, r_call_func, r_call_this,
          r_arg0, r_arg_arr, call_argc, r_call_res,
          c->stable_call_proto, c->imp_call_stable_builtin,
          known_intrinsic, args_prepared, c->bc_off);

      if (c->has_captures) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      }

      jit_emit_throw_if_error(c, r_call_res);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
