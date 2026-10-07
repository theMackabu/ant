#include "compile.h"
#include "silver/feedback.h"

static MIR_reg_t jit_emit_element_index_guard(
    jit_compile_t *c, MIR_reg_t key, MIR_reg_t integer_index,
    MIR_reg_t double_key, bool key_is_num, MIR_label_t slow, int site) {
  if (integer_index)
    return mir_emit_known_array_index_guard(
        c->ctx, c->jit_func, key, integer_index, slow,
        c->r_d_slot, site, c->cached_index_key, c->cached_index_value);
  return mir_emit_array_index_guard(
      c->ctx, c->jit_func, key, double_key, key_is_num,
      c->r_d_slot, slow, site);
}

static void jit_gfp_regs(jit_compile_t *c) {
  if (c->gfp_regs) return;
  MIR_reg_t *regs[] = {
    &c->r_gfp_ptr, &c->r_gfp_tag, &c->r_gfp_shape,
    &c->r_gfp_epoch, &c->r_gfp_e, &c->r_gfp_end, &c->r_gfp_t, &c->r_gfp_t2, &c->r_gfp_src,
    &c->r_gfp_idx, &c->r_gfp_lim,
  };
  for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
    char name[16];
    snprintf(name, sizeof(name), "gfp%zu", i);
    *regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  }
  c->gfp_regs = true;
}

#define GFP_INSN(...) MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, __VA_ARGS__))
#define GFP_REG(r) MIR_new_reg_op(c->ctx, r)
#define GFP_MEM(t, off, base) MIR_new_mem_op(c->ctx, t, (MIR_disp_t)(off), base, 0, 1)
#define GFP_LBL(l) MIR_new_label_op(c->ctx, l)
#define GFP_INT(v) MIR_new_int_op(c->ctx, v)
#define GFP_UINT(v) MIR_new_uint_op(c->ctx, v)

static void jit_emit_gfp_exotic_guard(jit_compile_t *c, MIR_reg_t ptr, MIR_label_t full) {
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U16, offsetof(ant_object_t, flags), ptr));
  GFP_INSN(MIR_AND, GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t), GFP_UINT(ANT_OBJECT_FLAG_EXOTIC));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(0));
}

