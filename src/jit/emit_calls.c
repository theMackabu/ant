#include "compile.h"
#include "silver/feedback.h"

static bool jit_new_func_flags(uint32_t *offset, uint16_t *mask) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  sv_func_t probe;
  memset(&probe, 0, sizeof(probe));
  probe.is_async = 1;
  probe.is_generator = 1;
  probe.is_derived_ctor = 1;
  const uint8_t *bytes = (const uint8_t *)&probe;
  size_t first = sizeof(probe), last = 0;
  for (size_t i = 0; i < sizeof(probe); i++)
    if (bytes[i]) { if (first == sizeof(probe)) first = i; last = i; }
  if (first == sizeof(probe) || last - first >= 2 || first + 2 > sizeof(probe)) return false;
  *offset = (uint32_t)first;
  *mask = (uint16_t)(bytes[first] | (uint16_t)bytes[first + 1] << 8);
  return true;
#else
  (void)offset; (void)mask;
  return false;
#endif
}

static_assert(sizeof(((sv_ctor_prop_fb_t *)0)->samples) == 8, "samples is an I64 counter");
static_assert(sizeof(((sv_ctor_prop_fb_t *)0)->hist[0]) == 8, "hist bins are I64 counters, scale 8");
static_assert(sizeof(((sv_ctor_prop_fb_t *)0)->inobj_frozen) == 1, "inobj_frozen is a U8");

