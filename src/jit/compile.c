#include "compile.h"

sv_jit_func_t sv_jit_compile(ant_t *js, sv_func_t *func, sv_closure_t *hint_closure) {
  jit_compile_t compile = {
      .js = js,
      .func = func,
      .hint_closure = hint_closure,
  };
  jit_compile_t *c = &compile;
  if (c->func->jit_compile_failed || c->func->jit_compiling) return NULL;
  if (c->func->jit_code == NULL && c->func->jit_compiled_tfb_ver != 0 &&
      c->func->tfb_version == c->func->jit_compiled_tfb_ver) {
    c->func->jit_compile_failed = true;
    return NULL;
  }

  if (!jit_is_eligible(c->func)) {
    c->func->jit_compile_failed = true;
    return NULL;
  }

  c->func->jit_compiling = true;
  c->jc = c->js->jit_ctx;

  if (!c->jc) {
    sv_jit_init(c->js);
    c->jc = c->js->jit_ctx;
  }

  if (!c->jc) {
    c->func->jit_compiling = false;
    return NULL;
  }

  jit_load_externals_once(c->jc);
  bool jit_compile_hot = c->func->jit_loop_hot ||
                         c->func->back_edge_count >= JIT_HOT_COMPILE_BACKEDGE_THRESHOLD;
  c->ctx = jit_compile_hot ? c->jc->ctx_hot : c->jc->ctx;

  c->forward_arguments = jit_can_forward_arguments(func);
  if (!jit_setup_frame(c)) return NULL;

  while (c->ip < c->end) {
    c->bc_off = (int)(c->ip - c->func->code);
    c->op = (sv_op_t)*c->ip;
    c->sz = sv_op_size[c->op];
    if (c->sz == 0) {
      c->ok = false;
      break;
    }

    for (int i = 0; i < c->lm.count; i++) {
      if (c->lm.entries[i].bc_off == c->bc_off) {
        vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
        c->element_available = false;
        if (c->integer_locals) {
          for (int li = 0; li < c->n_locals; li++)
            if (!c->entry_integer_regs || !c->entry_integer_regs[li]) c->integer_locals[li] = 0;
        }
        MIR_append_insn(c->ctx, c->jit_func, c->lm.entries[i].label);
        if (c->lm.entries[i].sp >= 0)
          c->vs.sp = c->lm.entries[i].sp;
        if (c->vs.slot_type)
          memset(c->vs.slot_type, SLOT_BOXED, (size_t)c->vs.max);
        if (c->vs.known_func)
          memset(c->vs.known_func, 0, (size_t)c->vs.max * sizeof(sv_func_t *));
        if (c->vs.has_const)
          memset(c->vs.has_const, 0, (size_t)c->vs.max * sizeof(bool));
        if (c->vs.known_bool)
          memset(c->vs.known_bool, 0, (size_t)c->vs.max);
        if (c->known_func_locals)
          memset(c->known_func_locals, 0,
                 (size_t)c->n_locals * sizeof(sv_func_t *));
        if (c->known_type_locals && c->local_d_regs) {
          for (int li = 0; li < c->n_locals; li++)
            if (c->known_type_locals[li] == SV_TI_NUM && c->has_captures && c->captured_locals && c->captured_locals[li])
              mir_i64_to_d(c->ctx, c->jit_func, c->local_d_regs[li],
                           c->local_regs[li], c->r_d_slot);
        }
      }
    }

    c->previous_insn = DLIST_TAIL(MIR_insn_t, c->jit_func->u.func->insns);
    c->integer_store = -1;
    c->integer_value = 0;
    c->integer_range = (jit_integer_range_t){0};
    if (c->integer_locals && c->integer_local_ranges && (c->op == OP_PUT_LOCAL || c->op == OP_PUT_LOCAL8 || c->op == OP_SET_LOCAL || c->op == OP_SET_LOCAL8) && c->vs.sp > 0 &&
        c->vs.slot_type[c->vs.sp - 1] == SLOT_I32) {
      int idx = (c->op == OP_PUT_LOCAL8 || c->op == OP_SET_LOCAL8)
                    ? sv_get_u8(c->ip + 1)
                    : sv_get_u16(c->ip + 1);
      if (idx < c->n_locals && c->dnum_locals && c->dnum_locals[idx]) {
        char name[48];
        snprintf(name, sizeof(name), "integer_local_%d", c->integer_local_site++);
        c->integer_value = c->entry_integer_regs && c->entry_integer_regs[idx]
                               ? c->entry_integer_regs[idx]
                               : MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->integer_value), MIR_new_reg_op(c->ctx, vstack_top(&c->vs))));
        c->integer_store = idx;
        c->integer_range = c->entry_integer_regs && c->entry_integer_regs[idx]
                               ? c->entry_integer_ranges[idx]
                               : c->vs.integer_range[c->vs.sp - 1];
      }
    }

    switch (c->op) {
      case OP_CONST_I8:
      case OP_CONST:
      case OP_CONST8:
      case OP_UNDEF:
      case OP_NULL:
      case OP_TRUE:
      case OP_FALSE:
      case OP_THIS:
      case OP_SPECIAL_OBJ:
      case OP_OBJECT:
      case OP_ARRAY:
      case OP_REGEXP:
        jit_emit_literals(c);
        break;
      case OP_POP:
      case OP_DUP:
      case OP_DUP2:
      case OP_NIP:
      case OP_INSERT2:
      case OP_INSERT3:
      case OP_SWAP:
      case OP_SWAP_UNDER:
      case OP_ROT4_UNDER:
      case OP_ROT3L:
        jit_emit_stack(c);
        break;
      case OP_GET_ARG:
      case OP_PUT_ARG:
      case OP_SET_ARG:
      case OP_REST:
      case OP_GET_LOCAL:
      case OP_GET_LOCAL8:
      case OP_GET_SLOT_RAW:
      case OP_PUT_LOCAL:
      case OP_PUT_LOCAL8:
      case OP_SET_LOCAL:
      case OP_SET_LOCAL8:
      case OP_SET_LOCAL_UNDEF:
      case OP_INC_LOCAL:
      case OP_DEC_LOCAL:
      case OP_ADD_LOCAL:
        jit_emit_locals(c);
        break;
      case OP_ADD:
      case OP_ADD_NUM:
      case OP_SUB:
      case OP_SUB_NUM:
      case OP_MUL:
      case OP_MUL_NUM:
      case OP_DIV:
      case OP_DIV_NUM:
      case OP_MOD:
      case OP_NEG:
      case OP_INC:
      case OP_DEC:
      case OP_POST_INC:
      case OP_POST_DEC:
        jit_emit_arithmetic(c);
        break;
      case OP_LT:
      case OP_LE:
      case OP_GT:
      case OP_GE:
      case OP_SEQ:
      case OP_EQ:
      case OP_NE:
      case OP_SNE:
      case OP_IN:
      case OP_INSTANCEOF:
      case OP_CALL_IS_PROTO:
        jit_emit_compare(c);
        break;
      case OP_BAND:
      case OP_BOR:
      case OP_BXOR:
      case OP_SHL:
      case OP_SHR:
      case OP_USHR:
      case OP_BNOT:
      case OP_NOT:
      case OP_IS_UNDEF:
      case OP_IS_NULL:
      case OP_IS_UNDEF_OR_NULL:
      case OP_IS_PRIMITIVE_TYPE:
      case OP_TYPEOF:
      case OP_VOID:
        jit_emit_bitwise(c);
        break;
      case OP_JMP:
      case OP_JMP8:
      case OP_JMP_NOT_NULLISH:
      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_FALSE:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE:
      case OP_JMP_TRUE8:
      case OP_RETURN:
      case OP_RETURN_UNDEF:
      case OP_TRY_PUSH:
      case OP_TRY_POP:
      case OP_THROW:
      case OP_THROW_ERROR:
      case OP_CATCH:
      case OP_NIP_CATCH:
      case OP_NOP:
      case OP_HALT:
      case OP_LINE_NUM:
      case OP_COL_NUM:
      case OP_LABEL:
        jit_emit_control(c);
        break;
      case OP_CALL:
      case OP_TAIL_CALL:
      case OP_NEW:
      case OP_CHECK_CTOR:
        jit_emit_calls(c);
        break;
      case OP_CALL_CALL:
      case OP_CALL_CALL_SLOT:
      case OP_APPLY:
      case OP_CALL_SUPER:
      case OP_TAIL_CALL_METHOD:
      case OP_CALL_METHOD:
        jit_emit_methods(c);
        break;
      case OP_CALL_CHAR_CODE_AT:
      case OP_CALL_ARRAY_INCLUDES:
      case OP_CALL_STRING_INTRINSIC:
      case OP_CALL_MAP_TEMPLATE:
      case OP_TAIL_MAP_TEMPLATE:
      case OP_RE_EXEC_TRUTHY:
      case OP_RE_EXEC_DISCARD:
      case OP_LOAD_STABLE_BUILTIN:
      case OP_CALL_STABLE_BUILTIN:
        jit_emit_intrinsics(c);
        break;
      case OP_STR_APPEND_LOCAL:
      case OP_STR_ALC_SNAPSHOT:
      case OP_STR_FLUSH_LOCAL:
      case OP_TO_STRING:
      case OP_TO_STRING_DEFER_NUMBER:
        jit_emit_strings(c);
        break;
      case OP_TO_PROPKEY:
      case OP_GET_FIELD:
      case OP_GET_FIELD2:
      case OP_GET_FIELD_OPT:
      case OP_PUT_FIELD:
      case OP_DEFINE_FIELD:
      case OP_DEFINE_SLOT:
      case OP_DEFINE_METHOD_COMP:
      case OP_GET_ELEM:
      case OP_GET_ELEM_OPT:
      case OP_GET_ELEM2:
      case OP_PUT_ELEM:
      case OP_GET_PRIVATE:
      case OP_PUT_PRIVATE:
      case OP_GET_LENGTH:
      case OP_DELETE:
      case OP_SET_PROTO:
        jit_emit_properties(c);
        break;
      case OP_GET_UPVAL:
      case OP_PUT_UPVAL:
      case OP_SET_UPVAL:
      case OP_CLOSE_UPVAL:
      case OP_GET_GLOBAL:
      case OP_GET_GLOBAL_UNDEF:
      case OP_GET_EVAL_GLOBAL:
      case OP_GET_EVAL_GLOBAL_UNDEF:
      case OP_PUT_GLOBAL:
      case OP_PUT_EVAL_GLOBAL:
      case OP_DELETE_EVAL_VAR:
      case OP_IMPORT_DEFAULT:
      case OP_IMPORT_NAMED:
      case OP_EXPORT:
      case OP_CLOSURE:
      case OP_SET_NAME:
        jit_emit_bindings(c);
        break;
      case OP_FOR_OF:
      case OP_DESTRUCTURE_INIT:
      case OP_ITER_NEXT:
      case OP_ITER_CLOSE:
      case OP_DESTRUCTURE_NEXT:
      case OP_DESTRUCTURE_CLOSE:
        jit_emit_iteration(c);
        break;
      default:
        c->ok = false;
        break;
    }

    for (MIR_insn_t insn = DLIST_NEXT(MIR_insn_t, c->previous_insn); insn;
         insn = DLIST_NEXT(MIR_insn_t, insn)) {
      if (c->op != OP_GET_ELEM && MIR_call_code_p(insn->code)) c->element_available = false;
      for (size_t oi = 0; oi < MIR_insn_nops(c->ctx, insn); oi++) {
        int output = 0;
        (void)MIR_insn_op_mode(c->ctx, insn, oi, &output);
        if (!output) continue;
        if (insn->ops[oi].mode == MIR_OP_MEM && c->op != OP_GET_ELEM) {
          MIR_reg_t base = insn->ops[oi].u.mem.base;
          if (base != c->r_slotbuf && base != c->r_lbuf && base != c->r_args_buf && base != c->r_iter_roots)
            c->element_available = false;
        }
        if (!c->integer_locals || insn->ops[oi].mode != MIR_OP_REG) continue;
        MIR_reg_t reg = insn->ops[oi].u.reg;
        if (reg <= c->local_reg_limit && c->local_by_reg[reg])
          c->integer_locals[c->local_by_reg[reg] - 1] = 0;
      }
    }
    if (c->integer_locals && c->integer_store >= 0) {
      c->integer_locals[c->integer_store] = c->integer_value;
      c->integer_local_ranges[c->integer_store] = c->integer_range;
    }
    if (!c->ok) break;
    c->ip += c->sz;
  }

  if (!c->ok || c->vs.sp > 0) {
    jit_emit_exit_ret(c, MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)));
  }

  if (c->needs_bailout) {
    MIR_append_insn(c->ctx, c->jit_func, c->bailout_tramp);

    if (c->r_jit_open_upvalues) {
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 4,
                                        MIR_new_ref_op(c->ctx, c->adopt_open_upvalues_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_adopt_open_upvalues),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues)));
    }

    MIR_reg_t r_resume_res = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                              MIR_JSVAL, "resume_res");
    if (c->has_captured_params && !c->writes_params) {
      mir_emit_fill_uncaptured_param_slots_from_args(
          c->ctx, c->jit_func, c->r_slotbuf, c->r_args, c->r_argc, c->captured_params, c->param_count);
    }
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 17,
                                      MIR_new_ref_op(c->ctx, c->resume_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_resume),
                                      MIR_new_reg_op(c->ctx, r_resume_res),
                                      MIR_new_reg_op(c->ctx, c->r_vm),
                                      MIR_new_reg_op(c->ctx, c->r_closure),
                                      MIR_new_reg_op(c->ctx, c->r_this_curr),
                                      c->feat.needs_new_target ? MIR_new_reg_op(c->ctx, c->r_new_target)
                                        : MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)),
                                      c->feat.needs_super ? MIR_new_reg_op(c->ctx, c->r_super_val)
                                        : MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)),
                                      MIR_new_reg_op(c->ctx, c->r_args),
                                      MIR_new_reg_op(c->ctx, c->r_argc),
                                      MIR_new_reg_op(c->ctx, c->r_args_buf),
                                      MIR_new_reg_op(c->ctx, c->r_bailout_sp),
                                      c->params_in_slotbuf ? MIR_new_reg_op(c->ctx, c->r_slotbuf) : MIR_new_uint_op(c->ctx, 0),
                                      MIR_new_int_op(c->ctx, c->params_in_slotbuf ? c->param_count : 0),
                                      MIR_new_reg_op(c->ctx, c->r_lbuf),
                                      MIR_new_int_op(c->ctx, c->n_locals),
                                      MIR_new_reg_op(c->ctx, c->r_bailout_off)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, r_resume_res)));
  }

  MIR_finish_func(c->ctx);
  MIR_finish_module(c->ctx);
  if (sv_dump_jit_unlikely) MIR_output_module(c->ctx, stderr, c->mod);

  free(c->vs.regs);
  free(c->vs.d_regs);
  free(c->vs.known_func);
  free(c->vs.slot_type);
  free(c->vs.known_const);
  free(c->vs.has_const);
  free(c->vs.known_bool);
  free(c->vs.integer_range);
  free(c->local_regs);
  free(c->integer_locals);
  free(c->local_by_reg);
  free(c->integer_local_ranges);
  free(c->entry_integer_ranges);
  free(c->entry_integer_regs);
  free(c->dnum_locals);
  free(c->local_d_regs);
  free(c->known_func_locals);
  free(c->known_type_locals);
  free(c->self_binding_guards);
  free(c->captured_params);
  free(c->captured_locals);
  free(c->feat.builder_target_slots);

  if (!c->ok) {
    MIR_remove_module(c->ctx, c->mod);
    c->func->jit_compile_failed = true;
    c->func->jit_compiling = false;
    return NULL;
  }

  MIR_load_module(c->ctx, c->mod);
  MIR_link(c->ctx, MIR_set_gen_interface, NULL);

  sv_jit_func_t generated = MIR_gen(c->ctx, c->jit_func);
  c->func->jit_compiling = false;
  if (!generated) {
    c->func->jit_compile_failed = true;
    return NULL;
  }

  c->func->jit_compiled_tfb_ver = c->func->tfb_version;
  return generated;
}
