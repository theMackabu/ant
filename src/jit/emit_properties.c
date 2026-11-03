#include "compile.h"
#include "silver/feedback.h"
#include "protectors.h"

static void jit_emit_index_read_fast(
    jit_compile_t *c, MIR_reg_t obj, MIR_reg_t key, MIR_reg_t dst, sv_asm_label_t done) {
  const sv_asm_t *a = &c->as;
  char name[40];
  snprintf(name, sizeof(name), "ix_t_%d", c->bc_off);
  MIR_reg_t t = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  sv_asm_label_t call = sv_asm_new_label(a);
  sv_asm_label_t generic = sv_asm_new_label(a);

  sv_asm_u64_bgt_imm(a, generic, key, NANBOX_PREFIX);
  sv_asm_i64_ursh_imm(a, t, obj, NANBOX_TYPE_SHIFT);
  sv_asm_i64_beq_imm(a, call, t, NANBOX_TOBJ_TAG);
  sv_asm_i64_bne_imm(a, generic, t, NANBOX_TSTR_TAG);
  sv_asm_bind(a, call);
  sv_asm_append(a, MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->get_elem_inline_proto), MIR_new_ref_op(c->ctx, c->imp_get_index_fast), MIR_new_reg_op(c->ctx, t), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, key)));
  sv_asm_i64_beq_imm(a, generic, t, T_EMPTY);
  sv_asm_mov(a, dst, t);
  sv_asm_jump(a, done);
  sv_asm_bind(a, generic);
}
static MIR_reg_t jit_emit_element_index_guard(
    jit_compile_t *c, MIR_reg_t key, MIR_reg_t integer_index,
    MIR_reg_t double_key, bool key_is_num, sv_asm_label_t slow, int site) {
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

static void jit_emit_gfp_exotic_guard(jit_compile_t *c, MIR_reg_t ptr, sv_asm_label_t full) {
  const sv_asm_t *a = &c->as;
  sv_asm_load(a, MIR_T_U16, c->r_gfp_t, ptr, offsetof(ant_object_t, flags));
  sv_asm_i64_and_imm(a, c->r_gfp_t, c->r_gfp_t, ANT_OBJECT_FLAG_EXOTIC);
  sv_asm_i64_bne_imm(a, full, c->r_gfp_t, 0);
}

static void jit_emit_in_field_fast(
  jit_compile_t *c, uint16_t ic_idx, MIR_reg_t obj, MIR_reg_t dst, sv_asm_label_t slow, sv_asm_label_t done
) {
  const sv_asm_t *a = &c->as;
  sv_func_t *func = c->func;
  if (!func->ic_slots || ic_idx >= func->ic_count || !c->r_ic_epoch_val) return;
  
  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  uint8_t kind = ic->get_kind;
  if (
    !(ic->cached_aux & SV_GF_IC_AUX_ACTIVE_BIT) ||
    (kind != SV_GF_IC_OWN && kind != SV_GF_IC_PROTOTYPE && kind != SV_GF_IC_MISSING)
  ) return;
  
  jit_gfp_regs(c);
  MIR_reg_t r_ic = c->r_gfp_e, r_ptr = c->r_gfp_ptr, r_tag = c->r_gfp_tag;
  MIR_reg_t r_t = c->r_gfp_t, r_t2 = c->r_gfp_t2, r_proto = c->r_gfp_src;
  sv_asm_label_t result = sv_asm_new_label(a);
  
  sv_asm_mov_imm(a, r_ic, (uint64_t)(uintptr_t)ic);
  sv_asm_load(a, MIR_T_I64, r_t, r_ic, offsetof(sv_ic_entry_t, cached_aux));
  sv_asm_i64_and_imm(a, r_t, r_t, SV_GF_IC_AUX_ACTIVE_BIT);
  sv_asm_i64_beq_imm(a, slow, r_t, 0);
  sv_asm_load(a, MIR_T_U32, r_t, r_ic, offsetof(sv_ic_entry_t, epoch));
  sv_asm_load(a, MIR_T_U32, r_t2, c->r_ic_epoch_val, 0);
  sv_asm_i64_bne(a, slow, r_t, r_t2);
  sv_asm_load(a, MIR_T_U8, r_t, r_ic, offsetof(sv_ic_entry_t, get_kind));
  sv_asm_i64_bne_imm(a, slow, r_t, kind);
  
  mir_emit_value_to_objptr_or_jmp(c->ctx, c->jit_func, obj, r_ptr, r_tag, slow);
  jit_emit_gfp_exotic_guard(c, r_ptr, slow);
  sv_asm_load(a, MIR_T_P, r_t, r_ptr, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_T_P, r_t2, r_ic, offsetof(sv_ic_entry_t, cached_shape));
  sv_asm_i64_bne(a, slow, r_t, r_t2);
  if (kind == SV_GF_IC_OWN) {
    mir_load_imm(c->ctx, c->jit_func, dst, Ant_Value_True);
    sv_asm_jump(a, done);
    return;
  }
  
  if (kind == SV_GF_IC_MISSING) {
    sv_asm_i64_bne_imm(a, slow, r_tag, NANBOX_TOBJ_TAG);
    sv_asm_load(a, MIR_T_U8, r_t, r_ptr, offsetof(ant_object_t, type_tag));
    sv_asm_i64_bne_imm(a, slow, r_t, kTypeObject);
  }
  
  sv_asm_load(a, MIR_T_I64, r_proto, r_ptr, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_T_I64, r_proto, r_proto, ANT_SHAPE_PROTO_OFFSET);
  sv_asm_load(a, MIR_T_I64, r_t, r_ic, offsetof(sv_ic_entry_t, guard.receiver_proto));
  sv_asm_i64_bne(a, slow, r_proto, r_t);
  if (kind == SV_GF_IC_MISSING)
    sv_asm_i64_beq_imm(a, result, r_proto, mkval(kTypeNull, 0));
  sv_asm_i64_ursh_imm(a, r_t, r_proto, NANBOX_TYPE_SHIFT);
  sv_asm_i64_bne_imm(a, slow, r_t, NANBOX_TOBJ_TAG);
  mir_emit_decode_ref(c->ctx, c->jit_func, r_proto, r_proto);
  sv_asm_load(a, MIR_T_U32, r_t, r_proto, offsetof(ant_object_t, ic_identity));
  sv_asm_load(a, MIR_T_U64, r_t2, r_ic, offsetof(sv_ic_entry_t, cached_aux));
  sv_asm_i64_ursh_imm(a, r_t2, r_t2, SV_GF_IC_PROTO_ID_SHIFT);
  sv_asm_i64_bne(a, slow, r_t, r_t2);
  
  if (kind == SV_GF_IC_PROTOTYPE) {
    sv_asm_load(a, MIR_T_P, r_t2, r_ic, offsetof(sv_ic_entry_t, cached_holder));
    sv_asm_i64_beq_imm(a, slow, r_t2, 0);
    jit_emit_gfp_exotic_guard(c, r_t2, slow);
    sv_asm_load(a, MIR_T_P, r_t, r_t2, offsetof(ant_object_t, shape));
    sv_asm_i64_beq_imm(a, slow, r_t, 0);
  }
  
  sv_asm_bind(a, result);
  mir_load_imm(c->ctx, c->jit_func, dst, kind == SV_GF_IC_MISSING ? Ant_Value_False : Ant_Value_True);
  sv_asm_jump(a, done);
}

static void jit_emit_get_field_poly_loop(
    jit_compile_t *c, sv_ic_entry_t *ic, MIR_reg_t obj, MIR_reg_t dst,
    sv_asm_label_t hit, sv_asm_label_t full) {
  const sv_asm_t *a = &c->as;
  sv_asm_label_t loop = sv_asm_new_label(a), next = sv_asm_new_label(a);
  sv_asm_label_t own = sv_asm_new_label(a), read = sv_asm_new_label(a);
  sv_asm_label_t load_overflow = sv_asm_new_label(a);

  sv_asm_mov_imm(a, c->r_gfp_e, (uint64_t)(uintptr_t)&ic->guard.get.poly);
  sv_asm_load(a, MIR_T_P, c->r_gfp_e, c->r_gfp_e, 0);
  sv_asm_i64_beq_imm(a, full, c->r_gfp_e, 0);
  mir_emit_value_to_objptr_or_jmp(c->ctx, c->jit_func, obj, c->r_gfp_ptr, c->r_gfp_tag, full);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_ptr, full);
  sv_asm_load(a, MIR_T_P, c->r_gfp_shape, c->r_gfp_ptr, offsetof(ant_object_t, shape));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_shape, 0);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_epoch, c->r_ic_epoch_val, 0);
  sv_asm_i64_add_imm(a, c->r_gfp_end, c->r_gfp_e, SV_GF_POLY_WAYS * sizeof(sv_gf_poly_entry_t));

  sv_asm_bind(a, loop);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, shape));
  sv_asm_i64_bne(a, next, c->r_gfp_t, c->r_gfp_shape);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, epoch));
  sv_asm_i64_bne(a, next, c->r_gfp_t, c->r_gfp_epoch);
  sv_asm_load(a, MIR_T_U8, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, kind));
  sv_asm_i64_beq_imm(a, own, c->r_gfp_t, SV_GF_IC_OWN);
  sv_asm_i64_bne_imm(a, next, c->r_gfp_t, SV_GF_IC_PROTOTYPE);

  sv_asm_load(a, MIR_JSVAL, c->r_gfp_t, c->r_gfp_ptr, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_JSVAL, c->r_gfp_t, c->r_gfp_t, ANT_SHAPE_PROTO_OFFSET);
  sv_asm_load(a, MIR_JSVAL, c->r_gfp_t2, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, receiver_proto));
  sv_asm_i64_bne(a, next, c->r_gfp_t, c->r_gfp_t2);
  sv_asm_i64_ursh_imm(a, c->r_gfp_t2, c->r_gfp_t, NANBOX_TYPE_SHIFT);
  sv_asm_i64_bne_imm(a, full, c->r_gfp_t2, NANBOX_TOBJ_TAG);
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_gfp_t, c->r_gfp_t);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_t, offsetof(ant_object_t, ic_identity));
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t2, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, proto_id));
  sv_asm_i64_bne(a, next, c->r_gfp_t, c->r_gfp_t2);
  sv_asm_load(a, MIR_T_P, c->r_gfp_src, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, holder));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_src, 0);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, shape));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_t, 0);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_src, full);
  sv_asm_jump(a, read);

  sv_asm_bind(a, own);
  sv_asm_mov(a, c->r_gfp_src, c->r_gfp_ptr);
  sv_asm_jump(a, read);

  sv_asm_bind(a, next);
  sv_asm_i64_add_imm(a, c->r_gfp_e, c->r_gfp_e, sizeof(sv_gf_poly_entry_t));
  sv_asm_u64_blt(a, loop, c->r_gfp_e, c->r_gfp_end);
  sv_asm_jump(a, full);

  sv_asm_bind(a, read);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_idx, c->r_gfp_e, offsetof(sv_gf_poly_entry_t, index));
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, prop_count));
  sv_asm_u64_bge(a, full, c->r_gfp_idx, c->r_gfp_t);
  sv_asm_load(a, MIR_T_P, c->r_gfp_lim, c->r_gfp_src, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_T_U8, c->r_gfp_lim, c->r_gfp_lim, ANT_SHAPE_INOBJ_LIMIT_OFFSET);
  sv_asm_u64_bge(a, load_overflow, c->r_gfp_idx, c->r_gfp_lim);
  sv_asm_load_index(a, MIR_T_I64, dst, c->r_gfp_src, offsetof(ant_object_t, inobj), c->r_gfp_idx, 8);
  sv_asm_jump(a, hit);

  sv_asm_bind(a, load_overflow);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, overflow_prop));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_t, 0);
  sv_asm_i64_sub(a, c->r_gfp_idx, c->r_gfp_idx, c->r_gfp_lim);
  sv_asm_load_index(a, MIR_T_I64, dst, c->r_gfp_t, 0, c->r_gfp_idx, 8);
  sv_asm_jump(a, hit);
}

