#include "jit_internal.h"
jit_value_info_t vstack_value_info(const jit_vstack_t *vs, int idx) {
  jit_value_info_t info = {0};
  if (vs->known_func) info.known_func = vs->known_func[idx];
  if (vs->has_const && vs->has_const[idx]) {
    info.has_const = true;
    info.known_const = vs->known_const[idx];
  }
  if (vs->known_bool) info.known_bool = vs->known_bool[idx];
  if (vs->integer_range) info.integer_range = vs->integer_range[idx];
  return info;
}

void vstack_set_value_info(
    jit_vstack_t *vs, int idx, jit_value_info_t info) {
  if (vs->known_func) vs->known_func[idx] = info.known_func;
  if (vs->has_const) {
    vs->has_const[idx] = info.has_const;
    if (info.has_const) vs->known_const[idx] = info.known_const;
  }
  if (vs->known_bool) vs->known_bool[idx] = info.known_bool;
  if (vs->integer_range) vs->integer_range[idx] = info.integer_range;
}

void vstack_clear_value_info(jit_vstack_t *vs, int idx) {
  vstack_set_value_info(vs, idx, (jit_value_info_t){0});
}

static inline bool vstack_check_push(jit_vstack_t *vs) {
  if (vs->sp < vs->max) return true;
  vs->overflow = true;
  return false;
}

MIR_reg_t vstack_push(jit_vstack_t *vs) {
  if (!vstack_check_push(vs)) return vs->regs[vs->max - 1];
  vstack_clear_value_info(vs, vs->sp);
  if (vs->slot_type) vs->slot_type[vs->sp] = SLOT_BOXED;
  return vs->regs[vs->sp++];
}

MIR_reg_t vstack_push_const(jit_vstack_t *vs, uint64_t val) {
  if (!vstack_check_push(vs)) return vs->regs[vs->max - 1];
  vstack_clear_value_info(vs, vs->sp);
  if (vs->slot_type) vs->slot_type[vs->sp] = SLOT_BOXED;
  if (vs->has_const) {
    vs->has_const[vs->sp] = true;
    vs->known_const[vs->sp] = val;
  }
  return vs->regs[vs->sp++];
}

MIR_reg_t vstack_pop(jit_vstack_t *vs) {
  return vs->regs[--vs->sp];
}

MIR_reg_t vstack_top(jit_vstack_t *vs) {
  return vs->regs[vs->sp - 1];
}

jit_integer_range_t jit_word_range(sv_op_t op, jit_integer_range_t left, jit_integer_range_t right) {
  jit_integer_range_t full = {INT32_MIN, INT32_MAX, true};
  if (op == OP_BAND) {
    if (right.known && right.min == right.max && right.min >= 0 && right.max <= INT32_MAX)
      return (jit_integer_range_t){0, right.max, true};
    if (left.known && left.min == left.max && left.min >= 0 && left.max <= INT32_MAX)
      return (jit_integer_range_t){0, left.max, true};
  }
  if (op == OP_USHR) full = (jit_integer_range_t){0, UINT32_MAX, true};
  if (!right.known || right.min != right.max) return full;
  unsigned shift = (unsigned)right.min & 31u;
  if (op == OP_SHR) {
    if (!left.known || left.min < INT32_MIN || left.max > INT32_MAX) left = full;
    return (jit_integer_range_t){left.min >> shift, left.max >> shift, true};
  }
  if (op == OP_USHR) {
    int64_t maximum = left.known && left.min >= 0 ? left.max : UINT32_MAX;
    return (jit_integer_range_t){0, maximum >> shift, true};
  }
  if (op == OP_SHL && left.known && left.min >= 0 && left.max <= (INT32_MAX >> shift))
    return (jit_integer_range_t){left.min << shift, left.max << shift, true};
  return full;
}

static jit_integer_range_t jit_number_integer_range(double number) {
  if (number < INT32_MIN || number > UINT32_MAX || !isfinite(number) ||
      trunc(number) != number || (number == 0 && signbit(number)))
    return (jit_integer_range_t){0};
  return (jit_integer_range_t){(int64_t)number, (int64_t)number, true};
}

static jit_integer_range_t jit_constant_integer_range(sv_func_t *func, uint8_t *ip) {
  if (!ip) return (jit_integer_range_t){0};
  if (*ip == OP_CONST_I8) return jit_number_integer_range(sv_get_i8(ip + 1));
  if (*ip == OP_CONST || *ip == OP_CONST8) {
    uint32_t index = *ip == OP_CONST ? sv_get_u32(ip + 1) : sv_get_u8(ip + 1);
    if (index < (uint32_t)func->const_count && vtype(func->constants[index]) == kTypeNumber)
      return jit_number_integer_range(tod(func->constants[index]));
  }
  return (jit_integer_range_t){0};
}

