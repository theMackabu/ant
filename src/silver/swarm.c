#ifdef ANT_JIT

#include "silver/swarm.h"
#include "silver/glue.h"
#include "silver/engine.h"
#include "silver/opcode.h"

#include "internal.h"
#include "debug.h"
#include "shapes.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmacro-redefined"
#include <mir.h>
#include <mir-gen.h>
#pragma GCC diagnostic pop
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
  bool externals_loaded;
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

static void jit_load_externals_once(sv_jit_ctx_t *jc) {
  if (jc == NULL || jc->externals_loaded) return;
  MIR_context_t ctx = jc->ctx;
#define LOAD_EXT(name) MIR_load_external(ctx, #name, name)
  LOAD_EXT(jit_helper_add);
  LOAD_EXT(jit_helper_sub);
  LOAD_EXT(jit_helper_mul);
  LOAD_EXT(jit_helper_div);
  LOAD_EXT(jit_helper_mod);
  LOAD_EXT(jit_helper_str_append_local);
  LOAD_EXT(jit_helper_str_append_local_snapshot);
  LOAD_EXT(jit_helper_str_flush_local);
  LOAD_EXT(jit_helper_lt);
  LOAD_EXT(jit_helper_le);
  LOAD_EXT(jit_helper_gt);
  LOAD_EXT(jit_helper_ge);
  LOAD_EXT(jit_helper_call);
  LOAD_EXT(jit_helper_apply);
  LOAD_EXT(jit_helper_rest);
  LOAD_EXT(jit_helper_special_obj);
  LOAD_EXT(jit_helper_for_of);
  LOAD_EXT(jit_helper_destructure_close);
  LOAD_EXT(jit_helper_destructure_next);
  LOAD_EXT(jit_helper_get_global);
  LOAD_EXT(jit_helper_get_field);
  LOAD_EXT(jit_helper_to_propkey);
  LOAD_EXT(jit_helper_bailout_resume);
  LOAD_EXT(jit_helper_close_upval);
  LOAD_EXT(jit_helper_closure);
  LOAD_EXT(jit_helper_in);
  LOAD_EXT(jit_helper_instanceof);
  LOAD_EXT(jit_helper_call_is_proto);
  LOAD_EXT(jit_helper_get_length);
  LOAD_EXT(jit_helper_define_field);
  LOAD_EXT(jit_helper_define_method_comp);
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
  jc->externals_loaded = true;
}

void sv_jit_init(ant_t *js) {
  if (jit_ctx_get(js)) return;
  sv_jit_ctx_t *jc = calloc(1, sizeof(*jc));
  if (!jc) return;
  jc->ctx = MIR_init();
  MIR_gen_init(jc->ctx);
  MIR_gen_set_optimize_level(jc->ctx, 2);
  jit_load_externals_once(jc);
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
  uint64_t    *known_const;  
  bool        *has_const;    
  int          sp;           
  int          max;
} jit_vstack_t;

static MIR_reg_t vstack_push(jit_vstack_t *vs) {
  if (vs->known_func) vs->known_func[vs->sp] = NULL;
  if (vs->slot_type) vs->slot_type[vs->sp] = 0; 
  if (vs->has_const) vs->has_const[vs->sp] = false;
  return vs->regs[vs->sp++];
}

static MIR_reg_t vstack_push_const(jit_vstack_t *vs, uint64_t val) {
  if (vs->known_func) vs->known_func[vs->sp] = NULL;
  if (vs->slot_type) vs->slot_type[vs->sp] = 0;
  if (vs->has_const) { vs->has_const[vs->sp] = true; vs->known_const[vs->sp] = val; }
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

static void mir_emit_fill_param_slots_from_args(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
  bool *captured_params, int param_count
) {
  if (!captured_params) return;
  for (int i = 0; i < param_count; i++) {
    if (!captured_params[i]) continue;
    MIR_label_t arg_present = MIR_new_label(ctx);
    MIR_label_t arg_done = MIR_new_label(ctx);
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_UBGT,
        MIR_new_label_op(ctx, arg_present),
        MIR_new_reg_op(ctx, r_argc),
        MIR_new_int_op(ctx, (int64_t)i)));
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
        MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_JMP,
        MIR_new_label_op(ctx, arg_done)));
    MIR_append_insn(ctx, fn, arg_present);
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args, 0, 1)));
    MIR_append_insn(ctx, fn, arg_done);
  }
}

static void mir_emit_spill_child_captured_locals(
  MIR_context_t ctx, MIR_item_t fn,
  sv_func_t *parent_func, sv_func_t *child,
  MIR_reg_t *local_regs, int n_locals, MIR_reg_t r_lbuf
) {
  if (!child || !parent_func || !local_regs || n_locals <= 0 || !r_lbuf) return;

  for (int i = 0; i < child->upvalue_count; i++) {
    sv_upval_desc_t *desc = &child->upval_descs[i];
    if (!desc->is_local) continue;

    int li = (int)desc->index - parent_func->param_count;
    if (li < 0 || li >= n_locals) continue;

    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(li * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
        MIR_new_reg_op(ctx, local_regs[li])));
  }
}

static void mir_emit_close_marked_slots(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
  MIR_reg_t r_vm, MIR_reg_t r_slots,
  bool *captured, int start_idx, int slot_count
) {
  if (!captured || start_idx >= slot_count || slot_count <= 0 || !r_slots) return;
  if (start_idx < 0) start_idx = 0;

  for (int i = start_idx; i < slot_count; i++) {
    if (!captured[i]) continue;
    MIR_append_insn(ctx, fn,
      MIR_new_call_insn(ctx, 6,
        MIR_new_ref_op(ctx, close_upval_proto),
        MIR_new_ref_op(ctx, imp_close_upval),
        MIR_new_reg_op(ctx, r_vm),
        MIR_new_uint_op(ctx, (uint64_t)i),
        MIR_new_reg_op(ctx, r_slots),
        MIR_new_int_op(ctx, i + 1)));
  }
}

static inline void mir_emit_self_tail(
  MIR_context_t ctx, MIR_item_t fn,
  int call_argc, int param_count,
  MIR_reg_t r_tco_args, MIR_reg_t r_arg_arr,
  MIR_reg_t r_args, MIR_reg_t r_argc,
  MIR_reg_t *local_regs, int n_locals,
  bool has_captured_slots, MIR_reg_t r_slotbuf, bool *captured_params,
  bool has_captures, bool *captured_locals,
  MIR_reg_t r_lbuf, MIR_label_t entry
) {
  for (int i = 0; i < call_argc && i < param_count; i++)
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_tco_args, 0, 1),
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_arg_arr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_args),
      MIR_new_reg_op(ctx, r_tco_args)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_argc),
      MIR_new_int_op(ctx, (int64_t)call_argc)));
  if (has_captured_slots)
    mir_emit_fill_param_slots_from_args(ctx, fn, r_slotbuf, r_tco_args, r_argc, captured_params, param_count);
  for (int i = 0; i < n_locals; i++)
    mir_load_imm(ctx, fn, local_regs[i], mkval(T_UNDEF, 0));
  if (has_captures) {
    for (int i = 0; i < n_locals; i++)
      if (captured_locals[i])
        MIR_append_insn(ctx, fn,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_I64,
              (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
  }
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, entry)));
}

static inline bool jit_const_is_heap(ant_value_t cv) {
  uint8_t t = vtype(cv);
  return 
    ((1u << t) & ((1u << T_OBJ) 
    | (1u << T_STR) 
    | (1u << T_ARR) 
    | (1u << T_PROMISE) 
    | (1u << T_BIGINT) 
    | (1u << T_GENERATOR) 
    | (1u << T_SYMBOL))) != 0;
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
#define NANBOX_TOBJ_TAG   ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_OBJ)
#define NANBOX_TARR_TAG   ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_ARR)
#define NANBOX_TPROM_TAG  ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_PROMISE)

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

static void mir_emit_resolve_call_this(MIR_context_t ctx, MIR_item_t fn,
                                       MIR_reg_t dst, MIR_reg_t r_closure,
                                       MIR_reg_t fallback_this,
                                       MIR_reg_t r_flags, MIR_reg_t r_bound) {
  MIR_label_t not_arrow = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_flags),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_closure_t, call_flags),
        r_closure, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, r_flags),
      MIR_new_reg_op(ctx, r_flags),
      MIR_new_uint_op(ctx, SV_CALL_IS_ARROW)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, not_arrow),
      MIR_new_reg_op(ctx, r_flags),
      MIR_new_uint_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_JSVAL,
        (MIR_disp_t)offsetof(sv_closure_t, bound_this),
        r_closure, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, not_arrow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_reg_op(ctx, fallback_this)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_bound),
      MIR_new_mem_op(ctx, MIR_JSVAL,
        (MIR_disp_t)offsetof(sv_closure_t, bound_this),
        r_closure, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, done),
      MIR_new_reg_op(ctx, r_bound),
      MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_reg_op(ctx, r_bound)));
  MIR_append_insn(ctx, fn, done);
}

static void mir_emit_value_to_objptr_or_jmp(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t v, MIR_reg_t out_ptr,
  MIR_reg_t r_tag, MIR_label_t slow
) {
  MIR_label_t is_obj = MIR_new_label(ctx);
  MIR_label_t is_func = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, is_obj),
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TOBJ_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, is_obj),
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, is_obj),
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TPROM_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, is_func),
      MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, slow)));

  MIR_append_insn(ctx, fn, is_obj);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, is_func);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(sv_closure_t, func_obj),
        out_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn, done);
}