static void jit_emit_get_field_mega_probe(
    jit_compile_t *c, sv_gf_mega_cache_t *cache, const char *key,
    MIR_reg_t obj, MIR_reg_t dst, sv_asm_label_t hit, sv_asm_label_t full) {
  const sv_asm_t *a = &c->as;
  sv_asm_label_t own = sv_asm_new_label(a), read = sv_asm_new_label(a);
  sv_asm_label_t load_overflow = sv_asm_new_label(a);

  mir_emit_value_to_objptr_or_jmp(c->ctx, c->jit_func, obj, c->r_gfp_ptr, c->r_gfp_tag, full);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_ptr, full);
  sv_asm_load(a, MIR_T_P, c->r_gfp_shape, c->r_gfp_ptr, offsetof(ant_object_t, shape));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_shape, 0);
  sv_asm_load(a, MIR_JSVAL, c->r_gfp_t2, c->r_gfp_shape, ANT_SHAPE_PROTO_OFFSET);

  sv_asm_i64_ursh_imm(a, c->r_gfp_e, c->r_gfp_shape, SV_GF_MEGA_SHAPE_SHIFT);
  sv_asm_i64_xor_imm(a, c->r_gfp_e, c->r_gfp_e, (uint64_t)(uintptr_t)key >> SV_GF_MEGA_KEY_SHIFT);
  sv_asm_i64_ursh_imm(a, c->r_gfp_t, c->r_gfp_t2, SV_GF_MEGA_PROTO_SHIFT);
  sv_asm_i64_xor(a, c->r_gfp_e, c->r_gfp_e, c->r_gfp_t);
  sv_asm_i64_ursh_imm(a, c->r_gfp_t, c->r_gfp_e, SV_GF_MEGA_MIX_SHIFT);
  sv_asm_i64_xor(a, c->r_gfp_e, c->r_gfp_e, c->r_gfp_t);
  sv_asm_i64_and_imm(a, c->r_gfp_e, c->r_gfp_e, SV_GF_MEGA_PRIMARY - 1);
  sv_asm_i64_mul_imm(a, c->r_gfp_e, c->r_gfp_e, sizeof(sv_gf_mega_entry_t));
  sv_asm_i64_add_imm(a, c->r_gfp_e, c->r_gfp_e, (uint64_t)(uintptr_t)cache->primary);

  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, shape));
  sv_asm_i64_bne(a, full, c->r_gfp_t, c->r_gfp_shape);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, key));
  sv_asm_i64_bne_imm(a, full, c->r_gfp_t, (uint64_t)(uintptr_t)key);
  sv_asm_load(a, MIR_JSVAL, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, receiver_proto));
  sv_asm_i64_bne(a, full, c->r_gfp_t, c->r_gfp_t2);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_epoch, c->r_ic_epoch_val, 0);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, epoch));
  sv_asm_i64_bne(a, full, c->r_gfp_t, c->r_gfp_epoch);
  sv_asm_load(a, MIR_T_U8, c->r_gfp_t, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, kind));
  sv_asm_i64_beq_imm(a, own, c->r_gfp_t, SV_GF_IC_OWN);
  sv_asm_i64_bne_imm(a, full, c->r_gfp_t, SV_GF_IC_PROTOTYPE);

  sv_asm_i64_ursh_imm(a, c->r_gfp_t, c->r_gfp_t2, NANBOX_TYPE_SHIFT);
  sv_asm_i64_bne_imm(a, full, c->r_gfp_t, NANBOX_TOBJ_TAG);
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_gfp_t, c->r_gfp_t2);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_t, offsetof(ant_object_t, ic_identity));
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t2, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, proto_id));
  sv_asm_i64_bne(a, full, c->r_gfp_t, c->r_gfp_t2);
  sv_asm_load(a, MIR_T_P, c->r_gfp_src, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, holder));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_src, 0);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, shape));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_t, 0);
  jit_emit_gfp_exotic_guard(c, c->r_gfp_src, full);
  sv_asm_jump(a, read);

  sv_asm_bind(a, own);
  sv_asm_mov(a, c->r_gfp_src, c->r_gfp_ptr);

  sv_asm_bind(a, read);
  sv_asm_load(a, MIR_T_U32, c->r_gfp_idx, c->r_gfp_e, offsetof(sv_gf_mega_entry_t, index));
  sv_asm_load(a, MIR_T_U32, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, prop_count));
  sv_asm_u64_bge(a, full, c->r_gfp_idx, c->r_gfp_t);
  sv_asm_load(a, MIR_T_P, c->r_gfp_lim, c->r_gfp_src, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_T_U8, c->r_gfp_lim, c->r_gfp_lim, ANT_SHAPE_INOBJ_LIMIT_OFFSET);
  sv_asm_u64_bge(a, load_overflow, c->r_gfp_idx, c->r_gfp_lim);
  sv_asm_load_index(a, MIR_T_I64, dst, c->r_gfp_src, offsetof(ant_object_t, inobj), c->r_gfp_idx, 8);
  sv_asm_jump(a, hit);

  sv_asm_bind(a, load_overflow);
  sv_asm_load(a, MIR_T_P, c->r_gfp_t, c->r_gfp_src, offsetof(ant_object_t, overflow_prop));
  sv_asm_i64_beq_imm(a, full, c->r_gfp_t, 0);
  sv_asm_i64_sub(a, c->r_gfp_idx, c->r_gfp_idx, c->r_gfp_lim);
  sv_asm_load_index(a, MIR_T_I64, dst, c->r_gfp_t, 0, c->r_gfp_idx, 8);
  sv_asm_jump(a, hit);
}