void jit_emit_integer_constant(
    MIR_context_t ctx, MIR_item_t fn, jit_vstack_t *vs, MIR_reg_t dst, double number) {
  jit_integer_range_t range = jit_number_integer_range(number);
  if (!range.known) return;
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst), MIR_new_uint_op(ctx, (uint64_t)range.min)));
  vs->slot_type[vs->sp - 1] = SLOT_I32;
  vs->integer_range[vs->sp - 1] = range;
}

void jit_entry_integer_ranges(
    sv_func_t *func, jit_integer_range_t *ranges,
    int n_locals, int param_count) {
  enum { UNASSIGNED,
         ASSIGNED,
         REJECTED };
  uint8_t *state = calloc((size_t)n_locals, sizeof(*state));
  if (!state) return;

  bool before_branch = true;
  uint8_t *previous = NULL;
  uint8_t *before_previous = NULL;
  uint8_t *end = func->code + func->code_len;

  for (uint8_t *ip = func->code; ip < end;) {
    sv_op_t op = (sv_op_t)*ip;
    int size = sv_op_size[op];
    if (!size || ip + size > end || op == OP_PUT_LOCAL_CHK) {
      memset(ranges, 0, (size_t)n_locals * sizeof(*ranges));
      break;
    }

    uint16_t flags = sv_op_flags[op];
    if (flags & (SV_OPF_JIT_BRANCH32 | SV_OPF_JIT_BRANCH8))
      before_branch = false;

    int index = -1;
    switch (op) {
      case OP_PUT_LOCAL:
      case OP_SET_LOCAL:
      case OP_PUT_LOCAL8:
      case OP_SET_LOCAL8: {
        bool short_op = (op == OP_PUT_LOCAL8 || op == OP_SET_LOCAL8);
        index = short_op ? sv_get_u8(ip + 1) : sv_get_u16(ip + 1);
        if (index >= n_locals) break;
        if (!before_branch || state[index] != UNASSIGNED) {
          state[index] = REJECTED;
          break;
        }

        state[index] = ASSIGNED;
        ranges[index] = jit_constant_integer_range(func, previous);
        if (!previous) break;

        sv_op_t prev_op = (sv_op_t)*previous;
        bool word_op = prev_op == OP_BAND || prev_op == OP_BOR || prev_op == OP_BXOR ||
                       prev_op == OP_SHL || prev_op == OP_SHR || prev_op == OP_USHR;
        if (!word_op) break;

        uint8_t *type_feedback = sv_func_type_feedback(func);
        uint8_t feedback = type_feedback ? type_feedback[previous - func->code] : 0;
        if (sv_tfb_specialization_ready(feedback))
          ranges[index] = jit_word_range(
              prev_op, (jit_integer_range_t){0},
              jit_constant_integer_range(func, before_previous));
        break;
      }

      case OP_INC_LOCAL:
      case OP_DEC_LOCAL:
      case OP_ADD_LOCAL:
        index = sv_get_u8(ip + 1);
        if (index < n_locals) state[index] = REJECTED;
        break;

      case OP_SET_LOCAL_UNDEF:
        index = sv_get_u16(ip + 1);
        if (index < n_locals && (state[index] != UNASSIGNED || !before_branch))
          state[index] = REJECTED;
        break;

      default:
        if (flags & SV_OPF_BUILDER_TARGET) {
          index = (int)sv_get_u16(ip + 1) - param_count;
          if (index >= 0 && index < n_locals) state[index] = REJECTED;
        }
        break;
    }

    if (index >= 0 && index < n_locals && state[index] == REJECTED)
      ranges[index] = (jit_integer_range_t){0};

    before_previous = previous;
    previous = ip;
    ip += size;
  }

  free(state);
}