static bool mir_emit_get_field_ic_fastpath(
  MIR_context_t ctx,
  MIR_item_t fn,
  sv_func_t *func,
  int bc_off,
  uint16_t ic_idx,
  sv_atom_t *atom,
  MIR_reg_t obj,
  MIR_reg_t dst,
  MIR_label_t slow,
  MIR_reg_t r_global_epoch
) {
  if (!func || !func->ic_slots || !atom) return false;
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return false;
  if (is_length_key(atom->str, atom->len)) return false;

  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  if (!sv_gf_ic_active(ic->cached_aux)) return false;
  char gf_ic_name[32], gf_ice_name[32];
  char gf_ot_name[32], gf_op_name[32], gf_os_name[32], gf_ics_name[32];
  char gf_h_name[32], gf_hs_name[32], gf_idx_name[32], gf_pc_name[32];
  char gf_ica_name[32], gf_il_name[32], gf_ovf_name[32], gf_ovi_name[32];
  char gf_io_name[32], gf_src_name[32];
  snprintf(gf_ic_name, sizeof(gf_ic_name), "gf_ic_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ice_name, sizeof(gf_ice_name), "gf_ice_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ot_name, sizeof(gf_ot_name), "gf_ot_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_op_name, sizeof(gf_op_name), "gf_op_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_os_name, sizeof(gf_os_name), "gf_os_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ics_name, sizeof(gf_ics_name), "gf_ics_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_h_name, sizeof(gf_h_name), "gf_h_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_hs_name, sizeof(gf_hs_name), "gf_hs_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_idx_name, sizeof(gf_idx_name), "gf_idx_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_pc_name, sizeof(gf_pc_name), "gf_pc_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ica_name, sizeof(gf_ica_name), "gf_ica_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_il_name, sizeof(gf_il_name), "gf_il_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ovf_name, sizeof(gf_ovf_name), "gf_ovf_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ovi_name, sizeof(gf_ovi_name), "gf_ovi_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_io_name, sizeof(gf_io_name), "gf_io_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_src_name, sizeof(gf_src_name), "gf_src_%d_%u", bc_off, (unsigned)ic_idx);

  MIR_reg_t r_ic = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ic_name);
  MIR_reg_t r_ic_epoch = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ice_name);
  MIR_reg_t r_obj_tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ot_name);
  MIR_reg_t r_obj_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_op_name);
  MIR_reg_t r_obj_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_os_name);
  MIR_reg_t r_ic_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ics_name);
  MIR_reg_t r_holder = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_h_name);
  MIR_reg_t r_holder_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_hs_name);
  MIR_reg_t r_ic_idx_val = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_idx_name);
  MIR_reg_t r_holder_prop_count = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pc_name);
  MIR_reg_t r_ic_aux = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ica_name);
  MIR_reg_t r_inobj_limit = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_il_name);
  MIR_reg_t r_overflow = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ovf_name);
  MIR_reg_t r_overflow_idx = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ovi_name);
  MIR_reg_t r_is_own = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_io_name);
  MIR_reg_t r_source = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_src_name);

  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t fast_done = MIR_new_label(ctx);
  MIR_label_t own_path = MIR_new_label(ctx);
  MIR_label_t do_read = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic),
      MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_aux),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, r_ic_aux),
      MIR_new_reg_op(ctx, r_ic_aux),
      MIR_new_uint_op(ctx, (uint64_t)SV_GF_IC_AUX_ACTIVE_BIT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_ic_aux),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_epoch),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
  {
    char ce_name[40];
    snprintf(ce_name, sizeof(ce_name), "gf_ce_%d_%u", bc_off, (unsigned)ic_idx);
    MIR_reg_t r_cur_epoch = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, ce_name);
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_cur_epoch),
        MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow),
        MIR_new_reg_op(ctx, r_ic_epoch),
        MIR_new_reg_op(ctx, r_cur_epoch)));
  }

  mir_emit_value_to_objptr_or_jmp(
    ctx, fn, obj, r_obj_ptr, r_obj_tag, slow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_obj_shape),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, shape), r_obj_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_shape),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_obj_shape),
      MIR_new_reg_op(ctx, r_ic_shape)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_is_own),
      MIR_new_mem_op(ctx, MIR_T_U8,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_is_own), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, own_path),
      MIR_new_reg_op(ctx, r_is_own),
      MIR_new_int_op(ctx, 0)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_holder),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_holder),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_holder_shape),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, shape), r_holder, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_holder_shape),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_source),
      MIR_new_reg_op(ctx, r_holder)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, do_read)));

  MIR_append_insn(ctx, fn, own_path);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_source),
      MIR_new_reg_op(ctx, r_obj_ptr)));

  MIR_append_insn(ctx, fn, do_read);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_idx_val),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_holder_prop_count),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(ant_object_t, prop_count), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_ic_idx_val),
      MIR_new_reg_op(ctx, r_holder_prop_count)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_inobj_limit),
      MIR_new_mem_op(ctx, MIR_T_U8,
        (MIR_disp_t)offsetof(ant_object_t, inobj_limit), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE,
      MIR_new_label_op(ctx, load_overflow),
      MIR_new_reg_op(ctx, r_ic_idx_val),
      MIR_new_reg_op(ctx, r_inobj_limit)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(ant_object_t, inobj), r_source, r_ic_idx_val, 8)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, fast_done)));

  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_overflow),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, overflow_prop), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_overflow),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_SUB,
      MIR_new_reg_op(ctx, r_overflow_idx),
      MIR_new_reg_op(ctx, r_ic_idx_val),
      MIR_new_reg_op(ctx, r_inobj_limit)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, r_overflow, r_overflow_idx, 8)));
  MIR_append_insn(ctx, fn, fast_done);

  return true;
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

static sv_func_t *scan_closure_child(sv_func_t *func, uint8_t *ip) {
  if ((sv_op_t)*ip != OP_CLOSURE) return NULL;

  uint32_t idx = sv_get_u32(ip + 1);
  if (idx >= (uint32_t)func->const_count) return NULL;

  ant_value_t cv = func->constants[idx];
  if (vtype(cv) != T_CFUNC) return NULL;

  return (sv_func_t *)(uintptr_t)vdata(cv);
}

typedef enum {
  JIT_CHILD_PLAIN = 0,
  JIT_CHILD_INHERITED_ONLY,
  JIT_CHILD_LOCAL_ONLY,
  JIT_CHILD_PARAM_ONLY,
  JIT_CHILD_MIXED,
} jit_child_kind_t;

static jit_child_kind_t classify_child_closure_kind(sv_func_t *parent, sv_func_t *child) {
  if (!parent || !child || child->upvalue_count <= 0) return JIT_CHILD_PLAIN;

  bool has_inherited = false;
  bool has_param = false;
  bool has_local = false;
  for (int i = 0; i < child->upvalue_count; i++) {
    sv_upval_desc_t *desc = &child->upval_descs[i];
    if (!desc->is_local) {
      has_inherited = true;
      continue;
    }
    if (desc->index < (uint16_t)parent->param_count) has_param = true;
    else has_local = true;
  }

  if (!has_param && !has_local) return has_inherited ? JIT_CHILD_INHERITED_ONLY : JIT_CHILD_PLAIN;
  if (has_param && !has_local) return JIT_CHILD_PARAM_ONLY;
  if (has_local && !has_param) return JIT_CHILD_LOCAL_ONLY;
  
  return JIT_CHILD_MIXED;
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
    sv_func_t *child = scan_closure_child(func, ip);
    if (!child) {
      ip += sz;
      continue;
    }

    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (!desc->is_local) continue;

      int li = (int)desc->index - func->param_count;
      if (li < 0 || li >= n_locals) continue;
      captured[li] = true;
    }
    ip += sz;
  }
  return captured;
}

static bool *scan_captured_params(sv_func_t *func) {
  int param_count = func ? func->param_count : 0;
  if (param_count <= 0) return NULL;
  bool *captured = calloc((size_t)param_count, sizeof(bool));
  if (!captured) return NULL;

  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    sv_func_t *child = scan_closure_child(func, ip);
    if (!child) {
      ip += sz;
      continue;
    }

    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (!desc->is_local) continue;
      if (desc->index >= (uint16_t)param_count) continue;
      captured[desc->index] = true;
    }
    ip += sz;
  }

  return captured;
}


#define JIT_INLINE_MAX_BYTECODE 128

static bool jit_inlineable(sv_func_t *f) {
  if (!f) return false;
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
      case OP_THIS:
      case OP_GET_LOCAL: case OP_GET_LOCAL8:
      case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
      case OP_SET_LOCAL: case OP_SET_LOCAL8:
      case OP_GET_UPVAL:
      case OP_POP: case OP_DUP: case OP_DUP2:
      case OP_INSERT2: case OP_INSERT3:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_MOD: case OP_NEG:
      case OP_LT: case OP_LE: case OP_GT: case OP_GE:
      case OP_SEQ: case OP_SNE: case OP_EQ: case OP_NE:
      case OP_IS_UNDEF: case OP_IS_NULL:
      case OP_JMP: case OP_JMP_TRUE: case OP_JMP_FALSE:
      case OP_JMP_TRUE8: case OP_JMP_FALSE8:
      case OP_JMP_TRUE_PEEK: case OP_JMP_FALSE_PEEK:
      case OP_RETURN: case OP_RETURN_UNDEF:
        break;
      case OP_GET_FIELD: case OP_GET_FIELD2: case OP_GET_GLOBAL:
        break;
      case OP_SPECIAL_OBJ:
        // TODO: RE_ENABLE once SPECIAL_OBJ semantics match the interpreter in JIT.
        return false;
      case OP_NOP: case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;
      default:
        return false;
    }
    ip += sz;
  }
  return true;
}

#define INL_MAX_LABELS 128

typedef struct {
  int          bc_off;
  MIR_label_t  label;
  int          sp;
} inl_label_entry_t;

typedef struct {
  inl_label_entry_t entries[INL_MAX_LABELS];
  int               count;
} inl_label_map_t;

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
  lm->entries[lm->count].label  = lbl;
  lm->entries[lm->count].sp     = sp;
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

