#ifdef ANT_JIT

#include "silver/swarm.h"
#include "silver/glue.h"
#include "silver/engine.h"
#include "silver/opcode.h"

#include "internal.h"
#include "debug.h"

#include <mir.h>
#include <mir-gen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static const char *sv_op_name[] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = #name,
#include "silver/opcode.h"
};

typedef struct {
  MIR_context_t ctx;
} sv_jit_ctx_t;

static sv_jit_ctx_t *jit_ctx_get(ant_t *js) {
  return (sv_jit_ctx_t *)js->jit_ctx;
}

static void jit_ctx_set(ant_t *js, sv_jit_ctx_t *ctx) {
  js->jit_ctx = ctx;
}

static void jit_ctx_remove(ant_t *js) {
  js->jit_ctx = NULL;
}

void sv_jit_init(ant_t *js) {
  if (jit_ctx_get(js)) return;
  sv_jit_ctx_t *jc = calloc(1, sizeof(*jc));
  if (!jc) return;
  jc->ctx = MIR_init();
  MIR_gen_init(jc->ctx);
  MIR_gen_set_optimize_level(jc->ctx, 2);
  jit_ctx_set(js, jc);
}

void sv_jit_destroy(ant_t *js) {
  sv_jit_ctx_t *jc = jit_ctx_get(js);
  if (!jc) return;
  MIR_gen_finish(jc->ctx);
  MIR_finish(jc->ctx);
  free(jc);
  jit_ctx_remove(js);
}


typedef struct {
  MIR_reg_t   *regs;        
  MIR_reg_t   *d_regs;      
  sv_func_t  **known_func;  
  uint8_t     *slot_type;    
  int          sp;           
  int          max;
} jit_vstack_t;

static MIR_reg_t vstack_push(jit_vstack_t *vs) {
  if (vs->known_func) vs->known_func[vs->sp] = NULL;
  if (vs->slot_type) vs->slot_type[vs->sp] = 0; 
  return vs->regs[vs->sp++];
}

static MIR_reg_t vstack_pop(jit_vstack_t *vs) {
  return vs->regs[--vs->sp];
}

static MIR_reg_t vstack_top(jit_vstack_t *vs) {
  return vs->regs[vs->sp - 1];
}


#define MAX_LABELS 1024

typedef struct {
  int          bc_off;
  MIR_label_t  label;
  int          sp;     
} jit_label_t;

typedef struct {
  jit_label_t entries[MAX_LABELS];
  int         count;
} jit_label_map_t;

static MIR_label_t label_for_offset(MIR_context_t ctx, jit_label_map_t *lm,
                                    int bc_off) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off == bc_off) return lm->entries[i].label;
  if (lm->count >= MAX_LABELS) return NULL;
  MIR_label_t lbl = MIR_new_label(ctx);
  lm->entries[lm->count].bc_off = bc_off;
  lm->entries[lm->count].label  = lbl;
  lm->entries[lm->count].sp     = -1;
  lm->count++;
  return lbl;
}

static void label_record_sp(jit_label_map_t *lm, int bc_off, int sp) {
  for (int i = 0; i < lm->count; i++)
    if (lm->entries[i].bc_off != bc_off) continue;
    else { if (lm->entries[i].sp < 0) lm->entries[i].sp = sp; return; }
}

static MIR_label_t label_for_branch(MIR_context_t ctx, jit_label_map_t *lm,
                                    int bc_off, int sp) {
  MIR_label_t lbl = label_for_offset(ctx, lm, bc_off);
  label_record_sp(lm, bc_off, sp);
  return lbl;
}


#define MIR_JSVAL MIR_T_I64

#define JIT_ERR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | T_ERR)


static void mir_i64_to_d(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t dst_d, MIR_reg_t src_i64,
                         MIR_reg_t slot) {
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_T_I64, 0, slot, 0, 1),
      MIR_new_reg_op(ctx, src_i64)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_DMOV,
      MIR_new_reg_op(ctx, dst_d),
      MIR_new_mem_op(ctx, MIR_T_D, 0, slot, 0, 1)));
}

static void mir_d_to_i64(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t dst_i64, MIR_reg_t src_d,
                         MIR_reg_t slot) {
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_DMOV,
      MIR_new_mem_op(ctx, MIR_T_D, 0, slot, 0, 1),
      MIR_new_reg_op(ctx, src_d)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst_i64),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, slot, 0, 1)));
}


#define SLOT_BOXED 0
#define SLOT_NUM   1

static void vstack_ensure_boxed(jit_vstack_t *vs, int idx,
                                MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t d_slot) {
  if (!vs->slot_type || vs->slot_type[idx] != SLOT_NUM) return;
  mir_d_to_i64(ctx, fn, vs->regs[idx], vs->d_regs[idx], d_slot);
  vs->slot_type[idx] = SLOT_BOXED;
}

static void vstack_ensure_num(jit_vstack_t *vs, int idx,
                              MIR_context_t ctx, MIR_item_t fn,
                              MIR_reg_t d_slot) {
  if (!vs->slot_type || vs->slot_type[idx] == SLOT_NUM) return;
  mir_i64_to_d(ctx, fn, vs->d_regs[idx], vs->regs[idx], d_slot);
  vs->slot_type[idx] = SLOT_NUM;
}

static void vstack_flush_to_boxed(jit_vstack_t *vs,
                                  MIR_context_t ctx, MIR_item_t fn,
                                  MIR_reg_t d_slot) {
  if (!vs->slot_type) return;
  for (int i = 0; i < vs->sp; i++)
    vstack_ensure_boxed(vs, i, ctx, fn, d_slot);
}

static void mir_emit_bailout_check(MIR_context_t ctx, MIR_item_t fn,
                                   MIR_reg_t res,
                                   MIR_reg_t restore_val,
                                   MIR_reg_t r_bailout_off, int bc_off,
                                   MIR_reg_t r_bailout_sp,  int pre_op_sp,
                                   MIR_label_t bailout_tramp,
                                   MIR_reg_t r_args_buf,
                                   jit_vstack_t *vs,
                                   MIR_reg_t *local_regs, int n_locals,
                                   MIR_reg_t r_lbuf,
                                   MIR_reg_t r_d_slot) {
  MIR_label_t no_bail = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, no_bail),
      MIR_new_reg_op(ctx, res),
      MIR_new_uint_op(ctx, (uint64_t)SV_JIT_BAILOUT)));
  if (restore_val)
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, res),
        MIR_new_reg_op(ctx, restore_val)));
  for (int i = 0; i < pre_op_sp; i++) {
    if (vs->slot_type && vs->slot_type[i] == SLOT_NUM)
      mir_d_to_i64(ctx, fn, vs->regs[i], vs->d_regs[i], r_d_slot);
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
        MIR_new_reg_op(ctx, vs->regs[i])));
  }
  for (int i = 0; i < n_locals; i++)
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
        MIR_new_reg_op(ctx, local_regs[i])));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_bailout_off),
      MIR_new_int_op(ctx, bc_off)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_bailout_sp),
      MIR_new_int_op(ctx, pre_op_sp)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, bailout_tramp)));
  MIR_append_insn(ctx, fn, no_bail);
}


static void mir_load_imm(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t dst, uint64_t imm) {
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_uint_op(ctx, imm)));
}


static inline bool jit_const_is_heap(ant_value_t cv) {
  uint8_t t = vtype(cv);
  return ((1u << t) & GC_HEAP_TYPE_MASK) != 0;
}


static void mir_load_const_slot(MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t dst, ant_value_t *slot) {
  mir_load_imm(ctx, fn, dst, (uint64_t)(uintptr_t)slot);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, dst, 0, 1)));
}


static void mir_call_helper2(MIR_context_t ctx, MIR_item_t fn,
                             MIR_reg_t dst,
                             MIR_item_t proto, MIR_item_t func_item,
                             MIR_reg_t vm_reg, MIR_reg_t js_reg,
                             MIR_reg_t arg0,  MIR_reg_t arg1) {
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

static void mir_emit_is_num_guard(MIR_context_t ctx, MIR_item_t fn,
                                   MIR_reg_t r_bool, MIR_reg_t v,
                                   MIR_label_t slow) {
  (void)r_bool;
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGT,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_PREFIX)));
}

#define NANBOX_TFUNC_TAG  ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_FUNC)

static void mir_emit_get_closure(MIR_context_t ctx, MIR_item_t fn,
                                 MIR_reg_t dst, MIR_reg_t v,
                                 MIR_reg_t r_tag, MIR_label_t fallback) {
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, fallback),
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, dst),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
}


#define MAX_OSR_ENTRIES 64

typedef struct {
  int offsets[MAX_OSR_ENTRIES];
  int count;
} osr_entry_map_t;

static void scan_osr_entries(sv_func_t *func, osr_entry_map_t *osr) {
  osr->count = 0;
  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    int src = (int)(ip - func->code);
    int target = -1;
    switch (op) {
      case OP_JMP:
      case OP_JMP_FALSE:
      case OP_JMP_TRUE:
      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_NOT_NULLISH:
        target = src + sz + sv_get_i32(ip + 1);
        break;
      case OP_JMP8:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE8:
        target = src + sz + (int8_t)sv_get_i8(ip + 1);
        break;
      default: break;
    }
    if (target >= 0 && target < src) {
      bool found = false;
      for (int i = 0; i < osr->count; i++)
        if (osr->offsets[i] == target) { found = true; break; }
      if (!found && osr->count < MAX_OSR_ENTRIES)
        osr->offsets[osr->count++] = target;
    }
    ip += sz;
  }
}

static bool *scan_captured_locals(sv_func_t *func, int n_locals) {
  if (n_locals <= 0) return NULL;
  bool *captured = calloc((size_t)n_locals, sizeof(bool));
  if (!captured) return NULL;
  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if (op == OP_CLOSURE) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx < (uint32_t)func->const_count) {
        sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[idx]);
        for (int i = 0; i < child->upvalue_count; i++) {
          sv_upval_desc_t *desc = &child->upval_descs[i];
          if (desc->is_local) {
            int li = (int)desc->index - func->param_count;
            if (li >= 0 && li < n_locals)
              captured[li] = true;
          }
        }
      }
    }
    ip += sz;
  }
  return captured;
}


#define JIT_INLINE_MAX_BYTECODE 64

static bool jit_inlineable(sv_func_t *f) {
  if (!f) return false;
  if (f->upvalue_count != 0) return false;
  if (f->is_async || f->is_generator) return false;
  if (f->code_len > JIT_INLINE_MAX_BYTECODE) return false;

  uint8_t *ip  = f->code;
  uint8_t *end = f->code + f->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    switch (op) {
      case OP_GET_ARG:
      case OP_CONST_I8: case OP_CONST: case OP_CONST8:
      case OP_UNDEF: case OP_NULL: case OP_TRUE: case OP_FALSE:
      case OP_GET_LOCAL: case OP_GET_LOCAL8:
      case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
      case OP_SET_LOCAL: case OP_SET_LOCAL8:
      case OP_POP: case OP_DUP:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_MOD:
      case OP_LT:  case OP_LE:
      case OP_RETURN: case OP_RETURN_UNDEF:
      case OP_NOP: case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;
      default:
        return false;
    }
    ip += sz;
  }
  return true;
}