bool jit_emit_integer_arithmetic(
    MIR_context_t ctx, MIR_item_t fn, jit_vstack_t *vs, sv_op_t op) {
  int li = vs->sp - 2, ri = vs->sp - 1;
  if (vs->slot_type[li] != SLOT_I32 || vs->slot_type[ri] != SLOT_I32) return false;
  jit_integer_range_t l = vs->integer_range[li], r = vs->integer_range[ri];
  if (!l.known || !r.known) return false;
  __int128 lo, hi;
  MIR_insn_code_t code;
  if (op == OP_ADD || op == OP_ADD_NUM) {
    code = MIR_ADD;
    lo = (__int128)l.min + r.min;
    hi = (__int128)l.max + r.max;
  } else if (op == OP_SUB || op == OP_SUB_NUM) {
    code = MIR_SUB;
    lo = (__int128)l.min - r.max;
    hi = (__int128)l.max - r.min;
  } else {
    if ((l.min < 0 && r.min <= 0 && r.max >= 0) ||
        (r.min < 0 && l.min <= 0 && l.max >= 0)) return false;
    code = MIR_MUL;
    __int128 products[] = {(__int128)l.min * r.min, (__int128)l.min * r.max,
                           (__int128)l.max * r.min, (__int128)l.max * r.max};
    lo = hi = products[0];
    for (int i = 1; i < 4; i++) {
      if (products[i] < lo) lo = products[i];
      if (products[i] > hi) hi = products[i];
    }
  }
  if (lo < INT32_MIN || hi > UINT32_MAX) return false;
  MIR_reg_t right = vstack_pop(vs), left = vstack_pop(vs), dst = vstack_push(vs);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, code, MIR_new_reg_op(ctx, dst), MIR_new_reg_op(ctx, left), MIR_new_reg_op(ctx, right)));
  vs->slot_type[vs->sp - 1] = SLOT_I32;
  vs->integer_range[vs->sp - 1] = (jit_integer_range_t){(int64_t)lo, (int64_t)hi, true};
  return true;
}

MIR_label_t label_for_offset(MIR_context_t ctx, jit_label_map_t *lm,
                             int bc_off) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off == bc_off) return lm->entries[i].label;
  if (lm->count >= MAX_LABELS) return NULL;
  MIR_label_t lbl = MIR_new_label(ctx);
  lm->entries[lm->count].bc_off = bc_off;
  lm->entries[lm->count].label = lbl;
  lm->entries[lm->count].sp = -1;
  lm->count++;
  return lbl;
}

int jit_constant_object_span(sv_func_t *func, sv_obj_site_cache_t *site, jit_label_map_t *lm) {
  if (!site || !site->key_atoms || !site->key_count) return 0;
  uint8_t *begin = func->code + site->bc_off + sv_op_size[OP_OBJECT];
  uint8_t *ip = begin, *end = func->code + func->code_len;
  for (uint16_t i = 0; i < site->key_count; i++) {
    ant_value_t value;
    int size = sv_literal_constant_at(func, ip, &value);
    if (!size) return 0;
    ip += size;
    if (ip + sv_op_size[OP_DEFINE_SLOT] > end || *ip != OP_DEFINE_SLOT ||
        sv_get_u32(ip + 1) != site->key_atoms[i] || sv_get_u16(ip + 5) != i) return 0;
    ip += sv_op_size[OP_DEFINE_SLOT];
  }
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off >= begin - func->code && lm->entries[i].bc_off < ip - func->code) return 0;
  return (int)(ip - begin);
}

static void label_record_sp(jit_label_map_t *lm, int bc_off, int sp) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off != bc_off)
      continue;
    else {
      if (lm->entries[i].sp < 0) lm->entries[i].sp = sp;
      return;
    }
}

MIR_label_t label_for_branch(MIR_context_t ctx, jit_label_map_t *lm,
                             int bc_off, int sp) {
  MIR_label_t lbl = label_for_offset(ctx, lm, bc_off);
  label_record_sp(lm, bc_off, sp);
  return lbl;
}

void mir_emit_decode_ref(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t dst, MIR_reg_t value) {
  MIR_reg_t base = MIR_reg(ctx, "cage_base", fn->u.func);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, dst), MIR_new_reg_op(ctx, base)));
}

void mir_emit_cage_offset(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t dst, MIR_reg_t ptr) {
  MIR_reg_t base = MIR_reg(ctx, "cage_base", fn->u.func);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_SUB, MIR_new_reg_op(ctx, dst),
                               MIR_new_reg_op(ctx, ptr), MIR_new_reg_op(ctx, base)));
}

void mir_i64_to_d(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst_d, MIR_reg_t src_i64,
                  MIR_reg_t slot) {
#if SV_JIT_HAS_BITCAST
  (void)slot;
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_I2DB,
                               MIR_new_reg_op(ctx, dst_d),
                               MIR_new_reg_op(ctx, src_i64)));