static void jit_emit_get_field_poly(
    jit_compile_t *c, uint16_t ic_idx, sv_atom_t *atom,
    MIR_reg_t obj, MIR_reg_t dst, sv_asm_label_t hit) {
  const sv_asm_t *a = &c->as;
  if (!c->func->ic_slots || ic_idx >= c->func->ic_count) return;
  sv_ic_entry_t *ic = &c->func->ic_slots[ic_idx];
  const sv_gf_poly_t *poly = ic->guard.get.poly;
  bool mega = (ic->shape_ref_mask & SV_IC_POLY_MEGA) != 0;
  if ((!poly && !mega) || is_length_key(atom->str, atom->len)) return;
  if (sv_gf_kind_is_symbol_description(ic->get_kind)) return;
  if (mega) ic->shape_ref_mask |= SV_IC_MEGA_COMPILED;

  if (!mega && c->func->code_len > JIT_GFP_MAX_CODE_LEN) return;
  bool probe_shared = mega;
  sv_gf_mega_cache_t *cache = probe_shared ? sv_gf_mega_ensure(c->js) : NULL;
  if (probe_shared && !cache) return;
  if (!probe_shared && poly->kept_epoch == c->js->ic.epoch && poly->kept >= SV_GF_POLY_WAYS) return;

  jit_gfp_regs(c);
  sv_asm_label_t full = sv_asm_new_label(a);
  if (probe_shared) jit_emit_get_field_mega_probe(c, cache, atom->str, obj, dst, hit, full);
  else jit_emit_get_field_poly_loop(c, ic, obj, dst, hit, full);
  sv_asm_bind(a, full);
}