static bool jit_emit_inline_body(
  MIR_context_t ctx, MIR_item_t jit_func,
  sv_func_t *callee,
  MIR_reg_t *arg_regs, int caller_argc,
  MIR_reg_t result, MIR_label_t slow, MIR_label_t join,
  MIR_reg_t r_bool, MIR_reg_t *p_d_slot, int id
) {
  int inl_max_stack = callee->max_stack > 0 ? callee->max_stack : 4;
  MIR_reg_t inl_vs[inl_max_stack];
  for (int i = 0; i < inl_max_stack; i++) {
    char rn[32]; snprintf(rn, sizeof(rn), "inl%d_s%d", id, i);
    inl_vs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
  }
  int isp = 0;

  int inl_n_locals = callee->max_locals;
  MIR_reg_t inl_locals[inl_n_locals > 0 ? inl_n_locals : 1];
  for (int i = 0; i < inl_n_locals; i++) {
    char rn[32]; snprintf(rn, sizeof(rn), "inl%d_l%d", id, i);
    inl_locals[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
    mir_load_imm(ctx, jit_func, inl_locals[i], mkval(T_UNDEF, 0));
  }

  int inl_arith = 0;

  uint8_t *ip  = callee->code;
  uint8_t *end = callee->code + callee->code_len;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];

    switch (op) {
      case OP_GET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        MIR_reg_t dst = inl_vs[isp++];
        if ((int)idx < caller_argc)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, arg_regs[idx])));
        else
          mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
        break;
      }

      case OP_CONST_I8: {
        double d = (double)(int8_t)sv_get_i8(ip + 1);
        union { double d; uint64_t u; } u = {d};
        mir_load_imm(ctx, jit_func, inl_vs[isp++], u.u);
        break;
      }
      case OP_CONST: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->const_count) return false;
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
        if (idx >= (uint8_t)callee->const_count) return false;
        ant_value_t cv = callee->constants[idx];
        MIR_reg_t dst = inl_vs[isp++];
        if (jit_const_is_heap(cv))
          mir_load_const_slot(ctx, jit_func, dst, &callee->constants[idx]);
        else
          mir_load_imm(ctx, jit_func, dst, cv);
        break;
      }
      case OP_UNDEF: mir_load_imm(ctx, jit_func, inl_vs[isp++], mkval(T_UNDEF, 0)); break;
      case OP_NULL:  mir_load_imm(ctx, jit_func, inl_vs[isp++], mkval(T_NULL, 0));  break;
      case OP_TRUE:  mir_load_imm(ctx, jit_func, inl_vs[isp++], js_true);  break;
      case OP_FALSE: mir_load_imm(ctx, jit_func, inl_vs[isp++], js_false); break;

      case OP_GET_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_vs[isp++]),
            MIR_new_reg_op(ctx, inl_locals[idx])));
        break;
      }
      case OP_GET_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_vs[isp++]),
            MIR_new_reg_op(ctx, inl_locals[idx])));
        break;
      }
      case OP_PUT_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_PUT_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_SET_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        break;
      }
      case OP_SET_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        break;
      }

      case OP_POP: isp--; break;
      case OP_DUP: {
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_vs[isp]),
            MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        isp++;
        break;
      }

      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];

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
          case OP_ADD_NUM: mir_op = MIR_DADD; break;
          case OP_SUB:
          case OP_SUB_NUM: mir_op = MIR_DSUB; break;
          case OP_MUL:
          case OP_MUL_NUM: mir_op = MIR_DMUL; break;
          default:         mir_op = MIR_DDIV; break;
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, mir_op,
            MIR_new_reg_op(ctx, fd3),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, fd2)));
        mir_d_to_i64(ctx, jit_func, rd, fd3, *p_d_slot);
        break;
      }

      case OP_LT: case OP_LE: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];

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

        char cmp_rn[32];
        snprintf(cmp_rn, sizeof(cmp_rn), "inl%d_%s_%d", id,
                 op == OP_LT ? "lt" : "le", cn);
        MIR_reg_t r_tmp = MIR_new_func_reg(ctx, jit_func->u.func,
                                            MIR_T_I64, cmp_rn);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, op == OP_LT ? MIR_DLT : MIR_DLE,
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

      case OP_RETURN: {
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
        mir_load_imm(ctx, jit_func, result, mkval(T_UNDEF, 0));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        break;

      case OP_NOP: case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;

      default:
        return false;
    }
    ip += sz;
  }
  return true;
}

static void scan_branch_targets(sv_func_t *func, jit_label_map_t *lm,
                                MIR_context_t ctx) {
  uint8_t *ip   = func->code;
  uint8_t *end  = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    switch (op) {
      case OP_JMP:
      case OP_JMP_FALSE:
      case OP_JMP_TRUE:
      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_NOT_NULLISH:
      case OP_TRY_PUSH:
      case OP_CATCH:
      case OP_FINALLY: {
        int off = (int)(ip - func->code) + sv_get_i32(ip + 1) + sz;
        label_for_offset(ctx, lm, off);
        break;
      }
      case OP_JMP8:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE8: {
        int off = (int)(ip - func->code) + (int8_t)sv_get_i8(ip + 1) + sz;
        label_for_offset(ctx, lm, off);
        break;
      }
      default: break;
    }
    ip += sz;
  }
}


typedef struct {
  bool needs_bailout;      
  bool needs_inc_local;    
  bool needs_args_buf;     
  bool needs_close_upval;  
  bool needs_args_norm;    
} jit_features_t;

static jit_features_t jit_prescan_features(sv_func_t *func) {
  jit_features_t f = {false, false, false, false, false};
  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    switch (op) {
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_NEG:
      case OP_LT:  case OP_LE:  case OP_GT:  case OP_GE:
      case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT:
      case OP_SHL:  case OP_SHR: case OP_USHR:
      case OP_TYPEOF:
      case OP_ADD_LOCAL:
        f.needs_bailout = true;
        break;
      case OP_INC_LOCAL: case OP_DEC_LOCAL:
        f.needs_inc_local = true; 
        break;
      case OP_CALL: case OP_CALL_METHOD:
      case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD:
      case OP_ARRAY: case OP_NEW:
        f.needs_args_buf = true;
        break;
      case OP_CLOSE_UPVAL: case OP_CLOSURE:
        f.needs_close_upval = true;
        break;
      case OP_GET_ARG:
      case OP_SET_ARG:
        f.needs_args_norm = true;
        break;
      default: break;
    }
    ip += sz;
  }
  if (f.needs_bailout) f.needs_args_buf = true;
  return f;
}

static bool jit_is_eligible(sv_func_t *func) {
  if (func->is_async || func->is_generator) return false;

  bool eligible = true;
  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    switch (op) {
      case OP_CONST_I8: case OP_CONST: case OP_CONST8:
      case OP_UNDEF: case OP_NULL: case OP_TRUE: case OP_FALSE:
      case OP_THIS:
      case OP_GET_ARG: case OP_SET_ARG:
      case OP_GET_LOCAL:  case OP_PUT_LOCAL:  case OP_SET_LOCAL:
      case OP_GET_LOCAL8: case OP_PUT_LOCAL8: case OP_SET_LOCAL8:
      case OP_SET_LOCAL_UNDEF:                  
      case OP_GET_UPVAL: case OP_PUT_UPVAL: case OP_SET_UPVAL:
      case OP_CLOSE_UPVAL:
      case OP_POP: case OP_DUP: case OP_DUP2:
      case OP_INSERT2: case OP_INSERT3:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_NEG: case OP_IS_UNDEF: case OP_IS_NULL:
      case OP_LT:  case OP_LE:  case OP_GT:  case OP_GE:
      case OP_NE:  case OP_SNE:
      case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT:
      case OP_SHL:  case OP_SHR: case OP_USHR:
      case OP_NOT: case OP_TYPEOF: case OP_VOID:
      case OP_DELETE:
      case OP_INSTANCEOF:
      case OP_NEW:
      case OP_JMP: case OP_JMP8:
      case OP_JMP_FALSE:  case OP_JMP_FALSE8:
      case OP_JMP_TRUE:   case OP_JMP_TRUE8:
      case OP_CALL: case OP_CALL_METHOD:
      case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD:
      case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF:
      case OP_PUT_GLOBAL:
      case OP_GET_FIELD: case OP_GET_FIELD2: case OP_PUT_FIELD:
      case OP_GET_ELEM: case OP_GET_ELEM2: case OP_PUT_ELEM:
      case OP_OBJECT: case OP_ARRAY: case OP_SET_PROTO:
      case OP_SWAP: case OP_ROT3L:
      case OP_IN: case OP_GET_LENGTH:
      case OP_DEFINE_FIELD: case OP_SEQ: case OP_EQ:
      case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL:
      case OP_TO_PROPKEY:
      case OP_RETURN: case OP_RETURN_UNDEF:
      case OP_CLOSURE: case OP_SET_NAME:
      case OP_TRY_PUSH: case OP_TRY_POP:
      case OP_THROW: case OP_THROW_ERROR:
      case OP_CATCH: case OP_NIP_CATCH:
      case OP_NOP: case OP_HALT:
      case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;
      case OP_SPECIAL_OBJ:
        if (sv_get_u8(ip + 1) != 1) {
          if (sv_jit_warn_unlikely)
            fprintf(stderr, "jit: ineligible op SPECIAL_OBJ(%d) in %s\n",
                    sv_get_u8(ip + 1),
                    func->name ? func->name : "<anonymous>");
          eligible = false;
        }
        break;
      default:
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op %s in %s\n",
                  (op < OP__COUNT && sv_op_name[op]) ? sv_op_name[op] : "???",
                  func->name ? func->name : "<anonymous>");
        eligible = false;
        break;
    }
    ip += sz;
  }
  return eligible;
}

