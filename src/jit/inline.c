#include "jit_internal.h"
static bool jit_op_inline_side_effect(sv_op_t op) {
  switch (op) {
    case OP_PUT_FIELD:
    case OP_LOAD_STABLE_BUILTIN:
    case OP_CALL:
    case OP_CALL_METHOD:
    case OP_CALL_STABLE_BUILTIN:
    case OP_TAIL_CALL:
    case OP_TAIL_CALL_METHOD:
      return true;
    default:
      return false;
  }
}

static bool jit_op_inline_restarts_callee_on_guard_failure(sv_op_t op) {
  switch (op) {
    case OP_GET_GLOBAL:
    case OP_GET_ELEM:
    case OP_BAND:
    case OP_BOR:
    case OP_BXOR:
    case OP_SHL:
    case OP_SHR:
    case OP_USHR:
      return true;
    default:
      return false;
  }
}

static bool jit_op_inline_pure_tail(sv_op_t op) {
  switch (op) {
    case OP_RETURN:
    case OP_RETURN_UNDEF:
    case OP_POP:
    case OP_DUP:
    case OP_NIP:
    case OP_GET_LOCAL:
    case OP_GET_LOCAL8:
    case OP_PUT_LOCAL:
    case OP_PUT_LOCAL8:
    case OP_SET_LOCAL:
    case OP_SET_LOCAL8:
    case OP_GET_ARG:
    case OP_CONST:
    case OP_CONST8:
    case OP_CONST_I8:
    case OP_UNDEF:
    case OP_NULL:
    case OP_TRUE:
    case OP_FALSE:
    case OP_THIS:
    case OP_IS_UNDEF:
    case OP_IS_NULL:
    case OP_IS_UNDEF_OR_NULL:
    case OP_SEQ:
    case OP_SNE:
    case OP_NOP:
    case OP_LINE_NUM:
    case OP_COL_NUM:
    case OP_LABEL:
      return true;
    default:
      return false;
  }
}

bool jit_inlineable(sv_func_t *f) {
  if (!f) return false;
  if (f->is_async || f->is_generator) return false;
  // derived ctors need the super-rebound `this` returned from RETURN/
  // RETURN_UNDEF (see the main emission); the inline path doesn't model it
  if (f->is_derived_ctor) return false;
  if (f->code_len > JIT_INLINE_MAX_BYTECODE) return false;

  uint8_t *ip = f->code;
  uint8_t *end = f->code + f->code_len;
  bool seen_effect = false;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;

    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_INLINEABLE) == 0) return false;
    if ((flags & SV_OPF_JIT_INLINE_ARGC) != 0 && sv_get_u16(ip + 1) > SV_JIT_ARGS_BUF_CAP) return false;
    if (
        op == OP_CALL_STABLE_BUILTIN &&
        sv_get_u16(ip + 2) > SV_JIT_ARGS_BUF_CAP) return false;

    // OP_SPECIAL_OBJ(0) materializes `arguments`. keep these functions on
    // the interpreter until JIT routes a real per-call activation/object
    // with matching lifetime and semantics.
    if (op == OP_SPECIAL_OBJ && sv_get_u8(ip + 1) == 0) return false;

    // These guards branch to the shared slow path, which invokes the whole
    // callee. Never emit them after an operation that may already have had an
    // observable effect.
    if (seen_effect && jit_op_inline_restarts_callee_on_guard_failure(op))
      return false;

    if (seen_effect) {
      if (op == OP_JMP) {
        if (sv_get_i32(ip + 1) < 0) return false;
      } else if (!jit_op_inline_pure_tail(op) && !jit_op_inline_side_effect(op))
        return false;
    }

    if (jit_op_inline_side_effect(op)) seen_effect = true;
    ip += sz;
  }

  return true;
}

static void mir_emit_inline_read_guard(
    MIR_context_t ctx,
    MIR_item_t fn,
    MIR_reg_t value,
    MIR_reg_t result,
    MIR_reg_t scratch,
    MIR_label_t slow,
    MIR_label_t join,
    MIR_label_t ok) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, SV_JIT_BAILOUT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, ok),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, JIT_ERR_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, result),
                               MIR_new_reg_op(ctx, value)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
}

static MIR_label_t inl_label_for_offset(MIR_context_t ctx,
                                        inl_label_map_t *lm,
                                        int bc_off, int sp) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off == bc_off) {
      if (lm->entries[i].sp < 0) lm->entries[i].sp = sp;
      return lm->entries[i].label;
    }
  if (lm->count >= INL_MAX_LABELS) return NULL;
  MIR_label_t lbl = MIR_new_label(ctx);
  lm->entries[lm->count].bc_off = bc_off;
  lm->entries[lm->count].label = lbl;
  lm->entries[lm->count].sp = sp;
  lm->count++;
  return lbl;
}

static MIR_label_t inl_label_lookup(inl_label_map_t *lm, int bc_off, int *out_sp) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off == bc_off) {
      if (out_sp) *out_sp = lm->entries[i].sp;
      return lm->entries[i].label;
    }
  return NULL;
}

static bool jit_starts_numeric_const(sv_func_t *func, uint8_t *ip, uint8_t *end, int *out_size) {
  if (!func || !ip || ip >= end) return false;
  sv_op_t op = (sv_op_t)*ip;
  int sz = sv_op_size[op];
  if (sz == 0 || ip + sz > end) return false;
  if (out_size) *out_size = sz;

  switch (op) {
    case OP_CONST_I8:
      return true;
    case OP_CONST: {
      uint32_t idx = sv_get_u32(ip + 1);
      return idx < (uint32_t)func->const_count &&
             vtype(func->constants[idx]) == kTypeNumber;
    }
    case OP_CONST8: {
      uint8_t idx = sv_get_u8(ip + 1);
      return (uint32_t)idx < (uint32_t)func->const_count &&
             vtype(func->constants[idx]) == kTypeNumber;
    }
    default:
      return false;
  }
}

bool jit_has_immediate_numeric_local_init(sv_func_t *func, uint8_t *ip, uint8_t *end, uint16_t local_idx) {
  int const_size = 0;
  if (!jit_starts_numeric_const(func, ip, end, &const_size)) return false;
  uint8_t *put_ip = ip + const_size;
  if (put_ip >= end) return false;

  sv_op_t put_op = (sv_op_t)*put_ip;
  int put_size = sv_op_size[put_op];
  if (put_size == 0 || put_ip + put_size > end) return false;

  if (put_op == OP_PUT_LOCAL || put_op == OP_SET_LOCAL)
    return sv_get_u16(put_ip + 1) == local_idx;
  if (put_op == OP_PUT_LOCAL8 || put_op == OP_SET_LOCAL8)
    return local_idx <= UINT8_MAX && sv_get_u8(put_ip + 1) == (uint8_t)local_idx;
  return false;
}