static void jit_emit_get_field_poly_loop(
    jit_compile_t *c, sv_ic_entry_t *ic, MIR_reg_t obj, MIR_reg_t dst,
    MIR_label_t hit, MIR_label_t full) {
  MIR_label_t loop = MIR_new_label(c->ctx), next = MIR_new_label(c->ctx);
  MIR_label_t own = MIR_new_label(c->ctx), read = MIR_new_label(c->ctx);
  MIR_label_t load_overflow = MIR_new_label(c->ctx);

  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_e), GFP_UINT((uint64_t)(uintptr_t)&ic->guard.get.poly));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_e), GFP_MEM(MIR_T_P, 0, c->r_gfp_e));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_e), GFP_INT(0));
  mir_emit_value_to_objptr_or_jmp(c->ctx, c->jit_func, obj, c->r_gfp_ptr, c->r_gfp_tag, full);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_ptr, full);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_shape), GFP_MEM(MIR_T_P, offsetof(ant_object_t, shape), c->r_gfp_ptr));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_shape), GFP_INT(0));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_epoch), GFP_MEM(MIR_T_U32, 0, c->r_ic_epoch_val));
  GFP_INSN(MIR_ADD, GFP_REG(c->r_gfp_end), GFP_REG(c->r_gfp_e),
           GFP_UINT(SV_GF_POLY_WAYS * sizeof(sv_gf_poly_entry_t)));

  MIR_append_insn(c->ctx, c->jit_func, loop);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(sv_gf_poly_entry_t, shape), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(next), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_shape));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(sv_gf_poly_entry_t, epoch), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(next), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_epoch));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U8, offsetof(sv_gf_poly_entry_t, kind), c->r_gfp_e));
  GFP_INSN(MIR_BEQ, GFP_LBL(own), GFP_REG(c->r_gfp_t), GFP_INT(SV_GF_IC_OWN));
  GFP_INSN(MIR_BNE, GFP_LBL(next), GFP_REG(c->r_gfp_t), GFP_INT(SV_GF_IC_PROTOTYPE));

  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_JSVAL, offsetof(ant_object_t, proto), c->r_gfp_ptr));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t2), GFP_MEM(MIR_JSVAL, offsetof(sv_gf_poly_entry_t, receiver_proto), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(next), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2));
  GFP_INSN(MIR_URSH, GFP_REG(c->r_gfp_t2), GFP_REG(c->r_gfp_t), GFP_UINT(NANBOX_TYPE_SHIFT));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t2), GFP_UINT(NANBOX_TOBJ_TAG));
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_gfp_t, c->r_gfp_t);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(ant_object_t, ic_identity), c->r_gfp_t));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t2), GFP_MEM(MIR_T_U32, offsetof(sv_gf_poly_entry_t, proto_id), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(next), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_src), GFP_MEM(MIR_T_P, offsetof(sv_gf_poly_entry_t, holder), c->r_gfp_e));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_src), GFP_INT(0));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(ant_object_t, shape), c->r_gfp_src));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(0));
  jit_emit_gfp_exotic_guard(c, c->r_gfp_src, full);
  GFP_INSN(MIR_JMP, GFP_LBL(read));

  MIR_append_insn(c->ctx, c->jit_func, own);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_src), GFP_REG(c->r_gfp_ptr));
  GFP_INSN(MIR_JMP, GFP_LBL(read));

  MIR_append_insn(c->ctx, c->jit_func, next);
  GFP_INSN(MIR_ADD, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_UINT(sizeof(sv_gf_poly_entry_t)));
  GFP_INSN(MIR_UBLT, GFP_LBL(loop), GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_end));
  GFP_INSN(MIR_JMP, GFP_LBL(full));

  MIR_append_insn(c->ctx, c->jit_func, read);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_idx), GFP_MEM(MIR_T_U32, offsetof(sv_gf_poly_entry_t, index), c->r_gfp_e));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(ant_object_t, prop_count), c->r_gfp_src));
  GFP_INSN(MIR_UBGE, GFP_LBL(full), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_t));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_lim), GFP_MEM(MIR_T_U8, offsetof(ant_object_t, inobj_limit), c->r_gfp_src));
  GFP_INSN(MIR_UBGE, GFP_LBL(load_overflow), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_lim));
  GFP_INSN(MIR_MOV, GFP_REG(dst), MIR_new_mem_op(c->ctx, MIR_T_I64,
           (MIR_disp_t)offsetof(ant_object_t, inobj), c->r_gfp_src, c->r_gfp_idx, 8));
  GFP_INSN(MIR_JMP, GFP_LBL(hit));

  MIR_append_insn(c->ctx, c->jit_func, load_overflow);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(ant_object_t, overflow_prop), c->r_gfp_src));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(0));
  GFP_INSN(MIR_SUB, GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_lim));
  GFP_INSN(MIR_MOV, GFP_REG(dst), MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_gfp_t, c->r_gfp_idx, 8));
  GFP_INSN(MIR_JMP, GFP_LBL(hit));
}