#else
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_T_I64, 0, slot, 0, 1),
                               MIR_new_reg_op(ctx, src_i64)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DMOV,
                               MIR_new_reg_op(ctx, dst_d),
                               MIR_new_mem_op(ctx, MIR_T_D, 0, slot, 0, 1)));
#endif
}

void mir_d_to_i64_non_nan(MIR_context_t ctx, MIR_item_t fn,
                          MIR_reg_t dst_i64, MIR_reg_t src_d,
                          MIR_reg_t slot) {
#if SV_JIT_HAS_BITCAST
  (void)slot;
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_D2IB,
                               MIR_new_reg_op(ctx, dst_i64),
                               MIR_new_reg_op(ctx, src_d)));
#else
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DMOV,
                               MIR_new_mem_op(ctx, MIR_T_D, 0, slot, 0, 1),
                               MIR_new_reg_op(ctx, src_d)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst_i64),
                               MIR_new_mem_op(ctx, MIR_T_I64, 0, slot, 0, 1)));
#endif
}

void mir_d_to_i64(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst_i64, MIR_reg_t src_d,
                  MIR_reg_t slot) {
  MIR_label_t done = MIR_new_label(ctx);
  mir_d_to_i64_non_nan(ctx, fn, dst_i64, src_d, slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBLE, MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, dst_i64), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst_i64),
                               MIR_new_uint_op(ctx, UINT64_C(0x7FF8000000000000))));
  MIR_append_insn(ctx, fn, done);
}

void mir_emit_get_length(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t obj, MIR_reg_t dst,
    MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_d_slot,
    MIR_item_t helper1_proto, MIR_item_t imp_get_length,
    bool builder_slot,
    int owner_id, int bc_off) {
  char tag_name[48], ptr_name[48], len_name[48], dbl_name[48];
  snprintf(tag_name, sizeof(tag_name), "gl_tag_%d_%d", owner_id, bc_off);
  snprintf(ptr_name, sizeof(ptr_name), "gl_ptr_%d_%d", owner_id, bc_off);
  snprintf(len_name, sizeof(len_name), "gl_len_%d_%d", owner_id, bc_off);
  snprintf(dbl_name, sizeof(dbl_name), "gl_dbl_%d_%d", owner_id, bc_off);
  MIR_reg_t tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, tag_name);
  MIR_reg_t ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, ptr_name);
  MIR_reg_t len = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, len_name);
  MIR_reg_t dbl = MIR_new_func_reg(ctx, fn->u.func, MIR_T_D, dbl_name);
  MIR_label_t array = MIR_new_label(ctx);
  MIR_label_t ascii = MIR_new_label(ctx);
  MIR_label_t encode = MIR_new_label(ctx);
  MIR_label_t slow = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);
  MIR_label_t flat = MIR_new_label(ctx);
  MIR_label_t nonflat = MIR_new_label(ctx);
  MIR_label_t builder = builder_slot ? MIR_new_label(ctx) : slow;

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, obj),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, array),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, JIT_STR_TAG)));

  mir_emit_decode_ref(ctx, fn, ptr, obj);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, ptr),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, nonflat),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn, flat);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_flat_string_t, meta), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_uint_op(ctx, STR_META_ASCII_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, ascii),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, STR_ASCII_YES)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_uint_op(ctx, STR_META_UTF16_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, len),
                               MIR_new_uint_op(ctx, STR_UTF16_LEN_UNKNOWN)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, encode)));

  MIR_append_insn(ctx, fn, ascii);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(ant_flat_string_t, len), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, encode)));

  MIR_append_insn(ctx, fn, nonflat);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, builder), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, ptr), MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, len), MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_rope_heap_t, cached), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, len), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  mir_emit_decode_ref(ctx, fn, ptr, len);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, flat)));

  if (builder_slot) {
    MIR_append_insn(ctx, fn, builder);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE,
                                 MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, tag),
                                 MIR_new_uint_op(ctx, STR_HEAP_TAG_BUILDER)));

    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_AND,
                                 MIR_new_reg_op(ctx, ptr),
                                 MIR_new_reg_op(ctx, ptr),
                                 MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_reg_op(ctx, tag),
                                 MIR_new_mem_op(ctx, MIR_T_U8,
                                                (MIR_disp_t)offsetof(ant_string_builder_t, ascii_state), ptr, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE,
                                 MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, tag),
                                 MIR_new_uint_op(ctx, STR_ASCII_YES)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_reg_op(ctx, len),
                                 MIR_new_mem_op(ctx, MIR_T_U64,
                                                (MIR_disp_t)offsetof(ant_string_builder_t, len), ptr, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, encode)));
  }

  MIR_append_insn(ctx, fn, array);
  mir_emit_decode_ref(ctx, fn, ptr, obj);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, u.array.len), ptr, 0, 1)));

  MIR_append_insn(ctx, fn, encode);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UI2D,
                               MIR_new_reg_op(ctx, dbl),
                               MIR_new_reg_op(ctx, len)));
  mir_d_to_i64_non_nan(ctx, fn, dst, dbl, r_d_slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 6,
                                    MIR_new_ref_op(ctx, helper1_proto),
                                    MIR_new_ref_op(ctx, imp_get_length),
                                    MIR_new_reg_op(ctx, dst),
                                    MIR_new_reg_op(ctx, r_vm),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_reg_op(ctx, obj)));
  MIR_append_insn(ctx, fn, done);
}

