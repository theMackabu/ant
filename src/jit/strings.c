#include "jit_internal.h"
void mir_emit_branch_if_string_builder(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t scratch, MIR_label_t builder) {
  MIR_label_t done = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_BUILDER)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, builder),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn, done);
}

bool jit_upvalue_is_builder_target(sv_func_t *func, uint32_t idx) {
  sv_func_t *f = func;
  uint32_t i = idx;

  for (int depth = 0; depth < 256; depth++) {
    if (!f || !f->upval_descs || i >= (uint32_t)f->upvalue_count) return true;
    sv_upval_desc_t *d = &f->upval_descs[i];
    sv_func_t *p = f->parent;
    if (!p) return true;

    if (d->is_local) {
      uint8_t *ip = p->code;
      uint8_t *end = p->code + p->code_len;
      while (ip < end) {
        sv_op_t op = (sv_op_t)*ip;
        int sz = sv_op_size[op];
        if (sz == 0) return true;
        if (ip + sz > end) return true;
        if ((sv_op_flags[op] & SV_OPF_BUILDER_TARGET) != 0 &&
            sv_get_u16(ip + 1) == d->index) return true;
        ip += sz;
      }
      return false;
    }

    f = p;
    i = d->index;
  }
  return true;
}

MIR_label_t mir_emit_string_builder_read_open(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t scratch,
    MIR_reg_t r_vm, MIR_reg_t r_js,
    MIR_item_t helper1_proto, MIR_item_t imp_str_read_value) {
  MIR_label_t done = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_BUILDER)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 6,
                                    MIR_new_ref_op(ctx, helper1_proto),
                                    MIR_new_ref_op(ctx, imp_str_read_value),
                                    MIR_new_reg_op(ctx, value),
                                    MIR_new_reg_op(ctx, r_vm),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_reg_op(ctx, value)));
  return done;
}

void mir_emit_string_builder_append_ascii_byte(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t result,
    MIR_reg_t d_slot,
    MIR_label_t slow, MIR_label_t done,
    int owner_id, int bc_off) {
  char tag_name[48], lhs_ptr_name[48], rhs_ptr_name[48];
  char len_name[48], tail_len_name[48], byte_name[48], cached_name[48];
  char cached_d_name[48];
  snprintf(tag_name, sizeof(tag_name), "sab_tag_%d_%d", owner_id, bc_off);
  snprintf(lhs_ptr_name, sizeof(lhs_ptr_name), "sab_lhs_%d_%d", owner_id, bc_off);
  snprintf(rhs_ptr_name, sizeof(rhs_ptr_name), "sab_rhs_%d_%d", owner_id, bc_off);
  snprintf(len_name, sizeof(len_name), "sab_len_%d_%d", owner_id, bc_off);
  snprintf(tail_len_name, sizeof(tail_len_name), "sab_tail_%d_%d", owner_id, bc_off);
  snprintf(byte_name, sizeof(byte_name), "sab_byte_%d_%d", owner_id, bc_off);
  snprintf(cached_name, sizeof(cached_name), "sab_cache_%d_%d", owner_id, bc_off);
  snprintf(cached_d_name, sizeof(cached_d_name), "sab_cache_d_%d_%d", owner_id, bc_off);

  MIR_reg_t tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, tag_name);
  MIR_reg_t lhs_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, lhs_ptr_name);
  MIR_reg_t rhs_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, rhs_ptr_name);
  MIR_reg_t len = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, len_name);
  MIR_reg_t tail_len = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, tail_len_name);
  MIR_reg_t byte = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, byte_name);
  MIR_reg_t cached = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, cached_name);
  MIR_reg_t cached_d = MIR_new_func_reg(ctx, fn->u.func, MIR_T_D, cached_d_name);
  MIR_label_t cached_nonnum = MIR_new_label(ctx);
  MIR_label_t cached_done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, lhs),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, JIT_STR_TAG)));
  mir_emit_decode_ref(ctx, fn, lhs_ptr, lhs);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, lhs_ptr),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_BUILDER)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, lhs_ptr),
                               MIR_new_reg_op(ctx, lhs_ptr),
                               MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, rhs),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, JIT_STR_TAG)));
  mir_emit_decode_ref(ctx, fn, rhs_ptr, rhs);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, rhs_ptr),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_flat_string_t, len), rhs_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_int_op(ctx, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, byte),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_flat_string_t, bytes), rhs_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, byte),
                               MIR_new_int_op(ctx, 0x80)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, tail_len),
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, tail_len), lhs_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tail_len),
                               MIR_new_int_op(ctx, STR_BUILDER_TAIL_CAP)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, cached),
                               MIR_new_mem_op(ctx, MIR_JSVAL,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, cached), lhs_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGT,
                               MIR_new_label_op(ctx, cached_nonnum),
                               MIR_new_reg_op(ctx, cached),
                               MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  mir_i64_to_d(ctx, fn, cached_d, cached, d_slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DADD,
                               MIR_new_reg_op(ctx, cached_d),
                               MIR_new_reg_op(ctx, cached_d),
                               MIR_new_double_op(ctx, 1.0)));
  mir_d_to_i64_non_nan(ctx, fn, cached, cached_d, d_slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, cached_done)));
  MIR_append_insn(ctx, fn, cached_nonnum);
  mir_load_imm(ctx, fn, cached, mkval(kTypeUndefined, 0));
  MIR_append_insn(ctx, fn, cached_done);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, tail), lhs_ptr, tail_len, 1),
                               MIR_new_reg_op(ctx, byte)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_ADD,
                               MIR_new_reg_op(ctx, tail_len),
                               MIR_new_reg_op(ctx, tail_len),
                               MIR_new_int_op(ctx, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, tail_len), lhs_ptr, 0, 1),
                               MIR_new_reg_op(ctx, tail_len)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, len), lhs_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_ADD,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_int_op(ctx, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, len), lhs_ptr, 0, 1),
                               MIR_new_reg_op(ctx, len)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_JSVAL,
                                              (MIR_disp_t)offsetof(ant_string_builder_t, cached), lhs_ptr, 0, 1),
                               MIR_new_reg_op(ctx, cached)));
  mir_load_imm(ctx, fn, result, mkval(kTypeUndefined, 0));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}