sv_jit_func_t sv_jit_compile(ant_t *js, sv_func_t *func, sv_closure_t *hint_closure) {
  if (func->jit_compile_failed) return NULL;
  if (func->jit_code == NULL && func->jit_compiled_tfb_ver != 0 &&
      func->tfb_version == func->jit_compiled_tfb_ver) {
    func->jit_compile_failed = true;
    return NULL;
  }
  if (!jit_is_eligible(func)) { func->jit_compile_failed = true; return NULL; }

  sv_jit_ctx_t *jc = jit_ctx_get(js);
  if (!jc) { sv_jit_init(js); jc = jit_ctx_get(js); }
  if (!jc) return NULL;

  MIR_context_t ctx = jc->ctx;

  char fname[128];
  snprintf(fname, sizeof(fname), "jit_%s_%p",
           func->name ? func->name : "anon", (void *)func);

  MIR_module_t mod = MIR_new_module(ctx, fname);

  MIR_type_t ret_type = MIR_JSVAL;

  MIR_item_t self_proto = MIR_new_proto(ctx, "jit_proto",
    1, &ret_type,
    5,
    MIR_T_I64, "vm",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_P,    "closure");
  MIR_type_t h2_ret = MIR_JSVAL;
  MIR_item_t helper2_proto = MIR_new_proto(ctx, "helper2_proto",
    1, &h2_ret,
    4,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "l",
    MIR_JSVAL,  "r");

  MIR_type_t call_ret = MIR_JSVAL;
  MIR_item_t call_proto = MIR_new_proto(ctx, "call_proto",
    1, &call_ret,
    6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js_p",
    MIR_JSVAL,  "func",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc");

  MIR_type_t gg_ret = MIR_JSVAL;
  MIR_item_t gg_proto = MIR_new_proto(ctx, "gg_proto",
    1, &gg_ret, 3,
    MIR_T_I64, "js",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

  MIR_type_t gf_ret = MIR_JSVAL;
  MIR_item_t gf_proto = MIR_new_proto(ctx, "gf_proto",
    1, &gf_ret, 7,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t ge_ret = MIR_JSVAL;
  MIR_item_t ge_proto = MIR_new_proto(ctx, "ge_proto",
    1, &ge_ret, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "obj",
    MIR_JSVAL, "key",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t h1_ret = MIR_JSVAL;
  MIR_item_t helper1_proto = MIR_new_proto(ctx, "helper1_proto",
    1, &h1_ret, 3,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "v");

  MIR_type_t truthy_ret = MIR_T_I64;
  MIR_item_t truthy_proto = MIR_new_proto(ctx, "truthy_proto",
    1, &truthy_ret, 2,
    MIR_T_I64, "js",
    MIR_JSVAL,  "v");

  MIR_type_t br_ret = MIR_JSVAL;
  MIR_item_t resume_proto = MIR_new_proto(ctx, "resume_proto",
    1, &br_ret, 10,
    MIR_T_I64,  "vm",
    MIR_T_P,    "closure",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_P,    "vstack",
    MIR_T_I64,  "vstack_sp",
    MIR_T_P,    "locals",
    MIR_T_I64,  "n_locals",
    MIR_T_I64,  "bc_offset");

  MIR_type_t cl_ret = MIR_JSVAL;
  MIR_item_t closure_proto = MIR_new_proto(ctx, "closure_proto",
    1, &cl_ret, 9,
    MIR_T_I64,  "vm",
    MIR_T_I64,  "js",
    MIR_T_P,    "parent",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_I32,  "const_idx",
    MIR_T_P,    "locals",
    MIR_T_I32,  "n_locals");

  MIR_item_t close_upval_proto = MIR_new_proto(ctx, "close_upval_proto",
    0, NULL, 4,
    MIR_T_I64, "vm",
    MIR_T_I32, "slot_idx",
    MIR_T_P,   "locals",
    MIR_T_I32, "n_locals");

  MIR_item_t set_name_proto = MIR_new_proto(ctx, "sn_proto",
    0, NULL, 5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "fn",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

  MIR_type_t so_ret = MIR_T_I64;
  MIR_item_t stack_ovf_proto = MIR_new_proto(ctx, "so_proto",
    1, &so_ret, 1,
    MIR_T_I64, "js");

  MIR_type_t soe_ret = MIR_JSVAL;
  MIR_item_t stack_ovf_err_proto = MIR_new_proto(ctx, "soe_proto",
    1, &soe_ret, 2,
    MIR_T_I64, "vm",
    MIR_T_I64, "js");

  MIR_item_t define_field_proto = MIR_new_proto(ctx, "df_proto",
    0, NULL, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_JSVAL,  "val",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

  MIR_type_t pf_ret = MIR_JSVAL;
  MIR_item_t put_field_proto = MIR_new_proto(ctx, "pf_proto",
    1, &pf_ret, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_JSVAL,  "val",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

  MIR_type_t pe_ret = MIR_JSVAL;
  MIR_item_t put_elem_proto = MIR_new_proto(ctx, "pe_proto",
    1, &pe_ret, 5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_JSVAL,  "key",
    MIR_JSVAL,  "val");

  MIR_type_t pg_ret = MIR_JSVAL;
  MIR_item_t put_global_proto = MIR_new_proto(ctx, "pg_proto",
    1, &pg_ret, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "val",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_I32, "strict");

  MIR_type_t obj_ret = MIR_JSVAL;
  MIR_item_t object_proto = MIR_new_proto(ctx, "obj_proto",
    1, &obj_ret, 2,
    MIR_T_I64, "vm",
    MIR_T_I64, "js");

  MIR_type_t arr_ret = MIR_JSVAL;
  MIR_item_t array_proto = MIR_new_proto(ctx, "arr_proto",
    1, &arr_ret, 4,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "elements",
    MIR_T_I32, "count");

  MIR_type_t te_ret = MIR_JSVAL;
  MIR_item_t throw_error_proto = MIR_new_proto(ctx, "te_proto",
    1, &te_ret, 5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_I32, "err_type");

  MIR_type_t nw_ret = MIR_JSVAL;
  MIR_item_t new_proto = MIR_new_proto(ctx, "new_proto",
    1, &nw_ret, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "func",
    MIR_JSVAL,  "new_target",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc");

  MIR_item_t imp_add   = MIR_new_import(ctx, "jit_helper_add");
  MIR_item_t imp_sub   = MIR_new_import(ctx, "jit_helper_sub");
  MIR_item_t imp_mul   = MIR_new_import(ctx, "jit_helper_mul");
  MIR_item_t imp_div   = MIR_new_import(ctx, "jit_helper_div");
  MIR_item_t imp_mod   = MIR_new_import(ctx, "jit_helper_mod");
  MIR_item_t imp_lt    = MIR_new_import(ctx, "jit_helper_lt");
  MIR_item_t imp_le    = MIR_new_import(ctx, "jit_helper_le");
  MIR_item_t imp_gt    = MIR_new_import(ctx, "jit_helper_gt");
  MIR_item_t imp_ge    = MIR_new_import(ctx, "jit_helper_ge");
  MIR_item_t imp_call  = MIR_new_import(ctx, "jit_helper_call");
  MIR_item_t imp_tov   = MIR_new_import(ctx, "jit_helper_tov");
  MIR_item_t imp_gg         = MIR_new_import(ctx, "jit_helper_get_global");
  MIR_item_t imp_get_field  = MIR_new_import(ctx, "jit_helper_get_field");
  MIR_item_t imp_to_propkey = MIR_new_import(ctx, "jit_helper_to_propkey");
  MIR_item_t imp_resume     = MIR_new_import(ctx, "jit_helper_bailout_resume");
  MIR_item_t imp_close_upval = MIR_new_import(ctx, "jit_helper_close_upval");
  MIR_item_t imp_closure     = MIR_new_import(ctx, "jit_helper_closure");
  MIR_item_t imp_in          = MIR_new_import(ctx, "jit_helper_in");
  MIR_item_t imp_get_length  = MIR_new_import(ctx, "jit_helper_get_length");
  MIR_item_t imp_define_field = MIR_new_import(ctx, "jit_helper_define_field");
  MIR_item_t imp_seq         = MIR_new_import(ctx, "jit_helper_seq");
  MIR_item_t imp_eq          = MIR_new_import(ctx, "jit_helper_eq");
  MIR_item_t imp_ne          = MIR_new_import(ctx, "jit_helper_ne");
  MIR_item_t imp_sne         = MIR_new_import(ctx, "jit_helper_sne");
  MIR_item_t imp_put_field   = MIR_new_import(ctx, "jit_helper_put_field");
  MIR_item_t imp_get_elem    = MIR_new_import(ctx, "jit_helper_get_elem");
  MIR_item_t imp_put_elem    = MIR_new_import(ctx, "jit_helper_put_elem");
  MIR_item_t imp_put_global  = MIR_new_import(ctx, "jit_helper_put_global");
  MIR_item_t imp_object      = MIR_new_import(ctx, "jit_helper_object");
  MIR_item_t imp_array       = MIR_new_import(ctx, "jit_helper_array");
  MIR_item_t imp_catch_value = MIR_new_import(ctx, "jit_helper_catch_value");
  MIR_item_t imp_throw       = MIR_new_import(ctx, "jit_helper_throw");
  MIR_item_t imp_throw_error = MIR_new_import(ctx, "jit_helper_throw_error");
  MIR_item_t imp_set_proto   = MIR_new_import(ctx, "jit_helper_set_proto");
  MIR_item_t imp_get_elem2   = MIR_new_import(ctx, "jit_helper_get_elem2");
  MIR_item_t imp_band        = MIR_new_import(ctx, "jit_helper_band");
  MIR_item_t imp_bor         = MIR_new_import(ctx, "jit_helper_bor");
  MIR_item_t imp_bxor        = MIR_new_import(ctx, "jit_helper_bxor");
  MIR_item_t imp_bnot        = MIR_new_import(ctx, "jit_helper_bnot");
  MIR_item_t imp_shl         = MIR_new_import(ctx, "jit_helper_shl");
  MIR_item_t imp_shr         = MIR_new_import(ctx, "jit_helper_shr");
  MIR_item_t imp_ushr        = MIR_new_import(ctx, "jit_helper_ushr");
  MIR_item_t imp_not         = MIR_new_import(ctx, "jit_helper_not");
  MIR_item_t imp_is_truthy   = MIR_new_import(ctx, "jit_helper_is_truthy");
  MIR_item_t imp_typeof      = MIR_new_import(ctx, "jit_helper_typeof");
  MIR_item_t imp_new         = MIR_new_import(ctx, "jit_helper_new");
  MIR_item_t imp_instanceof  = MIR_new_import(ctx, "jit_helper_instanceof");
  MIR_item_t imp_delete      = MIR_new_import(ctx, "jit_helper_delete");
  MIR_item_t imp_set_name   = MIR_new_import(ctx, "jit_helper_set_name");
  MIR_item_t imp_stack_ovf      = MIR_new_import(ctx, "jit_helper_stack_overflow");
  MIR_item_t imp_stack_ovf_err  = MIR_new_import(ctx, "jit_helper_stack_overflow_error");
  (void)imp_tov;

  MIR_item_t jit_func = MIR_new_func(ctx, fname,
    1, &ret_type,
    5,
    MIR_T_I64, "vm",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_P,    "closure");

  MIR_reg_t r_vm       = MIR_reg(ctx, "vm",       jit_func->u.func);
  MIR_reg_t r_this     = MIR_reg(ctx, "this_val", jit_func->u.func);
  MIR_reg_t r_args     = MIR_reg(ctx, "args",     jit_func->u.func);
  MIR_reg_t r_argc     = MIR_reg(ctx, "argc",     jit_func->u.func);
  MIR_reg_t r_closure  = MIR_reg(ctx, "closure",  jit_func->u.func);

  MIR_reg_t r_js = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "js_ptr");
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_js),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, r_vm, 0, 1)));

  {
    MIR_reg_t r_ovf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "stk_ovf");
    MIR_label_t no_overflow = MIR_new_label(ctx);
    MIR_append_insn(ctx, jit_func,
      MIR_new_call_insn(ctx, 4,
        MIR_new_ref_op(ctx, stack_ovf_proto),
        MIR_new_ref_op(ctx, imp_stack_ovf),
        MIR_new_reg_op(ctx, r_ovf),
        MIR_new_reg_op(ctx, r_js)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, no_overflow),
        MIR_new_reg_op(ctx, r_ovf),
        MIR_new_int_op(ctx, 0)));
    MIR_reg_t r_ovf_err = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "stk_err");
    MIR_append_insn(ctx, jit_func,
      MIR_new_call_insn(ctx, 5,
        MIR_new_ref_op(ctx, stack_ovf_err_proto),
        MIR_new_ref_op(ctx, imp_stack_ovf_err),
        MIR_new_reg_op(ctx, r_ovf_err),
        MIR_new_reg_op(ctx, r_vm),
        MIR_new_reg_op(ctx, r_js)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_ovf_err)));
    MIR_append_insn(ctx, jit_func, no_overflow);
  }

  jit_vstack_t vs = {0};
  vs.max = func->max_stack > 0 ? func->max_stack : 32;
  vs.regs = calloc((size_t)vs.max, sizeof(MIR_reg_t));
  vs.known_func = calloc((size_t)vs.max, sizeof(sv_func_t *));
  vs.d_regs = calloc((size_t)vs.max, sizeof(MIR_reg_t));
  vs.slot_type = calloc((size_t)vs.max, sizeof(uint8_t));
  if (!vs.regs || !vs.known_func || !vs.d_regs || !vs.slot_type) {
    free(vs.regs); free(vs.known_func); free(vs.d_regs); free(vs.slot_type);
    MIR_finish_func(ctx); MIR_finish_module(ctx); return NULL;
  }

  for (int i = 0; i < vs.max; i++) {
    char rname[32];
    snprintf(rname, sizeof(rname), "s%d", i);
    vs.regs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rname);
  }
  for (int i = 0; i < vs.max; i++) {
    char dname[32];
    snprintf(dname, sizeof(dname), "sd%d", i);
    vs.d_regs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dname);
  }

  int n_locals = func->max_locals;
  MIR_reg_t *local_regs = NULL;
  sv_func_t **known_func_locals = NULL;
  uint8_t *known_type_locals = NULL;
  if (n_locals > 0) {
    local_regs = calloc((size_t)n_locals, sizeof(MIR_reg_t));
    known_func_locals = calloc((size_t)n_locals, sizeof(sv_func_t *));
    known_type_locals = calloc((size_t)n_locals, sizeof(uint8_t));
    if (!local_regs || !known_func_locals || !known_type_locals) {
      free(vs.regs); free(vs.known_func); free(vs.d_regs); free(vs.slot_type);
      free(local_regs); free(known_func_locals); free(known_type_locals);
      MIR_finish_func(ctx); MIR_finish_module(ctx); return NULL;
    }
    if (func->local_types && func->local_type_count > 0) {
      int ncopy = func->local_type_count < n_locals ? func->local_type_count : n_locals;
      for (int i = 0; i < ncopy; i++)
        known_type_locals[i] = func->local_types[i].type;
    }
    for (int i = 0; i < n_locals; i++) {
      char rname[32];
      snprintf(rname, sizeof(rname), "l%d", i);
      local_regs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rname);
      mir_load_imm(ctx, jit_func, local_regs[i],
                   mkval(T_UNDEF, 0));
    }
  }

  MIR_reg_t r_tmp  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "tmp");
  MIR_reg_t r_tmp2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "tmp2");
  MIR_reg_t r_bool = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bool_tmp");
  MIR_reg_t r_err_tmp = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "err_tmp");
  (void)r_tmp2;

  jit_features_t feat = jit_prescan_features(func);


  MIR_reg_t r_d_slot = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "d_slot");
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_ALLOCA,
      MIR_new_reg_op(ctx, r_d_slot),
      MIR_new_uint_op(ctx, 8)));

  MIR_reg_t r_d_one = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, "d_one");
  if (feat.needs_inc_local) {
    union { double d; uint64_t u; } one = {1.0};
    mir_load_imm(ctx, jit_func, r_bool, one.u);
    mir_i64_to_d(ctx, jit_func, r_d_one, r_bool, r_d_slot);
  }

  MIR_reg_t r_args_buf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "args_buf");
  if (feat.needs_args_buf) {
    int scratch_slots = 16;
    if (vs.max > scratch_slots)  scratch_slots = vs.max;
    if (n_locals > scratch_slots) scratch_slots = n_locals;
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_args_buf),
        MIR_new_uint_op(ctx, (uint64_t)scratch_slots * sizeof(ant_value_t))));
  } else {
    mir_load_imm(ctx, jit_func, r_args_buf, 0);
  }

  MIR_reg_t r_cond_d = 0, r_cond_nan = 0, r_cond_zd = 0, r_cond_zero = 0;

  bool needs_bailout = feat.needs_bailout;

  MIR_reg_t   r_bailout_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "bail_val");
  MIR_reg_t   r_bailout_off = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bail_off");
  MIR_reg_t   r_bailout_sp  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bail_sp");
  MIR_label_t bailout_tramp = needs_bailout ? MIR_new_label(ctx) : NULL;

  bool needs_lbuf = needs_bailout || feat.needs_close_upval;
  MIR_reg_t r_lbuf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "lbuf");
  if (needs_lbuf && n_locals > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_lbuf),
        MIR_new_uint_op(ctx, (uint64_t)n_locals * sizeof(ant_value_t))));
  } else {
    mir_load_imm(ctx, jit_func, r_lbuf, 0);
  }

  bool *captured_locals = scan_captured_locals(func, n_locals);
  bool has_captures = false;
  if (captured_locals) {
    for (int i = 0; i < n_locals; i++)
      if (captured_locals[i]) { has_captures = true; break; }
  }
  if (has_captures) {
    for (int i = 0; i < n_locals; i++) {
      if (captured_locals[i])
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_I64,
              (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
    }
  }

  int param_count = func->param_count;
  if (param_count > 0 && feat.needs_args_norm) {
    MIR_label_t args_ok = MIR_new_label(ctx);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_UBGE,
        MIR_new_label_op(ctx, args_ok),
        MIR_new_reg_op(ctx, r_argc),
        MIR_new_int_op(ctx, (int64_t)param_count)));
    MIR_reg_t r_arg_local = MIR_new_func_reg(ctx, jit_func->u.func,
                                              MIR_T_I64, "arg_local");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_arg_local),
        MIR_new_uint_op(ctx, (uint64_t)param_count * sizeof(ant_value_t))));
    for (int i = 0; i < param_count; i++)
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_mem_op(ctx, MIR_T_I64,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_arg_local, 0, 1),
          MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
    for (int i = 0; i < param_count; i++) {
      MIR_label_t copy_skip = MIR_new_label(ctx);
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_UBLE,
          MIR_new_label_op(ctx, copy_skip),
          MIR_new_reg_op(ctx, r_argc),
          MIR_new_int_op(ctx, (int64_t)i)));
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_mem_op(ctx, MIR_T_I64,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_arg_local, 0, 1),
          MIR_new_mem_op(ctx, MIR_T_I64,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args, 0, 1)));
      MIR_append_insn(ctx, jit_func, copy_skip);
    }
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_args),
        MIR_new_reg_op(ctx, r_arg_local)));
    MIR_append_insn(ctx, jit_func, args_ok);
  }

  jit_label_map_t lm = {0};
  scan_branch_targets(func, &lm, ctx);

  osr_entry_map_t osr_map = {0};
  scan_osr_entries(func, &osr_map);
  if (osr_map.count > 0) {
    MIR_label_t normal_entry = MIR_new_label(ctx);

    MIR_disp_t osr_base = (MIR_disp_t)offsetof(struct sv_vm, jit_osr);

    MIR_reg_t r_osr_active = MIR_new_func_reg(ctx, jit_func->u.func,
                                               MIR_T_I64, "osr_active");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_osr_active),
        MIR_new_mem_op(ctx, MIR_T_U8,
          osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
          r_vm, 0, 1)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, normal_entry),
        MIR_new_reg_op(ctx, r_osr_active),
        MIR_new_int_op(ctx, 0)));

    MIR_reg_t r_osr_locals = MIR_new_func_reg(ctx, jit_func->u.func,
                                               MIR_T_I64, "osr_locals");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_osr_locals),
        MIR_new_mem_op(ctx, MIR_T_P,
          osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, locals),
          r_vm, 0, 1)));

    for (int i = 0; i < n_locals; i++)
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_reg_op(ctx, local_regs[i]),
          MIR_new_mem_op(ctx, MIR_JSVAL,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
            r_osr_locals, 0, 1)));

    if (has_captures) {
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_reg_op(ctx, r_lbuf),
          MIR_new_mem_op(ctx, MIR_T_P,
            osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, lp),
            r_vm, 0, 1)));
    }

    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_U8,
          osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
          r_vm, 0, 1),
        MIR_new_int_op(ctx, 0)));

    MIR_reg_t r_osr_off = MIR_new_func_reg(ctx, jit_func->u.func,
                                            MIR_T_I64, "osr_off");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_osr_off),
        MIR_new_mem_op(ctx, MIR_T_I32,
          osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, bc_offset),
          r_vm, 0, 1)));

    for (int i = 0; i < osr_map.count; i++) {
      MIR_label_t target_lbl = label_for_offset(ctx, &lm, osr_map.offsets[i]);
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_BEQ,
          MIR_new_label_op(ctx, target_lbl),
          MIR_new_reg_op(ctx, r_osr_off),
          MIR_new_int_op(ctx, osr_map.offsets[i])));
    }

    MIR_append_insn(ctx, jit_func, normal_entry);
  }

