#include "compile.h"

void jit_emit_literals(jit_compile_t *c) {
  switch (c->op) {
    case OP_CONST_I8: {
      double d = (double)(int8_t)sv_get_i8(c->ip + 1);
      union {
        double d;
        uint64_t u;
      } u = {d};
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_load_imm(c->ctx, c->jit_func, dst, u.u);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DMOV,
                                   MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                   MIR_new_double_op(c->ctx, d)));
      jit_emit_integer_constant(c->ctx, c->jit_func, &c->vs, dst, d);
      break;
    }

    case OP_CONST: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->const_count) {
        c->ok = false;
        break;
      }
      ant_value_t cv = c->func->constants[idx];
      MIR_reg_t dst = vstack_push(&c->vs);
      if (jit_const_is_heap(cv))
        mir_load_const_slot(c->ctx, c->jit_func, dst, &c->func->constants[idx]);
      else {
        mir_load_imm(c->ctx, c->jit_func, dst, cv);
        if (vtype(cv) == kTypeNumber) {
          union {
            uint64_t u;
            double d;
          } u = {cv};
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_DMOV,
                                       MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                       MIR_new_double_op(c->ctx, u.d)));
          c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
          jit_emit_integer_constant(c->ctx, c->jit_func, &c->vs, dst, u.d);
        }
      }
      break;
    }

    case OP_CONST8: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->func->const_count) {
        c->ok = false;
        break;
      }
      ant_value_t cv = c->func->constants[idx];
      MIR_reg_t dst = vstack_push(&c->vs);
      if (jit_const_is_heap(cv))
        mir_load_const_slot(c->ctx, c->jit_func, dst, &c->func->constants[idx]);
      else {
        mir_load_imm(c->ctx, c->jit_func, dst, cv);
        if (vtype(cv) == kTypeNumber) {
          union {
            uint64_t u;
            double d;
          } u = {cv};
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_DMOV,
                                       MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                       MIR_new_double_op(c->ctx, u.d)));
          c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
          jit_emit_integer_constant(c->ctx, c->jit_func, &c->vs, dst, u.d);
        }
      }
      break;
    }

    case OP_UNDEF:
      mir_load_imm(c->ctx, c->jit_func, vstack_push_const(&c->vs, mkval(kTypeUndefined, 0)), mkval(kTypeUndefined, 0));
      break;
    case OP_NULL:
      mir_load_imm(c->ctx, c->jit_func, vstack_push_const(&c->vs, mkval(kTypeNull, 0)), mkval(kTypeNull, 0));
      break;
    case OP_TRUE:
      mir_load_imm(c->ctx, c->jit_func, vstack_push_const(&c->vs, js_true), js_true);
      break;
    case OP_FALSE:
      mir_load_imm(c->ctx, c->jit_func, vstack_push_const(&c->vs, js_false), js_false);
      break;

    case OP_THIS: {
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, c->r_this_curr)));
      break;
    }

    case OP_SPECIAL_OBJ: {
      uint8_t which = sv_get_u8(c->ip + 1);
      MIR_reg_t dst = vstack_push(&c->vs);
      if (which == 0 && c->forward_arguments) {
        mir_load_imm(c->ctx, c->jit_func, dst, js_mkundef());
      } else if (which == 0 && c->func->is_strict) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 7,
                                          MIR_new_ref_op(c->ctx, c->strict_arguments_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_strict_arguments),
                                          MIR_new_reg_op(c->ctx, dst),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, c->r_args),
                                          MIR_new_reg_op(c->ctx, c->r_argc)));
        jit_emit_throw_if_error(c, dst);
      } else if (which == 1) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_reg_op(c->ctx, c->r_new_target)));
      } else if (which == 2) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_reg_op(c->ctx, c->r_super_val)));
      } else if (which == 3) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 6,
                                          MIR_new_ref_op(c->ctx, c->special_obj_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_special_obj),
                                          MIR_new_reg_op(c->ctx, dst),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_int_op(c->ctx, (int64_t)which)));
      } else
        mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      break;
    }

    case OP_OBJECT: {
      sv_obj_site_cache_t *site = sv_obj_site_for_offset(
          c->func, (uint32_t)c->bc_off);
      MIR_reg_t dst = vstack_push(&c->vs);
      int literal_span = jit_constant_object_span(c->func, site, &c->lm);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->object_proto),
                                        MIR_new_ref_op(c->ctx, literal_span ? c->imp_object_template : c->imp_object),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)site)));
      jit_emit_throw_if_error(c, dst);
      if (literal_span) c->ip += literal_span;
      break;
    }

    case OP_ARRAY: {
      uint16_t n = sv_get_u16(c->ip + 1);
      if (c->vs.sp < (int)n) {
        c->ok = false;
        break;
      }
      for (int i = 0; i < (int)n; i++)
        vstack_ensure_boxed(&c->vs, c->vs.sp - 1 - i, c->ctx, c->jit_func, c->r_d_slot);
      for (int i = (int)n - 1; i >= 0; i--) {
        MIR_reg_t elem = vstack_pop(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    c->r_args_buf, 0, 1),
                                     MIR_new_reg_op(c->ctx, elem)));
      }
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->array_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_array),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf),
                                        MIR_new_int_op(c->ctx, (int64_t)n)));
      break;
    }

    case OP_REGEXP: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t flags = vstack_pop(&c->vs);
      MIR_reg_t pattern = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->regexp_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_regexp),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, pattern),
                                        MIR_new_reg_op(c->ctx, flags)));
      break;
    }

    default:
      __builtin_unreachable();
  }
}