void mir_emit_numeric_local_store_mirror(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t local_d,
    MIR_reg_t src,
    MIR_reg_t src_d,
    bool src_is_num,
    MIR_reg_t r_bool,
    int resume_bc_off,
    int post_op_sp,
    const jit_bailout_emit_t *bail) {
  if (src_is_num) {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_DMOV,
                                 MIR_new_reg_op(ctx, local_d),
                                 MIR_new_reg_op(ctx, src_d)));
    return;
  }

  MIR_label_t slow = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);
  mir_emit_is_num_guard(ctx, fn, r_bool, src, slow);
  mir_i64_to_d(ctx, fn, local_d, src, bail->d_slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, bail->val),
                               MIR_new_uint_op(ctx, (uint64_t)SV_JIT_BAILOUT)));
  mir_emit_bailout_check(ctx, fn, bail->val, 0,
                         resume_bc_off, post_op_sp, bail);
  MIR_append_insn(ctx, fn, done);
}

static void mir_emit_string_concat_classify_side(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t tag, MIR_reg_t ptr,
    MIR_reg_t len, MIR_reg_t depth,
    MIR_label_t slow) {
  MIR_label_t flat = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  mir_emit_decode_ref(ctx, fn, ptr, value);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, flat),
                               MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, ptr),
                               MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_rope_heap_t, len), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, depth),
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_rope_heap_t, depth), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
  MIR_append_insn(ctx, fn, flat);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_flat_string_t, len), ptr, 0, 1)));
  mir_load_imm(ctx, fn, depth, 0);
  MIR_append_insn(ctx, fn, done);
}