#define JIT_TRY_MAX 16
  typedef struct {
    MIR_label_t catch_label;
    int catch_bc_off;
    int saved_sp;
  } jit_try_entry_t;

  jit_try_entry_t jit_try_stack[JIT_TRY_MAX];
  int jit_try_depth = 0;

  typedef struct { int bc_off; int saved_sp; } jit_catch_sp_t;
  jit_catch_sp_t catch_sp_map[JIT_TRY_MAX];
  int catch_sp_count = 0;

  MIR_label_t self_tail_entry = MIR_new_label(ctx);
  MIR_append_insn(ctx, jit_func, self_tail_entry);


  MIR_reg_t r_result = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "result");
  mir_load_imm(ctx, jit_func, r_result, mkval(T_UNDEF, 0));

  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;

  bool ok = true; 
  int  call_n  = 0; 
  int  upval_n = 0; 
  int  arith_n = 0; 

  while (ip < end) {
    int bc_off = (int)(ip - func->code);
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) { ok = false; break; }

    for (int i = 0; i < lm.count; i++) {
      if (lm.entries[i].bc_off == bc_off) {
        MIR_append_insn(ctx, jit_func, lm.entries[i].label);
        if (lm.entries[i].sp >= 0)
          vs.sp = lm.entries[i].sp;
        if (vs.slot_type)
          memset(vs.slot_type, SLOT_BOXED, (size_t)vs.max);
      }
    }

    switch (op) {

      case OP_CONST_I8: {
        double d = (double)(int8_t)sv_get_i8(ip + 1);
        union { double d; uint64_t u; } u = {d};
        MIR_reg_t dst = vstack_push(&vs);
        mir_load_imm(ctx, jit_func, dst, u.u);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DMOV,
            MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
            MIR_new_double_op(ctx, d)));
        vs.slot_type[vs.sp - 1] = SLOT_NUM;
        break;
      }

      case OP_CONST: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->const_count) { ok = false; break; }
        ant_value_t cv = func->constants[idx];
        MIR_reg_t dst = vstack_push(&vs);
        if (jit_const_is_heap(cv))
          mir_load_const_slot(ctx, jit_func, dst, &func->constants[idx]);
        else {
          mir_load_imm(ctx, jit_func, dst, cv);
          if (vtype(cv) == T_NUM) {
            union { uint64_t u; double d; } u = {cv};
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
                MIR_new_double_op(ctx, u.d)));
            vs.slot_type[vs.sp - 1] = SLOT_NUM;
          }
        }
        break;
      }

      case OP_CONST8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)func->const_count) { ok = false; break; }
        ant_value_t cv = func->constants[idx];
        MIR_reg_t dst = vstack_push(&vs);
        if (jit_const_is_heap(cv))
          mir_load_const_slot(ctx, jit_func, dst, &func->constants[idx]);
        else {
          mir_load_imm(ctx, jit_func, dst, cv);
          if (vtype(cv) == T_NUM) {
            union { uint64_t u; double d; } u = {cv};
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
                MIR_new_double_op(ctx, u.d)));
            vs.slot_type[vs.sp - 1] = SLOT_NUM;
          }
        }
        break;
      }

      case OP_UNDEF:
        mir_load_imm(ctx, jit_func, vstack_push(&vs), mkval(T_UNDEF, 0));
        break;
      case OP_NULL:
        mir_load_imm(ctx, jit_func, vstack_push(&vs), mkval(T_NULL, 0));
        break;
      case OP_TRUE:
        mir_load_imm(ctx, jit_func, vstack_push(&vs), js_true);
        break;
      case OP_FALSE:
        mir_load_imm(ctx, jit_func, vstack_push(&vs), js_false);
        break;

      case OP_THIS: {
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_this)));
        break;
      }

      case OP_GET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_mem_op(ctx, MIR_JSVAL,
              (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
              r_args, 0, 1)));
        break;
      }

      case OP_SET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        MIR_reg_t val = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_JSVAL,
              (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
              r_args, 0, 1),
            MIR_new_reg_op(ctx, val)));
        break;
      }

      case OP_GET_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)n_locals) { ok = false; break; }
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, local_regs[idx]),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        MIR_reg_t dst = vstack_push(&vs);
        if (known_func_locals) vs.known_func[vs.sp - 1] = known_func_locals[idx];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, local_regs[idx])));
        if (known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_i64_to_d(ctx, jit_func, vs.d_regs[vs.sp - 1], dst, r_d_slot);
          if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_NUM;
        }
        break;
      }
      case OP_GET_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, local_regs[idx]),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        MIR_reg_t dst = vstack_push(&vs);
        if (known_func_locals) vs.known_func[vs.sp - 1] = known_func_locals[idx];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, local_regs[idx])));
        if (known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_i64_to_d(ctx, jit_func, vs.d_regs[vs.sp - 1], dst, r_d_slot);
          if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_NUM;
        }
        break;
      }

      case OP_PUT_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)n_locals) { ok = false; break; }
        sv_func_t *kf = vs.known_func[vs.sp - 1];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, src)));
        break;
      }
      case OP_PUT_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        sv_func_t *kf = vs.known_func[vs.sp - 1];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, src)));
        break;
      }

      case OP_SET_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)n_locals) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, src)));
        break;
      }
      case OP_SET_LOCAL8: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, src)));
        break;
      }

      case OP_SET_LOCAL_UNDEF: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)n_locals) { ok = false; break; }
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        break;
      }

      case OP_POP:
        vstack_pop(&vs);
        break;

      case OP_DUP: {
        sv_func_t *kf = vs.known_func[vs.sp - 1];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t top = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        vs.known_func[vs.sp - 1] = kf;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, top)));
        break;
      }

      case OP_DUP2: {
        if (vs.sp < 2) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t ra = vs.regs[vs.sp - 2];
        MIR_reg_t rb = vs.regs[vs.sp - 1];
        MIR_reg_t da = vstack_push(&vs);
        MIR_reg_t db = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, da),
            MIR_new_reg_op(ctx, ra)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, db),
            MIR_new_reg_op(ctx, rb)));
        break;
      }

      case OP_INSERT2: {
        if (vs.sp < 2) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t r_a   = vs.regs[vs.sp - 1];
        MIR_reg_t r_obj = vs.regs[vs.sp - 2];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_tmp),
            MIR_new_reg_op(ctx, r_a)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[vs.sp - 1]),
            MIR_new_reg_op(ctx, r_obj)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[vs.sp - 2]),
            MIR_new_reg_op(ctx, r_tmp)));
        MIR_reg_t dup = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dup),
            MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_INSERT3: {
        if (vs.sp < 3) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        MIR_reg_t r_a    = vs.regs[vs.sp - 1];
        MIR_reg_t r_prop = vs.regs[vs.sp - 2];
        MIR_reg_t r_obj  = vs.regs[vs.sp - 3];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_tmp),
            MIR_new_reg_op(ctx, r_a)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[vs.sp - 1]),
            MIR_new_reg_op(ctx, r_prop)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[vs.sp - 2]),
            MIR_new_reg_op(ctx, r_obj)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[vs.sp - 3]),
            MIR_new_reg_op(ctx, r_tmp)));
        MIR_reg_t dup = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dup),
            MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_ADD:
      case OP_ADD_NUM: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool force_num_only = (op == OP_ADD_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];     
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1]; 
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DADD,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DADD,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int an = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "add_d1_%d", an);
          snprintf(d2, sizeof(d2), "add_d2_%d", an);
          snprintf(d3, sizeof(d3), "add_d3_%d", an);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DADD,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int an = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "add_d1_%d", an);
          snprintf(d2, sizeof(d2), "add_d2_%d", an);
          snprintf(d3, sizeof(d3), "add_d3_%d", an);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DADD,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_add,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_SUB:
      case OP_SUB_NUM: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool force_num_only = (op == OP_SUB_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_sub,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DSUB,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DSUB,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int sn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "sub_d1_%d", sn);
          snprintf(d2, sizeof(d2), "sub_d2_%d", sn);
          snprintf(d3, sizeof(d3), "sub_d3_%d", sn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DSUB,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int sn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "sub_d1_%d", sn);
          snprintf(d2, sizeof(d2), "sub_d2_%d", sn);
          snprintf(d3, sizeof(d3), "sub_d3_%d", sn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DSUB,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_sub, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_MUL:
      case OP_MUL_NUM: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool force_num_only = (op == OP_MUL_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_mul,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMUL,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMUL,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int mn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "mul_d1_%d", mn);
          snprintf(d2, sizeof(d2), "mul_d2_%d", mn);
          snprintf(d3, sizeof(d3), "mul_d3_%d", mn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMUL,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int mn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "mul_d1_%d", mn);
          snprintf(d2, sizeof(d2), "mul_d2_%d", mn);
          snprintf(d3, sizeof(d3), "mul_d3_%d", mn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMUL,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_mul, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_DIV:
      case OP_DIV_NUM: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool force_num_only = (op == OP_DIV_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_div,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DDIV,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_r   = vs.d_regs[vs.sp];
          MIR_reg_t fd_dst = vs.d_regs[vs.sp - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DDIV,
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_dst),
              MIR_new_reg_op(ctx, fd_r)));
          vs.slot_type[vs.sp - 1] = SLOT_NUM;
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int dn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "dv_d1_%d", dn);
          snprintf(d2, sizeof(d2), "dv_d2_%d", dn);
          snprintf(d3, sizeof(d3), "dv_d3_%d", dn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DDIV,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int dn = arith_n++;
          char d1[32], d2[32], d3[32];
          snprintf(d1, sizeof(d1), "dv_d1_%d", dn);
          snprintf(d2, sizeof(d2), "dv_d2_%d", dn);
          snprintf(d3, sizeof(d3), "dv_d3_%d", dn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d3);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DDIV,
              MIR_new_reg_op(ctx, fd3),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          mir_d_to_i64(ctx, jit_func, rd, fd3, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_div, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_MOD: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_val),
            MIR_new_reg_op(ctx, rl)));
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_mod, r_vm, r_js, rl, rr);
        mir_emit_bailout_check(ctx, jit_func, rd,
          r_bailout_val, r_bailout_off, bc_off,
          r_bailout_sp, vs.sp + 1, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        break;
      }

      case OP_NEG: {
        MIR_reg_t rs = vstack_top(&vs);  
        MIR_label_t slow = MIR_new_label(ctx);
        MIR_label_t done = MIR_new_label(ctx);

        mir_emit_is_num_guard(ctx, jit_func, r_bool, rs, slow);

        int nn = arith_n++;
        char neg_d1[32], neg_d2[32];
        snprintf(neg_d1, sizeof(neg_d1), "neg_d1_%d", nn);
        snprintf(neg_d2, sizeof(neg_d2), "neg_d2_%d", nn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, neg_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, neg_d2);
        mir_i64_to_d(ctx, jit_func, fd1, rs, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DNEG,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1)));
        mir_d_to_i64(ctx, jit_func, rs, fd2, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

        MIR_append_insn(ctx, jit_func, slow);
        for (int i = 0; i < vs.sp; i++) {
          if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
            mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, vs.regs[i])));
        }
        for (int i = 0; i < n_locals; i++)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[i])));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_off),
            MIR_new_int_op(ctx, bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_sp),
            MIR_new_int_op(ctx, vs.sp)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP,
            MIR_new_label_op(ctx, bailout_tramp)));

        MIR_append_insn(ctx, jit_func, done);
        break;
      }

      case OP_IS_UNDEF:
      case OP_IS_NULL: {
        MIR_reg_t rs = vstack_top(&vs);
        uint64_t cmp_val = (op == OP_IS_UNDEF) ? mkval(T_UNDEF, 0) : mkval(T_NULL, 0);
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

      case OP_LT: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_lt,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int ltn = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "lt_d1_%d", ltn);
          snprintf(d2, sizeof(d2), "lt_d2_%d", ltn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int ltn = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "lt_d1_%d", ltn);
          snprintf(d2, sizeof(d2), "lt_d2_%d", ltn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_lt, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_LE: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_le,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else if (fb_num_only) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, bail_direct);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
          int len = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "le_d1_%d", len);
          snprintf(d2, sizeof(d2), "le_d2_%d", len);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int len = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "le_d1_%d", len);
          snprintf(d2, sizeof(d2), "le_d2_%d", len);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DLE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_le, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_JMP:
      case OP_JMP8: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        bool short_op = (op == OP_JMP8);
        int target = bc_off + sz + (short_op ? (int8_t)sv_get_i8(ip + 1)
                                              : sv_get_i32(ip + 1));
        MIR_label_t lbl = label_for_branch(ctx, &lm, target, vs.sp);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl)));
        break;
      }

      case OP_JMP_FALSE:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE:
      case OP_JMP_TRUE8: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        MIR_reg_t cond = vstack_pop(&vs);
        bool short_op = (op == OP_JMP_FALSE8 || op == OP_JMP_TRUE8);
        bool is_false_branch = (op == OP_JMP_FALSE || op == OP_JMP_FALSE8);
        int target = bc_off + sz + (short_op ? (int8_t)sv_get_i8(ip + 1)
                                             : sv_get_i32(ip + 1));
        MIR_label_t lbl = label_for_branch(ctx, &lm, target, vs.sp);
        MIR_label_t lbl_not_bool = MIR_new_label(ctx);
        MIR_label_t lbl_not_num  = MIR_new_label(ctx);
        MIR_label_t lbl_done     = MIR_new_label(ctx);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, lbl_not_bool),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, js_false >> NANBOX_TYPE_SHIFT)));
        uint64_t cmp_bool = is_false_branch ? js_false : js_true;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, cmp_bool)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));

        MIR_append_insn(ctx, jit_func, lbl_not_bool);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_UBGT,
            MIR_new_label_op(ctx, lbl_not_num),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, NANBOX_PREFIX)));
        if (!r_d_slot) {
          r_d_slot = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "d_slot_cond");
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_ALLOCA,
              MIR_new_reg_op(ctx, r_d_slot),
              MIR_new_uint_op(ctx, 8)));
        }
        if (!r_cond_d) {
          r_cond_d    = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D,   "cond_d");
          r_cond_nan  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "cond_nan");
          r_cond_zd   = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D,   "zero_d");
          r_cond_zero = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "cond_zero");
        }
        mir_i64_to_d(ctx, jit_func, r_cond_d, cond, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DNE,    
            MIR_new_reg_op(ctx, r_cond_nan),
            MIR_new_reg_op(ctx, r_cond_d),
            MIR_new_reg_op(ctx, r_cond_d)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DMOV,
            MIR_new_reg_op(ctx, r_cond_zd),
            MIR_new_double_op(ctx, 0.0)));
        MIR_reg_t r_is_zero = r_cond_zero;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DEQ,    
            MIR_new_reg_op(ctx, r_is_zero),
            MIR_new_reg_op(ctx, r_cond_d),
            MIR_new_reg_op(ctx, r_cond_zd)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_OR,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_is_zero),
            MIR_new_reg_op(ctx, r_cond_nan)));
        if (is_false_branch) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,   
              MIR_new_label_op(ctx, lbl),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, 0)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,   
              MIR_new_label_op(ctx, lbl),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, 0)));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));

        MIR_append_insn(ctx, jit_func, lbl_not_num);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 5,
            MIR_new_ref_op(ctx, truthy_proto),
            MIR_new_ref_op(ctx, imp_is_truthy),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, cond)));
        if (is_false_branch) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, lbl),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, 0)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, lbl),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, 0)));
        }
        MIR_append_insn(ctx, jit_func, lbl_done);
        break;
      }

      case OP_TAIL_CALL:
      case OP_CALL: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        bool is_tail = (op == OP_TAIL_CALL);
        uint16_t call_argc = sv_get_u16(ip + 1);
        if (call_argc > 16 || vs.sp < (int)call_argc + 1) { ok = false; break; }

        if (!is_tail) {
          sv_func_t *inline_callee = vs.known_func[vs.sp - call_argc - 1];
          if (inline_callee && jit_inlineable(inline_callee)) {
            int cn = call_n++;

            MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
            for (int i = (int)call_argc - 1; i >= 0; i--)
              inl_arg_regs[i] = vstack_pop(&vs);
            MIR_reg_t r_inl_callee = vstack_pop(&vs); 

            MIR_reg_t r_call_res = vstack_push(&vs);

            MIR_label_t inl_slow = MIR_new_label(ctx);
            MIR_label_t inl_join = MIR_new_label(ctx);

            bool inlined = jit_emit_inline_body(
              ctx, jit_func, inline_callee,
              inl_arg_regs, (int)call_argc,
              r_call_res, inl_slow, inl_join,
              r_bool, &r_d_slot, cn);

            if (inlined) {
              MIR_append_insn(ctx, jit_func, inl_slow);
              for (int i = 0; i < (int)call_argc; i++)
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_mem_op(ctx, MIR_JSVAL,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                      r_args_buf, 0, 1),
                    MIR_new_reg_op(ctx, inl_arg_regs[i])));

              char rn_sl_this[32];
              snprintf(rn_sl_this, sizeof(rn_sl_this), "inl%d_slow_t", cn);
              MIR_reg_t r_slow_this = MIR_new_func_reg(ctx, jit_func->u.func,
                                                        MIR_JSVAL, rn_sl_this);
              mir_load_imm(ctx, jit_func, r_slow_this, mkval(T_UNDEF, 0));

              MIR_append_insn(ctx, jit_func,
                MIR_new_call_insn(ctx, 9,
                  MIR_new_ref_op(ctx, call_proto),
                  MIR_new_ref_op(ctx, imp_call),
                  MIR_new_reg_op(ctx, r_call_res),
                  MIR_new_reg_op(ctx, r_vm),
                  MIR_new_reg_op(ctx, r_js),
                  MIR_new_reg_op(ctx, r_inl_callee),
                  MIR_new_reg_op(ctx, r_slow_this),
                  MIR_new_reg_op(ctx, r_args_buf),
                  MIR_new_int_op(ctx, (int64_t)call_argc)));

              MIR_append_insn(ctx, jit_func, inl_join);
              break;
            }

            vs.sp = vs.sp - 1 + call_argc + 1;
          }
        }

        int cn = call_n++;

        char rn_arr[32], rn_this[32], rn_ccl[32], rn_cfn[32], rn_jptr[32];
        snprintf(rn_arr,  sizeof(rn_arr),  "arg_arr%d",       cn);
        snprintf(rn_this, sizeof(rn_this), "call_this%d",     cn);
        snprintf(rn_ccl,  sizeof(rn_ccl),  "callee_cl%d",     cn);
        snprintf(rn_cfn,  sizeof(rn_cfn),  "callee_func%d",   cn);
        snprintf(rn_jptr, sizeof(rn_jptr), "jit_ptr%d",       cn);

        MIR_reg_t r_arg_arr = r_args_buf;

        for (int i = (int)call_argc - 1; i >= 0; i--) {
          MIR_reg_t areg = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_arg_arr, 0, 1),
              MIR_new_reg_op(ctx, areg)));
        }

        sv_func_t *call_known = vs.known_func
          ? vs.known_func[vs.sp - 1] : NULL;

        MIR_reg_t r_call_func = vstack_pop(&vs);
        MIR_reg_t r_call_this = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn_this);
        mir_load_imm(ctx, jit_func, r_call_this, mkval(T_UNDEF, 0));

        MIR_reg_t r_call_res = vstack_push(&vs);

        if (call_known == func) {
          if (is_tail && jit_try_depth == 0) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_args),
                MIR_new_reg_op(ctx, r_arg_arr)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_argc),
                MIR_new_int_op(ctx, (int64_t)call_argc)));
            for (int i = 0; i < n_locals; i++)
              mir_load_imm(ctx, jit_func, local_regs[i], mkval(T_UNDEF, 0));
            if (has_captures) {
              for (int i = 0; i < n_locals; i++)
                if (captured_locals[i])
                  MIR_append_insn(ctx, jit_func,
                    MIR_new_insn(ctx, MIR_MOV,
                      MIR_new_mem_op(ctx, MIR_T_I64,
                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                      MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
            }
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP,
                MIR_new_label_op(ctx, self_tail_entry)));
            break;
          }
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 8,
              MIR_new_ref_op(ctx, self_proto),
              MIR_new_ref_op(ctx, jit_func),
              MIR_new_reg_op(ctx, r_call_res),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_call_this),
              MIR_new_reg_op(ctx, r_arg_arr),
              MIR_new_int_op(ctx, (int64_t)call_argc),
              MIR_new_reg_op(ctx, r_closure)));
          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, local_regs[i]),
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          }
          if (jit_try_depth > 0) {
            jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_result),
                MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP,
                MIR_new_label_op(ctx, h->catch_label)));
            MIR_append_insn(ctx, jit_func, no_err);
          } else {
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          break;
        }

        MIR_label_t lbl_self_call   = MIR_new_label(ctx);
        MIR_label_t lbl_interp_call = MIR_new_label(ctx);
        MIR_label_t lbl_call_done   = MIR_new_label(ctx);

        MIR_reg_t r_callee_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_ccl);
        mir_emit_get_closure(ctx, jit_func, r_callee_cl, r_call_func,
                             r_bool, lbl_interp_call);

        MIR_reg_t r_callee_fn = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_cfn);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, func),
              r_callee_cl, 0, 1)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_interp_call),
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_self_call),
            MIR_new_reg_op(ctx, r_callee_cl),
            MIR_new_reg_op(ctx, r_closure)));

        MIR_reg_t r_jit_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_jptr);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_func_t, jit_code),
              r_callee_fn, 0, 1)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_interp_call),
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_reg_op(ctx, r_jit_ptr),   
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_callee_cl)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_call_done)));

        MIR_append_insn(ctx, jit_func, lbl_self_call);
        if (is_tail && jit_try_depth == 0) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_args),
              MIR_new_reg_op(ctx, r_arg_arr)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_int_op(ctx, (int64_t)call_argc)));
          for (int i = 0; i < n_locals; i++)
            mir_load_imm(ctx, jit_func, local_regs[i], mkval(T_UNDEF, 0));
          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                    MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
          }
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, self_tail_entry)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 8,
              MIR_new_ref_op(ctx, self_proto),
              MIR_new_ref_op(ctx, jit_func),    
              MIR_new_reg_op(ctx, r_call_res),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_call_this),
              MIR_new_reg_op(ctx, r_arg_arr),
              MIR_new_int_op(ctx, (int64_t)call_argc),
              MIR_new_reg_op(ctx, r_closure)));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_call_done)));

        MIR_append_insn(ctx, jit_func, lbl_interp_call);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, call_proto),
            MIR_new_ref_op(ctx, imp_call),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc)));

        MIR_append_insn(ctx, jit_func, lbl_call_done);
        if (is_tail && jit_try_depth == 0) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
        } else {
          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, local_regs[i]),
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          }
          if (jit_try_depth > 0) {
            jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
                MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP,
                MIR_new_label_op(ctx, h->catch_label)));
            MIR_append_insn(ctx, jit_func, no_err);
          } else {
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          if (is_tail) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
          }
        }
        break;
      }

      case OP_SPECIAL_OBJ:
        mir_load_imm(ctx, jit_func, vstack_push(&vs), mkval(T_UNDEF, 0));
        break;

      case OP_GET_UPVAL: {
        uint16_t idx = sv_get_u16(ip + 1);

        if (vs.known_func && hint_closure &&
            idx < (uint16_t)func->upvalue_count &&
            hint_closure->upvalues && hint_closure->upvalues[idx]) {
          ant_value_t uv_val = *hint_closure->upvalues[idx]->location;
          if (vtype(uv_val) == T_FUNC) {
            sv_closure_t *ucl = js_func_closure(uv_val);
            if (ucl && ucl->func == func) {
              MIR_reg_t dst = vstack_push(&vs);
              vs.known_func[vs.sp - 1] = func;
              uint64_t func_tag = NANBOX_PREFIX
                | ((ant_value_t)(T_FUNC & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);
              mir_load_imm(ctx, jit_func, r_tmp, func_tag);
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_OR,
                  MIR_new_reg_op(ctx, dst),
                  MIR_new_reg_op(ctx, r_tmp),
                  MIR_new_reg_op(ctx, r_closure)));
              break;
            }
          }
        }

        int un = upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d",  un);
        snprintf(rn_uv,  sizeof(rn_uv),  "upv%d",   un);
        snprintf(rn_loc, sizeof(rn_loc),  "uvloc%d", un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        MIR_reg_t dst   = vstack_push(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_uvs),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, upvalues),
              r_closure, 0, 1)));

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
        break;
      }

      case OP_PUT_UPVAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        int un = upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d",  un);
        snprintf(rn_uv,  sizeof(rn_uv),  "upv%d",   un);
        snprintf(rn_loc, sizeof(rn_loc),  "uvloc%d", un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src   = vstack_pop(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_uvs),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, upvalues),
              r_closure, 0, 1)));
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
            MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_loc, 0, 1),
            MIR_new_reg_op(ctx, src)));
        break;
      }

      case OP_SET_UPVAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        int un = upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d",  un);
        snprintf(rn_uv,  sizeof(rn_uv),  "upv%d",   un);
        snprintf(rn_loc, sizeof(rn_loc),  "uvloc%d", un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src   = vstack_top(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_uvs),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, upvalues),
              r_closure, 0, 1)));
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
            MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_loc, 0, 1),
            MIR_new_reg_op(ctx, src)));
        break;
      }

      case OP_CLOSE_UPVAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (n_locals > 0) {
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, close_upval_proto),
            MIR_new_ref_op(ctx, imp_close_upval),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_uint_op(ctx, (uint64_t)idx),
            MIR_new_reg_op(ctx, r_lbuf),
            MIR_new_int_op(ctx, n_locals)));
        break;
      }

      case OP_GET_GLOBAL:
      case OP_GET_GLOBAL_UNDEF: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t dst = vstack_push(&vs);

        if (vs.known_func) {
          ant_value_t gv = jit_helper_get_global(js, atom->str, atom->len);
          if (vtype(gv) == T_FUNC) {
            sv_closure_t *gcl = js_func_closure(gv);
            if (gcl && gcl->func == func) {
              vs.known_func[vs.sp - 1] = func;
              uint64_t func_tag = NANBOX_PREFIX
                | ((ant_value_t)(T_FUNC & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);
              mir_load_imm(ctx, jit_func, r_tmp, func_tag);
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_OR,
                  MIR_new_reg_op(ctx, dst),
                  MIR_new_reg_op(ctx, r_tmp),
                  MIR_new_reg_op(ctx, r_closure)));
              break;
            }
          }
        }

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, gg_proto),
            MIR_new_ref_op(ctx, imp_gg),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len)));
        break;
      }

      case OP_RETURN: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t ret_val = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, ret_val)));
        break;
      }

      case OP_RETURN_UNDEF: {
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1,
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        break;
      }

      case OP_INC_LOCAL: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        if (known_func_locals) known_func_locals[idx] = NULL;
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        int in = arith_n++;
        char il_d1[32], il_d2[32];
        snprintf(il_d1, sizeof(il_d1), "il_d1_%d", in);
        snprintf(il_d2, sizeof(il_d2), "il_d2_%d", in);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, il_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, il_d2);
        mir_i64_to_d(ctx, jit_func, fd1, local_regs[idx], r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DADD,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, r_d_one)));
        mir_d_to_i64(ctx, jit_func, local_regs[idx], fd2, r_d_slot);
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[idx])));
        break;
      }

      case OP_DEC_LOCAL: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        if (known_func_locals) known_func_locals[idx] = NULL;
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        int dn = arith_n++;
        char dl_d1[32], dl_d2[32];
        snprintf(dl_d1, sizeof(dl_d1), "dl_d1_%d", dn);
        snprintf(dl_d2, sizeof(dl_d2), "dl_d2_%d", dn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dl_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dl_d2);
        mir_i64_to_d(ctx, jit_func, fd1, local_regs[idx], r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DSUB,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, r_d_one)));
        mir_d_to_i64(ctx, jit_func, local_regs[idx], fd2, r_d_slot);
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[idx])));
        break;
      }

      case OP_ADD_LOCAL: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        if (known_func_locals) known_func_locals[idx] = NULL;
        if (known_type_locals) known_type_locals[idx] = SV_TI_UNKNOWN;
        MIR_reg_t rr = vstack_pop(&vs);

        MIR_label_t slow = MIR_new_label(ctx);
        MIR_label_t done = MIR_new_label(ctx);

        mir_emit_is_num_guard(ctx, jit_func, r_bool, local_regs[idx], slow);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);

        int an = arith_n++;
        char al_d1[32], al_d2[32], al_d3[32];
        snprintf(al_d1, sizeof(al_d1), "al_d1_%d", an);
        snprintf(al_d2, sizeof(al_d2), "al_d2_%d", an);
        snprintf(al_d3, sizeof(al_d3), "al_d3_%d", an);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, al_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, al_d2);
        MIR_reg_t fd3 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, al_d3);
        mir_i64_to_d(ctx, jit_func, fd1, local_regs[idx], r_d_slot);
        mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DADD,
            MIR_new_reg_op(ctx, fd3),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, fd2)));
        mir_d_to_i64(ctx, jit_func, local_regs[idx], fd3, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

        MIR_append_insn(ctx, jit_func, slow);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_val),
            MIR_new_reg_op(ctx, local_regs[idx])));
        mir_call_helper2(ctx, jit_func, local_regs[idx],
                         helper2_proto, imp_add,
                         r_vm, r_js, local_regs[idx], rr);
        mir_emit_bailout_check(ctx, jit_func, local_regs[idx],
          r_bailout_val, r_bailout_off, bc_off,
          r_bailout_sp, vs.sp, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);

        MIR_append_insn(ctx, jit_func, done);
        if (has_captures && captured_locals && captured_locals[idx])
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[idx])));
        break;
      }

      case OP_TO_PROPKEY: {
        MIR_reg_t src = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_label_t is_key  = MIR_new_label(ctx);
        MIR_label_t pk_done = MIR_new_label(ctx);
        MIR_label_t pk_helper = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_UBLE,
            MIR_new_label_op(ctx, pk_helper),
            MIR_new_reg_op(ctx, src),
            MIR_new_uint_op(ctx, NANBOX_PREFIX)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, src),
            MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_AND,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, NANBOX_TYPE_MASK)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_key),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_int_op(ctx, T_STR)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_key),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_int_op(ctx, T_SYMBOL)));
        MIR_append_insn(ctx, jit_func, pk_helper);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_to_propkey),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, src)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, pk_done)));
        MIR_append_insn(ctx, jit_func, is_key);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, src)));
        MIR_append_insn(ctx, jit_func, pk_done);
        break;
      }

      case OP_GET_FIELD: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, gf_proto),
            MIR_new_ref_op(ctx, imp_get_field),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_FIELD2: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t obj = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, gf_proto),
            MIR_new_ref_op(ctx, imp_get_field),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_FIELD: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, put_field_proto),
            MIR_new_ref_op(ctx, imp_put_field),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_err_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_err_tmp)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_DEFINE_FIELD: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t obj = vstack_top(&vs); 
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, define_field_proto),
            MIR_new_ref_op(ctx, imp_define_field),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len)));
        break;
      }

      case OP_GET_ELEM: {
        MIR_reg_t key = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, ge_proto),
            MIR_new_ref_op(ctx, imp_get_elem),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, key),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_ELEM2: {
        MIR_reg_t key = vstack_pop(&vs);
        MIR_reg_t obj = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_get_elem2,
                         r_vm, r_js, obj, key);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_ELEM: {
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t key = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, put_elem_proto),
            MIR_new_ref_op(ctx, imp_put_elem),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, key),
            MIR_new_reg_op(ctx, val)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_err_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_err_tmp)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t val = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, put_global_proto),
            MIR_new_ref_op(ctx, imp_put_global),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_int_op(ctx, func->is_strict ? 1 : 0)));
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_err_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_err_tmp)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_OBJECT: {
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 5,
            MIR_new_ref_op(ctx, object_proto),
            MIR_new_ref_op(ctx, imp_object),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js)));
        break;
      }

      case OP_ARRAY: {
        uint16_t n = sv_get_u16(ip + 1);
        if (vs.sp < (int)n) { ok = false; break; }
        for (int i = (int)n - 1; i >= 0; i--) {
          MIR_reg_t elem = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, elem)));
        }
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, array_proto),
            MIR_new_ref_op(ctx, imp_array),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_args_buf),
            MIR_new_int_op(ctx, (int64_t)n)));
        break;
      }

      case OP_SET_PROTO: {
        MIR_reg_t proto = vstack_pop(&vs);
        MIR_reg_t obj = vstack_top(&vs);
        mir_call_helper2(ctx, jit_func, r_err_tmp,
                         helper2_proto, imp_set_proto,
                         r_vm, r_js, obj, proto);
        break;
      }

      case OP_SWAP: {
        if (vs.sp < 2) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t ra = vs.regs[vs.sp - 2];
        MIR_reg_t rb = vs.regs[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_tmp),
            MIR_new_reg_op(ctx, ra)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, ra),
            MIR_new_reg_op(ctx, rb)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, rb),
            MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_ROT3L: {
        if (vs.sp < 3) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        MIR_reg_t rx = vs.regs[vs.sp - 3];
        MIR_reg_t ra = vs.regs[vs.sp - 2];
        MIR_reg_t rb = vs.regs[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_tmp),
            MIR_new_reg_op(ctx, rx)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, rx),
            MIR_new_reg_op(ctx, ra)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, ra),
            MIR_new_reg_op(ctx, rb)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, rb),
            MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_IN: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_in,
                         r_vm, r_js, rl, rr);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_INSTANCEOF: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_instanceof,
                         r_vm, r_js, rl, rr);
        if (has_captures) {
          for (int i = 0; i < n_locals; i++)
            if (captured_locals[i])
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, local_regs[i]),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        }
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_LENGTH: {
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_get_length),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj)));
        break;
      }

      case OP_SEQ: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_seq,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_EQ: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_eq,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_NE: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_ne,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_SNE: {
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_sne,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_GT: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_gt,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int gtn = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "gt_d1_%d", gtn);
          snprintf(d2, sizeof(d2), "gt_d2_%d", gtn);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGT,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_gt, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_GE: {
        uint8_t fb = func->type_feedback ? func->type_feedback[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_ge,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        } else if (fb_num_only && l_is_num && r_is_num) {
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
        } else if (fb_num_only && (l_is_num || r_is_num)) {
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_reg_t boxed_reg = l_is_num ? rr : rl;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, boxed_reg, bail_direct);
          int boxed_idx = l_is_num ? (int)vs.sp : (int)(vs.sp - 1);
          mir_i64_to_d(ctx, jit_func, vs.d_regs[boxed_idx],
                       vs.regs[boxed_idx], r_d_slot);
          MIR_reg_t fd_l = vs.d_regs[vs.sp - 1];
          MIR_reg_t fd_r = vs.d_regs[vs.sp];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd_l),
              MIR_new_reg_op(ctx, fd_r)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_label_t skip_bail = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, skip_bail)));
          MIR_append_insn(ctx, jit_func, bail_direct);
          int pre_op_sp = vs.sp + 1;
          for (int i = 0; i < pre_op_sp; i++) {
            if (vs.slot_type && vs.slot_type[i] == SLOT_NUM)
              mir_d_to_i64(ctx, jit_func, vs.regs[i], vs.d_regs[i], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, vs.regs[i])));
          }
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_off),
              MIR_new_int_op(ctx, bc_off)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_sp),
              MIR_new_int_op(ctx, pre_op_sp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, bailout_tramp)));
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rl, slow);
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, slow);
          int gen = arith_n++;
          char d1[32], d2[32];
          snprintf(d1, sizeof(d1), "ge_d1_%d", gen);
          snprintf(d2, sizeof(d2), "ge_d2_%d", gen);
          MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d1);
          MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, d2);
          mir_i64_to_d(ctx, jit_func, fd1, rl, r_d_slot);
          mir_i64_to_d(ctx, jit_func, fd2, rr, r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DGE,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, fd2)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_tmp),
              MIR_new_reg_op(ctx, r_bool)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, rd),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_ge, r_vm, r_js, rl, rr);
          mir_emit_bailout_check(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_BAND: case OP_BOR: case OP_BXOR:
      case OP_SHL:  case OP_SHR: case OP_USHR: {
        int rr_idx = vs.sp - 1;
        int rl_idx = vs.sp - 2;
        vstack_ensure_boxed(&vs, rl_idx, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, rr_idx, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);
        MIR_item_t imp;
        switch (op) {
          case OP_BAND: imp = imp_band; break;
          case OP_BOR:  imp = imp_bor;  break;
          case OP_BXOR: imp = imp_bxor; break;
          case OP_SHL:  imp = imp_shl;  break;
          case OP_SHR:  imp = imp_shr;  break;
          default:      imp = imp_ushr; break;
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_val),
            MIR_new_reg_op(ctx, rl)));
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp, r_vm, r_js, rl, rr);
        mir_emit_bailout_check(ctx, jit_func, rd,
          r_bailout_val, r_bailout_off, bc_off,
          r_bailout_sp, vs.sp + 1, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
        break;
      }

      case OP_BNOT: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t rs = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_bnot),
            MIR_new_reg_op(ctx, rs),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, rs)));
        MIR_label_t no_bail = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_bail),
            MIR_new_reg_op(ctx, rs),
            MIR_new_uint_op(ctx, (uint64_t)SV_JIT_BAILOUT)));
        for (int i = 0; i < vs.sp; i++)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, vs.regs[i])));
        for (int i = 0; i < n_locals; i++)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[i])));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_off),
            MIR_new_int_op(ctx, bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_sp),
            MIR_new_int_op(ctx, vs.sp)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP,
            MIR_new_label_op(ctx, bailout_tramp)));
        MIR_append_insn(ctx, jit_func, no_bail);
        break;
      }

      case OP_NOT: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t rs = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_not),
            MIR_new_reg_op(ctx, rs),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, rs)));
        break;
      }

      case OP_TYPEOF: {
        MIR_reg_t rs = vstack_top(&vs);
        for (int i = 0; i < vs.sp; i++)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, vs.regs[i])));
        for (int i = 0; i < n_locals; i++)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[i])));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_off),
            MIR_new_int_op(ctx, bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bailout_sp),
            MIR_new_int_op(ctx, vs.sp)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP,
            MIR_new_label_op(ctx, bailout_tramp)));
        (void)rs; (void)imp_typeof;
        break;
      }

      case OP_VOID: {
        MIR_reg_t rs = vstack_top(&vs);
        mir_load_imm(ctx, jit_func, rs, mkval(T_UNDEF, 0));
        break;
      }

      case OP_DELETE: {
        MIR_reg_t rk = vstack_pop(&vs);
        MIR_reg_t ro = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_delete,
                         r_vm, r_js, ro, rk);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, dst)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_NEW: {
        uint16_t new_argc = sv_get_u16(ip + 1);
        if (new_argc > 16 || vs.sp < (int)new_argc + 2) { ok = false; break; }

        for (int i = (int)new_argc - 1; i >= 0; i--) {
          MIR_reg_t areg = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, areg)));
        }

        MIR_reg_t r_new_target = vstack_pop(&vs);
        MIR_reg_t r_new_func   = vstack_pop(&vs);
        MIR_reg_t r_new_res    = vstack_push(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, new_proto),
            MIR_new_ref_op(ctx, imp_new),
            MIR_new_reg_op(ctx, r_new_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_new_func),
            MIR_new_reg_op(ctx, r_new_target),
            MIR_new_reg_op(ctx, r_args_buf),
            MIR_new_int_op(ctx, (int64_t)new_argc)));

        if (has_captures) {
          for (int i = 0; i < n_locals; i++)
            if (captured_locals[i])
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, local_regs[i]),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        }

        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_new_res),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, no_err),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_new_res)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_new_res)));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_TAIL_CALL_METHOD:
      case OP_CALL_METHOD: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        bool is_tail = (op == OP_TAIL_CALL_METHOD);
        uint16_t call_argc = sv_get_u16(ip + 1);
        if (call_argc > 16 || vs.sp < (int)call_argc + 2) { ok = false; break; }

        int cn = call_n++;

        char rn_arr[32], rn_ccl[32], rn_cfn[32], rn_jptr[32];
        snprintf(rn_arr,  sizeof(rn_arr),  "cm_arr%d",  cn);
        snprintf(rn_ccl,  sizeof(rn_ccl),  "cm_cl%d",   cn);
        snprintf(rn_cfn,  sizeof(rn_cfn),  "cm_fn%d",   cn);
        snprintf(rn_jptr, sizeof(rn_jptr), "cm_jptr%d", cn);

        MIR_reg_t r_arg_arr = r_args_buf;

        for (int i = (int)call_argc - 1; i >= 0; i--) {
          MIR_reg_t areg = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_arg_arr, 0, 1),
              MIR_new_reg_op(ctx, areg)));
        }

        MIR_reg_t r_call_func = vstack_pop(&vs);
        MIR_reg_t r_call_this = vstack_pop(&vs); 
        MIR_reg_t r_call_res  = vstack_push(&vs);

        MIR_label_t lbl_cm_self   = MIR_new_label(ctx);
        MIR_label_t lbl_cm_interp = MIR_new_label(ctx);
        MIR_label_t lbl_cm_done   = MIR_new_label(ctx);

        MIR_reg_t r_callee_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_ccl);
        mir_emit_get_closure(ctx, jit_func, r_callee_cl, r_call_func,
                             r_bool, lbl_cm_interp);

        MIR_reg_t r_callee_fn = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_cfn);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, func),
              r_callee_cl, 0, 1)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_cm_interp),
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_cm_self),
            MIR_new_reg_op(ctx, r_callee_cl),
            MIR_new_reg_op(ctx, r_closure)));

        MIR_reg_t r_jit_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_jptr);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_func_t, jit_code),
              r_callee_fn, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_cm_interp),
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_callee_cl)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_cm_done)));

        MIR_append_insn(ctx, jit_func, lbl_cm_self);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_ref_op(ctx, jit_func),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_closure)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_cm_done)));

        MIR_append_insn(ctx, jit_func, lbl_cm_interp);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, call_proto),
            MIR_new_ref_op(ctx, imp_call),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc)));
        MIR_append_insn(ctx, jit_func, lbl_cm_done);
        if (is_tail && jit_try_depth == 0) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
        } else {
          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, local_regs[i]),
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          }
          if (jit_try_depth > 0) {
            jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
                MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP,
                MIR_new_label_op(ctx, h->catch_label)));
            MIR_append_insn(ctx, jit_func, no_err);
          } else {
            MIR_label_t no_err = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_reg_op(ctx, r_call_res),
                MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, no_err),
                MIR_new_reg_op(ctx, r_bool),
                MIR_new_uint_op(ctx, JIT_ERR_TAG)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          if (is_tail) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_call_res)));
          }
        }
        break;
      }

      case OP_NOP:
      case OP_HALT:
      case OP_LINE_NUM:
      case OP_COL_NUM:
      case OP_LABEL:
        break;

      case OP_SET_NAME: {
        uint32_t atom_idx = sv_get_u32(ip + 1);
        if (atom_idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[atom_idx];
        MIR_reg_t fn_val = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, set_name_proto),
            MIR_new_ref_op(ctx, imp_set_name),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, fn_val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len)));
        break;
      }

      case OP_TRY_PUSH: {
        int32_t off = sv_get_i32(ip + 1);
        int catch_off = bc_off + sz + off;
        MIR_label_t catch_lbl = label_for_branch(ctx, &lm, catch_off, vs.sp);
        if (jit_try_depth < JIT_TRY_MAX) {
          jit_try_stack[jit_try_depth].catch_label = catch_lbl;
          jit_try_stack[jit_try_depth].catch_bc_off = catch_off;
          jit_try_stack[jit_try_depth].saved_sp = vs.sp;
          jit_try_depth++;
        }
        if (catch_sp_count < JIT_TRY_MAX) {
          catch_sp_map[catch_sp_count].bc_off = catch_off;
          catch_sp_map[catch_sp_count].saved_sp = vs.sp;
          catch_sp_count++;
        }
        break;
      }

      case OP_TRY_POP:
        if (jit_try_depth > 0) jit_try_depth--;
        break;

      case OP_THROW: {
        MIR_reg_t thrown = vstack_pop(&vs);
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, thrown)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 6,
              MIR_new_ref_op(ctx, helper1_proto),
              MIR_new_ref_op(ctx, imp_throw),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_reg_op(ctx, thrown)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_err_tmp)));
        }
        break;
      }

      case OP_THROW_ERROR: {
        uint32_t atom_idx = sv_get_u32(ip + 1);
        uint8_t err_type = sv_get_u8(ip + 5);
        if (atom_idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[atom_idx];
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, throw_error_proto),
            MIR_new_ref_op(ctx, imp_throw_error),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_int_op(ctx, (int64_t)err_type)));
        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_err_tmp)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_err_tmp)));
        }
        break;
      }

      case OP_CATCH: {
        int catch_saved_sp = -1;
        for (int i = 0; i < catch_sp_count; i++) {
          if (catch_sp_map[i].bc_off == bc_off) {
            catch_saved_sp = catch_sp_map[i].saved_sp;
            break;
          }
        }
        if (catch_saved_sp >= 0) {
          vs.sp = catch_saved_sp + 1;
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 6,
              MIR_new_ref_op(ctx, helper1_proto),
              MIR_new_ref_op(ctx, imp_catch_value),
              MIR_new_reg_op(ctx, vs.regs[catch_saved_sp]),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_reg_op(ctx, vs.regs[catch_saved_sp])));
        } else {
          ok = false;
        }
        break;
      }

      case OP_NIP_CATCH: {
        MIR_reg_t a = vs.regs[vs.sp - 1];
        MIR_reg_t below = vs.regs[vs.sp - 2];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, below),
            MIR_new_reg_op(ctx, a)));
        vs.sp--;
        break;
      }

      case OP_CLOSURE: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->const_count) { ok = false; break; }
        MIR_reg_t dst = vstack_push(&vs);
        ant_value_t cv = func->constants[idx];
        vs.known_func[vs.sp - 1] = (sv_func_t *)(uintptr_t)vdata(cv);
        if (n_locals > 0) {
          for (int i = 0; i < n_locals; i++)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 12,
            MIR_new_ref_op(ctx, closure_proto),
            MIR_new_ref_op(ctx, imp_closure),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_reg_op(ctx, r_this),
            MIR_new_reg_op(ctx, r_args),
            MIR_new_reg_op(ctx, r_argc),
            MIR_new_uint_op(ctx, (uint64_t)idx),
            MIR_new_reg_op(ctx, r_lbuf),
            MIR_new_int_op(ctx, n_locals)));
        break;
      }

      default:
        ok = false;
        break;
    }

    if (!ok) break;
    ip += sz;
  }

  if (!ok || vs.sp > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_ret_insn(ctx, 1,
        MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
  }

  if (needs_bailout) {
    MIR_append_insn(ctx, jit_func, bailout_tramp);

    MIR_reg_t r_resume_res = MIR_new_func_reg(ctx, jit_func->u.func,
                                               MIR_JSVAL, "resume_res");
    MIR_append_insn(ctx, jit_func,
      MIR_new_call_insn(ctx, 13,
        MIR_new_ref_op(ctx, resume_proto),
        MIR_new_ref_op(ctx, imp_resume),
        MIR_new_reg_op(ctx, r_resume_res),
        MIR_new_reg_op(ctx, r_vm),
        MIR_new_reg_op(ctx, r_closure),
        MIR_new_reg_op(ctx, r_this),
        MIR_new_reg_op(ctx, r_args),
        MIR_new_reg_op(ctx, r_argc),
        MIR_new_reg_op(ctx, r_args_buf),
        MIR_new_reg_op(ctx, r_bailout_sp),
        MIR_new_reg_op(ctx, r_lbuf),
        MIR_new_int_op(ctx, n_locals),
        MIR_new_reg_op(ctx, r_bailout_off)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_resume_res)));
  }

  MIR_finish_func(ctx); MIR_finish_module(ctx);
  if (sv_dump_jit_unlikely) MIR_output_module(ctx, stderr, mod);

  free(vs.regs);
  free(vs.d_regs);
  free(vs.known_func);
  free(vs.slot_type);
  free(local_regs);
  free(known_func_locals);
  free(known_type_locals);
  free(captured_locals);

  if (!ok) return NULL;

  MIR_load_module(ctx, mod);