static void jit_emit_get_field_mega_probe(
    jit_compile_t *c, sv_gf_mega_cache_t *cache, const char *key,
    MIR_reg_t obj, MIR_reg_t dst, MIR_label_t hit, MIR_label_t full) {
  MIR_label_t own = MIR_new_label(c->ctx), read = MIR_new_label(c->ctx);
  MIR_label_t load_overflow = MIR_new_label(c->ctx);

  mir_emit_value_to_objptr_or_jmp(c->ctx, c->jit_func, obj, c->r_gfp_ptr, c->r_gfp_tag, full);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_ptr, full);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_shape), GFP_MEM(MIR_T_P, offsetof(ant_object_t, shape), c->r_gfp_ptr));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_shape), GFP_INT(0));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t2), GFP_MEM(MIR_JSVAL, offsetof(ant_object_t, proto), c->r_gfp_ptr));

  GFP_INSN(MIR_URSH, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_shape), GFP_UINT(SV_GF_MEGA_SHAPE_SHIFT));
  GFP_INSN(MIR_XOR, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e),
           GFP_UINT((uint64_t)(uintptr_t)key >> SV_GF_MEGA_KEY_SHIFT));
  GFP_INSN(MIR_URSH, GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2), GFP_UINT(SV_GF_MEGA_PROTO_SHIFT));
  GFP_INSN(MIR_XOR, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_t));
  GFP_INSN(MIR_URSH, GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_e), GFP_UINT(SV_GF_MEGA_MIX_SHIFT));
  GFP_INSN(MIR_XOR, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_t));
  GFP_INSN(MIR_AND, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_UINT(SV_GF_MEGA_PRIMARY - 1));
  GFP_INSN(MIR_MUL, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_UINT(sizeof(sv_gf_mega_entry_t)));
  GFP_INSN(MIR_ADD, GFP_REG(c->r_gfp_e), GFP_REG(c->r_gfp_e), GFP_UINT((uint64_t)(uintptr_t)cache->primary));

  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(sv_gf_mega_entry_t, shape), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_shape));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(sv_gf_mega_entry_t, key), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_UINT((uint64_t)(uintptr_t)key));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_JSVAL, offsetof(sv_gf_mega_entry_t, receiver_proto), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_epoch), GFP_MEM(MIR_T_U32, 0, c->r_ic_epoch_val));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(sv_gf_mega_entry_t, epoch), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_epoch));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U8, offsetof(sv_gf_mega_entry_t, kind), c->r_gfp_e));
  GFP_INSN(MIR_BEQ, GFP_LBL(own), GFP_REG(c->r_gfp_t), GFP_INT(SV_GF_IC_OWN));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(SV_GF_IC_PROTOTYPE));

  GFP_INSN(MIR_URSH, GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2), GFP_UINT(NANBOX_TYPE_SHIFT));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_UINT(NANBOX_TOBJ_TAG));
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_gfp_t, c->r_gfp_t2);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(ant_object_t, ic_identity), c->r_gfp_t));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t2), GFP_MEM(MIR_T_U32, offsetof(sv_gf_mega_entry_t, proto_id), c->r_gfp_e));
  GFP_INSN(MIR_BNE, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_REG(c->r_gfp_t2));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_src), GFP_MEM(MIR_T_P, offsetof(sv_gf_mega_entry_t, holder), c->r_gfp_e));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_src), GFP_INT(0));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(ant_object_t, shape), c->r_gfp_src));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(0));
  jit_emit_gfp_exotic_guard(c, c->r_gfp_src, full);
  GFP_INSN(MIR_JMP, GFP_LBL(read));

  MIR_append_insn(c->ctx, c->jit_func, own);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_src), GFP_REG(c->r_gfp_ptr));

  MIR_append_insn(c->ctx, c->jit_func, read);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_idx), GFP_MEM(MIR_T_U32, offsetof(sv_gf_mega_entry_t, index), c->r_gfp_e));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_U32, offsetof(ant_object_t, prop_count), c->r_gfp_src));
  GFP_INSN(MIR_UBGE, GFP_LBL(full), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_t));
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_lim), GFP_MEM(MIR_T_U8, offsetof(ant_object_t, inobj_limit), c->r_gfp_src));
  GFP_INSN(MIR_UBGE, GFP_LBL(load_overflow), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_lim));
  GFP_INSN(MIR_MOV, GFP_REG(dst), MIR_new_mem_op(c->ctx, MIR_T_I64,
           (MIR_disp_t)offsetof(ant_object_t, inobj), c->r_gfp_src, c->r_gfp_idx, 8));
  GFP_INSN(MIR_JMP, GFP_LBL(hit));

  MIR_append_insn(c->ctx, c->jit_func, load_overflow);
  GFP_INSN(MIR_MOV, GFP_REG(c->r_gfp_t), GFP_MEM(MIR_T_P, offsetof(ant_object_t, overflow_prop), c->r_gfp_src));
  GFP_INSN(MIR_BEQ, GFP_LBL(full), GFP_REG(c->r_gfp_t), GFP_INT(0));
  GFP_INSN(MIR_SUB, GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_idx), GFP_REG(c->r_gfp_lim));
  GFP_INSN(MIR_MOV, GFP_REG(dst), MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_gfp_t, c->r_gfp_idx, 8));
  GFP_INSN(MIR_JMP, GFP_LBL(hit));
}

