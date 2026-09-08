#include "compile.h"

void jit_emit_bindings(jit_compile_t *c) {
  switch (c->op) {
    case OP_GET_UPVAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      bool guarded_self_upval = c->self_binding_guards &&
                                c->self_binding_guards[c->bc_off] != 0;

      int un = c->upval_n++;
      char rn_uvs[32], rn_uv[32], rn_loc[32];
      snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d", un);
      snprintf(rn_uv, sizeof(rn_uv), "upv%d", un);
      snprintf(rn_loc, sizeof(rn_loc), "uvloc%d", un);

      MIR_reg_t r_uvs = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uvs);
      MIR_reg_t r_uv = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uv);
      MIR_reg_t r_loc = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_loc);
      int pre_op_sp = c->vs.sp;
      MIR_reg_t dst = vstack_push(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uvs),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_closure_t, upvalues),
                                                  c->r_closure, 0, 1)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uv),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)((int)idx * (int)sizeof(sv_upvalue_t *)),
                                                  r_uvs, 0, 1)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_loc),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_upvalue_t, location),
                                                  r_uv, 0, 1)));

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, r_loc, 0, 1)));
      if (guarded_self_upval) {
        c->vs.known_func[c->vs.sp - 1] = c->func;
        mir_emit_self_binding_guard(
            c->ctx, c->jit_func, dst, c->r_closure, c->r_tmp, c->r_tmp2,
            c->bc_off, pre_op_sp, &c->bailout_ctx);
      } else if (jit_upvalue_is_builder_target(c->func, idx)) {
        MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            c->ctx, c->jit_func, dst, c->r_bool,
            c->r_vm, c->r_js, c->helper1_proto, c->imp_str_read_value);
        jit_emit_throw_if_error(c, dst);
        MIR_append_insn(c->ctx, c->jit_func, sbr_done);
      }
      break;
    }

    case OP_PUT_UPVAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      int un = c->upval_n++;
      char rn_uvs[32], rn_uv[32], rn_loc[32];
      snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d", un);
      snprintf(rn_uv, sizeof(rn_uv), "upv%d", un);
      snprintf(rn_loc, sizeof(rn_loc), "uvloc%d", un);

      MIR_reg_t r_uvs = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uvs);
      MIR_reg_t r_uv = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uv);
      MIR_reg_t r_loc = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_loc);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uvs),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_closure_t, upvalues),
                                                  c->r_closure, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uv),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)((int)idx * (int)sizeof(sv_upvalue_t *)),
                                                  r_uvs, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_loc),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_upvalue_t, location),
                                                  r_uv, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, r_loc, 0, 1),
                                   MIR_new_reg_op(c->ctx, src)));
      mir_emit_upval_write_barrier(c->ctx, c->jit_func,
                                   c->upval_barrier_proto, c->imp_upval_barrier,
                                   c->r_js, r_uv, src, un);
      break;
    }

    case OP_SET_UPVAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      int un = c->upval_n++;
      char rn_uvs[32], rn_uv[32], rn_loc[32];
      snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d", un);
      snprintf(rn_uv, sizeof(rn_uv), "upv%d", un);
      snprintf(rn_loc, sizeof(rn_loc), "uvloc%d", un);

      MIR_reg_t r_uvs = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uvs);
      MIR_reg_t r_uv = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_uv);
      MIR_reg_t r_loc = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_loc);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_top(&c->vs);

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uvs),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_closure_t, upvalues),
                                                  c->r_closure, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_uv),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)((int)idx * (int)sizeof(sv_upvalue_t *)),
                                                  r_uvs, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_loc),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  (MIR_disp_t)offsetof(sv_upvalue_t, location),
                                                  r_uv, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, r_loc, 0, 1),
                                   MIR_new_reg_op(c->ctx, src)));
      mir_emit_upval_write_barrier(c->ctx, c->jit_func,
                                   c->upval_barrier_proto, c->imp_upval_barrier,
                                   c->r_js, r_uv, src, un);
      break;
    }

    case OP_CLOSE_UPVAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      if (c->has_captures && c->n_locals > 0 && c->r_lbuf) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                         MIR_new_reg_op(c->ctx, c->local_regs[i])));
      }
      if (c->has_captured_params && idx < (uint16_t)c->param_count)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_slotbuf, c->r_jit_open_upvalues,
                                    c->captured_params, (int)idx, c->param_count,
                                    mir_next_reg_site(&c->reg_site_n));
      if (c->has_captures)
        mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                    c->close_upval_proto, c->imp_close_upval,
                                    c->r_vm, c->r_lbuf, c->r_jit_open_upvalues, c->captured_locals,
                                    idx >= (uint16_t)c->param_count ? (int)idx - c->param_count : 0, c->n_locals,
                                    mir_next_reg_site(&c->reg_site_n));
      break;
    }

    case OP_GET_GLOBAL:
    case OP_GET_GLOBAL_UNDEF: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      bool known_self_global = c->self_binding_guards &&
                               c->self_binding_guards[c->bc_off] != 0;
      int pre_op_sp = c->vs.sp;
      MIR_reg_t dst = vstack_push(&c->vs);

      MIR_label_t gg_slow = MIR_new_label(c->ctx);
      MIR_label_t gg_done = MIR_new_label(c->ctx);
      bool gg_fast = c->r_ic_epoch_val != 0 &&
                     mir_emit_get_global_ic_fastpath(
                         c->ctx, c->jit_func, c->func, c->bc_off,
                         c->r_js, dst, gg_slow, c->r_ic_epoch_val, c->ip);
      if (gg_fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, gg_done)));
        MIR_append_insn(c->ctx, c->jit_func, gg_slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->gg_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_gg),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      jit_emit_throw_if_error(c, dst);
      if (gg_fast) MIR_append_insn(c->ctx, c->jit_func, gg_done);
      if (known_self_global) {
        c->vs.known_func[c->vs.sp - 1] = c->func;
        mir_emit_self_binding_guard_value_kept(
            c->ctx, c->jit_func, dst, c->r_closure, c->r_tmp, c->r_tmp2,
            c->bc_off, c->sz, pre_op_sp, &c->bailout_ctx);
      }
      break;
    }

    case OP_GET_EVAL_GLOBAL:
    case OP_GET_EVAL_GLOBAL_UNDEF: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->get_eval_global_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_eval_global),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_closure),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off),
                                        MIR_new_int_op(c->ctx, c->op == OP_GET_EVAL_GLOBAL_UNDEF ? 1 : 0)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_SET_NAME: {
      uint32_t atom_idx = sv_get_u32(c->ip + 1);
      if (atom_idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[atom_idx];
      MIR_reg_t fn_val = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->set_name_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_set_name),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, fn_val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len)));
      break;
    }

    case OP_CLOSURE: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->const_count) {
        c->ok = false;
        break;
      }
      const char *name_str = NULL;
      uint32_t name_len = 0;
      int fused_set_name_size = 0;
      uint8_t *next_ip = c->ip + c->sz;
      if (next_ip < c->end && (sv_op_t)*next_ip == OP_SET_NAME) {
        int next_sz = sv_op_size[OP_SET_NAME];
        bool next_has_label = false;
        int next_bc_off = (int)(next_ip - c->func->code);
        for (int i = 0; i < c->lm.count; i++) {
          if (c->lm.entries[i].bc_off == next_bc_off) {
            next_has_label = true;
            break;
          }
        }
        if (!next_has_label && next_ip + next_sz <= c->end) {
          uint32_t name_idx = sv_get_u32(next_ip + 1);
          if (name_idx < (uint32_t)c->func->atom_count) {
            sv_atom_t *name_atom = &c->func->atoms[name_idx];
            name_str = name_atom->str;
            name_len = name_atom->len;
            fused_set_name_size = next_sz;
          }
        }
      }
      MIR_reg_t dst = vstack_push(&c->vs);
      ant_value_t cv = c->func->constants[idx];
      sv_func_t *child = (sv_func_t *)vptr(cv);
      jit_child_kind_t child_kind = classify_child_closure_kind(c->func, child);
      MIR_reg_t r_child_slots = c->r_tmp2;
      int child_slot_base = 0;
      int child_slot_count = 0;
      c->vs.known_func[c->vs.sp - 1] = child;
      switch (child_kind) {
        case JIT_CHILD_PLAIN:
        case JIT_CHILD_INHERITED_ONLY:
          break;
        case JIT_CHILD_PARAM_ONLY:
          r_child_slots = c->r_slotbuf;
          child_slot_count = c->param_count;
          break;
        case JIT_CHILD_LOCAL_ONLY:
          mir_emit_spill_child_captured_locals(
              c->ctx, c->jit_func, c->func, child, c->local_regs, c->n_locals, c->r_lbuf);
          r_child_slots = c->r_lbuf;
          child_slot_base = c->param_count;
          child_slot_count = c->n_locals;
          break;
        case JIT_CHILD_MIXED:
          mir_emit_spill_child_captured_locals(
              c->ctx, c->jit_func, c->func, child, c->local_regs, c->n_locals, c->r_lbuf);
          r_child_slots = c->r_slotbuf;
          child_slot_count = c->slotbuf_count;
          break;
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 14,
                                        MIR_new_ref_op(c->ctx, c->closure_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_closure),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_closure),
                                        MIR_new_reg_op(c->ctx, c->r_this_curr),
                                        MIR_new_reg_op(c->ctx, r_child_slots),
                                        MIR_new_int_op(c->ctx, child_slot_base),
                                        MIR_new_int_op(c->ctx, child_slot_count),
                                        MIR_new_uint_op(c->ctx, (uint64_t)idx),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name_str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)name_len),
                                        c->r_jit_open_upvalues ? MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues) : MIR_new_uint_op(c->ctx, 0)));
      c->sz += fused_set_name_size;
      break;
    }

    case OP_DELETE_EVAL_VAR: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->delete_eval_var_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_delete_eval_var),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_closure),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_IMPORT_DEFAULT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t ns = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->import_default_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_import_default),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, ns)));
      break;
    }

    case OP_IMPORT_NAMED: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t ns = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->import_named_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_import_named),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, ns),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_EXPORT: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->export_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_export),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_closure),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_reg_op(c->ctx, val)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      break;
    }

    case OP_PUT_GLOBAL: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->put_global_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_global),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, c->func->is_strict ? 1 : 0)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      break;
    }

    case OP_PUT_EVAL_GLOBAL: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->put_eval_global_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_eval_global),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_closure),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, c->func->is_strict ? 1 : 0)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