static void mir_emit_short_string_concat(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t r_js,
    MIR_reg_t lp, MIR_reg_t rp, MIR_reg_t llen, MIR_reg_t rlen, MIR_reg_t len,
    MIR_reg_t ldepth, MIR_reg_t rdepth, MIR_reg_t dst,
    MIR_reg_t bucket, MIR_reg_t ptr, MIR_reg_t count, MIR_reg_t next,
    MIR_reg_t tmp, MIR_reg_t current, MIR_reg_t index,
    MIR_label_t slow) {
  MIR_label_t bucket_ready = MIR_new_label(ctx);
  MIR_label_t bump = MIR_new_label(ctx);
  MIR_label_t allocated = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, ldepth), MIR_new_reg_op(ctx, rdepth)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
  for (int side = 0; side < 2; side++) {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tmp), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_flat_string_t, meta), side ? rp : lp, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, STR_META_ASCII_MASK)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, (uint64_t)STR_ASCII_YES << STR_META_ASCII_SHIFT)));
  }

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, count), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_t, gc_pool_alloc), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, count), MIR_new_reg_op(ctx, count), MIR_new_reg_op(ctx, len)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, count), MIR_new_reg_op(ctx, count), MIR_new_uint_op(ctx, sizeof(ant_flat_string_t) + 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, count), MIR_new_uint_op(ctx, GC_POOL_PRESSURE_FLOOR)));

  for (size_t max_len = 1; max_len < STR_SHORT_CONS_THRESHOLD; max_len++) {
    size_t overhead = sizeof(ant_flat_string_t) + _Alignof(ant_flat_string_t);
    int class_index = pool_size_class_index(overhead + max_len);
    if (class_index < 0) continue;
    if (max_len + 1 < STR_SHORT_CONS_THRESHOLD &&
        class_index == pool_size_class_index(overhead + max_len + 1)) continue;
    MIR_label_t next_class = MIR_new_label(ctx);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, next_class), MIR_new_reg_op(ctx, len), MIR_new_uint_op(ctx, max_len)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, bucket), MIR_new_reg_op(ctx, r_js), MIR_new_uint_op(ctx, offsetof(ant_t, pool.string.classes) + (size_t)class_index * sizeof(ant_pool_bucket_t))));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, bucket_ready)));
    MIR_append_insn(ctx, fn, next_class);
  }
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, slow)));
  MIR_append_insn(ctx, fn, bucket_ready);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ptr), MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, slot_free), bucket, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, bump), MIR_new_reg_op(ctx, ptr), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, next), MIR_new_mem_op(ctx, MIR_T_P, 0, ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, slot_free), bucket, 0, 1), MIR_new_reg_op(ctx, next)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, allocated)));
  MIR_append_insn(ctx, fn, bump);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, current), MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, current), bucket, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, current), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ptr), MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, cursor), bucket, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, next), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_pool_bucket_t, slot_stride), bucket, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, ptr), MIR_new_reg_op(ctx, next)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tmp), MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, end), bucket, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, tmp)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_pool_bucket_t, cursor), bucket, 0, 1), MIR_new_reg_op(ctx, next)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_SUB, MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, current)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_SUB, MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, offsetof(ant_pool_block_t, data))));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_pool_block_t, used), current, 0, 1), MIR_new_reg_op(ctx, tmp)));
  MIR_append_insn(ctx, fn, allocated);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_t, gc_pool_alloc), r_js, 0, 1), MIR_new_reg_op(ctx, count)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_flat_string_t, len), ptr, 0, 1), MIR_new_reg_op(ctx, len)));
  mir_load_imm(ctx, fn, tmp, ((uint64_t)STR_ASCII_YES << STR_META_ASCII_SHIFT) | STR_UTF16_LEN_UNKNOWN);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_flat_string_t, meta), ptr, 0, 1), MIR_new_reg_op(ctx, tmp)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, offsetof(ant_flat_string_t, bytes))));
  for (int side = 0; side < 2; side++) {
    MIR_label_t copy = MIR_new_label(ctx);
    mir_load_imm(ctx, fn, index, 0);
    MIR_append_insn(ctx, fn, copy);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tmp), MIR_new_mem_op(ctx, MIR_T_U8, offsetof(ant_flat_string_t, bytes), side ? rp : lp, index, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U8, 0, next, index, 1), MIR_new_reg_op(ctx, tmp)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, index), MIR_new_reg_op(ctx, index), MIR_new_int_op(ctx, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBLT, MIR_new_label_op(ctx, copy), MIR_new_reg_op(ctx, index), MIR_new_reg_op(ctx, side ? rlen : llen)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, index)));
  }
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_U8, 0, next, 0, 1), MIR_new_int_op(ctx, 0)));
  mir_emit_cage_offset(ctx, fn, dst, ptr);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, dst), MIR_new_reg_op(ctx, dst), MIR_new_uint_op(ctx, mkval(kTypeString, STR_HEAP_TAG_FLAT))));
}