static void mir_emit_i32_to_num(MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t number, MIR_reg_t integer) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_I2D,
                               MIR_new_reg_op(ctx, number),
                               MIR_new_reg_op(ctx, integer)));
}

void mir_emit_slot_boxed(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t boxed, MIR_reg_t number,
                         uint8_t slot_type, MIR_reg_t d_slot) {
  if (slot_type == SLOT_I32)
    mir_emit_i32_to_num(ctx, fn, number, boxed);
  if (slot_type == SLOT_NUM || slot_type == SLOT_I32)
    mir_d_to_i64(ctx, fn, boxed, number, d_slot);
}

void vstack_ensure_boxed(jit_vstack_t *vs, int idx,
                         MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t d_slot) {
  if (!vs->slot_type || vs->slot_type[idx] == SLOT_BOXED) return;
  mir_emit_slot_boxed(
      ctx, fn, vs->regs[idx], vs->d_regs[idx], vs->slot_type[idx], d_slot);
  vs->slot_type[idx] = SLOT_BOXED;
}

void vstack_ensure_num(jit_vstack_t *vs, int idx,
                       MIR_context_t ctx, MIR_item_t fn,
                       MIR_reg_t d_slot) {
  if (!vs->slot_type || vs->slot_type[idx] == SLOT_NUM) return;
  if (vs->slot_type[idx] == SLOT_I32)
    mir_emit_i32_to_num(ctx, fn, vs->d_regs[idx], vs->regs[idx]);
  else
    mir_i64_to_d(ctx, fn, vs->d_regs[idx], vs->regs[idx], d_slot);
  vs->slot_type[idx] = SLOT_NUM;
}

bool vstack_prepare_num(jit_vstack_t *vs, int idx,
                        MIR_context_t ctx, MIR_item_t fn,
                        MIR_reg_t d_slot) {
  if (!vs->slot_type) return false;
  if (vs->slot_type[idx] == SLOT_I32)
    vstack_ensure_num(vs, idx, ctx, fn, d_slot);
  return vs->slot_type[idx] == SLOT_NUM;
}

void vstack_flush_to_boxed(jit_vstack_t *vs,
                           MIR_context_t ctx, MIR_item_t fn,
                           MIR_reg_t d_slot) {
  if (!vs->slot_type) return;
  for (int i = 0; i < vs->sp; i++)
    vstack_ensure_boxed(vs, i, ctx, fn, d_slot);
}

void vstack_rebox_binop_operands(
    jit_vstack_t *vs, MIR_context_t ctx, MIR_item_t fn,
    uint8_t lhs_type, uint8_t rhs_type,
    MIR_reg_t d_slot) {
  mir_emit_slot_boxed(ctx, fn, vs->regs[vs->sp - 1],
                      vs->d_regs[vs->sp - 1], lhs_type, d_slot);
  mir_emit_slot_boxed(ctx, fn, vs->regs[vs->sp],
                      vs->d_regs[vs->sp], rhs_type, d_slot);
}

bool jit_const_is_heap(ant_value_t cv) {
  uint8_t t = vtype(cv);
  return ((1u << t) & ((1u << kTypeObject) | (1u << kTypeString) | (1u << kTypeArray) | (1u << kTypePromise) | (1u << kTypeBigInt) | (1u << kTypeGenerator) | (1u << kTypeSymbol))) != 0;
}

void mir_load_const_slot(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t dst, ant_value_t *slot) {
  mir_load_imm(ctx, fn, dst, (uint64_t)(uintptr_t)slot);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_mem_op(ctx, MIR_T_I64, 0, dst, 0, 1)));
}