static bool jit_inline_body_feasible(sv_func_t *callee) {
  uint8_t *ip  = callee->code;
  uint8_t *end = callee->code + callee->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    switch (op) {
      default: break;
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
  MIR_reg_t r_bool, MIR_reg_t *p_d_slot, int id,
  MIR_reg_t r_inl_closure, MIR_reg_t r_inl_this,
  MIR_reg_t r_vm, MIR_reg_t r_js,
  MIR_item_t helper2_proto, MIR_item_t imp_seq,
  MIR_item_t imp_sne, MIR_item_t imp_eq, MIR_item_t imp_ne,
  MIR_item_t gf_proto, MIR_item_t imp_get_field,
  MIR_item_t gg_proto, MIR_item_t imp_gg
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
  int inl_upval_n = 0;

  inl_label_map_t inl_lm = {.count = 0};

  uint8_t *code_base = callee->code;
  uint8_t *ip  = callee->code;
  uint8_t *end = callee->code + callee->code_len;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    int inl_bc_off = (int)(ip - code_base);

    int label_sp = -1;
    MIR_label_t target_lbl = inl_label_lookup(&inl_lm, inl_bc_off, &label_sp);
    if (target_lbl) {
      MIR_append_insn(ctx, jit_func, target_lbl);
      if (label_sp >= 0) isp = label_sp;
    }

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

      case OP_THIS: {
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_vs[isp++]),
            MIR_new_reg_op(ctx, r_inl_this)));
        break;
      }

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

      case OP_GET_UPVAL: {
        if (!r_inl_closure) return false;
        uint16_t idx = sv_get_u16(ip + 1);
        int un = inl_upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "inl%d_uvs%d", id, un);
        snprintf(rn_uv,  sizeof(rn_uv),  "inl%d_uv%d",  id, un);
        snprintf(rn_loc, sizeof(rn_loc),  "inl%d_uvl%d", id, un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        MIR_reg_t dst   = inl_vs[isp++];

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
      case OP_DUP2: {
        if (isp < 2) return false;
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

      case OP_INSERT2: {
        if (isp < 2) return false;
        char tn[32]; snprintf(tn, sizeof(tn), "inl%d_ins2t", id);
        MIR_reg_t r_t = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, tn);
        MIR_reg_t r_a   = inl_vs[isp - 1];
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
        if (isp < 3) return false;
        char tn[32]; snprintf(tn, sizeof(tn), "inl%d_ins3t", id);
        MIR_reg_t r_t = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, tn);
        MIR_reg_t r_a    = inl_vs[isp - 1];
        MIR_reg_t r_prop = inl_vs[isp - 2];
        MIR_reg_t r_obj  = inl_vs[isp - 3];
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

      case OP_MOD: {
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

      case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
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

        MIR_insn_code_t cmp_op;
        const char *cmp_name;
        switch (op) {
          case OP_LT: cmp_op = MIR_DLT; cmp_name = "lt"; break;
          case OP_LE: cmp_op = MIR_DLE; cmp_name = "le"; break;
          case OP_GT: cmp_op = MIR_DGT; cmp_name = "gt"; break;
          default:    cmp_op = MIR_DGE; cmp_name = "ge"; break;
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

      case OP_SEQ: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_seq,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_SNE: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_sne,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_EQ: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_eq,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_NE: {
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_ne,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_IS_UNDEF: case OP_IS_NULL: {
        MIR_reg_t rs = inl_vs[isp - 1];
        uint64_t cmp_val = (op == OP_IS_UNDEF)
          ? mkval(T_UNDEF, 0) : mkval(T_NULL, 0);
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

      case OP_JMP: {
        int target = inl_bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        if (!lbl) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl)));
        break;
      }

      case OP_JMP_TRUE_PEEK: case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE: case OP_JMP_FALSE:
      case OP_JMP_TRUE8: case OP_JMP_FALSE8: {
        bool is_peek = (op == OP_JMP_TRUE_PEEK || op == OP_JMP_FALSE_PEEK);
        MIR_reg_t cond = is_peek ? inl_vs[isp - 1] : inl_vs[--isp];
        bool short_op = (op == OP_JMP_TRUE8 || op == OP_JMP_FALSE8);
        bool is_false_branch = (op == OP_JMP_FALSE || op == OP_JMP_FALSE8
                                || op == OP_JMP_FALSE_PEEK);
        int target = inl_bc_off + sz + (short_op ? (int8_t)sv_get_i8(ip + 1)
                                                  : sv_get_i32(ip + 1));
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        if (!lbl) return false;

        uint64_t cmp_bool = is_false_branch ? js_false : js_true;
        MIR_label_t lbl_not_bool = MIR_new_label(ctx);
        MIR_label_t lbl_done = MIR_new_label(ctx);

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
            MIR_new_label_op(ctx, is_false_branch ? lbl_done : lbl),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, NANBOX_PREFIX)));
        if (is_false_branch) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));
        }

        MIR_append_insn(ctx, jit_func, lbl_done);
        break;
      }

      case OP_GET_FIELD: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[--isp];
        MIR_reg_t dst = inl_vs[isp++];
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
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
            MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, slow),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        break;
      }
      case OP_GET_FIELD2: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[isp - 1];
        MIR_reg_t dst = inl_vs[isp++];
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
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
            MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, slow),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        break;
      }
      case OP_GET_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, gg_proto),
            MIR_new_ref_op(ctx, imp_gg),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
            MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
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

      case OP_SPECIAL_OBJ:
        mir_load_imm(ctx, jit_func, inl_vs[isp++], mkval(T_UNDEF, 0));
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
  bool needs_iter_roots;
  bool needs_close_upval;  
  bool needs_tco_args;     
  bool needs_ic_epoch;     
} jit_features_t;