static void jit_emit_get_field_poly(
    jit_compile_t *c, uint16_t ic_idx, sv_atom_t *atom,
    MIR_reg_t obj, MIR_reg_t dst, MIR_label_t hit) {
  if (!c->func->ic_slots || ic_idx >= c->func->ic_count) return;
  sv_ic_entry_t *ic = &c->func->ic_slots[ic_idx];
  const sv_gf_poly_t *poly = ic->guard.get.poly;
  bool mega = (ic->shape_ref_mask & SV_IC_POLY_MEGA) != 0;
  if ((!poly && !mega) || is_length_key(atom->str, atom->len)) return;
  if (ic->get_kind == SV_GF_IC_SYMBOL_DESCRIPTION ||
      ic->get_kind == SV_GF_IC_PRIMITIVE_SYMBOL_DESCRIPTION) return;
  if (mega) ic->shape_ref_mask |= SV_IC_MEGA_COMPILED;

  if (!mega && c->func->code_len > JIT_GFP_MAX_CODE_LEN) return;
  bool probe_shared = mega;
  sv_gf_mega_cache_t *cache = probe_shared ? sv_gf_mega_ensure(c->js) : NULL;
  if (probe_shared && !cache) return;
  if (!probe_shared && poly->kept_epoch == c->js->ic.epoch && poly->kept >= SV_GF_POLY_WAYS) return;

  jit_gfp_regs(c);
  MIR_label_t full = MIR_new_label(c->ctx);
  if (probe_shared) jit_emit_get_field_mega_probe(c, cache, atom->str, obj, dst, hit, full);
  else jit_emit_get_field_poly_loop(c, ic, obj, dst, hit, full);
  MIR_append_insn(c->ctx, c->jit_func, full);
}

#undef GFP_INSN
#undef GFP_REG
#undef GFP_MEM
#undef GFP_LBL
#undef GFP_INT
#undef GFP_UINT

static constexpr int32_t JIT_DEFINE_SLOT_INLINE_MAX_CODE_LEN = 1024;

static void jit_emit_define_slot_inline(
    jit_compile_t *c, sv_atom_t *atom, uint16_t slot,
    MIR_reg_t obj, MIR_reg_t val, MIR_label_t done) {
  if (c->func->code_len > JIT_DEFINE_SLOT_INLINE_MAX_CODE_LEN) return;
  ant_t *js = c->js;
  const char *key = atom->str;
  if (!key || ant_property_key_is_watched(js, key)) return;

  char name[40];
  snprintf(name, sizeof(name), "ds_p_%d", c->bc_off);
  MIR_reg_t ptr = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "ds_t_%d", c->bc_off);
  MIR_reg_t t = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "ds_l_%d", c->bc_off);
  MIR_reg_t lim = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  MIR_label_t helper = MIR_new_label(c->ctx), overflow = MIR_new_label(c->ctx);

  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_URSH, MIR_new_reg_op(c->ctx, t),
      MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, helper),
      MIR_new_reg_op(c->ctx, t), MIR_new_uint_op(c->ctx, NANBOX_TOBJ_TAG)));
  mir_emit_decode_ref(c->ctx, c->jit_func, ptr, obj);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, t),
      MIR_new_mem_op(c->ctx, MIR_T_U16, (MIR_disp_t)offsetof(ant_object_t, flags), ptr, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_AND, MIR_new_reg_op(c->ctx, t),
      MIR_new_reg_op(c->ctx, t), MIR_new_uint_op(c->ctx, ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_GENERATION)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, helper),
      MIR_new_reg_op(c->ctx, t), MIR_new_uint_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, t),
      MIR_new_mem_op(c->ctx, MIR_T_U32, (MIR_disp_t)offsetof(ant_object_t, prop_count), ptr, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, helper),
      MIR_new_reg_op(c->ctx, t), MIR_new_uint_op(c->ctx, slot)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, lim),
      MIR_new_mem_op(c->ctx, MIR_T_U8, (MIR_disp_t)offsetof(ant_object_t, inobj_limit), ptr, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, overflow),
      MIR_new_reg_op(c->ctx, lim), MIR_new_uint_op(c->ctx, slot)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)(offsetof(ant_object_t, inobj) + slot * sizeof(ant_value_t)), ptr, 0, 1),
      MIR_new_reg_op(c->ctx, val)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));

  MIR_append_insn(c->ctx, c->jit_func, overflow);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, t),
      MIR_new_mem_op(c->ctx, MIR_T_P, (MIR_disp_t)offsetof(ant_object_t, overflow_prop), ptr, 0, 1)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, helper),
      MIR_new_reg_op(c->ctx, t), MIR_new_uint_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, lim),
      MIR_new_uint_op(c->ctx, slot), MIR_new_reg_op(c->ctx, lim)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, t, lim, 8), MIR_new_reg_op(c->ctx, val)));
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
  MIR_append_insn(c->ctx, c->jit_func, helper);
}