void mir_call_helper2(MIR_context_t ctx, MIR_item_t fn,
                      MIR_reg_t dst,
                      MIR_item_t proto, MIR_item_t func_item,
                      MIR_reg_t vm_reg, MIR_reg_t js_reg,
                      MIR_reg_t arg0, MIR_reg_t arg1) {
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 7,
                                    MIR_new_ref_op(ctx, proto),
                                    MIR_new_ref_op(ctx, func_item),
                                    MIR_new_reg_op(ctx, dst),
                                    MIR_new_reg_op(ctx, vm_reg),
                                    MIR_new_reg_op(ctx, js_reg),
                                    MIR_new_reg_op(ctx, arg0),
                                    MIR_new_reg_op(ctx, arg1)));
}

void mir_emit_is_num_guard(MIR_context_t ctx, MIR_item_t fn,
                           MIR_reg_t r_bool, MIR_reg_t v,
                           MIR_label_t slow) {
  (void)r_bool;
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGT,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, v),
                               MIR_new_uint_op(ctx, NANBOX_PREFIX)));
}

int mir_next_reg_site(int *next_site) {
  ANT_ASSERT(next_site != NULL, "missing MIR register site counter");
  return (*next_site)++;
}

static MIR_reg_t mir_emit_integer_conversion_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double, bool is_known_double,
    MIR_reg_t d_slot, double minimum, double maximum,
    MIR_label_t slow, int site, bool exact) {
  char double_name[48], integer_name[48];
  snprintf(double_name, sizeof(double_name), "spec_d_%d", site);
  snprintf(integer_name, sizeof(integer_name), "spec_i_%d", site);

  MIR_reg_t number = known_double;
  if (!is_known_double) {
    number = MIR_new_func_reg(ctx, fn->u.func, MIR_T_D, double_name);
    mir_emit_is_num_guard(ctx, fn, 0, boxed, slow);
    mir_i64_to_d(ctx, fn, number, boxed, d_slot);
  }

  MIR_reg_t integer = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, integer_name);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DBNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, number),
                               MIR_new_reg_op(ctx, number)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DBLT,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, number),
                               MIR_new_double_op(ctx, minimum)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DBGT,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, number),
                               MIR_new_double_op(ctx, maximum)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_D2I,
                               MIR_new_reg_op(ctx, integer),
                               MIR_new_reg_op(ctx, number)));
  if (!exact) return integer;
  char roundtrip_name[48];
  snprintf(roundtrip_name, sizeof(roundtrip_name), "spec_rt_%d", site);
  MIR_reg_t roundtrip = MIR_new_func_reg(ctx, fn->u.func, MIR_T_D, roundtrip_name);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_I2D,
                               MIR_new_reg_op(ctx, roundtrip),
                               MIR_new_reg_op(ctx, integer)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DBNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, number),
                               MIR_new_reg_op(ctx, roundtrip)));

  return integer;
}

MIR_reg_t mir_emit_exact_integer_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double, bool is_known_double,
    MIR_reg_t d_slot, double minimum, double maximum,
    MIR_label_t slow, int site) {
  return mir_emit_integer_conversion_guard(
      ctx, fn, boxed, known_double, is_known_double, d_slot,
      minimum, maximum, slow, site, true);
}

void mir_emit_primitive_type_test(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t value, MIR_reg_t scratch, uint8_t type) {
  if (type == kTypeNumber) {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ULE, MIR_new_reg_op(ctx, scratch), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  } else {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, scratch), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, scratch), MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | type)));
  }
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, value), MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, js_false)));
}