static constexpr int32_t JIT_DEFINE_SLOT_INLINE_MAX_CODE_LEN = 1024;

static void jit_emit_define_slot_inline(
    jit_compile_t *c, sv_atom_t *atom, uint16_t slot,
    MIR_reg_t obj, MIR_reg_t val, sv_asm_label_t done) {
  const sv_asm_t *a = &c->as;
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
  sv_asm_label_t helper = sv_asm_new_label(a), overflow = sv_asm_new_label(a);

  sv_asm_i64_ursh_imm(a, t, obj, NANBOX_TYPE_SHIFT);
  sv_asm_i64_bne_imm(a, helper, t, NANBOX_TOBJ_TAG);
  mir_emit_decode_ref(c->ctx, c->jit_func, ptr, obj);
  sv_asm_load(a, MIR_T_U16, t, ptr, offsetof(ant_object_t, flags));
  sv_asm_i64_and_imm(a, t, t, ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_GENERATION);
  sv_asm_i64_bne_imm(a, helper, t, 0);
  sv_asm_load(a, MIR_T_U32, t, ptr, offsetof(ant_object_t, prop_count));
  sv_asm_u64_ble_imm(a, helper, t, slot);
  sv_asm_load(a, MIR_T_P, lim, ptr, offsetof(ant_object_t, shape));
  sv_asm_load(a, MIR_T_U8, lim, lim, ANT_SHAPE_INOBJ_LIMIT_OFFSET);
  sv_asm_u64_ble_imm(a, overflow, lim, slot);
  sv_asm_store(a, MIR_JSVAL, ptr, (MIR_disp_t)(offsetof(ant_object_t, inobj) + slot * sizeof(ant_value_t)), val);
  sv_asm_jump(a, done);

  sv_asm_bind(a, overflow);
  sv_asm_load(a, MIR_T_P, t, ptr, offsetof(ant_object_t, overflow_prop));
  sv_asm_i64_beq_imm(a, helper, t, 0);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, lim),
      MIR_new_uint_op(c->ctx, slot), MIR_new_reg_op(c->ctx, lim)));
  sv_asm_store_index(a, MIR_JSVAL, t, 0, lim, 8, val);
  sv_asm_jump(a, done);
  sv_asm_bind(a, helper);
}

static void jit_emit_element_barrier_filter(
    jit_compile_t *c, MIR_reg_t val, MIR_reg_t flags, sv_asm_label_t skip) {
  const sv_asm_t *a = &c->as;
  sv_asm_u64_ble_imm(a, skip, val, NANBOX_PREFIX);
  sv_asm_i64_and_imm(a, c->r_bool, flags, ANT_OBJECT_FLAG_GENERATION);
  sv_asm_i64_beq_imm(a, skip, c->r_bool, 0);
}

void jit_emit_dense_add(
    jit_compile_t *c, MIR_reg_t ptr, MIR_reg_t data, MIR_reg_t flags,
    MIR_reg_t index, MIR_reg_t len, MIR_reg_t val, sv_asm_label_t slow, int site) {
  const sv_asm_t *a = &c->as;
  mir_emit_array_add_guard(c->ctx, c->jit_func, c->r_js, ptr, flags, slow, site);
  sv_asm_label_t length_kept = len ? sv_asm_new_label(a) : NULL;
  if (len)
    sv_asm_u64_bgt(a, length_kept, len, index);
  sv_asm_i64_add_imm(a, c->r_bool, index, 1);
  sv_asm_store(a, MIR_T_U32, ptr, offsetof(ant_object_t, u.array.len), c->r_bool);
  if (length_kept) sv_asm_bind(a, length_kept);
  sv_asm_i64_or_imm(a, flags, flags, ANT_OBJECT_FLAG_MAY_HAVE_DENSE_ELEMENTS);
  sv_asm_store(a, MIR_T_U16, ptr, offsetof(ant_object_t, flags), flags);
  sv_asm_store_index(a, MIR_JSVAL, data, 0, index, sizeof(ant_value_t), val);
}