void jit_emit_element_barrier(
    jit_compile_t *c, MIR_reg_t obj, MIR_reg_t index,
    MIR_reg_t val, MIR_reg_t flags, MIR_label_t skip) {
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, skip),
                               MIR_new_reg_op(c->ctx, val), MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_AND, MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, flags),
                               MIR_new_uint_op(c->ctx, ANT_OBJECT_FLAG_GENERATION)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, skip),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_int_op(c->ctx, 0)));
  MIR_label_t string = MIR_new_label(c->ctx);
  MIR_label_t barrier = MIR_new_label(c->ctx);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_URSH, MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, val), MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, string),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_uint_op(c->ctx, JIT_STR_TAG)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_uint_op(c->ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeUndefined)));
  static_assert(kTypeNull == kTypeUndefined + 1 && kTypeBool == kTypeUndefined + 2);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, skip),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_uint_op(c->ctx, 2)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, barrier)));
  // only ropes are ever young
  MIR_append_insn(c->ctx, c->jit_func, string);
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_bool, val);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_AND, MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_uint_op(c->ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, skip),
                               MIR_new_reg_op(c->ctx, c->r_bool), MIR_new_uint_op(c->ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(c->ctx, c->jit_func, barrier);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->elem_barrier_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_elem_barrier),
                                    MIR_new_reg_op(c->ctx, c->r_js),
                                    MIR_new_reg_op(c->ctx, obj),
                                    MIR_new_reg_op(c->ctx, index),
                                    MIR_new_reg_op(c->ctx, val)));
}

static bool jit_field_ic_may_load(jit_compile_t *c, ant_value_t builtin) {
  sv_func_t *func = c->func;
  uint16_t ic_idx = sv_get_u16(c->ip + 5);
  if (!func->ic_slots || ic_idx >= func->ic_count) return true;
  
  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  if (!sv_gf_ic_active(ic->cached_aux) || !ic->cached_shape) return true;
  if (ic->get_kind != SV_GF_IC_PROTOTYPE && ic->get_kind != SV_GF_IC_PRIMITIVE_DATA) return false;
  
  ant_object_t *holder = ic->cached_holder;
  if (!holder || ic->cached_index >= holder->prop_count) return true;
  return ant_object_prop_get_unchecked(holder, ic->cached_index) == builtin;
}

static void jit_note_known_builtin(jit_compile_t *c, const sv_atom_t *atom) {
  if (!c->vs.known_builtin) return;
  // a method load keeps its receiver below the method on the stack
  if (c->vs.sp >= 2 && c->vs.known_builtin[c->vs.sp - 2] == JIT_BUILTIN_MATH) {
    c->vs.known_builtin[c->vs.sp - 1] = jit_math_field_builtin(JIT_BUILTIN_MATH, atom->str, atom->len);
    return;
  }
  if (atom->len == 4 && memcmp(atom->str, "push", 4) == 0 && jit_field_ic_may_load(c, c->js->sym.array_push_fn))
    c->vs.known_builtin[c->vs.sp - 1] = JIT_BUILTIN_ARRAY_PUSH;
  else if (atom->len == 8 && memcmp(atom->str, "toString", 8) == 0 && jit_field_ic_may_load(c, c->js->sym.number_to_string_fn))
    c->vs.known_builtin[c->vs.sp - 1] = JIT_BUILTIN_NUMBER_TO_STRING;
}