void jit_emit_inline_body(
    MIR_context_t ctx, MIR_item_t jit_func, ant_t *js,
    sv_func_t *callee,
    MIR_reg_t *arg_regs, int caller_argc,
    const uint8_t *arg_num, const MIR_reg_t *arg_d,
    MIR_reg_t result, MIR_label_t slow, MIR_label_t join,
    MIR_reg_t r_bool, MIR_reg_t *p_d_slot, int id, int *p_reg_site,
    MIR_reg_t r_inl_closure, MIR_reg_t r_inl_this,
    MIR_reg_t r_inl_new_target, MIR_reg_t r_inl_super,
    MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_ic_epoch,
    MIR_item_t helper2_proto, MIR_item_t imp_seq,
    MIR_item_t imp_sne, MIR_item_t imp_eq, MIR_item_t imp_ne,
    MIR_item_t gf_proto, MIR_item_t imp_get_field_inline,
    MIR_item_t special_obj_proto, MIR_item_t imp_special_obj,
    const jit_inline_ext_t *ext) {
  int inl_max_stack = callee->max_stack > 0 ? callee->max_stack : 4;
  MIR_reg_t inl_vs[inl_max_stack];
  for (int i = 0; i < inl_max_stack; i++) {
    char rn[32];
    snprintf(rn, sizeof(rn), "inl%d_s%d", id, i);
    inl_vs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
  }
  int isp = 0;

  MIR_reg_t inl_d[inl_max_stack];
  uint8_t inl_num[inl_max_stack];
  for (int i = 0; i < inl_max_stack; i++) {
    char rn[32];
    snprintf(rn, sizeof(rn), "inl%d_dt%d", id, i);
    inl_d[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, rn);
    inl_num[i] = 0;
  }

#define INL_ENSURE_D_SLOT()                                        \
  do {                                                             \
    if (!*p_d_slot) {                                              \
      *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,          \
                                   MIR_T_I64, "d_slot_inl");       \
      MIR_append_insn(ctx, jit_func,                               \
                      MIR_new_insn(ctx, MIR_ALLOCA,                \
                                   MIR_new_reg_op(ctx, *p_d_slot), \
                                   MIR_new_uint_op(ctx, 8)));      \
    }                                                              \
  } while (0)
#define INL_FLUSH_SLOT(k)                                              \
  do {                                                                 \
    int _fk = (k);                                                     \
    if (inl_num[_fk]) {                                                \
      INL_ENSURE_D_SLOT();                                             \
      mir_d_to_i64(ctx, jit_func, inl_vs[_fk], inl_d[_fk], *p_d_slot); \
      inl_num[_fk] = 0;                                                \
    }                                                                  \
  } while (0)