void mir_emit_uncurried_char_code_at(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t target, MIR_reg_t args,
    MIR_reg_t result, MIR_reg_t js, MIR_reg_t d_slot,
    MIR_label_t slow, MIR_label_t done, int site) {
  char name[48];
  MIR_reg_t regs[5];
  for (int i = 0; i < 5; i++) {
    snprintf(name, sizeof(name), "char_%d_%d", site, i);
    regs[i] = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
  }
  MIR_reg_t tag = regs[0], ptr = regs[1], value = regs[2], index = regs[3], len = regs[4];
  snprintf(name, sizeof(name), "char_double_%d", site);
  MIR_reg_t number = MIR_new_func_reg(ctx, fn->u.func, MIR_T_D, name);
  MIR_label_t flat = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, target), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeFunction)));
  mir_emit_decode_ref(ctx, fn, ptr, target);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tag), MIR_new_mem_op(ctx, MIR_T_U32, offsetof(sv_closure_t, call_flags), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, SV_CALL_IS_UNCURRY | SV_CALL_HAS_BOUND_ARGS)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, SV_CALL_IS_UNCURRY)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, value), MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(sv_closure_t, bound_this), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeBuiltin)));
  mir_emit_decode_ref(ctx, fn, ptr, value);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tag), MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_cfunc_meta_t, fn), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, (uintptr_t)builtin_string_charCodeAt)));

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, value), MIR_new_mem_op(ctx, MIR_JSVAL, 0, args, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, flat), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  mir_emit_decode_ref(ctx, fn, ptr, value);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, ptr), MIR_new_reg_op(ctx, ptr), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, value), MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_rope_heap_t, cached), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, value), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));

  MIR_append_insn(ctx, fn, flat);
  mir_emit_decode_ref(ctx, fn, ptr, value);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tag), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_flat_string_t, meta), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_META_ASCII_MASK)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, (uint64_t)STR_ASCII_YES << STR_META_ASCII_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, index), MIR_new_mem_op(ctx, MIR_JSVAL, sizeof(ant_value_t), args, 0, 1)));
  MIR_reg_t integer = mir_emit_exact_integer_guard(
      ctx, fn, index, 0, false, d_slot, 0, INT32_MAX, slow, site);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, len), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(ant_flat_string_t, len), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, integer), MIR_new_reg_op(ctx, len)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tag), MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_t, vm_exec_depth), js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tag), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, value), MIR_new_mem_op(ctx, MIR_T_U8, offsetof(ant_flat_string_t, bytes), ptr, integer, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_I2D, MIR_new_reg_op(ctx, number), MIR_new_reg_op(ctx, value)));
  mir_d_to_i64_non_nan(ctx, fn, result, number, d_slot);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}

MIR_reg_t mir_emit_word32_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double,
    bool is_known_double, bool is_known_i32,
    MIR_reg_t d_slot, MIR_label_t slow, int site) {
  if (is_known_i32) return boxed;
  return mir_emit_integer_conversion_guard(
      ctx, fn, boxed, known_double, is_known_double, d_slot,
      (double)INT32_MIN, (double)UINT32_MAX, slow, site, false);
}

MIR_reg_t mir_emit_array_index_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double, bool is_known_double,
    MIR_reg_t d_slot, MIR_label_t slow, int site) {
  return mir_emit_exact_integer_guard(
      ctx, fn, boxed, known_double, is_known_double, d_slot,
      0.0, (double)UINT32_MAX - 1.0, slow, site);
}

MIR_reg_t mir_emit_known_array_index_guard(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t boxed,
    MIR_reg_t integer, MIR_label_t slow, MIR_reg_t d_slot, int site,
    MIR_reg_t cached_key, MIR_reg_t cached_index) {
  if (!integer) {
    MIR_label_t hit = MIR_new_label(ctx);
    mir_emit_is_num_guard(ctx, fn, 0, boxed, slow);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, hit), MIR_new_reg_op(ctx, boxed), MIR_new_reg_op(ctx, cached_key)));
    MIR_reg_t checked = mir_emit_array_index_guard(
        ctx, fn, boxed, 0, false, d_slot, slow, site);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, cached_key), MIR_new_reg_op(ctx, boxed)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, cached_index), MIR_new_reg_op(ctx, checked)));
    MIR_append_insn(ctx, fn, hit);
    char name[48];
    snprintf(name, sizeof(name), "checked_index_%d", site);
    MIR_reg_t result = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, result), MIR_new_reg_op(ctx, cached_index)));
    return result;
  }
  return integer;
}