void mir_emit_string_concat_fastpath(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_js, MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t dst,
    MIR_label_t slow, int owner_id, int bc_off, bool flat_only) {
  char names[16][48];
  MIR_reg_t regs[16];
  static const char *suffix[16] = {
      "tag", "lp", "rp", "llen", "rlen", "ldepth", "rdepth", "len",
      "depth", "head", "used", "cap", "ptr", "count", "next", "tmp"};
  for (int i = 0; i < 16; i++) {
    snprintf(names[i], sizeof(names[i]), "sc_%s_%d_%d", suffix[i], owner_id, bc_off);
    regs[i] = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, names[i]);
  }
  MIR_reg_t tag = regs[0], lp = regs[1], rp = regs[2];
  MIR_reg_t llen = regs[3], rlen = regs[4];
  MIR_reg_t ldepth = regs[5], rdepth = regs[6], len = regs[7], depth = regs[8];
  MIR_reg_t head = regs[9], used = regs[10], cap = regs[11], ptr = regs[12];
  MIR_reg_t count = regs[13], next = regs[14], tmp = regs[15];

  mir_emit_string_concat_classify_side(
      ctx, fn, lhs, tag, lp, llen, ldepth, slow);
  mir_emit_string_concat_classify_side(
      ctx, fn, rhs, tag, rp, rlen, rdepth, slow);

  MIR_label_t return_left = MIR_new_label(ctx);
  MIR_label_t return_right = MIR_new_label(ctx);
  MIR_label_t depth_ready = MIR_new_label(ctx);
  MIR_label_t depth_done = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);
  MIR_label_t short_flat = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, return_right),
                               MIR_new_reg_op(ctx, llen), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, return_left),
                               MIR_new_reg_op(ctx, rlen), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, len),
                               MIR_new_reg_op(ctx, llen), MIR_new_reg_op(ctx, rlen)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBLT, MIR_new_label_op(ctx, short_flat),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_int_op(ctx, STR_SHORT_CONS_THRESHOLD)));

  if (flat_only) {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, slow)));
  } else {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, count),
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_t, rope_gc.young_alloc), r_js, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, count),
                                 MIR_new_uint_op(ctx, GC_ROPE_NURSERY_THRESHOLD)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, head),
                                 MIR_new_mem_op(ctx, MIR_T_P,
                                                (MIR_disp_t)offsetof(ant_t, rope_gc.young.head), r_js, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, head), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, used),
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_pool_block_t, used), head, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, cap),
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_pool_block_t, cap), head, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, next),
                                 MIR_new_reg_op(ctx, used), MIR_new_uint_op(ctx, sizeof(ant_rope_heap_t))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, next), MIR_new_reg_op(ctx, cap)));

    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, ptr),
                                 MIR_new_reg_op(ctx, head),
                                 MIR_new_uint_op(ctx, offsetof(ant_pool_block_t, data))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, ptr),
                                 MIR_new_reg_op(ctx, ptr), MIR_new_reg_op(ctx, used)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_pool_block_t, used), head, 0, 1),
                                 MIR_new_reg_op(ctx, next)));

    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, next),
                                 MIR_new_reg_op(ctx, count), MIR_new_uint_op(ctx, sizeof(ant_rope_heap_t))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_t, rope_gc.young_alloc), r_js, 0, 1),
                                 MIR_new_reg_op(ctx, next)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, count),
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_t, gc_pool_alloc), r_js, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, count),
                                 MIR_new_reg_op(ctx, count), MIR_new_uint_op(ctx, sizeof(ant_rope_heap_t))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_t, gc_pool_alloc), r_js, 0, 1),
                                 MIR_new_reg_op(ctx, count)));

    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, len), ptr, 0, 1),
                                 MIR_new_reg_op(ctx, len)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, depth),
                                 MIR_new_reg_op(ctx, ldepth)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, depth_ready),
                                 MIR_new_reg_op(ctx, ldepth), MIR_new_reg_op(ctx, rdepth)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, depth),
                                 MIR_new_reg_op(ctx, rdepth)));
    MIR_append_insn(ctx, fn, depth_ready);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, depth_done),
                                 MIR_new_reg_op(ctx, depth),
                                 MIR_new_uint_op(ctx, ANT_ROPE_DEPTH_SATURATED)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, depth),
                                 MIR_new_reg_op(ctx, depth), MIR_new_int_op(ctx, 1)));
    MIR_append_insn(ctx, fn, depth_done);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U16,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, depth), ptr, 0, 1),
                                 MIR_new_reg_op(ctx, depth)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U16,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, flags), ptr, 0, 1),
                                 MIR_new_uint_op(ctx, ANT_ROPE_FLAG_YOUNG)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U32,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, mark_epoch), ptr, 0, 1),
                                 MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_JSVAL,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, left), ptr, 0, 1),
                                 MIR_new_reg_op(ctx, lhs)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_JSVAL,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, right), ptr, 0, 1),
                                 MIR_new_reg_op(ctx, rhs)));
    mir_load_imm(ctx, fn, tmp, mkval(kTypeUndefined, 0));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_JSVAL,
                                                (MIR_disp_t)offsetof(ant_rope_heap_t, cached), ptr, 0, 1),
                                 MIR_new_reg_op(ctx, tmp)));
    mir_emit_cage_offset(ctx, fn, dst, ptr);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, dst),
                                 MIR_new_reg_op(ctx, dst),
                                 MIR_new_uint_op(ctx, mkval(kTypeString, STR_HEAP_TAG_ROPE))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
  }

  MIR_append_insn(ctx, fn, short_flat);
  mir_emit_short_string_concat(ctx, fn, r_js, lp, rp, llen, rlen, len,
                               ldepth, rdepth, dst, head, ptr, count, next, tmp, cap, used, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, return_left);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst), MIR_new_reg_op(ctx, lhs)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
  MIR_append_insn(ctx, fn, return_right);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst), MIR_new_reg_op(ctx, rhs)));
  MIR_append_insn(ctx, fn, done);
}