static void jit_emit_new_direct(
    jit_compile_t *c, MIR_reg_t func, MIR_reg_t target, MIR_reg_t res,
    int argc, MIR_label_t slow, MIR_label_t done) {
  char name[40];
  snprintf(name, sizeof(name), "nd_this_%d", c->bc_off);
  MIR_reg_t r_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
  snprintf(name, sizeof(name), "nd_cl_%d", c->bc_off);
  MIR_reg_t r_cl = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "nd_code_%d", c->bc_off);
  MIR_reg_t r_code = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "nd_super_%d", c->bc_off);
  MIR_reg_t r_super = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
  snprintf(name, sizeof(name), "nd_func_%d", c->bc_off);
  MIR_reg_t r_func = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_func), MIR_new_reg_op(c->ctx, func)));
  func = r_func;

  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, func)));
  mir_emit_get_closure(c->ctx, c->jit_func, r_cl, func, c->r_bool, slow);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_super),
                               MIR_new_mem_op(c->ctx, MIR_T_U32, (MIR_disp_t)offsetof(sv_closure_t, call_flags), r_cl, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, r_super), MIR_new_uint_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_code),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_closure_t, func), r_cl, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, r_code), MIR_new_int_op(c->ctx, 0)));
  uint32_t flags_offset;
  uint16_t flags_mask;
  bool flags_checked = jit_new_func_flags(&flags_offset, &flags_mask);
  if (flags_checked) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_super),
                                 MIR_new_mem_op(c->ctx, MIR_T_U16, (MIR_disp_t)flags_offset, r_code, 0, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_AND, MIR_new_reg_op(c->ctx, r_super),
                                 MIR_new_reg_op(c->ctx, r_super), MIR_new_uint_op(c->ctx, flags_mask)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, slow),
                                 MIR_new_reg_op(c->ctx, r_super), MIR_new_uint_op(c->ctx, 0)));
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_code),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_func_t, jit_code), r_code, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, r_code), MIR_new_int_op(c->ctx, 0)));

  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 7,
                                    MIR_new_ref_op(c->ctx, c->new_this_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_new_this),
                                    MIR_new_reg_op(c->ctx, r_this),
                                    MIR_new_reg_op(c->ctx, c->r_js),
                                    MIR_new_reg_op(c->ctx, func),
                                    MIR_new_reg_op(c->ctx, target),
                                    MIR_new_int_op(c->ctx, flags_checked ? 1 : 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, r_this), MIR_new_uint_op(c->ctx, T_EMPTY)));

  mir_emit_get_closure(c->ctx, c->jit_func, r_cl, func, c->r_bool, slow);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_code),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_closure_t, func), r_cl, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_code),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_func_t, jit_code), r_code, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, slow),
                               MIR_new_reg_op(c->ctx, r_code), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_super),
                               MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)offsetof(sv_closure_t, super_val), r_cl, 0, 1)));

  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 10,
                                    MIR_new_ref_op(c->ctx, c->self_proto),
                                    MIR_new_reg_op(c->ctx, r_code),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->r_vm),
                                    MIR_new_reg_op(c->ctx, r_this),
                                    MIR_new_reg_op(c->ctx, func),
                                    MIR_new_reg_op(c->ctx, r_super),
                                    MIR_new_reg_op(c->ctx, c->r_args_buf),
                                    MIR_new_int_op(c->ctx, (int64_t)argc),
                                    MIR_new_reg_op(c->ctx, r_cl)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_URSH, MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, res), MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, done),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));

  MIR_label_t result_helper = MIR_new_label(c->ctx);
  MIR_reg_t r_fb = r_code, r_n = r_super, r_p = r_cl;
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, result_helper),
                               MIR_new_reg_op(c->ctx, res), MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_fb),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_closure_t, func), r_cl, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_fb),
                               MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(sv_func_t, type_feedback), r_fb, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_AND, MIR_new_reg_op(c->ctx, r_n),
                               MIR_new_reg_op(c->ctx, r_fb), MIR_new_uint_op(c->ctx, ant_sidecar)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, result_helper),
                               MIR_new_reg_op(c->ctx, r_n), MIR_new_uint_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, r_fb),
                               MIR_new_reg_op(c->ctx, r_fb), MIR_new_uint_op(c->ctx, ant_sidecar)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_n),
                               MIR_new_mem_op(c->ctx, MIR_T_U8,
                                              (MIR_disp_t)(offsetof(sv_func_sidecar_t, ctor_prop_fb) +
                                                           offsetof(sv_ctor_prop_fb_t, inobj_frozen)), r_fb, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, result_helper),
                               MIR_new_reg_op(c->ctx, r_n), MIR_new_uint_op(c->ctx, 0)));
  mir_emit_decode_ref(c->ctx, c->jit_func, r_p, r_this);
  MIR_label_t bin_ok = MIR_new_label(c->ctx);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_n),
                               MIR_new_mem_op(c->ctx, MIR_T_U32, (MIR_disp_t)offsetof(ant_object_t, prop_count), r_p, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, bin_ok),
                               MIR_new_reg_op(c->ctx, r_n), MIR_new_uint_op(c->ctx, SV_TFB_CTOR_PROP_OVERFLOW_FROM)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_n),
                               MIR_new_uint_op(c->ctx, SV_TFB_CTOR_PROP_OVERFLOW_FROM)));
  MIR_append_insn(c->ctx, c->jit_func, bin_ok);
  const MIR_disp_t hist = (MIR_disp_t)(offsetof(sv_func_sidecar_t, ctor_prop_fb) + offsetof(sv_ctor_prop_fb_t, hist));
  const MIR_disp_t samples = (MIR_disp_t)(offsetof(sv_func_sidecar_t, ctor_prop_fb) + offsetof(sv_ctor_prop_fb_t, samples));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_p),
                               MIR_new_mem_op(c->ctx, MIR_T_I64, hist, r_fb, r_n, 8)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, r_p),
                               MIR_new_reg_op(c->ctx, r_p), MIR_new_uint_op(c->ctx, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_mem_op(c->ctx, MIR_T_I64, hist, r_fb, r_n, 8),
                               MIR_new_reg_op(c->ctx, r_p)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_p),
                               MIR_new_mem_op(c->ctx, MIR_T_I64, samples, r_fb, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, r_p),
                               MIR_new_reg_op(c->ctx, r_p), MIR_new_uint_op(c->ctx, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_mem_op(c->ctx, MIR_T_I64, samples, r_fb, 0, 1),
                               MIR_new_reg_op(c->ctx, r_p)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, r_this)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));

  MIR_append_insn(c->ctx, c->jit_func, result_helper);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->new_result_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_new_result),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, func),
                                    MIR_new_reg_op(c->ctx, r_this),
                                    MIR_new_reg_op(c->ctx, res)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
}

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

      if (!is_tail || c->jit_try_depth == 0) {
        sv_func_t *inline_callee = c->vs.known_func[c->vs.sp - call_argc - 1];
        if (!inline_callee)
          inline_callee = sv_tfb_get_call_target(c->func, c->bc_off);
        bool speculative = (inline_callee && !c->vs.known_func[c->vs.sp - call_argc - 1]);
        if (inline_callee && (!is_tail || inline_callee != c->func) && jit_inlineable(inline_callee)) {
          sv_func_retain_for_jit(inline_callee);
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
          if (inl_uses_this) {
            mir_load_imm(c->ctx, c->jit_func, r_inl_this,
                inline_callee->is_strict || inline_callee->is_arrow ? js_mkundef() : c->js->global);
            mir_emit_resolve_call_this(c->ctx, c->jit_func, r_inl_this, r_inl_cl,
                                       r_inl_this, r_inl_flags, r_inl_bound);
          } else
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
          if (is_tail) jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_call_res));
          else jit_emit_throw_if_error(c, r_call_res);
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

      MIR_label_t new_slow = MIR_new_label(c->ctx);
      MIR_label_t new_done = MIR_new_label(c->ctx);
      jit_emit_new_direct(c, r_new_func, r_ctor_target, r_new_res, (int)new_argc, new_slow, new_done);
      MIR_append_insn(c->ctx, c->jit_func, new_slow);
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
      MIR_append_insn(c->ctx, c->jit_func, new_done);

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