#define INL_FLUSH_ALL()                 \
  do {                                  \
    for (int _fa = 0; _fa < isp; _fa++) \
      INL_FLUSH_SLOT(_fa);              \
  } while (0)

  int inl_n_locals = callee->max_locals;
  MIR_reg_t inl_locals[inl_n_locals > 0 ? inl_n_locals : 1];
  for (int i = 0; i < inl_n_locals; i++) {
    char rn[32];
    snprintf(rn, sizeof(rn), "inl%d_l%d", id, i);
    inl_locals[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
    mir_load_imm(ctx, jit_func, inl_locals[i], mkval(kTypeUndefined, 0));
  }

  MIR_reg_t inl_undef = 0;
  {
    const uint8_t *scan = callee->code;
    const uint8_t *scan_end = callee->code + callee->code_len;
    while (scan < scan_end) {
      sv_op_t sop = (sv_op_t)*scan;
      if (sop == OP_CALL || sop == OP_CALL_METHOD ||
          sop == OP_TAIL_CALL || sop == OP_TAIL_CALL_METHOD) {
        char rn[32];
        snprintf(rn, sizeof(rn), "inl%d_undef", id);
        inl_undef = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
        mir_load_imm(ctx, jit_func, inl_undef, mkval(kTypeUndefined, 0));
        break;
      }
      int ssz = sv_op_size[sop];
      if (ssz <= 0) break;
      scan += ssz;
    }
  }

  int inl_arith = 0;
  int inl_upval_n = 0;

  inl_label_map_t inl_lm = {.count = 0};

  uint8_t *code_base = callee->code;
  uint8_t *ip = callee->code;
  uint8_t *end = callee->code + callee->code_len;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    int inl_bc_off = (int)(ip - code_base);

    int label_sp = -1;
    MIR_label_t target_lbl = inl_label_lookup(&inl_lm, inl_bc_off, &label_sp);
    if (target_lbl) {
      INL_FLUSH_ALL();
      MIR_append_insn(ctx, jit_func, target_lbl);
      if (label_sp >= 0) isp = label_sp;
      memset(inl_num, 0, (size_t)inl_max_stack);
    }

    switch (op) {
      case OP_GET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        if ((int)idx < caller_argc && arg_num && arg_num[idx]) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_DMOV,
                                       MIR_new_reg_op(ctx, inl_d[isp]),
                                       MIR_new_reg_op(ctx, arg_d[idx])));
          inl_num[isp++] = 1;
          break;
        }
        inl_num[isp] = 0;
        MIR_reg_t dst = inl_vs[isp++];
        if ((int)idx < caller_argc)
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, dst),
                                       MIR_new_reg_op(ctx, arg_regs[idx])));
        else
          mir_load_imm(ctx, jit_func, dst, mkval(kTypeUndefined, 0));
        break;
      }

      case OP_CONST_I8: {
        double d = (double)(int8_t)sv_get_i8(ip + 1);
        union {
          double d;
          uint64_t u;
        } u = {d};
        mir_load_imm(ctx, jit_func, inl_vs[isp++], u.u);
        break;
      }
      case OP_CONST: {
        uint32_t idx = sv_get_u32(ip + 1);
        ANT_ASSERT(idx < (uint32_t)callee->const_count,
                   "invalid inline constant index");
        ant_value_t cv = callee->constants[idx];
        MIR_reg_t dst = inl_vs[isp++];
        if (jit_const_is_heap(cv))
          mir_load_const_slot(ctx, jit_func, dst, &callee->constants[idx]);
        else
          mir_load_imm(ctx, jit_func, dst, cv);
        break;
      }
      case OP_CONST8: {
        uint8_t idx = sv_get_u8(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)callee->const_count,
                   "invalid inline constant index");
        ant_value_t cv = callee->constants[idx];
        MIR_reg_t dst = inl_vs[isp++];
        if (jit_const_is_heap(cv))
          mir_load_const_slot(ctx, jit_func, dst, &callee->constants[idx]);
        else
          mir_load_imm(ctx, jit_func, dst, cv);
        break;
      }
      case OP_UNDEF:
        mir_load_imm(ctx, jit_func, inl_vs[isp++], mkval(kTypeUndefined, 0));
        break;
      case OP_NULL:
        mir_load_imm(ctx, jit_func, inl_vs[isp++], mkval(kTypeNull, 0));
        break;
      case OP_TRUE:
        mir_load_imm(ctx, jit_func, inl_vs[isp++], js_true);
        break;
      case OP_FALSE:
        mir_load_imm(ctx, jit_func, inl_vs[isp++], js_false);
        break;

      case OP_THIS: {
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp++]),
                                     MIR_new_reg_op(ctx, r_inl_this)));
        break;
      }

      case OP_GET_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp++]),
                                     MIR_new_reg_op(ctx, inl_locals[idx])));
        break;
      }
      case OP_GET_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp++]),
                                     MIR_new_reg_op(ctx, inl_locals[idx])));
        break;
      }
      case OP_PUT_LOCAL: {
        INL_FLUSH_SLOT(isp - 1);
        uint16_t idx = sv_get_u16(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_locals[idx]),
                                     MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_PUT_LOCAL8: {
        INL_FLUSH_SLOT(isp - 1);
        uint8_t idx = sv_get_u8(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_locals[idx]),
                                     MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_SET_LOCAL: {
        INL_FLUSH_SLOT(isp - 1);
        uint16_t idx = sv_get_u16(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_locals[idx]),
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        break;
      }
      case OP_SET_LOCAL8: {
        INL_FLUSH_SLOT(isp - 1);
        uint8_t idx = sv_get_u8(ip + 1);
        ANT_ASSERT((uint32_t)idx < (uint32_t)inl_n_locals,
                   "invalid inline local index");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_locals[idx]),
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        break;
      }

      case OP_GET_UPVAL: {
        ANT_ASSERT(r_inl_closure != 0,
                   "inline upvalue access requires a closure");
        uint16_t idx = sv_get_u16(ip + 1);
        int un = inl_upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "inl%d_uvs%d", id, un);
        snprintf(rn_uv, sizeof(rn_uv), "inl%d_uv%d", id, un);
        snprintf(rn_loc, sizeof(rn_loc), "inl%d_uvl%d", id, un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        MIR_reg_t dst = inl_vs[isp++];

        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_uvs),
                                     MIR_new_mem_op(ctx, MIR_T_P,
                                                    (MIR_disp_t)offsetof(sv_closure_t, upvalues),
                                                    r_inl_closure, 0, 1)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_uv),
                                     MIR_new_mem_op(ctx, MIR_T_P,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(sv_upvalue_t *)),
                                                    r_uvs, 0, 1)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_loc),
                                     MIR_new_mem_op(ctx, MIR_T_P,
                                                    (MIR_disp_t)offsetof(sv_upvalue_t, location),
                                                    r_uv, 0, 1)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, dst),
                                     MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_loc, 0, 1)));
        if (jit_upvalue_is_builder_target(callee, idx))
          mir_emit_branch_if_string_builder(ctx, jit_func, dst, r_bool, slow);
        break;
      }

      case OP_POP:
        isp--;
        inl_num[isp] = 0;
        break;
      case OP_DUP: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp]),
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        isp++;
        break;
      }
      case OP_DUP2: {
        ANT_ASSERT(isp >= 2, "invalid inline stack depth");
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t ra = inl_vs[isp - 2];
        MIR_reg_t rb = inl_vs[isp - 1];
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp]),
                                     MIR_new_reg_op(ctx, ra)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp + 1]),
                                     MIR_new_reg_op(ctx, rb)));
        isp += 2;
        break;
      }

      case OP_NIP: {
        ANT_ASSERT(isp >= 2, "invalid inline stack depth");
        INL_FLUSH_SLOT(isp - 1);
        inl_num[isp - 2] = 0;
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 2]),
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        isp--;
        break;
      }

      case OP_INSERT2: {
        ANT_ASSERT(isp >= 2, "invalid inline stack depth");
        INL_FLUSH_ALL();
        char tn[32];
        snprintf(tn, sizeof(tn), "inl%d_ins2t", id);
        MIR_reg_t r_t = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, tn);
        MIR_reg_t r_a = inl_vs[isp - 1];
        MIR_reg_t r_obj = inl_vs[isp - 2];
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_t),
                                     MIR_new_reg_op(ctx, r_a)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1]),
                                     MIR_new_reg_op(ctx, r_obj)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 2]),
                                     MIR_new_reg_op(ctx, r_t)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp]),
                                     MIR_new_reg_op(ctx, r_t)));
        isp++;
        break;
      }
      case OP_INSERT3: {
        ANT_ASSERT(isp >= 3, "invalid inline stack depth");
        INL_FLUSH_ALL();
        char tn[32];
        snprintf(tn, sizeof(tn), "inl%d_ins3t", id);
        MIR_reg_t r_t = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, tn);
        MIR_reg_t r_a = inl_vs[isp - 1];
        MIR_reg_t r_prop = inl_vs[isp - 2];
        MIR_reg_t r_obj = inl_vs[isp - 3];
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_t),
                                     MIR_new_reg_op(ctx, r_a)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 1]),
                                     MIR_new_reg_op(ctx, r_prop)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 2]),
                                     MIR_new_reg_op(ctx, r_obj)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp - 3]),
                                     MIR_new_reg_op(ctx, r_t)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, inl_vs[isp]),
                                     MIR_new_reg_op(ctx, r_t)));
        isp++;
        break;
      }

      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV:
      case OP_ADD_NUM:
      case OP_SUB_NUM:
      case OP_MUL_NUM:
      case OP_DIV_NUM: {
        int str_bc_off = (int)(ip - callee->code);
        uint8_t fb = sv_func_type_feedback(callee)
                         ? sv_func_type_feedback(callee)[str_bc_off]
                         : 0;
        bool fb_str_only = op == OP_ADD && fb && !(fb & ~SV_TFB_STR);
        if (fb_str_only) {
          INL_FLUSH_SLOT(isp - 1);
          INL_FLUSH_SLOT(isp - 2);
          MIR_reg_t rr = inl_vs[--isp];
          MIR_reg_t rl = inl_vs[--isp];
          MIR_reg_t rd = inl_vs[isp++];
          inl_num[isp - 1] = 0;
          mir_emit_string_concat_fastpath(
              ctx, jit_func, r_js, rl, rr, rd, slow, id, str_bc_off, false);
          break;
        }

        if (inl_num[isp - 1] && inl_num[isp - 2]) {
          MIR_insn_code_t dop;
          switch (op) {
            case OP_ADD:
            case OP_ADD_NUM:
              dop = MIR_DADD;
              break;
            case OP_SUB:
            case OP_SUB_NUM:
              dop = MIR_DSUB;
              break;
            case OP_MUL:
            case OP_MUL_NUM:
              dop = MIR_DMUL;
              break;
            default:
              dop = MIR_DDIV;
              break;
          }
          isp -= 2;
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, dop,
                                       MIR_new_reg_op(ctx, inl_d[isp]),
                                       MIR_new_reg_op(ctx, inl_d[isp]),
                                       MIR_new_reg_op(ctx, inl_d[isp + 1])));
          inl_num[isp++] = 1;
          inl_num[isp] = 0;
          break;
        }
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        inl_num[isp - 1] = 0;

        if (!(op == OP_ADD_NUM || op == OP_SUB_NUM ||
              op == OP_MUL_NUM || op == OP_DIV_NUM)) {
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
        }

        if (!*p_d_slot) {
          *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,
                                       MIR_T_I64, "d_slot_inl");
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_ALLOCA,
                                       MIR_new_reg_op(ctx, *p_d_slot),
                                       MIR_new_uint_op(ctx, 8)));
        }

        int an = inl_arith++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "inl%d_fd1_%d", id, an);
        snprintf(d2, sizeof(d2), "inl%d_fd2_%d", id, an);
        snprintf(d3, sizeof(d3), "inl%d_fd3_%d", id, an);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);

        mir_i64_to_d(ctx, jit_func, fd1, rl, *p_d_slot);
        mir_i64_to_d(ctx, jit_func, fd2, rr, *p_d_slot);

        MIR_insn_code_t mir_op;
        switch (op) {
          case OP_ADD:
          case OP_ADD_NUM:
            mir_op = MIR_DADD;
            break;
          case OP_SUB:
          case OP_SUB_NUM:
            mir_op = MIR_DSUB;
            break;
          case OP_MUL:
          case OP_MUL_NUM:
            mir_op = MIR_DMUL;
            break;
          default:
            mir_op = MIR_DDIV;
            break;
        }
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, mir_op,
                                     MIR_new_reg_op(ctx, fd3),
                                     MIR_new_reg_op(ctx, fd1),
                                     MIR_new_reg_op(ctx, fd2)));
        mir_d_to_i64(ctx, jit_func, rd, fd3, *p_d_slot);
        break;
      }

      case OP_MOD: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 1], slow);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 2], slow);

        if (!*p_d_slot) {
          *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,
                                       MIR_T_I64, "d_slot_inl");
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_ALLOCA,
                                       MIR_new_reg_op(ctx, *p_d_slot),
                                       MIR_new_uint_op(ctx, 8)));
        }

        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];

        int mn = inl_arith++;
        char md1[32], md2[32], md3[32], md4[32], md5[32];
        snprintf(md1, sizeof(md1), "inl%d_mod1_%d", id, mn);
        snprintf(md2, sizeof(md2), "inl%d_mod2_%d", id, mn);
        snprintf(md3, sizeof(md3), "inl%d_mod3_%d", id, mn);
        snprintf(md4, sizeof(md4), "inl%d_mod4_%d", id, mn);
        snprintf(md5, sizeof(md5), "inl%d_mod5_%d", id, mn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, md1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, md2);
        MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, md3);
        MIR_reg_t fd4 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, md4);
        MIR_reg_t fd5 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, md5);

        mir_i64_to_d(ctx, jit_func, fd1, rl, *p_d_slot);
        mir_i64_to_d(ctx, jit_func, fd2, rr, *p_d_slot);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_DDIV,
                                     MIR_new_reg_op(ctx, fd3),
                                     MIR_new_reg_op(ctx, fd1),
                                     MIR_new_reg_op(ctx, fd2)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_D2I,
                                     MIR_new_reg_op(ctx, rd),
                                     MIR_new_reg_op(ctx, fd3)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_I2D,
                                     MIR_new_reg_op(ctx, fd4),
                                     MIR_new_reg_op(ctx, rd)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_DMUL,
                                     MIR_new_reg_op(ctx, fd4),
                                     MIR_new_reg_op(ctx, fd4),
                                     MIR_new_reg_op(ctx, fd2)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_DSUB,
                                     MIR_new_reg_op(ctx, fd5),
                                     MIR_new_reg_op(ctx, fd1),
                                     MIR_new_reg_op(ctx, fd4)));
        mir_d_to_i64(ctx, jit_func, rd, fd5, *p_d_slot);
        break;
      }

      case OP_NEG: {
        if (inl_num[isp - 1]) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_DNEG,
                                       MIR_new_reg_op(ctx, inl_d[isp - 1]),
                                       MIR_new_reg_op(ctx, inl_d[isp - 1])));
          break;
        }

        MIR_reg_t rs = inl_vs[isp - 1];
        mir_emit_is_num_guard(ctx, jit_func, r_bool, rs, slow);

        if (!*p_d_slot) {
          *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,
                                       MIR_T_I64, "d_slot_inl");
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_ALLOCA,
                                       MIR_new_reg_op(ctx, *p_d_slot),
                                       MIR_new_uint_op(ctx, 8)));
        }

        int nn = inl_arith++;
        char nd1[32], nd2[32];
        snprintf(nd1, sizeof(nd1), "inl%d_neg1_%d", id, nn);
        snprintf(nd2, sizeof(nd2), "inl%d_neg2_%d", id, nn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, nd1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, nd2);
        mir_i64_to_d(ctx, jit_func, fd1, rs, *p_d_slot);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_DNEG,
                                     MIR_new_reg_op(ctx, fd2),
                                     MIR_new_reg_op(ctx, fd1)));
        mir_d_to_i64(ctx, jit_func, rs, fd2, *p_d_slot);
        break;
      }

      case OP_LT:
      case OP_LE:
      case OP_GT:
      case OP_GE: {
        if (inl_num[isp - 1] && inl_num[isp - 2]) {
          MIR_insn_code_t dcmp;
          switch (op) {
            case OP_LT:
              dcmp = MIR_DLT;
              break;
            case OP_LE:
              dcmp = MIR_DLE;
              break;
            case OP_GT:
              dcmp = MIR_DGT;
              break;
            default:
              dcmp = MIR_DGE;
              break;
          }
          isp -= 2;
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, dcmp,
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_reg_op(ctx, inl_d[isp]),
                                       MIR_new_reg_op(ctx, inl_d[isp + 1])));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_OR,
                                       MIR_new_reg_op(ctx, inl_vs[isp]),
                                       MIR_new_uint_op(ctx, js_false),
                                       MIR_new_reg_op(ctx, r_bool)));
          inl_num[isp++] = 0;
          inl_num[isp] = 0;
          break;
        }
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        inl_num[isp - 1] = 0;

        mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);

        if (!*p_d_slot) {
          *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,
                                       MIR_T_I64, "d_slot_inl");
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_ALLOCA,
                                       MIR_new_reg_op(ctx, *p_d_slot),
                                       MIR_new_uint_op(ctx, 8)));
        }

        int cn = inl_arith++;
        char cd1[32], cd2[32];
        snprintf(cd1, sizeof(cd1), "inl%d_cd1_%d", id, cn);
        snprintf(cd2, sizeof(cd2), "inl%d_cd2_%d", id, cn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, cd1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, cd2);
        mir_i64_to_d(ctx, jit_func, fd1, rl, *p_d_slot);
        mir_i64_to_d(ctx, jit_func, fd2, rr, *p_d_slot);

        MIR_insn_code_t cmp_op;
        const char *cmp_name;
        switch (op) {
          case OP_LT:
            cmp_op = MIR_DLT;
            cmp_name = "lt";
            break;
          case OP_LE:
            cmp_op = MIR_DLE;
            cmp_name = "le";
            break;
          case OP_GT:
            cmp_op = MIR_DGT;
            cmp_name = "gt";
            break;
          default:
            cmp_op = MIR_DGE;
            cmp_name = "ge";
            break;
        }

        char cmp_rn[32];
        snprintf(cmp_rn, sizeof(cmp_rn), "inl%d_%s_%d", id, cmp_name, cn);
        MIR_reg_t r_tmp = MIR_new_func_reg(ctx, jit_func->u.func,
                                           MIR_T_I64, cmp_rn);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, cmp_op,
                                     MIR_new_reg_op(ctx, r_tmp),
                                     MIR_new_reg_op(ctx, fd1),
                                     MIR_new_reg_op(ctx, fd2)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_OR,
                                     MIR_new_reg_op(ctx, rd),
                                     MIR_new_uint_op(ctx, js_false),
                                     MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_SEQ:
      case OP_SNE:
      case OP_EQ:
      case OP_NE: {
        uint8_t feedback = sv_func_type_feedback(callee)
                               ? sv_func_type_feedback(callee)[inl_bc_off]
                               : 0;
        if (sv_tfb_specialization_ready(feedback) &&
            (feedback & SV_TFB_CLASS_MASK) == SV_TFB_NUM) {
          int right_idx = isp - 1;
          int left_idx = isp - 2;
          MIR_reg_t rr = inl_vs[right_idx];
          MIR_reg_t rl = inl_vs[left_idx];
          MIR_label_t eq_slow = MIR_new_label(ctx);
          MIR_label_t eq_done = MIR_new_label(ctx);
          INL_ENSURE_D_SLOT();
          if (!inl_num[left_idx]) {
            mir_emit_is_num_guard(
                ctx, jit_func, r_bool, rl, eq_slow);
            mir_i64_to_d(
                ctx, jit_func, inl_d[left_idx], rl, *p_d_slot);
          }
          if (!inl_num[right_idx]) {
            mir_emit_is_num_guard(
                ctx, jit_func, r_bool, rr, eq_slow);
            mir_i64_to_d(
                ctx, jit_func, inl_d[right_idx], rr, *p_d_slot);
          }
          isp -= 2;
          MIR_reg_t rd = inl_vs[isp++];
          mir_emit_numeric_equality(
              ctx, jit_func, inl_d[left_idx], inl_d[right_idx], rd, r_bool,
              op == OP_NE || op == OP_SNE);
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, eq_done)));
          MIR_append_insn(ctx, jit_func, eq_slow);
          MIR_item_t helper = op == OP_SEQ  ? imp_seq
                              : op == OP_EQ ? imp_eq
                              : op == OP_NE ? imp_ne
                                            : imp_sne;
          if (inl_num[left_idx])
            mir_d_to_i64(
                ctx, jit_func, rl, inl_d[left_idx], *p_d_slot);
          if (inl_num[right_idx])
            mir_d_to_i64(
                ctx, jit_func, rr, inl_d[right_idx], *p_d_slot);
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, helper,
                           r_vm, r_js, rl, rr);
          MIR_append_insn(ctx, jit_func, eq_done);
          inl_num[isp - 1] = 0;
          inl_num[isp] = 0;
        } else {
          INL_FLUSH_SLOT(isp - 1);
          INL_FLUSH_SLOT(isp - 2);
          MIR_reg_t rr = inl_vs[--isp];
          MIR_reg_t rl = inl_vs[--isp];
          MIR_reg_t rd = inl_vs[isp++];
          MIR_item_t helper = op == OP_SEQ  ? imp_seq
                              : op == OP_EQ ? imp_eq
                              : op == OP_NE ? imp_ne
                                            : imp_sne;
          if (op == OP_SEQ || op == OP_SNE) {
            MIR_label_t eq_slow = MIR_new_label(ctx);
            MIR_label_t eq_done = MIR_new_label(ctx);
            mir_emit_strict_tagged_equality(
                ctx, jit_func, rl, rr, rd, r_bool, op == OP_SNE,
                eq_slow, eq_done);
            MIR_append_insn(ctx, jit_func, eq_slow);
            mir_call_helper2(ctx, jit_func, rd,
                             helper2_proto, helper,
                             r_vm, r_js, rl, rr);
            MIR_append_insn(ctx, jit_func, eq_done);
          } else {
            mir_call_helper2(ctx, jit_func, rd,
                             helper2_proto, helper,
                             r_vm, r_js, rl, rr);
          }
        }
        break;
      }

      case OP_IS_UNDEF:
      case OP_IS_NULL: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t rs = inl_vs[isp - 1];
        uint64_t cmp_val = (op == OP_IS_UNDEF)
                               ? mkval(kTypeUndefined, 0)
                               : mkval(kTypeNull, 0);
        MIR_label_t is_true = MIR_new_label(ctx);
        MIR_label_t is_done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, is_true),
                                     MIR_new_reg_op(ctx, rs),
                                     MIR_new_uint_op(ctx, cmp_val)));
        mir_load_imm(ctx, jit_func, rs, js_false);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
        MIR_append_insn(ctx, jit_func, is_true);
        mir_load_imm(ctx, jit_func, rs, js_true);
        MIR_append_insn(ctx, jit_func, is_done);
        break;
      }

      case OP_IS_UNDEF_OR_NULL: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t rs = inl_vs[isp - 1];
        MIR_label_t is_true = MIR_new_label(ctx);
        MIR_label_t is_done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, is_true),
                                     MIR_new_reg_op(ctx, rs),
                                     MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, is_true),
                                     MIR_new_reg_op(ctx, rs),
                                     MIR_new_uint_op(ctx, mkval(kTypeNull, 0))));
        mir_load_imm(ctx, jit_func, rs, js_false);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
        MIR_append_insn(ctx, jit_func, is_true);
        mir_load_imm(ctx, jit_func, rs, js_true);
        MIR_append_insn(ctx, jit_func, is_done);
        break;
      }

      case OP_IS_PRIMITIVE_TYPE: {
        INL_FLUSH_ALL();
        mir_emit_primitive_type_test(ctx, jit_func, inl_vs[isp - 1], r_bool, ip[1]);
        break;
      }

      case OP_JMP: {
        INL_FLUSH_ALL();
        int target = inl_bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        ANT_ASSERT(lbl != NULL, "inline label map exhausted");
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl)));
        break;
      }

      case OP_JMP_NOT_NULLISH: {
        INL_FLUSH_ALL();
        MIR_reg_t cond = inl_vs[isp - 1];
        int target = inl_bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        ANT_ASSERT(lbl != NULL, "inline label map exhausted");
        MIR_label_t done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, done),
                                     MIR_new_reg_op(ctx, cond),
                                     MIR_new_uint_op(ctx, mkval(kTypeNull, 0))));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BNE,
                                     MIR_new_label_op(ctx, lbl),
                                     MIR_new_reg_op(ctx, cond),
                                     MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
        MIR_append_insn(ctx, jit_func, done);
        break;
      }

      case OP_JMP_TRUE_PEEK:
      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE:
      case OP_JMP_FALSE:
      case OP_JMP_TRUE8:
      case OP_JMP_FALSE8: {
        INL_FLUSH_ALL();
        bool is_peek = (op == OP_JMP_TRUE_PEEK || op == OP_JMP_FALSE_PEEK);
        MIR_reg_t cond = is_peek ? inl_vs[isp - 1] : inl_vs[--isp];
        bool short_op = (op == OP_JMP_TRUE8 || op == OP_JMP_FALSE8);
        bool is_false_branch = (op == OP_JMP_FALSE || op == OP_JMP_FALSE8 || op == OP_JMP_FALSE_PEEK);
        int target = inl_bc_off + sz + (short_op ? (int8_t)sv_get_i8(ip + 1) : sv_get_i32(ip + 1));
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        ANT_ASSERT(lbl != NULL, "inline label map exhausted");

        uint64_t cmp_bool = is_false_branch ? js_false : js_true;

        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_URSH,
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_reg_op(ctx, cond),
                                     MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BNE,
                                     MIR_new_label_op(ctx, slow),
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_uint_op(ctx, js_false >> NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, lbl),
                                     MIR_new_reg_op(ctx, cond),
                                     MIR_new_uint_op(ctx, cmp_bool)));
        break;
      }

      case OP_GET_FIELD: {
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        ANT_ASSERT(idx < (uint32_t)callee->atom_count,
                   "invalid inline atom index");
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[--isp];
        MIR_reg_t dst = inl_vs[isp++];
        uint16_t gf_ic_idx = sv_get_u16(ip + 5);
        MIR_label_t gf_done = MIR_new_label(ctx);
        MIR_label_t gf_slowl = MIR_new_label(ctx);
        bool gf_fast = r_ic_epoch != 0 &&
                       mir_emit_get_field_ic_fastpath(
                           ctx, jit_func, callee, -(id * 100000 + inl_bc_off + 1), gf_ic_idx,
                           atom, obj, dst, gf_slowl, r_ic_epoch);
        if (gf_fast) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, gf_done)));
          MIR_append_insn(ctx, jit_func, gf_slowl);
        }
        MIR_append_insn(ctx, jit_func,
                        MIR_new_call_insn(ctx, 10,
                                          MIR_new_ref_op(ctx, gf_proto),
                                          MIR_new_ref_op(ctx, imp_get_field_inline),
                                          MIR_new_reg_op(ctx, dst),
                                          MIR_new_reg_op(ctx, r_vm),
                                          MIR_new_reg_op(ctx, r_js),
                                          MIR_new_reg_op(ctx, obj),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
                                          MIR_new_uint_op(ctx, (uint64_t)atom->len),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
                                          MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        mir_emit_inline_read_guard(
            ctx, jit_func, dst, result, r_bool, slow, join, gf_done);
        MIR_append_insn(ctx, jit_func, gf_done);
        break;
      }
      case OP_GET_FIELD_OPT: {
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        ANT_ASSERT(idx < (uint32_t)callee->atom_count,
                   "invalid inline atom index");
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[--isp];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_label_t nullish = MIR_new_label(ctx);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_label_t value_ok = MIR_new_label(ctx);
        MIR_label_t gf_slow = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, nullish),
                                     MIR_new_reg_op(ctx, obj),
                                     MIR_new_uint_op(ctx, mkval(kTypeNull, 0))));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BEQ,
                                     MIR_new_label_op(ctx, nullish),
                                     MIR_new_reg_op(ctx, obj),
                                     MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
        uint16_t gf_ic_idx = sv_get_u16(ip + 5);
        bool gf_fast = r_ic_epoch != 0 &&
                       mir_emit_get_field_ic_fastpath(
                           ctx, jit_func, callee, -(id * 100000 + inl_bc_off + 1), gf_ic_idx,
                           atom, obj, dst, gf_slow, r_ic_epoch);
        if (gf_fast) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, no_err)));
          MIR_append_insn(ctx, jit_func, gf_slow);
        }
        MIR_append_insn(ctx, jit_func,
                        MIR_new_call_insn(ctx, 10,
                                          MIR_new_ref_op(ctx, gf_proto),
                                          MIR_new_ref_op(ctx, imp_get_field_inline),
                                          MIR_new_reg_op(ctx, dst),
                                          MIR_new_reg_op(ctx, r_vm),
                                          MIR_new_reg_op(ctx, r_js),
                                          MIR_new_reg_op(ctx, obj),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
                                          MIR_new_uint_op(ctx, (uint64_t)atom->len),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
                                          MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        mir_emit_inline_read_guard(
            ctx, jit_func, dst, result, r_bool, slow, join, value_ok);
        MIR_append_insn(ctx, jit_func, value_ok);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP,
                                     MIR_new_label_op(ctx, no_err)));
        MIR_append_insn(ctx, jit_func, nullish);
        mir_load_imm(ctx, jit_func, dst, mkval(kTypeUndefined, 0));
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }
      case OP_GET_FIELD2: {
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        ANT_ASSERT(idx < (uint32_t)callee->atom_count,
                   "invalid inline atom index");
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[isp - 1];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_label_t value_ok = MIR_new_label(ctx);
        MIR_label_t gf_slow = MIR_new_label(ctx);
        uint16_t gf_ic_idx = sv_get_u16(ip + 5);
        bool gf_fast = r_ic_epoch != 0 &&
                       mir_emit_get_field_ic_fastpath(
                           ctx, jit_func, callee, -(id * 100000 + inl_bc_off + 1), gf_ic_idx,
                           atom, obj, dst, gf_slow, r_ic_epoch);
        if (gf_fast) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, value_ok)));
          MIR_append_insn(ctx, jit_func, gf_slow);
        }
        MIR_append_insn(ctx, jit_func,
                        MIR_new_call_insn(ctx, 10,
                                          MIR_new_ref_op(ctx, gf_proto),
                                          MIR_new_ref_op(ctx, imp_get_field_inline),
                                          MIR_new_reg_op(ctx, dst),
                                          MIR_new_reg_op(ctx, r_vm),
                                          MIR_new_reg_op(ctx, r_js),
                                          MIR_new_reg_op(ctx, obj),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
                                          MIR_new_uint_op(ctx, (uint64_t)atom->len),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
                                          MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        mir_emit_inline_read_guard(
            ctx, jit_func, dst, result, r_bool, slow, join, value_ok);
        MIR_append_insn(ctx, jit_func, value_ok);
        break;
      }
      case OP_GET_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        ANT_ASSERT(idx < (uint32_t)callee->atom_count,
                   "invalid inline atom index");
        MIR_reg_t dst = inl_vs[isp++];
        int gg_site = -(id * 100000 + inl_bc_off + 1);
        bool gg_fast = r_ic_epoch != 0 &&
                       mir_emit_get_global_ic_fastpath(
                           ctx, jit_func, callee, gg_site,
                           r_js, dst, slow, r_ic_epoch, ip);
        if (!gg_fast)
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, slow)));
        break;
      }

      case OP_LOAD_STABLE_BUILTIN: {
        uint8_t stable_kind = sv_get_u8(ip + 1);
        ANT_ASSERT(
            stable_kind == SV_STABLE_BUILTIN_PROMISE_RESOLVE,
            "invalid inline stable-builtin load");
        MIR_reg_t stable_this = inl_vs[isp++];
        MIR_reg_t stable_fn = inl_vs[isp++];
        int stable_site = -(id * 100000 + inl_bc_off + 1);
        mir_emit_load_stable_builtin(
            ctx, jit_func, js, r_js, stable_kind,
            stable_this, stable_fn, ext->r_args_buf,
            ext->stable_load_proto, ext->imp_load_stable_builtin,
            stable_site);
        MIR_label_t stable_ok = MIR_new_label(ctx);
        mir_emit_inline_read_guard(
            ctx, jit_func, stable_fn, result, r_bool,
            slow, join, stable_ok);
        MIR_append_insn(ctx, jit_func, stable_ok);
        break;
      }

      case OP_PUT_FIELD: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        uint32_t pf_idx = sv_get_u32(ip + 1);
        ANT_ASSERT(pf_idx < (uint32_t)callee->atom_count,
                   "invalid inline atom index");
        sv_atom_t *pf_atom = &callee->atoms[pf_idx];
        uint16_t pf_ic_idx = sv_get_u16(ip + 5);
        sv_ic_entry_t *pf_ic = NULL;
        if (callee->ic_slots && pf_ic_idx != UINT16_MAX && pf_ic_idx < callee->ic_count)
          pf_ic = &callee->ic_slots[pf_ic_idx];
        MIR_reg_t pf_val = inl_vs[--isp];
        MIR_reg_t pf_obj = inl_vs[--isp];
        MIR_label_t pf_slow = MIR_new_label(ctx);
        MIR_label_t pf_ok = MIR_new_label(ctx);
        int pf_name_off = -(id * 100000 + inl_bc_off + 1);
        bool pf_fast = r_ic_epoch != 0 &&
                       mir_emit_put_field_ic_fastpath(
                           ctx, jit_func, js, callee,
                           pf_name_off, pf_ic_idx, pf_atom,
                           r_js, pf_obj, pf_val, pf_slow, r_ic_epoch,
                           ext->shape_transition_proto, ext->imp_shape_transition,
                           ext->remember_obj_proto, ext->imp_remember_obj);
        if (pf_fast) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, pf_ok)));
          MIR_append_insn(ctx, jit_func, pf_slow);
        }
        MIR_append_insn(ctx, jit_func,
                        MIR_new_call_insn(ctx, 9,
                                          MIR_new_ref_op(ctx, ext->put_field_proto),
                                          MIR_new_ref_op(ctx, ext->imp_put_field),
                                          MIR_new_reg_op(ctx, result),
                                          MIR_new_reg_op(ctx, r_vm),
                                          MIR_new_reg_op(ctx, r_js),
                                          MIR_new_reg_op(ctx, pf_obj),
                                          MIR_new_reg_op(ctx, pf_val),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)pf_atom),
                                          MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)pf_ic)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_URSH,
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_reg_op(ctx, result),
                                     MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BNE,
                                     MIR_new_label_op(ctx, pf_ok),
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        MIR_append_insn(ctx, jit_func, pf_ok);
        break;
      }

      case OP_GET_ELEM: {
        INL_FLUSH_SLOT(isp - 2);
        uint8_t feedback = sv_func_type_feedback(callee)
                               ? sv_func_type_feedback(callee)[inl_bc_off]
                               : 0;
        bool specialize = sv_tfb_specialization_ready(feedback);
        bool key_is_num = inl_num[isp - 1] != 0;
        MIR_reg_t key_double = inl_d[isp - 1];

        if (specialize) {
          INL_ENSURE_D_SLOT();
          MIR_reg_t ge_key = inl_vs[--isp];
          MIR_reg_t ge_obj = inl_vs[--isp];
          (void)inl_vs[isp++];
          int index_site = mir_next_reg_site(p_reg_site);
          int element_site = mir_next_reg_site(p_reg_site);
          MIR_reg_t index = mir_emit_array_index_guard(
              ctx, jit_func, ge_key, key_double, key_is_num,
              *p_d_slot, slow, index_site);
          (void)mir_emit_dense_element_guard(
              ctx, jit_func, ge_obj, index, r_bool, JIT_ELEMENT_NUMERIC_READ, slow, element_site);
          mir_i64_to_d(
              ctx, jit_func, inl_d[isp - 1], r_bool, *p_d_slot);
          inl_num[isp - 1] = 1;
          inl_num[isp] = 0;
          break;
        }

        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t ge_key = inl_vs[--isp];
        MIR_reg_t ge_obj = inl_vs[--isp];
        MIR_reg_t ge_dst = inl_vs[isp++];
        inl_num[isp - 1] = 0;
        MIR_label_t ge_ok = MIR_new_label(ctx);
        MIR_label_t ge_slow = MIR_new_label(ctx);
        INL_ENSURE_D_SLOT();
        MIR_reg_t ge_index = mir_emit_array_index_guard(
            ctx, jit_func, ge_key, key_double, key_is_num, *p_d_slot, ge_slow,
            mir_next_reg_site(p_reg_site));
        (void)mir_emit_dense_element_guard(
            ctx, jit_func, ge_obj, ge_index, r_bool, JIT_ELEMENT_READ,
            ge_slow, mir_next_reg_site(p_reg_site));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, ge_dst), MIR_new_reg_op(ctx, r_bool)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, ge_ok)));
        MIR_append_insn(ctx, jit_func, ge_slow);
        mir_call_helper2(ctx, jit_func, ge_dst,
                         helper2_proto, ext->imp_get_elem_inline,
                         r_vm, r_js, ge_obj, ge_key);
        mir_emit_inline_read_guard(
            ctx, jit_func, ge_dst, result, r_bool, slow, join, ge_ok);
        MIR_append_insn(ctx, jit_func, ge_ok);
        break;
      }

      case OP_GET_LENGTH: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t gl_obj = inl_vs[--isp];
        MIR_reg_t gl_dst = inl_vs[isp++];
        MIR_label_t gl_ok = MIR_new_label(ctx);
        INL_ENSURE_D_SLOT();
        mir_emit_get_length(
            ctx, jit_func, gl_obj, gl_dst,
            r_vm, r_js, *p_d_slot,
            ext->helper1_proto, ext->imp_get_length_inline,
            false,
            id, inl_bc_off);
        mir_emit_inline_read_guard(
            ctx, jit_func, gl_dst, result, r_bool, slow, join, gl_ok);
        MIR_append_insn(ctx, jit_func, gl_ok);
        break;
      }

      case OP_BAND:
      case OP_BOR:
      case OP_BXOR:
      case OP_SHL:
      case OP_SHR:
      case OP_USHR: {
        uint8_t feedback = sv_func_type_feedback(callee)
                               ? sv_func_type_feedback(callee)[inl_bc_off]
                               : 0;
        bool specialize = sv_tfb_specialization_ready(feedback);
        bool l_is_num = inl_num[isp - 2] != 0;
        bool r_is_num = inl_num[isp - 1] != 0;

        if (specialize) {
          INL_ENSURE_D_SLOT();
          MIR_reg_t bw_rr = inl_vs[--isp];
          MIR_reg_t bw_rl = inl_vs[--isp];
          MIR_reg_t bw_rd = inl_vs[isp++];
          int left_site = mir_next_reg_site(p_reg_site);
          int right_site = mir_next_reg_site(p_reg_site);
          int result_site = mir_next_reg_site(p_reg_site);
          MIR_reg_t left = mir_emit_word32_guard(
              ctx, jit_func, bw_rl, inl_d[isp - 1], l_is_num, false,
              *p_d_slot, slow, left_site);
          MIR_reg_t right = mir_emit_word32_guard(
              ctx, jit_func, bw_rr, inl_d[isp], r_is_num, false,
              *p_d_slot, slow, right_site);
          mir_emit_word32_binary(
              ctx, jit_func, op, left, right, bw_rd,
              inl_d[isp - 1], true, result_site);
          inl_num[isp - 1] = 1;
          inl_num[isp] = 0;
          break;
        }

        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 1], slow);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 2], slow);
        MIR_reg_t bw_rr = inl_vs[--isp];
        MIR_reg_t bw_rl = inl_vs[--isp];
        MIR_reg_t bw_rd = inl_vs[isp++];
        inl_num[isp - 1] = 0;
        MIR_item_t bw_imp;
        switch (op) {
          case OP_BAND:
            bw_imp = ext->imp_band;
            break;
          case OP_BOR:
            bw_imp = ext->imp_bor;
            break;
          case OP_BXOR:
            bw_imp = ext->imp_bxor;
            break;
          case OP_SHL:
            bw_imp = ext->imp_shl;
            break;
          case OP_SHR:
            bw_imp = ext->imp_shr;
            break;
          default:
            bw_imp = ext->imp_ushr;
            break;
        }
        mir_call_helper2(ctx, jit_func, bw_rd,
                         helper2_proto, bw_imp, r_vm, r_js, bw_rl, bw_rr);
        break;
      }

      case OP_CALL_STABLE_BUILTIN: {
        int stable_kind = ip[1];
        uint16_t stable_argc = sv_get_u16(ip + 2);
        ANT_ASSERT(stable_argc <= SV_JIT_ARGS_BUF_CAP,
                   "nested stable-builtin call escaped feasibility");
        ANT_ASSERT(isp >= (int)stable_argc + 2,
                   "invalid inline stable-builtin call stack depth");
        INL_FLUSH_ALL();

        MIR_reg_t stable_arg0 = stable_argc > 0
                                    ? inl_vs[isp - (int)stable_argc]
                                    : 0;
        bool stable_args_prepared = !(
            stable_kind == SV_STABLE_BUILTIN_PROMISE_RESOLVE &&
            stable_argc == 1);
        for (int i = (int)stable_argc - 1; i >= 0; i--)
          if (stable_args_prepared)
            MIR_append_insn(ctx, jit_func,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_mem_op(ctx, MIR_JSVAL,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                        ext->r_args_buf, 0, 1),
                                         MIR_new_reg_op(ctx, inl_vs[isp - (int)stable_argc + i])));
        isp -= (int)stable_argc;
        MIR_reg_t stable_fn = inl_vs[--isp];
        MIR_reg_t stable_this = inl_vs[--isp];
        MIR_reg_t stable_dst = inl_vs[isp++];

        int stable_site = -(id * 100000 + inl_bc_off + 1);
        mir_emit_call_stable_builtin(
            ctx, jit_func, js, r_vm, r_js,
            stable_kind, stable_fn, stable_this,
            stable_arg0, ext->r_args_buf, stable_argc, stable_dst,
            ext->stable_call_proto, ext->imp_call_stable_builtin,
            false, stable_args_prepared,
            stable_site);

        MIR_label_t stable_ok = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_URSH,
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_reg_op(ctx, stable_dst),
                                     MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_BNE,
                                     MIR_new_label_op(ctx, stable_ok),
                                     MIR_new_reg_op(ctx, r_bool),
                                     MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, result),
                                     MIR_new_reg_op(ctx, stable_dst)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        MIR_append_insn(ctx, jit_func, stable_ok);
        break;
      }

      case OP_CALL:
      case OP_TAIL_CALL:
      case OP_CALL_METHOD:
      case OP_TAIL_CALL_METHOD: {
        bool nc_method = (op == OP_CALL_METHOD || op == OP_TAIL_CALL_METHOD);
        bool nc_tail = (op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD);
        uint16_t nc_argc = sv_get_u16(ip + 1);
        ANT_ASSERT(nc_argc <= SV_JIT_ARGS_BUF_CAP,
                   "nested inline call escaped feasibility");
        ANT_ASSERT(isp >= (int)nc_argc + (nc_method ? 2 : 1),
                   "invalid inline call stack depth");
        INL_FLUSH_ALL();

        for (int i = (int)nc_argc - 1; i >= 0; i--)
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_mem_op(ctx, MIR_JSVAL,
                                                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                      ext->r_args_buf, 0, 1),
                                       MIR_new_reg_op(ctx, inl_vs[isp - (int)nc_argc + i])));
        isp -= (int)nc_argc;
        MIR_reg_t nc_fn = inl_vs[--isp];
        MIR_reg_t nc_this = nc_method ? inl_vs[--isp] : inl_undef;
        MIR_reg_t nc_dst = nc_tail ? result : inl_vs[isp++];

        MIR_label_t dv_generic = MIR_new_label(ctx);
        MIR_label_t dv_done = MIR_new_label(ctx);
        {
          int dv_off = (int)(ip - callee->code);
          char dv_rn[48];
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_cl", id, dv_off);
          MIR_reg_t r_dv_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, dv_rn);
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_fn", id, dv_off);
          MIR_reg_t r_dv_fn = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, dv_rn);
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_jp", id, dv_off);
          MIR_reg_t r_dv_jp = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, dv_rn);
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_this", id, dv_off);
          MIR_reg_t r_dv_this = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, dv_rn);
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_sup", id, dv_off);
          MIR_reg_t r_dv_sup = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, dv_rn);
          snprintf(dv_rn, sizeof(dv_rn), "inl%d_dv%d_bnd", id, dv_off);
          MIR_reg_t r_dv_bound = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, dv_rn);

          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_BEQ,
                                       MIR_new_label_op(ctx, dv_generic),
                                       MIR_new_reg_op(ctx, nc_fn),
                                       MIR_new_reg_op(ctx, r_inl_super)));
          mir_emit_get_closure(ctx, jit_func, r_dv_cl, nc_fn, r_bool, dv_generic);
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_mem_op(ctx, MIR_T_U32,
                                                      (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                                                      r_dv_cl, 0, 1)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_AND,
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_uint_op(ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_BNE,
                                       MIR_new_label_op(ctx, dv_generic),
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_uint_op(ctx, 0)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_dv_fn),
                                       MIR_new_mem_op(ctx, MIR_T_P,
                                                      (MIR_disp_t)offsetof(sv_closure_t, func),
                                                      r_dv_cl, 0, 1)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_dv_sup),
                                       MIR_new_mem_op(ctx, MIR_T_I64,
                                                      (MIR_disp_t)offsetof(sv_closure_t, super_val),
                                                      r_dv_cl, 0, 1)));
          mir_emit_resolve_call_this(ctx, jit_func, r_dv_this, r_dv_cl,
                                     nc_this, r_bool, r_dv_bound);
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_BEQ,
                                       MIR_new_label_op(ctx, dv_generic),
                                       MIR_new_reg_op(ctx, r_dv_fn),
                                       MIR_new_int_op(ctx, 0)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_dv_jp),
                                       MIR_new_mem_op(ctx, MIR_T_P,
                                                      (MIR_disp_t)offsetof(sv_func_t, jit_code),
                                                      r_dv_fn, 0, 1)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_BEQ,
                                       MIR_new_label_op(ctx, dv_generic),
                                       MIR_new_reg_op(ctx, r_dv_jp),
                                       MIR_new_int_op(ctx, 0)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_call_insn(ctx, 10,
                                            MIR_new_ref_op(ctx, ext->self_proto),
                                            MIR_new_reg_op(ctx, r_dv_jp),
                                            MIR_new_reg_op(ctx, nc_dst),
                                            MIR_new_reg_op(ctx, r_vm),
                                            MIR_new_reg_op(ctx, r_dv_this),
                                            MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0)),
                                            MIR_new_reg_op(ctx, r_dv_sup),
                                            MIR_new_reg_op(ctx, ext->r_args_buf),
                                            MIR_new_int_op(ctx, (int64_t)nc_argc),
                                            MIR_new_reg_op(ctx, r_dv_cl)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, dv_done)));
        }
        MIR_append_insn(ctx, jit_func, dv_generic);

        if (nc_method) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_call_insn(ctx, 12,
                                            MIR_new_ref_op(ctx, ext->call_method_proto),
                                            MIR_new_ref_op(ctx, ext->imp_call_method),
                                            MIR_new_reg_op(ctx, nc_dst),
                                            MIR_new_reg_op(ctx, r_vm),
                                            MIR_new_reg_op(ctx, r_js),
                                            MIR_new_reg_op(ctx, nc_fn),
                                            MIR_new_reg_op(ctx, nc_this),
                                            MIR_new_reg_op(ctx, ext->r_args_buf),
                                            MIR_new_int_op(ctx, (int64_t)nc_argc),
                                            MIR_new_reg_op(ctx, inl_undef),
                                            MIR_new_reg_op(ctx, inl_undef),
                                            MIR_new_int_op(ctx, 0)));
        } else {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_call_insn(ctx, 9,
                                            MIR_new_ref_op(ctx, ext->call_proto),
                                            MIR_new_ref_op(ctx, ext->imp_call),
                                            MIR_new_reg_op(ctx, nc_dst),
                                            MIR_new_reg_op(ctx, r_vm),
                                            MIR_new_reg_op(ctx, r_js),
                                            MIR_new_reg_op(ctx, nc_fn),
                                            MIR_new_reg_op(ctx, nc_this),
                                            MIR_new_reg_op(ctx, ext->r_args_buf),
                                            MIR_new_int_op(ctx, (int64_t)nc_argc)));
        }
        MIR_append_insn(ctx, jit_func, dv_done);

        if (nc_tail) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        } else {
          MIR_label_t nc_ok = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_URSH,
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_reg_op(ctx, nc_dst),
                                       MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_BNE,
                                       MIR_new_label_op(ctx, nc_ok),
                                       MIR_new_reg_op(ctx, r_bool),
                                       MIR_new_uint_op(ctx, JIT_ERR_TAG)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, result),
                                       MIR_new_reg_op(ctx, nc_dst)));
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
          MIR_append_insn(ctx, jit_func, nc_ok);
        }
        break;
      }

      case OP_RETURN: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t ret = inl_vs[--isp];
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, result),
                                     MIR_new_reg_op(ctx, ret)));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        break;
      }
      case OP_RETURN_UNDEF:
        mir_load_imm(ctx, jit_func, result, mkval(kTypeUndefined, 0));
        MIR_append_insn(ctx, jit_func,
                        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        break;

      case OP_SPECIAL_OBJ: {
        INL_FLUSH_ALL();
        uint8_t which = sv_get_u8(ip + 1);
        MIR_reg_t dst = inl_vs[isp++];
        if (which == 1) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, dst),
                                       MIR_new_reg_op(ctx, r_inl_new_target)));
        } else if (which == 2) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, dst),
                                       MIR_new_reg_op(ctx, r_inl_super)));
        } else if (which == 3) {
          MIR_append_insn(ctx, jit_func,
                          MIR_new_call_insn(ctx, 6,
                                            MIR_new_ref_op(ctx, special_obj_proto),
                                            MIR_new_ref_op(ctx, imp_special_obj),
                                            MIR_new_reg_op(ctx, dst),
                                            MIR_new_reg_op(ctx, r_vm),
                                            MIR_new_reg_op(ctx, r_js),
                                            MIR_new_int_op(ctx, (int64_t)which)));
        } else {
          mir_load_imm(ctx, jit_func, dst, mkval(kTypeUndefined, 0));
        }
        break;
      }

      case OP_NOP:
      case OP_LINE_NUM:
      case OP_COL_NUM:
      case OP_LABEL:
        break;

      default:
        ANT_ASSERT(false, "inlineable opcode has no emitter");
    }
    ip += sz;
  }
}