void jit_emit_properties(jit_compile_t *c) {
  switch (c->op) {
    case OP_TO_PROPKEY: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t is_key = MIR_new_label(c->ctx);
      MIR_label_t pk_done = MIR_new_label(c->ctx);
      MIR_label_t pk_helper = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBLE,
                                   MIR_new_label_op(c->ctx, pk_helper),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_AND,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_MASK)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_key),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeString)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, is_key),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeSymbol)));
      MIR_append_insn(c->ctx, c->jit_func, pk_helper);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->helper1_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_to_propkey),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, pk_done)));
      MIR_append_insn(c->ctx, c->jit_func, is_key);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func, pk_done);
      break;
    }

    case OP_GET_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      jit_note_known_builtin(c, atom);
      break;
    }

    case OP_GET_FIELD2: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      jit_note_known_builtin(c, atom);
      break;
    }

    case OP_GET_FIELD_OPT: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      MIR_label_t nullish = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      bool fast = mir_emit_get_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
          c->r_ic_epoch_val);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->gf_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_field),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_LENGTH: {
      bool builder_length = false;
      int raw_slot_size = sv_op_size[OP_GET_SLOT_RAW];
      if (c->builder_target_slots && c->bc_off >= raw_slot_size) {
        uint8_t *prev_ip = c->ip - raw_slot_size;
        if (*prev_ip == OP_GET_SLOT_RAW) {
          uint16_t slot_idx = sv_get_u16(prev_ip + 1);
          int slot_count = c->param_count + c->n_locals;
          builder_length = (int)slot_idx < slot_count &&
                           c->builder_target_slots[slot_idx];
        }
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_emit_get_length(
          c->ctx, c->jit_func, c->js, obj, dst,
          c->r_js, c->r_d_slot,
          c->get_length_proto, c->imp_get_length,
          builder_length,
          -1, c->bc_off);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_DELETE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rk = vstack_pop(&c->vs);
      MIR_reg_t ro = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_delete,
                       c->r_vm, c->r_js, ro, rk);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      sv_ic_entry_t *ic_slot = NULL;
      uint16_t pf_ic_idx = sv_get_u16(c->ip + 5);
      if (c->func->ic_slots && pf_ic_idx != UINT16_MAX && pf_ic_idx < c->func->ic_count)
        ic_slot = &c->func->ic_slots[pf_ic_idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      bool fast = mir_emit_put_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, pf_ic_idx, atom,
          c->r_js, obj, val, slow, c->r_ic_epoch_val,
          c->shape_transition_proto, c->imp_shape_transition,
          c->remember_obj_proto, c->imp_remember_obj);
      if (fast) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, no_err)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->put_field_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_field),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ic_slot),
                                        MIR_new_int_op(c->ctx, c->func->is_strict)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_DEFINE_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->define_field_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_field),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len)));
      break;
    }

    case OP_DEFINE_SLOT: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      uint16_t slot = sv_get_u16(c->ip + 5);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_label_t define_done = MIR_new_label(c->ctx);
      jit_emit_define_slot_inline(c, atom, slot, obj, val, define_done);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->define_slot_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_slot),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, (int64_t)slot)));
      MIR_append_insn(c->ctx, c->jit_func, define_done);
      break;
    }

    case OP_DEFINE_METHOD_COMP: {
      uint8_t flags = sv_get_u8(c->ip + 1);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t fn_val = vstack_pop(&c->vs);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->define_method_comp_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_define_method_comp),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_reg_op(c->ctx, fn_val),
                                        MIR_new_int_op(c->ctx, (int64_t)flags)));
      break;
    }

    case OP_GET_ELEM: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 1] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, integer_index), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1])));
      }
      bool key_is_num = !integer_index && vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool obj_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);

      if (specialize && !obj_is_num) {
        c->element_available = false;
        MIR_reg_t key = vstack_pop(&c->vs);
        MIR_reg_t obj = vstack_pop(&c->vs);
        (void)vstack_push(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int index_site = mir_next_reg_site(&c->reg_site_n);
        int element_site = mir_next_reg_site(&c->reg_site_n);
        MIR_reg_t index = jit_emit_element_index_guard(
            c, key, integer_index, c->vs.d_regs[c->vs.sp], key_is_num,
            bail_direct, index_site);
        MIR_reg_t loaded = c->r_err_tmp;
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, loaded, JIT_ELEMENT_NUMERIC_READ, bail_direct, element_site, NULL);
        mir_i64_to_d(
            c->ctx, c->jit_func, c->vs.d_regs[c->vs.sp - 1], loaded, c->r_d_slot);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
            c->vs.sp - 1, SLOT_BOXED, c->vs.sp,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        MIR_append_insn(c->ctx, c->jit_func, done);
        if (c->previous_ip && c->integer_locals && c->integer_local_ranges &&
            c->dnum_locals && !c->func->has_dynamic_eval && c->ctx == c->jc->ctx_hot) {
          sv_op_t previous = *c->previous_ip;
          int local = previous == OP_GET_LOCAL8 || previous == OP_SET_LOCAL8
                    ? sv_get_u8(c->previous_ip + 1)
                    : previous == OP_GET_LOCAL || previous == OP_SET_LOCAL
                    ? sv_get_u16(c->previous_ip + 1) : -1;
          if (local >= 0 && local < c->n_locals && c->dnum_locals[local] &&
              !(c->captured_locals && c->captured_locals[local])) {
            char name[48];
            snprintf(name, sizeof(name), "validated_index_local_%d", c->integer_local_site++);
            c->integer_value = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
            MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
                MIR_new_reg_op(c->ctx, c->integer_value), MIR_new_reg_op(c->ctx, index)));
            c->integer_store = local;
            c->integer_range = (jit_integer_range_t){.min = 0, .max = UINT32_MAX - 1, .known = true};
          }
        }
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t element_done = NULL;
      if (!obj_is_num) {
        MIR_label_t slow = MIR_new_label(c->ctx);
        element_done = MIR_new_label(c->ctx);
        MIR_label_t loaded = MIR_new_label(c->ctx);
        if (c->element_available) {
          MIR_label_t miss = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, c->cached_element_valid), MIR_new_int_op(c->ctx, 0)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, c->cached_element_object)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, miss), MIR_new_reg_op(c->ctx, key), MIR_new_reg_op(c->ctx, c->cached_element_key)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->cached_element_value)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loaded)));
          MIR_append_insn(c->ctx, c->jit_func, miss);
        }
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_READ, slow,
            mir_next_reg_site(&c->reg_site_n), NULL);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_object), MIR_new_reg_op(c->ctx, obj)));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_key), MIR_new_reg_op(c->ctx, key)));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->cached_element_value), MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 1);
        MIR_append_insn(c->ctx, c->jit_func, loaded);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, element_done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 0);
      }
      c->element_available = !obj_is_num;
      sv_ic_entry_t *element_ic = code_arena_bump(c->js, sizeof(*element_ic));
      if (element_ic) memset(element_ic, 0, sizeof(*element_ic));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->ge_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_elem),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off),
                                        MIR_new_uint_op(c->ctx, (uintptr_t)element_ic)));
      jit_emit_throw_if_error(c, dst);
      if (element_done) MIR_append_insn(c->ctx, c->jit_func, element_done);
      break;
    }

    case OP_GET_ELEM_OPT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t nullish = MIR_new_label(c->ctx);
      MIR_label_t no_err = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, nullish),
                                   MIR_new_reg_op(c->ctx, obj),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      sv_ic_entry_t *element_ic = code_arena_bump(c->js, sizeof(*element_ic));
      if (element_ic) memset(element_ic, 0, sizeof(*element_ic));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->ge_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_get_elem),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off),
                                        MIR_new_uint_op(c->ctx, (uintptr_t)element_ic)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_GET_ELEM2: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_get_elem2,
                       c->r_vm, c->r_js, obj, key);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_ELEM: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 2] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, integer_index), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 2])));
      }
      bool tagged_old_possible =
          sv_tfb_put_needs_tagged_old_guard(feedback);
      bool val_is_int = c->vs.slot_type[c->vs.sp - 1] == SLOT_I32;
      if (val_is_int)
        vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool val_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool key_is_num = !integer_index && vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool obj_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);

      if (specialize && !obj_is_num) {
        MIR_reg_t val = vstack_pop(&c->vs);
        MIR_reg_t key = vstack_pop(&c->vs);
        MIR_reg_t obj = vstack_pop(&c->vs);
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        int index_site = mir_next_reg_site(&c->reg_site_n);
        int element_site = mir_next_reg_site(&c->reg_site_n);

        MIR_reg_t index = jit_emit_element_index_guard(
            c, key, integer_index, c->vs.d_regs[c->vs.sp + 1], key_is_num,
            bail_direct, index_site);
        if (val_is_num)
          mir_d_to_i64(
              c->ctx, c->jit_func, val, c->vs.d_regs[c->vs.sp + 2], c->r_d_slot);
        else if (!val_is_int)
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, val, bail_direct);

        MIR_reg_t old_value = c->r_err_tmp;
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, old_value,
            JIT_ELEMENT_WRITE, bail_direct, element_site, NULL);
        MIR_label_t tagged_old_value = NULL;
        MIR_label_t store = NULL;
        if (tagged_old_possible) {
          tagged_old_value = MIR_new_label(c->ctx);
          store = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, tagged_old_value),
                                       MIR_new_reg_op(c->ctx, old_value),
                                       MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
          MIR_append_insn(c->ctx, c->jit_func, store);
        } else {
          mir_emit_is_num_guard(
              c->ctx, c->jit_func, c->r_bool, old_value, bail_direct);
        }
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, data, index,
                                                    sizeof(ant_value_t)),
                                     MIR_new_reg_op(c->ctx, val)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        if (tagged_old_possible) {
          MIR_append_insn(c->ctx, c->jit_func, tagged_old_value);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGE,
                                       MIR_new_label_op(c->ctx, bail_direct),
                                       MIR_new_reg_op(c->ctx, old_value),
                                       MIR_new_uint_op(c->ctx, ANT_SENTINEL_TAG)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, store)));
        }
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 3, &c->bailout_ctx,
            -1, SLOT_BOXED, c->vs.sp + 1,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_label_t element_done = NULL;
      if (!obj_is_num) {
        MIR_label_t slow = MIR_new_label(c->ctx);
        element_done = MIR_new_label(c->ctx);
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        int site = mir_next_reg_site(&c->reg_site_n);
        MIR_label_t add = MIR_new_label(c->ctx);
        jit_dense_element_t element = {.past_end = MIR_new_label(c->ctx)};
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_WRITE, slow,
            site, &element);
        MIR_reg_t flags = element.flags;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBGE, MIR_new_label_op(c->ctx, add),
                                     MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                     MIR_new_uint_op(c->ctx, ANT_SENTINEL_TAG)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, data, index, sizeof(ant_value_t)),
                                     MIR_new_reg_op(c->ctx, val)));
        jit_emit_element_barrier(c, obj, index, val, flags, element_done);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, element_done)));

        // an append within capacity, or a store into a hole, adds an
        // element; it stores on its own so the store above stays a plain one
        MIR_append_insn(c->ctx, c->jit_func, element.past_end);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, index), MIR_new_reg_op(c->ctx, element.len)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_mem_op(c->ctx, MIR_T_U32, (MIR_disp_t)offsetof(ant_object_t, u.array.cap),
                                                    element.ptr, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBGE, MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, index), MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func, add);
        mir_emit_array_add_guard(c->ctx, c->jit_func, c->r_js, element.ptr, flags, slow, site);
        MIR_label_t length_kept = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBGT, MIR_new_label_op(c->ctx, length_kept),
                                     MIR_new_reg_op(c->ctx, element.len), MIR_new_reg_op(c->ctx, index)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, index), MIR_new_int_op(c->ctx, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_U32, (MIR_disp_t)offsetof(ant_object_t, u.array.len),
                                                    element.ptr, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func, length_kept);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR, MIR_new_reg_op(c->ctx, flags), MIR_new_reg_op(c->ctx, flags),
                                     MIR_new_uint_op(c->ctx, ANT_OBJECT_FLAG_MAY_HAVE_DENSE_ELEMENTS)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_U16, (MIR_disp_t)offsetof(ant_object_t, flags),
                                                    element.ptr, 0, 1),
                                     MIR_new_reg_op(c->ctx, flags)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, data, index, sizeof(ant_value_t)),
                                     MIR_new_reg_op(c->ctx, val)));
        jit_emit_element_barrier(c, obj, index, val, flags, element_done);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, element_done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->put_elem_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_elem),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, key),
                                        MIR_new_reg_op(c->ctx, val)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      if (element_done) MIR_append_insn(c->ctx, c->jit_func, element_done);
      break;
    }

    case OP_GET_PRIVATE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t token = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_get_private,
                       c->r_vm, c->r_js, obj, token);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_PUT_PRIVATE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t token = vstack_pop(&c->vs);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->private_put_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_put_private),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, obj),
                                        MIR_new_reg_op(c->ctx, val),
                                        MIR_new_reg_op(c->ctx, token)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_SET_PROTO: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t proto = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, c->r_err_tmp,
                       c->helper2_proto, c->imp_set_proto,
                       c->r_vm, c->r_js, obj, proto);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