static jit_features_t jit_prescan_features(sv_func_t *func) {
  jit_features_t f = {0};
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
      case OP_STR_APPEND_LOCAL:
      case OP_STR_ALC_SNAPSHOT:
      case OP_STR_FLUSH_LOCAL:
        f.needs_bailout = true;
        break;
      case OP_INC_LOCAL: case OP_DEC_LOCAL:
      case OP_POST_INC:
        f.needs_inc_local = true; 
        break;
      case OP_CALL: case OP_CALL_METHOD:
      case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD:
      case OP_ARRAY: case OP_NEW:
      case OP_APPLY: case OP_NEW_APPLY:
      case OP_FOR_OF:
      case OP_DESTRUCTURE_INIT: case OP_DESTRUCTURE_NEXT: case OP_DESTRUCTURE_CLOSE:
        f.needs_args_buf = true;
        f.needs_iter_roots = true;
        if (op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD)
          f.needs_tco_args = true;
        break;
      case OP_CLOSE_UPVAL: case OP_CLOSURE:
        f.needs_close_upval = true;
        break;
      case OP_SET_ARG:
        f.needs_bailout = true;
        break;
      case OP_GET_FIELD: case OP_GET_FIELD2: case OP_PUT_FIELD:
      case OP_INSTANCEOF: case OP_CALL_IS_PROTO:
        f.needs_ic_epoch = true;
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
      case OP_GET_SLOT_RAW:
      case OP_GET_UPVAL: case OP_PUT_UPVAL: case OP_SET_UPVAL:
      case OP_CLOSE_UPVAL:
      case OP_REST:
      case OP_POP: case OP_DUP: case OP_DUP2:
      case OP_INSERT2: case OP_INSERT3:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_POST_INC:
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
      case OP_JMP_FALSE_PEEK: case OP_JMP_TRUE_PEEK:
      case OP_CALL: case OP_CALL_METHOD:
      case OP_CALL_IS_PROTO:
      case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD:
      case OP_APPLY: case OP_NEW_APPLY:
      case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF:
      case OP_PUT_GLOBAL:
      case OP_GET_FIELD: case OP_GET_FIELD2: case OP_PUT_FIELD:
      case OP_GET_ELEM: case OP_GET_ELEM2: case OP_PUT_ELEM:
      case OP_OBJECT: case OP_ARRAY: case OP_SET_PROTO:
      case OP_SWAP: case OP_ROT3L:
      case OP_IN: case OP_GET_LENGTH:
      case OP_DEFINE_FIELD: case OP_DEFINE_METHOD_COMP: case OP_SEQ: case OP_EQ:
      case OP_FOR_OF:
      case OP_DESTRUCTURE_INIT: case OP_DESTRUCTURE_NEXT: case OP_DESTRUCTURE_CLOSE:
      case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL:
      case OP_STR_APPEND_LOCAL:
      case OP_STR_ALC_SNAPSHOT:
      case OP_TO_PROPKEY:
      case OP_RETURN: case OP_RETURN_UNDEF:
      case OP_SET_NAME:
      case OP_TRY_PUSH: case OP_TRY_POP:
      case OP_THROW: case OP_THROW_ERROR:
      case OP_CATCH: case OP_NIP_CATCH:
      case OP_NOP: case OP_HALT:
      case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;
      case OP_CLOSURE: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->const_count) return false;
        ant_value_t cv = func->constants[idx];
        if (vtype(cv) != T_CFUNC) return false;
        break;
      }
      case OP_SPECIAL_OBJ:
        // TODO: RE_ENABLE once SPECIAL_OBJ semantics match the interpreter in JIT.
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op SPECIAL_OBJ(%d) in %s\n",
                  sv_get_u8(ip + 1),
                  func->name ? func->name : "<anonymous>");
        eligible = false;
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
  if (func->jit_compile_failed || func->jit_compiling) return NULL;
  if (func->jit_code == NULL && func->jit_compiled_tfb_ver != 0 &&
      func->tfb_version == func->jit_compiled_tfb_ver) {
    func->jit_compile_failed = true;
    return NULL;
  }
  if (!jit_is_eligible(func)) { func->jit_compile_failed = true; return NULL; }
  func->jit_compiling = true;

  sv_jit_ctx_t *jc = jit_ctx_get(js);
  if (!jc) { sv_jit_init(js); jc = jit_ctx_get(js); }
  if (!jc) { func->jit_compiling = false; return NULL; }
  jit_load_externals_once(jc);

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
    1, &gg_ret, 4,
    MIR_T_I64, "js",
    MIR_T_P,   "str",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t rest_ret = MIR_JSVAL;
  MIR_item_t rest_proto = MIR_new_proto(ctx, "rest_proto",
    1, &rest_ret, 5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "args",
    MIR_T_I32, "argc",
    MIR_T_I32, "start");

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

  MIR_type_t inst_ret = MIR_JSVAL;
  MIR_item_t inst_proto = MIR_new_proto(ctx, "inst_proto",
    1, &inst_ret, 6,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "l",
    MIR_JSVAL, "r",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t cip_ret = MIR_JSVAL;
  MIR_item_t call_is_proto = MIR_new_proto(ctx, "cip_proto",
    1, &cip_ret, 7,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "this_val",
    MIR_JSVAL, "func_val",
    MIR_JSVAL, "arg",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t h1_ret = MIR_JSVAL;
  MIR_item_t helper1_proto = MIR_new_proto(ctx, "helper1_proto",
    1, &h1_ret, 3,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "v");

  MIR_type_t sal_ret = MIR_JSVAL;
  MIR_item_t str_append_local_proto = MIR_new_proto(ctx, "sal_proto",
    1, &sal_ret, 8,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "func",
    MIR_T_P,   "args",
    MIR_T_I32, "argc",
    MIR_T_P,   "locals",
    MIR_T_I32, "slot_idx",
    MIR_JSVAL, "rhs");

  MIR_type_t sals_ret = MIR_JSVAL;
  MIR_item_t str_append_local_snapshot_proto = MIR_new_proto(ctx, "sals_proto",
    1, &sals_ret, 9,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "func",
    MIR_T_P,   "args",
    MIR_T_I32, "argc",
    MIR_T_P,   "locals",
    MIR_T_I32, "slot_idx",
    MIR_JSVAL, "lhs",
    MIR_JSVAL, "rhs");

  MIR_type_t sfl_ret = MIR_JSVAL;
  MIR_item_t str_flush_local_proto = MIR_new_proto(ctx, "sfl_proto",
    1, &sfl_ret, 7,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "func",
    MIR_T_P,   "args",
    MIR_T_I32, "argc",
    MIR_T_P,   "locals",
    MIR_T_I32, "slot_idx");

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
    1, &cl_ret, 8,
    MIR_T_I64,  "vm",
    MIR_T_I64,  "js",
    MIR_T_P,    "parent",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "slots",
    MIR_T_I32,  "slot_base",
    MIR_T_I32,  "slot_count",
    MIR_T_I32,  "const_idx");

  MIR_item_t close_upval_proto = MIR_new_proto(ctx, "close_upval_proto",
    0, NULL, 4,
    MIR_T_I64, "vm",
    MIR_T_I32, "slot_idx",
    MIR_T_P,   "slots",
    MIR_T_I32, "slot_count");

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

  MIR_item_t define_method_comp_proto = MIR_new_proto(ctx, "dmc_proto",
    0, NULL, 5,
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_JSVAL,  "key",
    MIR_JSVAL,  "fn",
    MIR_T_I32, "flags");

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

  MIR_type_t special_obj_ret = MIR_JSVAL;
  MIR_item_t special_obj_proto = MIR_new_proto(ctx, "soj_proto",
    1, &special_obj_ret, 3,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_I32, "which");

  MIR_type_t for_of_ret = MIR_JSVAL;
  MIR_item_t for_of_proto = MIR_new_proto(ctx, "fo_proto",
    1, &for_of_ret, 4,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "iterable",
    MIR_T_P,   "iter_buf");

  MIR_item_t destructure_close_proto = MIR_new_proto(ctx, "dclose_proto",
    0, NULL, 3,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "iter_buf");

  MIR_type_t destructure_next_ret = MIR_JSVAL;
  MIR_item_t destructure_next_proto = MIR_new_proto(ctx, "dnext_proto",
    1, &destructure_next_ret, 3,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "iter_buf");

  MIR_item_t imp_add   = MIR_new_import(ctx, "jit_helper_add");
  MIR_item_t imp_sub   = MIR_new_import(ctx, "jit_helper_sub");
  MIR_item_t imp_mul   = MIR_new_import(ctx, "jit_helper_mul");
  MIR_item_t imp_div   = MIR_new_import(ctx, "jit_helper_div");
  MIR_item_t imp_mod   = MIR_new_import(ctx, "jit_helper_mod");
  MIR_item_t imp_str_append_local =
    MIR_new_import(ctx, "jit_helper_str_append_local");
  MIR_item_t imp_str_append_local_snapshot =
    MIR_new_import(ctx, "jit_helper_str_append_local_snapshot");
  MIR_item_t imp_str_flush_local =
    MIR_new_import(ctx, "jit_helper_str_flush_local");
  MIR_item_t imp_lt    = MIR_new_import(ctx, "jit_helper_lt");
  MIR_item_t imp_le    = MIR_new_import(ctx, "jit_helper_le");
  MIR_item_t imp_gt    = MIR_new_import(ctx, "jit_helper_gt");
  MIR_item_t imp_ge    = MIR_new_import(ctx, "jit_helper_ge");
  MIR_item_t imp_call  = MIR_new_import(ctx, "jit_helper_call");
  MIR_item_t imp_apply = MIR_new_import(ctx, "jit_helper_apply");
  MIR_item_t imp_rest  = MIR_new_import(ctx, "jit_helper_rest");
  MIR_item_t imp_special_obj = MIR_new_import(ctx, "jit_helper_special_obj");
  MIR_item_t imp_for_of      = MIR_new_import(ctx, "jit_helper_for_of");
  MIR_item_t imp_dnext       = MIR_new_import(ctx, "jit_helper_destructure_next");
  MIR_item_t imp_dclose      = MIR_new_import(ctx, "jit_helper_destructure_close");
  MIR_item_t imp_gg         = MIR_new_import(ctx, "jit_helper_get_global");
  MIR_item_t imp_get_field  = MIR_new_import(ctx, "jit_helper_get_field");
  MIR_item_t imp_to_propkey = MIR_new_import(ctx, "jit_helper_to_propkey");
  MIR_item_t imp_resume     = MIR_new_import(ctx, "jit_helper_bailout_resume");
  MIR_item_t imp_close_upval = MIR_new_import(ctx, "jit_helper_close_upval");
  MIR_item_t imp_closure     = MIR_new_import(ctx, "jit_helper_closure");
  MIR_item_t imp_in          = MIR_new_import(ctx, "jit_helper_in");
  MIR_item_t imp_get_length  = MIR_new_import(ctx, "jit_helper_get_length");
  MIR_item_t imp_define_field = MIR_new_import(ctx, "jit_helper_define_field");
  MIR_item_t imp_define_method_comp = MIR_new_import(ctx, "jit_helper_define_method_comp");
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
  MIR_item_t imp_call_is_proto = MIR_new_import(ctx, "jit_helper_call_is_proto");
  MIR_item_t imp_delete      = MIR_new_import(ctx, "jit_helper_delete");
  MIR_item_t imp_set_name   = MIR_new_import(ctx, "jit_helper_set_name");
  MIR_item_t imp_stack_ovf      = MIR_new_import(ctx, "jit_helper_stack_overflow");
  MIR_item_t imp_stack_ovf_err  = MIR_new_import(ctx, "jit_helper_stack_overflow_error");

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
  vs.known_const = calloc((size_t)vs.max, sizeof(uint64_t));
  vs.has_const = calloc((size_t)vs.max, sizeof(bool));
  if (!vs.regs || !vs.known_func || !vs.d_regs || !vs.slot_type || !vs.known_const || !vs.has_const) {
    free(vs.regs); free(vs.known_func); free(vs.d_regs); free(vs.slot_type);
    free(vs.known_const); free(vs.has_const);
    MIR_finish_func(ctx); MIR_finish_module(ctx); func->jit_compiling = false; return NULL;
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
  MIR_reg_t *local_d_regs = NULL;
  sv_func_t **known_func_locals = NULL;
  uint8_t *known_type_locals = NULL;
  if (n_locals > 0) {
    local_regs = calloc((size_t)n_locals, sizeof(MIR_reg_t));
    local_d_regs = calloc((size_t)n_locals, sizeof(MIR_reg_t));
    known_func_locals = calloc((size_t)n_locals, sizeof(sv_func_t *));
    known_type_locals = calloc((size_t)n_locals, sizeof(uint8_t));
    if (!local_regs || !local_d_regs || !known_func_locals || !known_type_locals) {
      free(vs.regs); free(vs.known_func); free(vs.d_regs); free(vs.slot_type);
      free(vs.known_const); free(vs.has_const);
      free(local_regs); free(local_d_regs); free(known_func_locals); free(known_type_locals);
      MIR_finish_func(ctx); MIR_finish_module(ctx); return NULL;
    }
    if (func->local_types && func->local_type_count > 0) {
      int ncopy = func->local_type_count < n_locals ? func->local_type_count : n_locals;
      for (int i = 0; i < ncopy; i++)
        known_type_locals[i] = func->local_types[i].type;
    }
    if (func->local_type_feedback) {
      for (int i = 0; i < n_locals; i++) {
        uint8_t ltf = func->local_type_feedback[i];
        if (ltf && !(ltf & ~SV_TFB_NUM))
          known_type_locals[i] = SV_TI_NUM;
      }
    }
    for (int i = 0; i < n_locals; i++) {
      char rname[32], dname[32];
      snprintf(rname, sizeof(rname), "l%d", i);
      snprintf(dname, sizeof(dname), "ld%d", i);
      local_regs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rname);
      local_d_regs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dname);
      mir_load_imm(ctx, jit_func, local_regs[i],
                   mkval(T_UNDEF, 0));
    }
  }

  MIR_reg_t r_tmp  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "tmp");
  MIR_reg_t r_tmp2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "tmp2");
  MIR_reg_t r_bool = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bool_tmp");
  MIR_reg_t r_err_tmp = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "err_tmp");
  mir_load_imm(ctx, jit_func, r_tmp2, 0);

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

  MIR_reg_t r_iter_roots = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "iter_roots");
  if (feat.needs_iter_roots && vs.max > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_iter_roots),
        MIR_new_uint_op(ctx, (uint64_t)vs.max * sizeof(ant_value_t))));
    for (int i = 0; i < vs.max; i++) {
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_mem_op(ctx, MIR_T_I64,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_iter_roots, 0, 1),
          MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
    }
  } else {
    mir_load_imm(ctx, jit_func, r_iter_roots, 0);
  }

  MIR_reg_t r_tco_args = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "tco_args");
  MIR_reg_t r_cond_d = 0, r_cond_nan = 0, r_cond_zd = 0, r_cond_zero = 0;
  bool needs_bailout = feat.needs_bailout;

  MIR_reg_t r_ic_epoch_val = 0;
  if (feat.needs_ic_epoch) {
    r_ic_epoch_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "ic_ep_ptr");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_ic_epoch_val),
        MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)&ant_ic_epoch_counter)));
  }

  MIR_reg_t   r_bailout_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "bail_val");
  MIR_reg_t   r_bailout_off = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bail_off");
  MIR_reg_t   r_bailout_sp  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "bail_sp");
  MIR_label_t bailout_tramp = needs_bailout ? MIR_new_label(ctx) : NULL;

  int param_count = func->param_count;
  bool *captured_params = scan_captured_params(func);
  bool *captured_locals = scan_captured_locals(func, n_locals);
  bool has_captured_params = false;
  bool has_captures = false;
  if (captured_params) {
    for (int i = 0; i < param_count; i++)
      if (captured_params[i]) { has_captured_params = true; break; }
  }
  if (captured_locals) {
    for (int i = 0; i < n_locals; i++)
      if (captured_locals[i]) { has_captures = true; break; }
  }
  bool has_captured_slots = has_captured_params || has_captures;
  bool use_unified_slotbuf = has_captured_slots && has_captures;
  int slotbuf_count = use_unified_slotbuf ? (param_count + n_locals) : param_count;

  if (has_captured_params && needs_bailout) {
    free(vs.regs); free(vs.known_func); free(vs.d_regs); free(vs.slot_type);
    free(vs.known_const); free(vs.has_const);
    free(local_regs); free(local_d_regs); free(known_func_locals); free(known_type_locals);
    free(captured_params); free(captured_locals);
    MIR_finish_func(ctx); MIR_finish_module(ctx); func->jit_compiling = false; return NULL;
  }

  MIR_reg_t r_slotbuf = r_tmp2;
  if (has_captured_slots && slotbuf_count > 0) {
    r_slotbuf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "slotbuf");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_slotbuf),
        MIR_new_uint_op(ctx, (uint64_t)slotbuf_count * sizeof(ant_value_t))));
    mir_emit_fill_param_slots_from_args(ctx, jit_func, r_slotbuf, r_args, r_argc, captured_params, param_count);
  }

  bool needs_lbuf = needs_bailout || feat.needs_close_upval || has_captures;
  MIR_reg_t r_lbuf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "lbuf");
  if (use_unified_slotbuf && n_locals > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ADD,
        MIR_new_reg_op(ctx, r_lbuf),
        MIR_new_reg_op(ctx, r_slotbuf),
        MIR_new_int_op(ctx, (int64_t)param_count * (int64_t)sizeof(ant_value_t))));
  } else if (needs_lbuf && n_locals > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_lbuf),
        MIR_new_uint_op(ctx, (uint64_t)n_locals * sizeof(ant_value_t))));
  } else {
    mir_load_imm(ctx, jit_func, r_lbuf, 0);
  }

  if (feat.needs_tco_args && param_count > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_tco_args),
        MIR_new_uint_op(ctx, (uint64_t)param_count * sizeof(ant_value_t))));
  } else mir_load_imm(ctx, jit_func, r_tco_args, 0);
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
        if (vs.has_const)
          memset(vs.has_const, 0, (size_t)vs.max * sizeof(bool));
        if (known_type_locals && local_d_regs) {
          for (int li = 0; li < n_locals; li++)
            if (known_type_locals[li] == SV_TI_NUM)
              mir_i64_to_d(ctx, jit_func, local_d_regs[li],
                           local_regs[li], r_d_slot);
        }
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
        mir_load_imm(ctx, jit_func, vstack_push_const(&vs, mkval(T_UNDEF, 0)), mkval(T_UNDEF, 0));
        break;
      case OP_NULL:
        mir_load_imm(ctx, jit_func, vstack_push_const(&vs, mkval(T_NULL, 0)), mkval(T_NULL, 0));
        break;
      case OP_TRUE:
        mir_load_imm(ctx, jit_func, vstack_push_const(&vs, js_true), js_true);
        break;
      case OP_FALSE:
        mir_load_imm(ctx, jit_func, vstack_push_const(&vs, js_false), js_false);
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
        if (has_captured_params && captured_params && idx < (uint16_t)param_count && captured_params[idx]) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                r_slotbuf, 0, 1)));
        } else {
          MIR_label_t arg_in_range = MIR_new_label(ctx);
          MIR_label_t arg_done = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_UBGT,
              MIR_new_label_op(ctx, arg_in_range),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_int_op(ctx, (int64_t)idx)));
          mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, arg_done)));
          MIR_append_insn(ctx, jit_func, arg_in_range);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                r_args, 0, 1)));
          MIR_append_insn(ctx, jit_func, arg_done);
        }
        break;
      }

      case OP_SET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_top(&vs);
        if (has_captured_params && captured_params && idx < (uint16_t)param_count && captured_params[idx]) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                r_slotbuf, 0, 1),
              MIR_new_reg_op(ctx, val)));
          if (idx < (uint16_t)param_count) {
            MIR_label_t arg_in_range = MIR_new_label(ctx);
            MIR_label_t arg_done = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_UBGT,
                MIR_new_label_op(ctx, arg_in_range),
                MIR_new_reg_op(ctx, r_argc),
                MIR_new_int_op(ctx, (int64_t)idx)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, arg_done)));
            MIR_append_insn(ctx, jit_func, arg_in_range);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_JSVAL,
                  (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                  r_args, 0, 1),
                MIR_new_reg_op(ctx, val)));
            MIR_append_insn(ctx, jit_func, arg_done);
          }
        } else {
          MIR_label_t arg_in_range = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_UBGT,
              MIR_new_label_op(ctx, arg_in_range),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_int_op(ctx, (int64_t)idx)));
          mir_load_imm(ctx, jit_func, r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
          mir_emit_bailout_check(ctx, jit_func, r_bailout_val,
            0, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, arg_in_range);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                r_args, 0, 1),
              MIR_new_reg_op(ctx, val)));
        }
        break;
      }

      case OP_REST: {
        uint16_t start = sv_get_u16(ip + 1);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, rest_proto),
            MIR_new_ref_op(ctx, imp_rest),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_args),
            MIR_new_reg_op(ctx, r_argc),
            MIR_new_int_op(ctx, (int64_t)start)));
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
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
              MIR_new_reg_op(ctx, local_d_regs[idx])));
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
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
              MIR_new_reg_op(ctx, local_d_regs[idx])));
          if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_NUM;
        }
        break;
      }

      case OP_GET_SLOT_RAW: {
        uint16_t slot_idx = sv_get_u16(ip + 1);
        if ((int)slot_idx < param_count) {
          uint16_t idx = slot_idx;
          MIR_reg_t dst = vstack_push(&vs);
          if (has_captured_params && captured_params && idx < (uint16_t)param_count &&
              captured_params[idx]) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, dst),
                MIR_new_mem_op(ctx, MIR_JSVAL,
                  (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                  r_slotbuf, 0, 1)));
          } else {
            MIR_label_t arg_in_range = MIR_new_label(ctx);
            MIR_label_t arg_done = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_UBGT,
                MIR_new_label_op(ctx, arg_in_range),
                MIR_new_reg_op(ctx, r_argc),
                MIR_new_int_op(ctx, (int64_t)idx)));
            mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, arg_done)));
            MIR_append_insn(ctx, jit_func, arg_in_range);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, dst),
                MIR_new_mem_op(ctx, MIR_JSVAL,
                  (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                  r_args, 0, 1)));
            MIR_append_insn(ctx, jit_func, arg_done);
          }
        } else {
          uint16_t idx = (uint16_t)(slot_idx - (uint16_t)param_count);
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
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, vs.d_regs[vs.sp - 1]),
                MIR_new_reg_op(ctx, local_d_regs[idx])));
            if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_NUM;
          }
        }
        break;
      }

      case OP_PUT_LOCAL: {
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)n_locals) { ok = false; break; }
        sv_func_t *kf = vs.known_func[vs.sp - 1];
        bool src_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;
        MIR_reg_t src_d = src_is_num ? vs.d_regs[vs.sp - 1] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          if (src_is_num)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, local_d_regs[idx]),
                MIR_new_reg_op(ctx, src_d)));
          else
            mir_i64_to_d(ctx, jit_func, local_d_regs[idx], local_regs[idx], r_d_slot);
        }
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
        bool src_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;
        MIR_reg_t src_d = src_is_num ? vs.d_regs[vs.sp - 1] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          if (src_is_num)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, local_d_regs[idx]),
                MIR_new_reg_op(ctx, src_d)));
          else
            mir_i64_to_d(ctx, jit_func, local_d_regs[idx], local_regs[idx], r_d_slot);
        }
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
        bool src_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;
        MIR_reg_t src_d = src_is_num ? vs.d_regs[vs.sp - 1] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          if (src_is_num)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, local_d_regs[idx]),
                MIR_new_reg_op(ctx, src_d)));
          else
            mir_i64_to_d(ctx, jit_func, local_d_regs[idx], local_regs[idx], r_d_slot);
        }
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
        bool src_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;
        MIR_reg_t src_d = src_is_num ? vs.d_regs[vs.sp - 1] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          if (src_is_num)
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_DMOV,
                MIR_new_reg_op(ctx, local_d_regs[idx]),
                MIR_new_reg_op(ctx, src_d)));
          else
            mir_i64_to_d(ctx, jit_func, local_d_regs[idx], local_regs[idx], r_d_slot);
        }
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
        bool kc = vs.has_const && vs.has_const[vs.sp - 1];
        uint64_t kcv = kc ? vs.known_const[vs.sp - 1] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t top = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        vs.known_func[vs.sp - 1] = kf;
        if (kc && vs.has_const) { vs.has_const[vs.sp - 1] = true; vs.known_const[vs.sp - 1] = kcv; }
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
        if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_BOXED;
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

      case OP_POST_INC: {
        int top_idx = vs.sp - 1;
        vstack_ensure_boxed(&vs, top_idx, ctx, jit_func, r_d_slot);

        MIR_reg_t rold = vstack_top(&vs);
        MIR_reg_t rnew = vstack_push(&vs);

        int pin = arith_n++;
        char pi_d1[32], pi_d2[32];
        snprintf(pi_d1, sizeof(pi_d1), "pi_d1_%d", pin);
        snprintf(pi_d2, sizeof(pi_d2), "pi_d2_%d", pin);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, pi_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, pi_d2);

        mir_i64_to_d(ctx, jit_func, fd1, rold, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DADD,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, r_d_one)));
        mir_d_to_i64(ctx, jit_func, rnew, fd2, r_d_slot);
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

      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_FALSE:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE:
      case OP_JMP_TRUE8: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        bool is_peek = (op == OP_JMP_FALSE_PEEK || op == OP_JMP_TRUE_PEEK);
        MIR_reg_t cond = is_peek ? vstack_top(&vs) : vstack_pop(&vs);
        bool short_op = (op == OP_JMP_FALSE8 || op == OP_JMP_TRUE8);
        bool is_false_branch = (op == OP_JMP_FALSE || op == OP_JMP_FALSE8
                                || op == OP_JMP_FALSE_PEEK);
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
          if (!inline_callee)
            inline_callee = sv_tfb_get_call_target(func, bc_off);
          bool speculative = (inline_callee && !vs.known_func[vs.sp - call_argc - 1]);
          if (inline_callee && jit_inlineable(inline_callee)
              && jit_inline_body_feasible(inline_callee)) {
            int cn = call_n++;

            MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
            for (int i = (int)call_argc - 1; i >= 0; i--)
              inl_arg_regs[i] = vstack_pop(&vs);
            MIR_reg_t r_inl_callee = vstack_pop(&vs); 

            MIR_reg_t r_call_res = vstack_push(&vs);

            MIR_label_t inl_slow = MIR_new_label(ctx);
            MIR_label_t inl_join = MIR_new_label(ctx);

            MIR_reg_t r_inl_cl = 0;
            char inl_this_rn[32], inl_flags_rn[32], inl_bound_rn[32];
            snprintf(inl_this_rn, sizeof(inl_this_rn), "inl%d_this", cn);
            snprintf(inl_flags_rn, sizeof(inl_flags_rn), "inl%d_flags", cn);
            snprintf(inl_bound_rn, sizeof(inl_bound_rn), "inl%d_bound", cn);
            MIR_reg_t r_inl_this = MIR_new_func_reg(ctx, jit_func->u.func,
                                                    MIR_JSVAL, inl_this_rn);
            MIR_reg_t r_inl_flags = MIR_new_func_reg(ctx, jit_func->u.func,
                                                     MIR_T_I64, inl_flags_rn);
            MIR_reg_t r_inl_bound = MIR_new_func_reg(ctx, jit_func->u.func,
                                                     MIR_JSVAL, inl_bound_rn);
            {
              char cl_rn[32]; snprintf(cl_rn, sizeof(cl_rn), "inl%d_cl", cn);
              r_inl_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cl_rn);
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_AND,
                  MIR_new_reg_op(ctx, r_inl_cl),
                  MIR_new_reg_op(ctx, r_inl_callee),
                  MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
            }

            if (speculative) {
              char gt_rn[32]; snprintf(gt_rn, sizeof(gt_rn), "inl%d_gt", cn);
              MIR_reg_t r_guard_tag = MIR_new_func_reg(ctx, jit_func->u.func,
                                                        MIR_T_I64, gt_rn);
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_URSH,
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_reg_op(ctx, r_inl_callee),
                  MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_BNE,
                  MIR_new_label_op(ctx, inl_slow),
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));

              char gf_rn[32]; snprintf(gf_rn, sizeof(gf_rn), "inl%d_gf", cn);
              MIR_reg_t r_guard_fn = MIR_new_func_reg(ctx, jit_func->u.func,
                                                       MIR_T_I64, gf_rn);
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, r_guard_fn),
                  MIR_new_mem_op(ctx, MIR_T_P,
                    (MIR_disp_t)offsetof(sv_closure_t, func),
                    r_inl_cl, 0, 1)));
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_BNE,
                  MIR_new_label_op(ctx, inl_slow),
                  MIR_new_reg_op(ctx, r_guard_fn),
                  MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)inline_callee)));
            }

            mir_emit_resolve_call_this(ctx, jit_func, r_inl_this, r_inl_cl,
                                       r_this, r_inl_flags, r_inl_bound);

            bool inlined = jit_emit_inline_body(
              ctx, jit_func, inline_callee,
              inl_arg_regs, (int)call_argc,
              r_call_res, inl_slow, inl_join,
              r_bool, &r_d_slot, cn,
              r_inl_cl, r_inl_this,
              r_vm, r_js,
              helper2_proto, imp_seq, imp_sne, imp_eq, imp_ne,
              gf_proto, imp_get_field,
              gg_proto, imp_gg);

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

            MIR_append_insn(ctx, jit_func, inl_slow);
            MIR_append_insn(ctx, jit_func, inl_join);
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
            mir_emit_self_tail(ctx, jit_func, (int)call_argc, param_count,
                               r_tco_args, r_arg_arr, r_args, r_argc,
                               local_regs, n_locals, has_captured_slots, r_slotbuf, captured_params,
                               has_captures,
                               captured_locals, r_lbuf, self_tail_entry);
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
        mir_emit_resolve_call_this(ctx, jit_func, r_call_this, r_callee_cl,
                                   r_call_this, r_bool, r_tmp2);

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
          mir_emit_self_tail(ctx, jit_func, (int)call_argc, param_count,
                             r_tco_args, r_arg_arr, r_args, r_argc,
                             local_regs, n_locals, has_captured_slots, r_slotbuf, captured_params,
                             has_captures,
                             captured_locals, r_lbuf, self_tail_entry);
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

      case OP_SPECIAL_OBJ: {
        // TODO: RE_ENABLE once SPECIAL_OBJ semantics match the interpreter in JIT.
        uint8_t which = sv_get_u8(ip + 1);
        MIR_reg_t dst = vstack_push(&vs);
        if (which == 2 || which == 3) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 6,
              MIR_new_ref_op(ctx, special_obj_proto),
              MIR_new_ref_op(ctx, imp_special_obj),
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_int_op(ctx, (int64_t)which)));
        } else mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
        break;
      }

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
        if (has_captures && n_locals > 0 && r_lbuf) {
          for (int i = 0; i < n_locals; i++)
            if (captured_locals[i])
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
        }
        if (has_captured_params && idx < (uint16_t)param_count)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_slotbuf, captured_params, (int)idx, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, captured_locals,
            idx >= (uint16_t)param_count ? (int)idx - param_count : 0, n_locals);
        break;
      }

      case OP_GET_GLOBAL:
      case OP_GET_GLOBAL_UNDEF: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t dst = vstack_push(&vs);

        if (vs.known_func) {
          ant_value_t gv = jit_helper_get_global(js, atom->str, func, bc_off);
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
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, gg_proto),
            MIR_new_ref_op(ctx, imp_gg),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        break;
      }

      case OP_RETURN: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t ret_val = vstack_pop(&vs);
        if (has_captured_slots)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_slotbuf, captured_params, 0, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, captured_locals, 0, n_locals);
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, ret_val)));
        break;
      }

      case OP_RETURN_UNDEF: {
        if (has_captured_slots)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_slotbuf, captured_params, 0, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, captured_locals, 0, n_locals);
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1,
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        break;
      }

      case OP_INC_LOCAL: {
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)n_locals) { ok = false; break; }
        if (known_func_locals) known_func_locals[idx] = NULL;
        bool loc_is_num = known_type_locals && known_type_locals[idx] == SV_TI_NUM;
        int in = arith_n++;
        char il_d1[32], il_d2[32];
        snprintf(il_d1, sizeof(il_d1), "il_d1_%d", in);
        snprintf(il_d2, sizeof(il_d2), "il_d2_%d", in);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, il_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, il_d2);
        if (loc_is_num && local_d_regs)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, local_d_regs[idx])));
        else
          mir_i64_to_d(ctx, jit_func, fd1, local_regs[idx], r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DADD,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, r_d_one)));
        if (local_d_regs)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, local_d_regs[idx]),
              MIR_new_reg_op(ctx, fd2)));
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
        bool loc_is_num = known_type_locals && known_type_locals[idx] == SV_TI_NUM;
        int dn = arith_n++;
        char dl_d1[32], dl_d2[32];
        snprintf(dl_d1, sizeof(dl_d1), "dl_d1_%d", dn);
        snprintf(dl_d2, sizeof(dl_d2), "dl_d2_%d", dn);
        MIR_reg_t fd1 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dl_d1);
        MIR_reg_t fd2 = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, dl_d2);
        if (loc_is_num && local_d_regs)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, fd1),
              MIR_new_reg_op(ctx, local_d_regs[idx])));
        else
          mir_i64_to_d(ctx, jit_func, fd1, local_regs[idx], r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DSUB,
            MIR_new_reg_op(ctx, fd2),
            MIR_new_reg_op(ctx, fd1),
            MIR_new_reg_op(ctx, r_d_one)));
        if (local_d_regs)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, local_d_regs[idx]),
              MIR_new_reg_op(ctx, fd2)));
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        if (local_d_regs)
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DMOV,
              MIR_new_reg_op(ctx, local_d_regs[idx]),
              MIR_new_reg_op(ctx, fd3)));
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

      case OP_STR_APPEND_LOCAL: {
        uint16_t slot_idx = sv_get_u16(ip + 1);
        int pre_op_sp = vs.sp;
        if ((int)slot_idx < param_count) {
          MIR_label_t arg_in_range = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_UBGT,
              MIR_new_label_op(ctx, arg_in_range),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_int_op(ctx, (int64_t)slot_idx)));
          mir_load_imm(ctx, jit_func, r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
          mir_emit_bailout_check(ctx, jit_func, r_bailout_val,
            0, r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, arg_in_range);

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          MIR_reg_t rhs = vstack_pop(&vs);

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 11,
              MIR_new_ref_op(ctx, str_append_local_proto),
              MIR_new_ref_op(ctx, imp_str_append_local),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_reg_op(ctx, r_args),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)slot_idx),
              MIR_new_reg_op(ctx, rhs)));
        } else {
          uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)param_count);
          if (local_idx >= (uint16_t)n_locals) { ok = false; break; }

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          MIR_reg_t rhs = vstack_pop(&vs);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[local_idx])));

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 11,
              MIR_new_ref_op(ctx, str_append_local_proto),
              MIR_new_ref_op(ctx, imp_str_append_local),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)param_count),
              MIR_new_reg_op(ctx, r_lbuf),
              MIR_new_int_op(ctx, (int64_t)slot_idx),
              MIR_new_reg_op(ctx, rhs)));

          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, local_regs[i]),
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          }

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, local_regs[local_idx]),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          if (known_func_locals) known_func_locals[local_idx] = NULL;
          if (known_type_locals) known_type_locals[local_idx] = SV_TI_UNKNOWN;
        }

        mir_emit_bailout_check(ctx, jit_func, r_err_tmp,
          0, r_bailout_off, bc_off,
          r_bailout_sp, pre_op_sp, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);

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

      case OP_STR_ALC_SNAPSHOT: {
        uint16_t slot_idx = sv_get_u16(ip + 1);
        int pre_op_sp = vs.sp;
        if ((int)slot_idx < param_count) {
          MIR_label_t arg_in_range = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_UBGT,
              MIR_new_label_op(ctx, arg_in_range),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_int_op(ctx, (int64_t)slot_idx)));
          mir_load_imm(ctx, jit_func, r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
          mir_emit_bailout_check(ctx, jit_func, r_bailout_val,
            0, r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);
          MIR_append_insn(ctx, jit_func, arg_in_range);

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
          MIR_reg_t rhs = vstack_pop(&vs);
          MIR_reg_t lhs = vstack_pop(&vs);

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 12,
              MIR_new_ref_op(ctx, str_append_local_snapshot_proto),
              MIR_new_ref_op(ctx, imp_str_append_local_snapshot),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_reg_op(ctx, r_args),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)slot_idx),
              MIR_new_reg_op(ctx, lhs),
              MIR_new_reg_op(ctx, rhs)));
        } else {
          uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)param_count);
          if (local_idx >= (uint16_t)n_locals) { ok = false; break; }

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
          MIR_reg_t rhs = vstack_pop(&vs);
          MIR_reg_t lhs = vstack_pop(&vs);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[local_idx])));

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 12,
              MIR_new_ref_op(ctx, str_append_local_snapshot_proto),
              MIR_new_ref_op(ctx, imp_str_append_local_snapshot),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)param_count),
              MIR_new_reg_op(ctx, r_lbuf),
              MIR_new_int_op(ctx, (int64_t)slot_idx),
              MIR_new_reg_op(ctx, lhs),
              MIR_new_reg_op(ctx, rhs)));

          if (has_captures) {
            for (int i = 0; i < n_locals; i++)
              if (captured_locals[i])
                MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_reg_op(ctx, local_regs[i]),
                    MIR_new_mem_op(ctx, MIR_T_I64,
                      (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          }

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, local_regs[local_idx]),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          if (known_func_locals) known_func_locals[local_idx] = NULL;
          if (known_type_locals) known_type_locals[local_idx] = SV_TI_UNKNOWN;
        }

        mir_emit_bailout_check(ctx, jit_func, r_err_tmp,
          0, r_bailout_off, bc_off,
          r_bailout_sp, pre_op_sp, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);

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

      case OP_TO_PROPKEY: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        uint16_t ic_idx = sv_get_u16(ip + 5);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_label_t slow = MIR_new_label(ctx);
        bool has_fast_path = mir_emit_get_field_ic_fastpath(
          ctx, jit_func, func, bc_off, ic_idx, atom, obj, dst, slow, r_ic_epoch_val);
        if (has_fast_path) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, no_err)));
          MIR_append_insn(ctx, jit_func, slow);
        }
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t obj = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        uint16_t ic_idx = sv_get_u16(ip + 5);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_label_t slow = MIR_new_label(ctx);
        bool has_fast_path = mir_emit_get_field_ic_fastpath(
          ctx, jit_func, func, bc_off, ic_idx, atom, obj, dst, slow, r_ic_epoch_val);
        if (has_fast_path) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, no_err)));
          MIR_append_insn(ctx, jit_func, slow);
        }
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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

      case OP_DEFINE_METHOD_COMP: {
        uint8_t flags = sv_get_u8(ip + 1);
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        MIR_reg_t fn_val = vstack_pop(&vs);
        MIR_reg_t key = vstack_pop(&vs);
        MIR_reg_t obj = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, define_method_comp_proto),
            MIR_new_ref_op(ctx, imp_define_method_comp),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, key),
            MIR_new_reg_op(ctx, fn_val),
            MIR_new_int_op(ctx, (int64_t)flags)));
        break;
      }

      case OP_GET_ELEM: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
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

      case OP_FOR_OF:
      case OP_DESTRUCTURE_INIT: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t iterable = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, for_of_proto),
            MIR_new_ref_op(ctx, imp_for_of),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, iterable),
            MIR_new_reg_op(ctx, r_args_buf)));
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
        for (int i = 0; i < 3; i++) {
          MIR_reg_t dst = vstack_push(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((vs.sp - 1) * (int)sizeof(ant_value_t)), r_iter_roots, 0, 1),
              MIR_new_reg_op(ctx, dst)));
        }
        break;
      }

      case OP_DESTRUCTURE_NEXT: {
        int iter_base = vs.sp - 3;
        for (int i = 0; i < 3; i++) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((iter_base + i) * (int)sizeof(ant_value_t)), r_iter_roots, 0, 1)));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, destructure_next_proto),
            MIR_new_ref_op(ctx, imp_dnext),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_args_buf)));
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
        vstack_pop(&vs);
        vstack_pop(&vs);
        vstack_pop(&vs);
        for (int i = 0; i < 4; i++) {
          MIR_reg_t dst = vstack_push(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1)));
          if (i < 3) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)((vs.sp - 1) * (int)sizeof(ant_value_t)), r_iter_roots, 0, 1),
                MIR_new_reg_op(ctx, dst)));
          }
        }
        break;
      }

      case OP_DESTRUCTURE_CLOSE: {
        int iter_base = vs.sp - 3;
        for (int i = 0; i < 3; i++) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((iter_base + i) * (int)sizeof(ant_value_t)), r_iter_roots, 0, 1)));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 5,
            MIR_new_ref_op(ctx, destructure_close_proto),
            MIR_new_ref_op(ctx, imp_dclose),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_args_buf)));
        vstack_pop(&vs);
        vstack_pop(&vs);
        vstack_pop(&vs);
        break;
      }

      case OP_PUT_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        for (int i = 0; i < (int)n; i++)
          vstack_ensure_boxed(&vs, vs.sp - 1 - i, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        uint16_t ic_idx = sv_get_u16(ip + 1);
        bool has_ic = func->ic_slots && ic_idx < func->ic_count;
        MIR_label_t slow = has_ic ? MIR_new_label(ctx) : NULL;
        MIR_label_t no_err = MIR_new_label(ctx);

        if (has_ic) {
          sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
          char inst_ic_name[32], inst_ice_name[32];
          char inst_rt_name[32], inst_ro_name[32], inst_ica_name[32], inst_lt_name[32];
          char inst_lo_name[32], inst_ls_name[32], inst_ics_name[32], inst_ici_name[32];
          snprintf(inst_ic_name, sizeof(inst_ic_name), "inst_ic_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ice_name, sizeof(inst_ice_name), "inst_ice_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_rt_name, sizeof(inst_rt_name), "inst_rt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ro_name, sizeof(inst_ro_name), "inst_ro_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ica_name, sizeof(inst_ica_name), "inst_ica_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lt_name, sizeof(inst_lt_name), "inst_lt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lo_name, sizeof(inst_lo_name), "inst_lo_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ls_name, sizeof(inst_ls_name), "inst_ls_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ics_name, sizeof(inst_ics_name), "inst_ics_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ici_name, sizeof(inst_ici_name), "inst_ici_%d_%u", bc_off, (unsigned)ic_idx);

          MIR_reg_t r_ic = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ic_name);
          MIR_reg_t r_ic_epoch = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ice_name);
          MIR_reg_t r_rhs_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_rt_name);
          MIR_reg_t r_rhs_obj = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ro_name);
          MIR_reg_t r_ic_aux = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ica_name);
          MIR_reg_t r_lhs_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lt_name);
          MIR_reg_t r_lhs_obj = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lo_name);
          MIR_reg_t r_lhs_shape = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ls_name);
          MIR_reg_t r_ic_shape = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ics_name);
          MIR_reg_t r_ic_idx_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ici_name);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_epoch),
              MIR_new_mem_op(ctx, MIR_T_I32,
                (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
          {
            char ice_cur_name[40];
            snprintf(ice_cur_name, sizeof(ice_cur_name), "inst_ce_%d_%u", bc_off, (unsigned)ic_idx);
            MIR_reg_t r_cur_ep = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, ice_cur_name);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_cur_ep),
                MIR_new_mem_op(ctx, MIR_T_U32, 0, r_ic_epoch_val, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, slow),
                MIR_new_reg_op(ctx, r_ic_epoch),
                MIR_new_reg_op(ctx, r_cur_ep)));
          }

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_URSH,
              MIR_new_reg_op(ctx, r_rhs_tag),
              MIR_new_reg_op(ctx, rr),
              MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_rhs_tag),
              MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_AND,
              MIR_new_reg_op(ctx, r_rhs_obj),
              MIR_new_reg_op(ctx, rr),
              MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_aux),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_rhs_obj),
              MIR_new_reg_op(ctx, r_ic_aux)));

          mir_emit_value_to_objptr_or_jmp(
            ctx, jit_func, rl, r_lhs_obj, r_lhs_tag, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_lhs_shape),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(ant_object_t, shape), r_lhs_obj, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_shape),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_lhs_shape),
              MIR_new_reg_op(ctx, r_ic_shape)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_idx_val),
              MIR_new_mem_op(ctx, MIR_T_I32,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, dst),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_ic_idx_val)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, no_err)));

          MIR_append_insn(ctx, jit_func, slow);
        }

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, inst_proto),
            MIR_new_ref_op(ctx, imp_instanceof),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, rl),
            MIR_new_reg_op(ctx, rr),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        if (has_captures) {
          for (int i = 0; i < n_locals; i++)
            if (captured_locals[i])
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, local_regs[i]),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        }
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

      case OP_CALL_IS_PROTO: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        MIR_reg_t arg = vstack_pop(&vs);
        MIR_reg_t fn = vstack_pop(&vs);
        MIR_reg_t this_obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        uint16_t ic_idx = sv_get_u16(ip + 1);
        bool has_ic = func->ic_slots && ic_idx < func->ic_count;
        MIR_label_t slow = has_ic ? MIR_new_label(ctx) : NULL;
        MIR_label_t no_err = MIR_new_label(ctx);

        if (has_ic) {
          sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
          char cip_bi_name[32], cip_ic_name[32], cip_ice_name[32];
          char cip_pt_name[32], cip_pp_name[32], cip_ot_name[32];
          char cip_op_name[32], cip_ich_name[32], cip_ics_name[32], cip_ici_name[32];
          snprintf(cip_bi_name, sizeof(cip_bi_name), "cip_bi_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ic_name, sizeof(cip_ic_name), "cip_ic_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ice_name, sizeof(cip_ice_name), "cip_ice_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_pt_name, sizeof(cip_pt_name), "cip_pt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_pp_name, sizeof(cip_pp_name), "cip_pp_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ot_name, sizeof(cip_ot_name), "cip_ot_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_op_name, sizeof(cip_op_name), "cip_op_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ich_name, sizeof(cip_ich_name), "cip_ich_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ics_name, sizeof(cip_ics_name), "cip_ics_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(cip_ici_name, sizeof(cip_ici_name), "cip_ici_%d_%u", bc_off, (unsigned)ic_idx);

          MIR_reg_t r_builtin = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_bi_name);
          MIR_reg_t r_ic = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ic_name);
          MIR_reg_t r_ic_epoch = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ice_name);
          MIR_reg_t r_proto_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_pt_name);
          MIR_reg_t r_proto_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_pp_name);
          MIR_reg_t r_obj_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ot_name);
          MIR_reg_t r_obj_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_op_name);
          MIR_reg_t r_ic_holder = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ich_name);
          MIR_reg_t r_ic_shape = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ics_name);
          MIR_reg_t r_ic_idx_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ici_name);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_builtin),
              MIR_new_uint_op(ctx, (uint64_t)js_mkfun(builtin_object_isPrototypeOf))));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, fn),
              MIR_new_reg_op(ctx, r_builtin)));

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_epoch),
              MIR_new_mem_op(ctx, MIR_T_I32,
                (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
          {
            char cip_ce_name[40];
            snprintf(cip_ce_name, sizeof(cip_ce_name), "cip_ce_%d_%u", bc_off, (unsigned)ic_idx);
            MIR_reg_t r_cur_ep = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_ce_name);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_cur_ep),
                MIR_new_mem_op(ctx, MIR_T_U32, 0, r_ic_epoch_val, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, slow),
                MIR_new_reg_op(ctx, r_ic_epoch),
                MIR_new_reg_op(ctx, r_cur_ep)));
          }

          mir_emit_value_to_objptr_or_jmp(
            ctx, jit_func, this_obj, r_proto_ptr, r_proto_tag, slow);
          mir_emit_value_to_objptr_or_jmp(
            ctx, jit_func, arg, r_obj_ptr, r_obj_tag, slow);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_holder),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_proto_ptr),
              MIR_new_reg_op(ctx, r_ic_holder)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_shape),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_obj_ptr),
              MIR_new_reg_op(ctx, r_ic_shape)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_idx_val),
              MIR_new_mem_op(ctx, MIR_T_I32,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_OR,
              MIR_new_reg_op(ctx, dst),
              MIR_new_uint_op(ctx, js_false),
              MIR_new_reg_op(ctx, r_ic_idx_val)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, no_err)));

          MIR_append_insn(ctx, jit_func, slow);
        }

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, call_is_proto),
            MIR_new_ref_op(ctx, imp_call_is_proto),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, this_obj),
            MIR_new_reg_op(ctx, fn),
            MIR_new_reg_op(ctx, arg),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off)));
        if (has_captures) {
          for (int i = 0; i < n_locals; i++)
            if (captured_locals[i])
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, local_regs[i]),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
        }
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

        char gl_tag_name[32], gl_ptr_name[32], gl_len_name[32], gl_dbl_name[32];
        snprintf(gl_tag_name, sizeof(gl_tag_name), "gl_tag_%d", bc_off);
        snprintf(gl_ptr_name, sizeof(gl_ptr_name), "gl_ptr_%d", bc_off);
        snprintf(gl_len_name, sizeof(gl_len_name), "gl_len_%d", bc_off);
        snprintf(gl_dbl_name, sizeof(gl_dbl_name), "gl_dbl_%d", bc_off);
        MIR_reg_t gl_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, gl_tag_name);
        MIR_reg_t gl_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, gl_ptr_name);
        MIR_reg_t gl_len = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, gl_len_name);
        MIR_reg_t gl_dbl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, gl_dbl_name);
        MIR_label_t gl_slow = MIR_new_label(ctx);
        MIR_label_t gl_done = MIR_new_label(ctx);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, gl_tag),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, gl_slow),
            MIR_new_reg_op(ctx, gl_tag),
            MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_AND,
            MIR_new_reg_op(ctx, gl_ptr),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, gl_len),
            MIR_new_mem_op(ctx, MIR_T_U32,
              (MIR_disp_t)offsetof(ant_object_t, u.array.len),
              gl_ptr, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_UI2D,
            MIR_new_reg_op(ctx, gl_dbl),
            MIR_new_reg_op(ctx, gl_len)));
        mir_d_to_i64(ctx, jit_func, dst, gl_dbl, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP,
            MIR_new_label_op(ctx, gl_done)));

        MIR_append_insn(ctx, jit_func, gl_slow);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_get_length),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj)));
        MIR_append_insn(ctx, jit_func, gl_done);
        break;
      }

      case OP_SEQ: {
        bool r_const = vs.has_const && vs.has_const[vs.sp - 1];
        bool l_const = vs.has_const && vs.has_const[vs.sp - 2];
        uint64_t cval = r_const ? vs.known_const[vs.sp - 1]
                      : l_const ? vs.known_const[vs.sp - 2] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        if (r_const || l_const) {
          MIR_reg_t other = r_const ? rl : rr;
          MIR_label_t is_true = MIR_new_label(ctx);
          MIR_label_t is_done = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_true),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, cval)));
          mir_load_imm(ctx, jit_func, dst, js_false);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
          MIR_append_insn(ctx, jit_func, is_true);
          mir_load_imm(ctx, jit_func, dst, js_true);
          MIR_append_insn(ctx, jit_func, is_done);
        } else {
          mir_call_helper2(ctx, jit_func, dst,
                           helper2_proto, imp_seq,
                           r_vm, r_js, rl, rr);
        }
        break;
      }

      case OP_EQ: {
        bool r_const = vs.has_const && vs.has_const[vs.sp - 1];
        bool l_const = vs.has_const && vs.has_const[vs.sp - 2];
        uint64_t cval = r_const ? vs.known_const[vs.sp - 1]
                      : l_const ? vs.known_const[vs.sp - 2] : 0;
        bool is_nullish = (r_const || l_const) &&
          (cval == mkval(T_NULL, 0) || cval == mkval(T_UNDEF, 0));
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        if (is_nullish) {
          MIR_reg_t other = r_const ? rl : rr;
          MIR_label_t is_true = MIR_new_label(ctx);
          MIR_label_t is_done = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_true),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_true),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
          mir_load_imm(ctx, jit_func, dst, js_false);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
          MIR_append_insn(ctx, jit_func, is_true);
          mir_load_imm(ctx, jit_func, dst, js_true);
          MIR_append_insn(ctx, jit_func, is_done);
        } else {
          mir_call_helper2(ctx, jit_func, dst,
                           helper2_proto, imp_eq,
                           r_vm, r_js, rl, rr);
        }
        break;
      }

      case OP_NE: {
        bool r_const = vs.has_const && vs.has_const[vs.sp - 1];
        bool l_const = vs.has_const && vs.has_const[vs.sp - 2];
        uint64_t cval = r_const ? vs.known_const[vs.sp - 1]
                      : l_const ? vs.known_const[vs.sp - 2] : 0;
        bool is_nullish = (r_const || l_const) &&
          (cval == mkval(T_NULL, 0) || cval == mkval(T_UNDEF, 0));
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        if (is_nullish) {
          MIR_reg_t other = r_const ? rl : rr;
          MIR_label_t is_false = MIR_new_label(ctx);
          MIR_label_t is_done = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_false),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_false),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
          mir_load_imm(ctx, jit_func, dst, js_true);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
          MIR_append_insn(ctx, jit_func, is_false);
          mir_load_imm(ctx, jit_func, dst, js_false);
          MIR_append_insn(ctx, jit_func, is_done);
        } else {
          mir_call_helper2(ctx, jit_func, dst,
                           helper2_proto, imp_ne,
                           r_vm, r_js, rl, rr);
        }
        break;
      }

      case OP_SNE: {
        bool r_const = vs.has_const && vs.has_const[vs.sp - 1];
        bool l_const = vs.has_const && vs.has_const[vs.sp - 2];
        uint64_t cval = r_const ? vs.known_const[vs.sp - 1]
                      : l_const ? vs.known_const[vs.sp - 2] : 0;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        if (r_const || l_const) {
          MIR_reg_t other = r_const ? rl : rr;
          MIR_label_t is_false = MIR_new_label(ctx);
          MIR_label_t is_done = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, is_false),
              MIR_new_reg_op(ctx, other),
              MIR_new_uint_op(ctx, cval)));
          mir_load_imm(ctx, jit_func, dst, js_true);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
          MIR_append_insn(ctx, jit_func, is_false);
          mir_load_imm(ctx, jit_func, dst, js_false);
          MIR_append_insn(ctx, jit_func, is_done);
        } else {
          mir_call_helper2(ctx, jit_func, dst,
                           helper2_proto, imp_sne,
                           r_vm, r_js, rl, rr);
        }
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
        if (vs.has_const) { 
          vs.has_const[vs.sp - 1] = true; 
          vs.known_const[vs.sp - 1] = mkval(T_UNDEF, 0);
        }
        break;
      }

      case OP_DELETE: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
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

        for (int i = 0; i < (int)new_argc + 2; i++)
          vstack_ensure_boxed(&vs, vs.sp - 1 - i, ctx, jit_func, r_d_slot);
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
        mir_emit_resolve_call_this(ctx, jit_func, r_call_this, r_callee_cl,
                                   r_call_this, r_bool, r_tmp2);

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

      case OP_APPLY: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        uint16_t apply_argc = sv_get_u16(ip + 1);
        if (apply_argc > 16 || vs.sp < (int)apply_argc + 2) { ok = false; break; }

        int cn = call_n++;
        MIR_reg_t r_arg_arr = r_args_buf;

        for (int i = (int)apply_argc - 1; i >= 0; i--) {
          MIR_reg_t areg = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_arg_arr, 0, 1),
              MIR_new_reg_op(ctx, areg)));
        }

        MIR_reg_t r_apply_this = vstack_pop(&vs);
        MIR_reg_t r_apply_func = vstack_pop(&vs);
        MIR_reg_t r_apply_res  = vstack_push(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, call_proto),
            MIR_new_ref_op(ctx, imp_apply),
            MIR_new_reg_op(ctx, r_apply_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_apply_func),
            MIR_new_reg_op(ctx, r_apply_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)apply_argc)));

        if (jit_try_depth > 0) {
          jit_try_entry_t *h = &jit_try_stack[jit_try_depth - 1];
          MIR_label_t no_err = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_URSH,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, r_apply_res),
              MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, no_err),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, JIT_ERR_TAG)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, vs.regs[h->saved_sp]),
              MIR_new_reg_op(ctx, r_apply_res)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
          MIR_append_insn(ctx, jit_func, no_err);
        } else {
          MIR_label_t no_err = MIR_new_label(ctx);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_URSH,
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_reg_op(ctx, r_apply_res),
              MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, no_err),
              MIR_new_reg_op(ctx, r_bool),
              MIR_new_uint_op(ctx, JIT_ERR_TAG)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_apply_res)));
          MIR_append_insn(ctx, jit_func, no_err);
        }
        (void)cn;
        break;
      }

      case OP_NOP:
      case OP_HALT:
      case OP_LINE_NUM:
      case OP_COL_NUM:
      case OP_LABEL:
        break;
      case OP_STR_FLUSH_LOCAL: {
        uint16_t slot_idx = sv_get_u16(ip + 1);
        int pre_op_sp = vs.sp;
        if ((int)slot_idx < param_count) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 10,
              MIR_new_ref_op(ctx, str_flush_local_proto),
              MIR_new_ref_op(ctx, imp_str_flush_local),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_reg_op(ctx, r_args),
              MIR_new_reg_op(ctx, r_argc),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)slot_idx)));
        } else {
          uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)param_count);
          if (local_idx >= (uint16_t)n_locals) { ok = false; break; }

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
              MIR_new_reg_op(ctx, local_regs[local_idx])));

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 10,
              MIR_new_ref_op(ctx, str_flush_local_proto),
              MIR_new_ref_op(ctx, imp_str_flush_local),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)param_count),
              MIR_new_reg_op(ctx, r_lbuf),
              MIR_new_int_op(ctx, (int64_t)slot_idx)));

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, local_regs[local_idx]),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), r_lbuf, 0, 1)));
          if (known_func_locals) known_func_locals[local_idx] = NULL;
          if (known_type_locals) known_type_locals[local_idx] = SV_TI_UNKNOWN;
        }

        mir_emit_bailout_check(ctx, jit_func, r_err_tmp,
          0, r_bailout_off, bc_off,
          r_bailout_sp, pre_op_sp, bailout_tramp,
          r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot);

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
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(cv);
        jit_child_kind_t child_kind = classify_child_closure_kind(func, child);
        MIR_reg_t r_child_slots = r_tmp2;
        int child_slot_base = 0;
        int child_slot_count = 0;
        vs.known_func[vs.sp - 1] = (sv_func_t *)(uintptr_t)vdata(cv);
        switch (child_kind) {
          case JIT_CHILD_PLAIN:
          case JIT_CHILD_INHERITED_ONLY:
            break;
          case JIT_CHILD_PARAM_ONLY:
            r_child_slots = r_slotbuf;
            child_slot_count = param_count;
            break;
          case JIT_CHILD_LOCAL_ONLY:
            mir_emit_spill_child_captured_locals(
              ctx, jit_func, func, child, local_regs, n_locals, r_lbuf);
            r_child_slots = r_lbuf;
            child_slot_base = param_count;
            child_slot_count = n_locals;
            break;
          case JIT_CHILD_MIXED:
            mir_emit_spill_child_captured_locals(
              ctx, jit_func, func, child, local_regs, n_locals, r_lbuf);
            r_child_slots = r_slotbuf;
            child_slot_count = slotbuf_count;
            break;
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 11,
            MIR_new_ref_op(ctx, closure_proto),
            MIR_new_ref_op(ctx, imp_closure),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_reg_op(ctx, r_this),
            MIR_new_reg_op(ctx, r_child_slots),
            MIR_new_int_op(ctx, child_slot_base),
            MIR_new_int_op(ctx, child_slot_count),
            MIR_new_uint_op(ctx, (uint64_t)idx)));
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

  MIR_finish_func(ctx);
  MIR_finish_module(ctx);
  if (sv_dump_jit_unlikely) MIR_output_module(ctx, stderr, mod);

  free(vs.regs);
  free(vs.d_regs);
  free(vs.known_func);
  free(vs.slot_type);
  free(vs.known_const);
  free(vs.has_const);
  free(local_regs);
  free(local_d_regs);
  free(known_func_locals);
  free(known_type_locals);
  free(captured_params);
  free(captured_locals);

  if (!ok) return NULL;

  MIR_load_module(ctx, mod);
  MIR_link(ctx, MIR_set_gen_interface, NULL);

  func->jit_compiled_tfb_ver = func->tfb_version;
  func->jit_compiling = false;
  return MIR_gen(ctx, jit_func);
}

static void sv_jit_compile_callees(ant_t *js, sv_func_t *func) {
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  for (int i = 0; i < count; i++) {
    if (fb[i].disabled || !fb[i].target) continue;
    sv_func_t *callee = fb[i].target;
    if (callee->jit_code || callee->jit_compile_failed || callee->jit_compiling) continue;
    if (callee->call_count < SV_JIT_THRESHOLD / 2) continue;
    if (!jit_is_eligible(callee)) continue;
    sv_jit_func_t cjit = sv_jit_compile(js, callee, NULL);
    if (cjit) callee->jit_code = (void *)cjit;
  }
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
  sv_jit_compile_callees(js, fn);
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
    if (func->code_len > 512) {
      func->call_count = SV_JIT_THRESHOLD + 1;
      func->back_edge_count = 0;
      return SV_JIT_RETRY_INTERP;
    }
    jit = sv_jit_compile(js, func, closure);
    if (!jit) return SV_JIT_RETRY_INTERP;
    func->jit_code = (void *)jit;
    sv_jit_compile_callees(js, func);
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