void jit_emit_element_barrier(
    jit_compile_t *c, MIR_reg_t obj, MIR_reg_t index,
    MIR_reg_t val, MIR_reg_t flags, sv_asm_label_t skip, sv_asm_label_t shared) {
  const sv_asm_t *a = &c->as;
  jit_emit_element_barrier_filter(c, val, flags, skip);
  if (shared) sv_asm_bind(a, shared);
  sv_asm_label_t string = sv_asm_new_label(a);
  sv_asm_label_t barrier = sv_asm_new_label(a);
  sv_asm_i64_ursh_imm(a, c->r_bool, val, NANBOX_TYPE_SHIFT);
  sv_asm_i64_beq_imm(a, string, c->r_bool, JIT_STR_TAG);
  sv_asm_i64_sub_imm(a, c->r_bool, c->r_bool, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeUndefined);
  static_assert(kTypeNull == kTypeUndefined + 1 && kTypeBool == kTypeUndefined + 2);
  sv_asm_u64_ble_imm(a, skip, c->r_bool, 2);
  sv_asm_jump(a, barrier);
  // only ropes are ever young
  sv_asm_bind(a, string);
  mir_emit_decode_ref(c->ctx, c->jit_func, c->r_bool, val);
  sv_asm_i64_and_imm(a, c->r_bool, c->r_bool, STR_HEAP_TAG_MASK);
  sv_asm_i64_bne_imm(a, skip, c->r_bool, STR_HEAP_TAG_ROPE);
  sv_asm_bind(a, barrier);
  sv_asm_append(a, MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->elem_barrier_proto), MIR_new_ref_op(c->ctx, c->imp_elem_barrier), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, index), MIR_new_reg_op(c->ctx, val)));
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

static void jit_emit_get_field_fast(
    jit_compile_t *c, uint16_t ic_idx, sv_atom_t *atom, MIR_reg_t obj, MIR_reg_t dst,
    sv_asm_label_t slow, sv_asm_label_t done, int pre_op_sp) {
  const sv_asm_t *a = &c->as;
  jit_stale_exit_t stale;
  jit_stale_exit_t *stale_exit = jit_stale_exit_for(c, &stale);
  bool fast = mir_emit_get_field_ic_fastpath(
      c->ctx, c->jit_func, c->js, c->func, c->bc_off, ic_idx, atom, obj, dst, slow,
      c->r_ic_epoch_val, stale_exit, c->op == OP_GET_LENGTH);
  jit_note_stale_exit(c, stale_exit, pre_op_sp);
  if (!fast) return;
  sv_asm_jump(a, done);
  sv_asm_bind(a, slow);
}