#define LOAD_EXT(name) MIR_load_external(ctx, #name, name)
  LOAD_EXT(jit_helper_add);
  LOAD_EXT(jit_helper_sub);
  LOAD_EXT(jit_helper_mul);
  LOAD_EXT(jit_helper_div);
  LOAD_EXT(jit_helper_mod);
  LOAD_EXT(jit_helper_lt);
  LOAD_EXT(jit_helper_le);
  LOAD_EXT(jit_helper_gt);
  LOAD_EXT(jit_helper_ge);
  LOAD_EXT(jit_helper_call);
  LOAD_EXT(jit_helper_tov);
  LOAD_EXT(jit_helper_get_global);
  LOAD_EXT(jit_helper_get_field);
  LOAD_EXT(jit_helper_to_propkey);
  LOAD_EXT(jit_helper_bailout_resume);
  LOAD_EXT(jit_helper_close_upval);
  LOAD_EXT(jit_helper_closure);
  LOAD_EXT(jit_helper_in);
  LOAD_EXT(jit_helper_instanceof);
  LOAD_EXT(jit_helper_get_length);
  LOAD_EXT(jit_helper_define_field);
  LOAD_EXT(jit_helper_seq);
  LOAD_EXT(jit_helper_eq);
  LOAD_EXT(jit_helper_ne);
  LOAD_EXT(jit_helper_sne);
  LOAD_EXT(jit_helper_put_field);
  LOAD_EXT(jit_helper_get_elem);
  LOAD_EXT(jit_helper_get_elem2);
  LOAD_EXT(jit_helper_put_elem);
  LOAD_EXT(jit_helper_put_global);
  LOAD_EXT(jit_helper_object);
  LOAD_EXT(jit_helper_array);
  LOAD_EXT(jit_helper_catch_value);
  LOAD_EXT(jit_helper_throw);
  LOAD_EXT(jit_helper_throw_error);
  LOAD_EXT(jit_helper_set_proto);
  LOAD_EXT(jit_helper_band);
  LOAD_EXT(jit_helper_bor);
  LOAD_EXT(jit_helper_bxor);
  LOAD_EXT(jit_helper_bnot);
  LOAD_EXT(jit_helper_shl);
  LOAD_EXT(jit_helper_shr);
  LOAD_EXT(jit_helper_ushr);
  LOAD_EXT(jit_helper_not);
  LOAD_EXT(jit_helper_is_truthy);
  LOAD_EXT(jit_helper_typeof);
  LOAD_EXT(jit_helper_new);
  LOAD_EXT(jit_helper_delete);
  LOAD_EXT(jit_helper_set_name);
  LOAD_EXT(jit_helper_stack_overflow);
  LOAD_EXT(jit_helper_stack_overflow_error);