MIR_reg_t mir_emit_dense_element_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t object, MIR_reg_t index, MIR_reg_t value,
    jit_element_access_t access, MIR_label_t slow, int site) {
  bool writable = access == JIT_ELEMENT_WRITE;
  char tag_name[48], ptr_name[48], flags_name[48];
  char data_name[48], len_name[48];
  snprintf(tag_name, sizeof(tag_name), "elem_tag_%d", site);
  snprintf(ptr_name, sizeof(ptr_name), "elem_ptr_%d", site);
  snprintf(flags_name, sizeof(flags_name), "elem_flags_%d", site);
  snprintf(data_name, sizeof(data_name), "elem_data_%d", site);
  snprintf(len_name, sizeof(len_name), "elem_len_%d", site);

  MIR_reg_t tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, tag_name);
  MIR_reg_t ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, ptr_name);
  MIR_reg_t flags = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, flags_name);
  MIR_reg_t data = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, data_name);
  MIR_reg_t len = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, len_name);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, object),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
  mir_emit_decode_ref(ctx, fn, ptr, object);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, flags),
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_object_t, flags), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_reg_op(ctx, flags),
                               MIR_new_uint_op(ctx,
                                               ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS |
                                                   (writable ? ANT_OBJECT_FLAG_FROZEN : 0))));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, tag),
                               MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, data),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, u.array.data), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, data),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, len),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, u.array.len), ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, index),
                               MIR_new_reg_op(ctx, len)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, value),
                               MIR_new_mem_op(ctx, MIR_JSVAL, 0, data, index, sizeof(ant_value_t))));
  if (access == JIT_ELEMENT_NUMERIC_READ) {
    mir_emit_is_num_guard(ctx, fn, 0, value, slow);
  } else if (access == JIT_ELEMENT_READ) {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, value),
        MIR_new_uint_op(ctx, ANT_SENTINEL_TAG)));
  }

  return data;
}

void mir_emit_word32_binary(
    MIR_context_t ctx, MIR_item_t fn, sv_op_t op,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t result_double, bool materialize_double, int site) {
  char shift_name[48];
  snprintf(shift_name, sizeof(shift_name), "word_shift_%d", site);
  MIR_reg_t shift = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, shift_name);
  MIR_insn_code_t operation;

  switch (op) {
    case OP_BAND:
      operation = MIR_ANDS;
      break;
    case OP_BOR:
      operation = MIR_ORS;
      break;
    case OP_BXOR:
      operation = MIR_XORS;
      break;
    case OP_SHL:
      operation = MIR_LSHS;
      break;
    case OP_SHR:
      operation = MIR_RSHS;
      break;
    default:
      operation = MIR_URSHS;
      break;
  }

  if (op == OP_SHL || op == OP_SHR || op == OP_USHR) {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ANDS,
                                 MIR_new_reg_op(ctx, shift),
                                 MIR_new_reg_op(ctx, right),
                                 MIR_new_int_op(ctx, 0x1f)));
    right = shift;
  }

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, operation,
                               MIR_new_reg_op(ctx, result),
                               MIR_new_reg_op(ctx, left),
                               MIR_new_reg_op(ctx, right)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, op == OP_USHR ? MIR_UEXT32 : MIR_EXT32,
                               MIR_new_reg_op(ctx, result),
                               MIR_new_reg_op(ctx, result)));
  if (materialize_double)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_I2D,
                                 MIR_new_reg_op(ctx, result_double),
                                 MIR_new_reg_op(ctx, result)));
}

void mir_emit_numeric_equality(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t scratch, bool invert) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_DEQ,
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, left),
                               MIR_new_reg_op(ctx, right)));
  if (invert)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_XOR,
                                 MIR_new_reg_op(ctx, scratch),
                                 MIR_new_reg_op(ctx, scratch),
                                 MIR_new_int_op(ctx, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_OR,
                               MIR_new_reg_op(ctx, result),
                               MIR_new_uint_op(ctx, js_false),
                               MIR_new_reg_op(ctx, scratch)));
}

void mir_emit_strict_tagged_equality(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t scratch, bool invert,
    MIR_label_t slow, MIR_label_t done) {
  MIR_label_t left_number = MIR_new_label(ctx);
  MIR_label_t equal = MIR_new_label(ctx);
  MIR_label_t not_equal = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBLE, MIR_new_label_op(ctx, left_number),
                               MIR_new_reg_op(ctx, left), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBLE, MIR_new_label_op(ctx, not_equal),
                               MIR_new_reg_op(ctx, right), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, equal),
                               MIR_new_reg_op(ctx, left), MIR_new_reg_op(ctx, right)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_XOR, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, left), MIR_new_reg_op(ctx, right)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_MASK << NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, not_equal),
                               MIR_new_reg_op(ctx, scratch), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, left), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, scratch),
                               MIR_new_uint_op(ctx,
                                               (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeBigInt)));

  MIR_append_insn(ctx, fn, not_equal);
  mir_load_imm(ctx, fn, result, invert ? js_true : js_false);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, left_number);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, not_equal),
                               MIR_new_reg_op(ctx, right), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, slow)));

  MIR_append_insn(ctx, fn, equal);
  mir_load_imm(ctx, fn, result, invert ? js_false : js_true);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}