void jit_emit_properties(jit_compile_t *c) {
  const sv_asm_t *a = &c->as;
  switch (c->op) {
    case OP_TO_PROPKEY: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      sv_asm_label_t is_key = sv_asm_new_label(a);
      sv_asm_label_t pk_done = sv_asm_new_label(a);
      sv_asm_label_t pk_helper = sv_asm_new_label(a);
      sv_asm_u64_ble_imm(a, pk_helper, src, NANBOX_PREFIX);
      sv_asm_i64_ursh_imm(a, c->r_bool, src, NANBOX_TYPE_SHIFT);
      sv_asm_i64_and_imm(a, c->r_bool, c->r_bool, NANBOX_TYPE_MASK);
      sv_asm_i64_beq_imm(a, is_key, c->r_bool, kTypeString);
      sv_asm_i64_beq_imm(a, is_key, c->r_bool, kTypeSymbol);
      sv_asm_bind(a, pk_helper);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->helper1_proto), MIR_new_ref_op(c->ctx, c->imp_to_propkey), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, src)));
      sv_asm_jump(a, pk_done);
      sv_asm_bind(a, is_key);
      sv_asm_mov(a, dst, src);
      sv_asm_bind(a, pk_done);
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
      int pre_op_sp = c->vs.sp;
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      sv_asm_label_t no_err = sv_asm_new_label(a);
      sv_asm_label_t slow = sv_asm_new_label(a);
      jit_emit_get_field_fast(c, ic_idx, atom, obj, dst, slow, no_err, pre_op_sp);
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->gf_proto), MIR_new_ref_op(c->ctx, c->imp_get_field), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      sv_asm_i64_ursh_imm(a, c->r_bool, dst, NANBOX_TYPE_SHIFT);
      sv_asm_i64_bne_imm(a, no_err, c->r_bool, JIT_ERR_TAG);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        sv_asm_mov(a, c->vs.regs[h->saved_sp], dst);
        sv_asm_jump(a, h->catch_label);
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      sv_asm_bind(a, no_err);
      jit_note_known_builtin(c, atom);
      break;
    }

    case OP_IN_FIELD: {
      uint32_t idx = sv_get_u32(c->ip + 1);
      if (idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[idx];
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      sv_asm_label_t in_slow = sv_asm_new_label(a);
      sv_asm_label_t in_done = sv_asm_new_label(a);
      jit_emit_in_field_fast(c, sv_get_u16(c->ip + 5), obj, dst, in_slow, in_done);
      sv_asm_bind(a, in_slow);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->gf_proto), MIR_new_ref_op(c->ctx, c->imp_in_field), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      jit_emit_throw_if_error(c, dst);
      sv_asm_bind(a, in_done);
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
      int pre_op_sp = c->vs.sp;
      MIR_reg_t obj = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      sv_asm_label_t no_err = sv_asm_new_label(a);
      sv_asm_label_t slow = sv_asm_new_label(a);
      jit_emit_get_field_fast(c, ic_idx, atom, obj, dst, slow, no_err, pre_op_sp);
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->gf_proto), MIR_new_ref_op(c->ctx, c->imp_get_field), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      sv_asm_i64_ursh_imm(a, c->r_bool, dst, NANBOX_TYPE_SHIFT);
      sv_asm_i64_bne_imm(a, no_err, c->r_bool, JIT_ERR_TAG);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        sv_asm_mov(a, c->vs.regs[h->saved_sp], dst);
        sv_asm_jump(a, h->catch_label);
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      sv_asm_bind(a, no_err);
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
      int pre_op_sp = c->vs.sp;
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 5);
      sv_asm_label_t nullish = sv_asm_new_label(a);
      sv_asm_label_t no_err = sv_asm_new_label(a);
      sv_asm_label_t slow = sv_asm_new_label(a);
      sv_asm_i64_beq_imm(a, nullish, obj, mkval(kTypeNull, 0));
      sv_asm_i64_beq_imm(a, nullish, obj, mkval(kTypeUndefined, 0));
      jit_emit_get_field_fast(c, ic_idx, atom, obj, dst, slow, no_err, pre_op_sp);
      jit_emit_get_field_poly(c, ic_idx, atom, obj, dst, no_err);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->gf_proto), MIR_new_ref_op(c->ctx, c->imp_get_field), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      sv_asm_i64_ursh_imm(a, c->r_bool, dst, NANBOX_TYPE_SHIFT);
      sv_asm_i64_bne_imm(a, no_err, c->r_bool, JIT_ERR_TAG);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        sv_asm_mov(a, c->vs.regs[h->saved_sp], dst);
        sv_asm_jump(a, h->catch_label);
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      sv_asm_bind(a, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      sv_asm_bind(a, no_err);
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
      int pre_op_sp = c->vs.sp;
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      sv_asm_label_t object_path = sv_asm_new_label(a);
      sv_asm_label_t done = sv_asm_new_label(a);
      mir_emit_get_length(
          c->ctx, c->jit_func, c->js, obj, dst,
          c->r_js, c->r_d_slot,
          c->get_length_proto, c->imp_get_length,
          builder_length,
          -1, c->bc_off,
          object_path, done);
      sv_asm_bind(a, object_path);
      sv_atom_t length = sv_length_atom(c->js);
      jit_emit_get_field_fast(c, sv_get_u16(c->ip + 1), &length, obj, dst, sv_asm_new_label(a), done, pre_op_sp);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->get_length_at_proto), MIR_new_ref_op(c->ctx, c->imp_get_length_at), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
      jit_emit_throw_if_error(c, dst);
      sv_asm_bind(a, done);
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
      sv_asm_label_t slow = sv_asm_new_label(a);
      sv_asm_label_t no_err = sv_asm_new_label(a);
      bool fast = mir_emit_put_field_ic_fastpath(
          c->ctx, c->jit_func, c->js, c->func, c->bc_off, pf_ic_idx, atom,
          c->r_js, obj, val, slow, c->r_ic_epoch_val,
          c->shape_transition_proto, c->imp_shape_transition,
          c->imp_protector_store, c->remember_obj_proto, c->imp_remember_obj, false);
      if (fast) {
        sv_asm_jump(a, no_err);
        sv_asm_bind(a, slow);
      }
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 10, MIR_new_ref_op(c->ctx, c->put_field_proto), MIR_new_ref_op(c->ctx, c->imp_put_field), MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, val), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ic_slot), MIR_new_int_op(c->ctx, c->func->is_strict)));
      sv_asm_i64_ursh_imm(a, c->r_bool, c->r_err_tmp, NANBOX_TYPE_SHIFT);
      sv_asm_i64_bne_imm(a, no_err, c->r_bool, JIT_ERR_TAG);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        sv_asm_mov(a, c->vs.regs[h->saved_sp], c->r_err_tmp);
        sv_asm_jump(a, h->catch_label);
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      }
      sv_asm_bind(a, no_err);
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
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->define_field_proto), MIR_new_ref_op(c->ctx, c->imp_define_field), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, val), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str), MIR_new_uint_op(c->ctx, (uint64_t)atom->len)));
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
      sv_asm_label_t define_done = sv_asm_new_label(a);
      jit_emit_define_slot_inline(c, atom, slot, obj, val, define_done);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->define_slot_proto), MIR_new_ref_op(c->ctx, c->imp_define_slot), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, val), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str), MIR_new_uint_op(c->ctx, (uint64_t)atom->len), MIR_new_int_op(c->ctx, (int64_t)slot)));
      sv_asm_bind(a, define_done);
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
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->define_method_comp_proto), MIR_new_ref_op(c->ctx, c->imp_define_method_comp), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, key), MIR_new_reg_op(c->ctx, fn_val), MIR_new_int_op(c->ctx, (int64_t)flags)));
      break;
    }

    case OP_GET_ELEM: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool specialize = sv_tfb_specialization_ready(feedback);
      bool typed_seen = sv_tfb_elem_saw_typed_array(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 1] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        sv_asm_mov(a, integer_index, c->vs.regs[c->vs.sp - 1]);
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
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
        int index_site = mir_next_reg_site(&c->reg_site_n);
        int element_site = mir_next_reg_site(&c->reg_site_n);
        MIR_reg_t index = jit_emit_element_index_guard(
            c, key, integer_index, c->vs.d_regs[c->vs.sp], key_is_num,
            bail_direct, index_site);
        MIR_reg_t loaded = c->r_err_tmp;
        sv_asm_label_t typed = typed_seen ? sv_asm_new_label(a) : bail_direct;
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, loaded, JIT_ELEMENT_NUMERIC_READ, typed, element_site, NULL);
        mir_i64_to_d(
            c->ctx, c->jit_func, c->vs.d_regs[c->vs.sp - 1], loaded, c->r_d_slot);
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        sv_asm_jump(a, done);
        if (typed_seen) {
          sv_asm_bind(a, typed);
          mir_emit_typed_array_read(
              c->ctx, c->jit_func, obj, index, c->vs.d_regs[c->vs.sp - 1], bail_direct, done, element_site,
              sv_tfb_elem_type(c->func, c->bc_off));
        }
        sv_asm_bind(a, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
            c->vs.sp - 1, SLOT_BOXED, c->vs.sp,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        sv_asm_bind(a, done);
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
            sv_asm_mov(a, c->integer_value, index);
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
      sv_asm_label_t element_done = NULL;
      if (!obj_is_num) {
        sv_asm_label_t slow = sv_asm_new_label(a);
        element_done = sv_asm_new_label(a);
        sv_asm_label_t loaded = sv_asm_new_label(a);
        if (c->element_available) {
          sv_asm_label_t miss = sv_asm_new_label(a);
          sv_asm_i64_beq_imm(a, miss, c->cached_element_valid, 0);
          sv_asm_i64_bne(a, miss, obj, c->cached_element_object);
          sv_asm_i64_bne(a, miss, key, c->cached_element_key);
          sv_asm_mov(a, c->r_err_tmp, c->cached_element_value);
          sv_asm_jump(a, loaded);
          sv_asm_bind(a, miss);
        }
        sv_asm_label_t typed = typed_seen ? sv_asm_new_label(a) : slow;
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        int element_site = mir_next_reg_site(&c->reg_site_n);
        (void)mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_READ, typed,
            element_site, NULL);
        sv_asm_mov(a, c->cached_element_object, obj);
        sv_asm_mov(a, c->cached_element_key, key);
        sv_asm_mov(a, c->cached_element_value, c->r_err_tmp);
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 1);
        sv_asm_bind(a, loaded);
        sv_asm_mov(a, dst, c->r_err_tmp);
        sv_asm_jump(a, element_done);
        if (typed_seen) {
          sv_asm_label_t typed_loaded = sv_asm_new_label(a);
          char name[48];
          snprintf(name, sizeof(name), "ta_number_%d", element_site);
          MIR_reg_t number = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, name);
          sv_asm_bind(a, typed);
          mir_emit_typed_array_read(
              c->ctx, c->jit_func, obj, index, number, slow, typed_loaded, element_site,
              sv_tfb_elem_type(c->func, c->bc_off));
          sv_asm_bind(a, typed_loaded);
          mir_d_to_i64(c->ctx, c->jit_func, dst, number, c->r_d_slot);
          sv_asm_jump(a, element_done);
        }
        sv_asm_bind(a, slow);
        mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 0);
        jit_emit_index_read_fast(c, obj, key, dst, element_done);
      }
      c->element_available = !obj_is_num;
      sv_ic_entry_t *element_ic = code_arena_bump(c->js, sizeof(*element_ic));
      if (element_ic) memset(element_ic, 0, sizeof(*element_ic));
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 10, MIR_new_ref_op(c->ctx, c->ge_proto), MIR_new_ref_op(c->ctx, c->imp_get_elem), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, key), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off), MIR_new_uint_op(c->ctx, (uintptr_t)element_ic)));
      jit_emit_throw_if_error(c, dst);
      if (element_done) sv_asm_bind(a, element_done);
      break;
    }

    case OP_GET_ELEM_OPT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      sv_asm_label_t nullish = sv_asm_new_label(a);
      sv_asm_label_t no_err = sv_asm_new_label(a);
      sv_asm_i64_beq_imm(a, nullish, obj, mkval(kTypeNull, 0));
      sv_asm_i64_beq_imm(a, nullish, obj, mkval(kTypeUndefined, 0));
      sv_ic_entry_t *element_ic = code_arena_bump(c->js, sizeof(*element_ic));
      if (element_ic) memset(element_ic, 0, sizeof(*element_ic));
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 10, MIR_new_ref_op(c->ctx, c->ge_proto), MIR_new_ref_op(c->ctx, c->imp_get_elem), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, key), MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func), MIR_new_int_op(c->ctx, (int64_t)c->bc_off), MIR_new_uint_op(c->ctx, (uintptr_t)element_ic)));
      sv_asm_i64_ursh_imm(a, c->r_bool, dst, NANBOX_TYPE_SHIFT);
      sv_asm_i64_bne_imm(a, no_err, c->r_bool, JIT_ERR_TAG);
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        sv_asm_mov(a, c->vs.regs[h->saved_sp], dst);
        sv_asm_jump(a, h->catch_label);
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      sv_asm_bind(a, nullish);
      mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
      sv_asm_bind(a, no_err);
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
      bool typed_seen = sv_tfb_elem_saw_typed_array(feedback);
      MIR_reg_t integer_index = 0;
      if (c->vs.slot_type[c->vs.sp - 2] == SLOT_I32) {
        char name[48];
        snprintf(name, sizeof(name), "integer_index_%d", mir_next_reg_site(&c->reg_site_n));
        integer_index = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
        sv_asm_mov(a, integer_index, c->vs.regs[c->vs.sp - 2]);
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
        sv_asm_label_t bail_direct = sv_asm_new_label(a);
        sv_asm_label_t done = sv_asm_new_label(a);
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
        sv_asm_label_t typed = typed_seen ? sv_asm_new_label(a) : bail_direct;
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, old_value,
            JIT_ELEMENT_WRITE, typed, element_site, NULL);
        sv_asm_label_t tagged_old_value = NULL;
        sv_asm_label_t store = NULL;
        if (tagged_old_possible) {
          tagged_old_value = sv_asm_new_label(a);
          store = sv_asm_new_label(a);
          sv_asm_u64_bgt_imm(a, tagged_old_value, old_value, NANBOX_PREFIX);
          sv_asm_bind(a, store);
        } else {
          mir_emit_is_num_guard(
              c->ctx, c->jit_func, c->r_bool, old_value, bail_direct);
        }
        sv_asm_store_index(a, MIR_JSVAL, data, 0, index, sizeof(ant_value_t), val);
        sv_asm_jump(a, done);
        if (tagged_old_possible) {
          sv_asm_bind(a, tagged_old_value);
          sv_asm_u64_bge_imm(a, bail_direct, old_value, ANT_SENTINEL_TAG);
          sv_asm_jump(a, store);
        }
        if (typed_seen) {
          sv_asm_bind(a, typed);
          mir_emit_typed_array_write(
              c->ctx, c->jit_func, obj, index, val, c->r_d_slot, bail_direct, done, element_site,
              sv_tfb_elem_type(c->func, c->bc_off));
        }
        sv_asm_bind(a, bail_direct);
        mir_emit_bailout_jump_typed(
            c->ctx, c->jit_func, c->bc_off, c->vs.sp + 3, &c->bailout_ctx,
            -1, SLOT_BOXED, c->vs.sp + 1,
            integer_index ? SLOT_I32 : (key_is_num ? SLOT_NUM : SLOT_BOXED));
        sv_asm_bind(a, done);
        break;
      }

      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t key = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_pop(&c->vs);
      sv_asm_label_t element_done = NULL;
      if (!obj_is_num) {
        sv_asm_label_t slow = sv_asm_new_label(a);
        element_done = sv_asm_new_label(a);
        MIR_reg_t index = mir_emit_known_array_index_guard(
            c->ctx, c->jit_func, key, integer_index, slow, c->r_d_slot,
            mir_next_reg_site(&c->reg_site_n), c->cached_index_key, c->cached_index_value);
        int site = mir_next_reg_site(&c->reg_site_n);
        sv_asm_label_t add = sv_asm_new_label(a);
        jit_dense_element_t element = {.past_end = sv_asm_new_label(a)};
        sv_asm_label_t typed = typed_seen ? sv_asm_new_label(a) : slow;
        MIR_reg_t data = mir_emit_dense_element_guard(
            c->ctx, c->jit_func, obj, index, c->r_err_tmp, JIT_ELEMENT_WRITE, typed,
            site, &element);
        MIR_reg_t flags = element.flags;
        sv_asm_u64_bge_imm(a, add, c->r_err_tmp, ANT_SENTINEL_TAG);
        sv_asm_store_index(a, MIR_JSVAL, data, 0, index, sizeof(ant_value_t), val);
        sv_asm_label_t barrier = sv_asm_new_label(a);
        jit_emit_element_barrier(c, obj, index, val, flags, element_done, barrier);
        sv_asm_jump(a, element_done);

        // an append within capacity, or a store into a hole, adds an
        // element; it stores on its own so the store above stays a plain one
        sv_asm_bind(a, element.past_end);
        sv_asm_i64_bne(a, slow, index, element.len);
        sv_asm_load(a, MIR_T_U32, c->r_bool, element.ptr, offsetof(ant_object_t, u.array.cap));
        sv_asm_u64_bge(a, slow, index, c->r_bool);
        sv_asm_bind(a, add);
        jit_emit_dense_add(c, element.ptr, data, flags, index, element.len, val, slow, site);
        jit_emit_element_barrier_filter(c, val, flags, element_done);
        sv_asm_jump(a, barrier);
        if (typed_seen) {
          sv_asm_bind(a, typed);
          mir_emit_typed_array_write(
              c->ctx, c->jit_func, obj, index, val, c->r_d_slot, slow, element_done, site,
              sv_tfb_elem_type(c->func, c->bc_off));
        }
        sv_asm_bind(a, slow);
      }
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->put_elem_proto), MIR_new_ref_op(c->ctx, c->imp_put_elem), MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, key), MIR_new_reg_op(c->ctx, val)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      if (element_done) sv_asm_bind(a, element_done);
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
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->private_put_proto), MIR_new_ref_op(c->ctx, c->imp_put_private), MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, val), MIR_new_reg_op(c->ctx, token)));
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_COPY_DATA_PROPS: {
      uint16_t excluded = sv_get_u16(c->ip + 1);
      for (int i = c->vs.sp - (int)excluded - 2; i < c->vs.sp; i++)
        vstack_ensure_boxed(&c->vs, i, c->ctx, c->jit_func, c->r_d_slot);
      
      for (int i = (int)excluded - 1; i >= 0; i--) {
        MIR_reg_t key = vstack_pop(&c->vs);
        sv_asm_store(a, MIR_JSVAL, c->r_args_buf, (MIR_disp_t)(i * (int)sizeof(ant_value_t)), key);
      }
      
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->copy_props_proto), MIR_new_ref_op(c->ctx, c->imp_copy_data_props), MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 2]), MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1]), MIR_new_reg_op(c->ctx, c->r_args_buf), MIR_new_int_op(c->ctx, excluded)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      break;
    }

    case OP_SPREAD: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t iterable = vstack_pop(&c->vs);
      MIR_reg_t arr = vstack_top(&c->vs);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->helper2_proto), MIR_new_ref_op(c->ctx, c->imp_spread), MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, arr), MIR_new_reg_op(c->ctx, iterable)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      break;
    }

    case OP_APPEND: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t arr = vstack_top(&c->vs);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->arr_push_proto), MIR_new_ref_op(c->ctx, c->imp_arr_push), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, arr), MIR_new_reg_op(c->ctx, val)));
      break;
    }

    case OP_DEF_PRIVATE: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_pop(&c->vs);
      MIR_reg_t token = vstack_pop(&c->vs);
      MIR_reg_t obj = vstack_top(&c->vs);
      sv_asm_append(a, MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->private_def_proto), MIR_new_ref_op(c->ctx, c->imp_def_private), MIR_new_reg_op(c->ctx, c->r_err_tmp), MIR_new_reg_op(c->ctx, c->r_vm), MIR_new_reg_op(c->ctx, c->r_js), MIR_new_reg_op(c->ctx, obj), MIR_new_reg_op(c->ctx, token), MIR_new_reg_op(c->ctx, val), MIR_new_int_op(c->ctx, sv_get_u8(c->ip + 1))));
      jit_emit_throw_if_error(c, c->r_err_tmp);
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