#undef LOAD_EXT
  MIR_link(ctx, MIR_set_gen_interface, NULL);

  func->jit_compiled_tfb_ver = func->tfb_version;
  return MIR_gen(ctx, jit_func);
}


ant_value_t sv_jit_try_compile_and_call(
  sv_vm_t *vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func,
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  sv_func_t *fn = closure->func;

  sv_jit_func_t jit = sv_jit_compile(js, fn, closure);
  if (!jit) {
    fn->call_count = 0;
    fn->back_edge_count = 0;
    return SV_JIT_RETRY_INTERP;
  }

  fn->jit_code = (void *)jit;
  sv_jit_enter(js);
  ant_value_t result = jit(vm, ctx->this_val, ctx->args, ctx->argc, closure);
  sv_jit_leave(js);
  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(fn);
    return SV_JIT_RETRY_INTERP;
  }
  sv_call_cleanup(js, ctx);
  if (out_this) *out_this = ctx->this_val;
  return result;
}


ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func,
  int bc_offset
) {
  sv_closure_t osr_closure;
  sv_closure_t *closure;
  if (vtype(frame->callee) == T_FUNC) {
    closure = js_func_closure(frame->callee);
  } else {
    memset(&osr_closure, 0, sizeof(osr_closure));
    osr_closure.func     = func;
    osr_closure.upvalues = frame->upvalues;
    closure = &osr_closure;
  }

  sv_jit_func_t jit;
  if (func->jit_code) {
    jit = (sv_jit_func_t)func->jit_code;
  } else {
    jit = sv_jit_compile(js, func, closure);
    if (!jit) return SV_JIT_RETRY_INTERP;
    func->jit_code = (void *)jit;
  }

  int nl = func->max_locals;
  ant_value_t osr_locals[nl > 0 ? nl : 1];
  for (int i = 0; i < nl; i++)
    osr_locals[i] = frame->lp[i];

  vm->jit_osr.active    = true;
  vm->jit_osr.bc_offset = bc_offset;
  vm->jit_osr.locals    = osr_locals;
  vm->jit_osr.n_locals  = nl;
  vm->jit_osr.lp        = frame->lp;

  sv_jit_enter(js);
  ant_value_t result = jit(vm, frame->this, frame->bp, frame->argc, closure);
  sv_jit_leave(js);

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }

  return result;
}

#endif 
