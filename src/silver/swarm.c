#ifdef ANT_JIT

#include "silver/swarm.h"
#include "silver/glue.h"
#include "silver/engine.h"
#include "silver/opcode.h"
#include "ops/globals.h"

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

typedef struct {
  MIR_context_t ctx;      /* opt level 1: fast compiles for the long tail */
  MIR_context_t ctx_hot;  /* opt level 3: loop-hot functions */
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
#define LOAD_EXT(name) do { \
    MIR_load_external(jc->ctx, #name, name); \
    MIR_load_external(jc->ctx_hot, #name, name); \
  } while (0)
  LOAD_EXT(jit_helper_add);
  LOAD_EXT(jit_helper_sub);
  LOAD_EXT(jit_helper_mul);
  LOAD_EXT(jit_helper_div);
  LOAD_EXT(jit_helper_mod);
  LOAD_EXT(jit_helper_str_read_value);
  LOAD_EXT(jit_helper_str_append_local);
  LOAD_EXT(jit_helper_str_append_local_snapshot);
  LOAD_EXT(jit_helper_str_flush_local);
  LOAD_EXT(jit_helper_lt);
  LOAD_EXT(jit_helper_le);
  LOAD_EXT(jit_helper_gt);
  LOAD_EXT(jit_helper_ge);
  LOAD_EXT(jit_helper_call);
  LOAD_EXT(jit_helper_call_method);
  LOAD_EXT(jit_helper_call_array_includes);
  LOAD_EXT(jit_helper_apply);
  LOAD_EXT(jit_helper_call_call);
  LOAD_EXT(jit_helper_call_call_slot);
  LOAD_EXT(jit_helper_rest);
  LOAD_EXT(jit_helper_special_obj);
  LOAD_EXT(jit_helper_for_of);
  LOAD_EXT(jit_helper_destructure_close);
  LOAD_EXT(jit_helper_destructure_next);
  LOAD_EXT(jit_helper_get_global);
  LOAD_EXT(jit_helper_get_eval_global);
  LOAD_EXT(jit_helper_put_eval_global);
  LOAD_EXT(jit_helper_delete_eval_var);
  LOAD_EXT(jit_helper_get_field);
  LOAD_EXT(jit_helper_get_field_inline);
  LOAD_EXT(jit_helper_import_default);
  LOAD_EXT(jit_helper_import_named);
  LOAD_EXT(jit_helper_export);
  LOAD_EXT(jit_helper_to_propkey);
  LOAD_EXT(js_template_to_string);
  LOAD_EXT(jit_helper_bailout_resume);
  LOAD_EXT(jit_helper_close_upval);
  LOAD_EXT(jit_helper_upval_barrier);
  LOAD_EXT(jit_helper_adopt_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues_rebase);
  LOAD_EXT(jit_helper_closure);
  LOAD_EXT(jit_helper_in);
  LOAD_EXT(jit_helper_instanceof);
  LOAD_EXT(jit_helper_call_is_proto);
  LOAD_EXT(jit_helper_get_length);
  LOAD_EXT(jit_helper_get_length_inline);
  LOAD_EXT(jit_helper_define_field);
  LOAD_EXT(jit_helper_define_method_comp);
  LOAD_EXT(jit_helper_seq);
  LOAD_EXT(jit_helper_eq);
  LOAD_EXT(jit_helper_ne);
  LOAD_EXT(jit_helper_sne);
  LOAD_EXT(jit_helper_put_field);
  LOAD_EXT(jit_helper_put_field_ic);
  LOAD_EXT(gc_remember_add);
  LOAD_EXT(jit_helper_get_elem);
  LOAD_EXT(jit_helper_get_elem2);
  LOAD_EXT(jit_helper_get_elem_inline);
  LOAD_EXT(jit_helper_put_elem);
  LOAD_EXT(jit_helper_get_private);
  LOAD_EXT(jit_helper_put_private);
  LOAD_EXT(jit_helper_put_global);
  LOAD_EXT(jit_helper_object);
  LOAD_EXT(jit_helper_define_slot);
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
  LOAD_EXT(jit_helper_normalize_sloppy_this);
#undef LOAD_EXT
  jc->externals_loaded = true;
}

void sv_jit_init(ant_t *js) {
  if (jit_ctx_get(js)) return;
  sv_jit_ctx_t *jc = calloc(1, sizeof(*jc));
  if (!jc) return;

  /* Two contexts because MIR crashes ("wrong get for bitmap_t") if the
     optimize level changes between MIR_gen calls in one context. */
  const char *forced = getenv("ANT_JIT_OPT");
  unsigned lvl_fast = forced ? (unsigned)atoi(forced) : 1;
  unsigned lvl_hot  = forced ? (unsigned)atoi(forced) : 3;

  jc->ctx = MIR_init();
  MIR_gen_init(jc->ctx);
  MIR_gen_set_optimize_level(jc->ctx, lvl_fast);

  jc->ctx_hot = MIR_init();
  MIR_gen_init(jc->ctx_hot);
  MIR_gen_set_optimize_level(jc->ctx_hot, lvl_hot);

  jit_load_externals_once(jc);
  jit_ctx_set(js, jc);
}

void sv_jit_destroy(ant_t *js) {
  sv_jit_ctx_t *jc = jit_ctx_get(js);
  if (!jc) return;
  MIR_gen_finish(jc->ctx);
  MIR_finish(jc->ctx);
  MIR_gen_finish(jc->ctx_hot);
  MIR_finish(jc->ctx_hot);
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
  /* Slot is statically known to hold a JS boolean (js_true/js_false). */
  uint8_t     *known_bool;
  int          sp;           
  int          max;
} jit_vstack_t;

typedef struct {
  sv_func_t *known_func;
  uint64_t known_const;
  bool has_const;
  uint8_t known_bool;
} jit_value_info_t;

static jit_value_info_t vstack_value_info(const jit_vstack_t *vs, int idx) {
  jit_value_info_t info = {0};
  if (vs->known_func) info.known_func = vs->known_func[idx];
  if (vs->has_const && vs->has_const[idx]) {
    info.has_const = true;
    info.known_const = vs->known_const[idx];
  }
  if (vs->known_bool) info.known_bool = vs->known_bool[idx];
  return info;
}

static void vstack_set_value_info(
  jit_vstack_t *vs, int idx, jit_value_info_t info
) {
  if (vs->known_func) vs->known_func[idx] = info.known_func;
  if (vs->has_const) {
    vs->has_const[idx] = info.has_const;
    if (info.has_const) vs->known_const[idx] = info.known_const;
  }
  if (vs->known_bool) vs->known_bool[idx] = info.known_bool;
}

static void vstack_clear_value_info(jit_vstack_t *vs, int idx) {
  vstack_set_value_info(vs, idx, (jit_value_info_t){0});
}

static MIR_reg_t vstack_push(jit_vstack_t *vs) {
  vstack_clear_value_info(vs, vs->sp);
  if (vs->slot_type) vs->slot_type[vs->sp] = 0;
  return vs->regs[vs->sp++];
}

static MIR_reg_t vstack_push_const(jit_vstack_t *vs, uint64_t val) {
  vstack_clear_value_info(vs, vs->sp);
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
#define JIT_STR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | T_STR)
#define NANBOX_TARR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_ARR)


/* Register-to-register bitcasts on targets with MIR_I2DB/MIR_D2IB
   patterns; other targets fall back to a memory round-trip via `slot`. */
#if defined(__aarch64__) || defined(__x86_64__)
#define SV_JIT_HAS_BITCAST 1
#else
#define SV_JIT_HAS_BITCAST 0
#endif

static void mir_i64_to_d(MIR_context_t ctx, MIR_item_t fn,
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

static void mir_d_to_i64(MIR_context_t ctx, MIR_item_t fn,
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

static void mir_emit_get_length(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t obj, MIR_reg_t dst,
  MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_d_slot,
  MIR_item_t helper1_proto, MIR_item_t imp_get_length,
  bool builder_slot,
  int owner_id, int bc_off
) {
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
  MIR_label_t nonflat = builder_slot ? MIR_new_label(ctx) : slow;

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

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, ptr),
      MIR_new_reg_op(ctx, obj),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
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

  if (builder_slot) {
    MIR_append_insn(ctx, fn, nonflat);
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
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, ptr),
      MIR_new_reg_op(ctx, obj),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
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
  mir_d_to_i64(ctx, fn, dst, dbl, r_d_slot);
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

typedef struct {
  MIR_reg_t val;
  MIR_reg_t off;
  MIR_reg_t sp;
  MIR_label_t tramp;
  MIR_reg_t args_buf;
  jit_vstack_t *vstack;
  MIR_reg_t *local_regs;
  int n_locals;
  MIR_reg_t lbuf;
  MIR_reg_t d_slot;
} jit_bailout_emit_t;

static bool jit_bailout_slot_was_num(jit_vstack_t *vs, int idx,
                                     int left_idx, bool left_is_num,
                                     int right_idx, bool right_is_num) {
  if (idx == left_idx) return left_is_num;
  if (idx == right_idx) return right_is_num;
  return vs->slot_type && vs->slot_type[idx] == SLOT_NUM;
}

/* Per-compile state set by sv_jit_compile: locals flagged in
   jit_cur_dnum_locals keep their live value only in local_d_regs ("d-only"
   locals); the boxed regs are stale and must be refreshed before any code
   reads them wholesale (bailout snapshots). Compiles are not reentrant,
   so plain statics are safe. */
static const uint8_t *jit_cur_dnum_locals;
static MIR_reg_t *jit_cur_local_d_regs;

static void mir_emit_dnum_rebox(MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t *local_regs, int n_locals,
                                MIR_reg_t r_d_slot) {
  if (!jit_cur_dnum_locals) return;
  for (int i = 0; i < n_locals; i++)
    if (jit_cur_dnum_locals[i])
      mir_d_to_i64(ctx, fn, local_regs[i], jit_cur_local_d_regs[i], r_d_slot);
}

static void mir_emit_bailout_jump_typed(MIR_context_t ctx, MIR_item_t fn,
                                        MIR_reg_t r_bailout_off, int bc_off,
                                        MIR_reg_t r_bailout_sp, int pre_op_sp,
                                        MIR_label_t bailout_tramp,
                                        MIR_reg_t r_args_buf,
                                        jit_vstack_t *vs,
                                        MIR_reg_t *local_regs, int n_locals,
                                        MIR_reg_t r_lbuf,
                                        MIR_reg_t r_d_slot,
                                        int left_idx, bool left_is_num,
                                        int right_idx, bool right_is_num) {
  for (int i = 0; i < pre_op_sp; i++) {
    if (jit_bailout_slot_was_num(vs, i, left_idx, left_is_num,
                                 right_idx, right_is_num))
      mir_d_to_i64(ctx, fn, vs->regs[i], vs->d_regs[i], r_d_slot);
    MIR_append_insn(ctx, fn,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args_buf, 0, 1),
        MIR_new_reg_op(ctx, vs->regs[i])));
  }
  mir_emit_dnum_rebox(ctx, fn, local_regs, n_locals, r_d_slot);
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
}

static void mir_emit_bailout_check_typed(MIR_context_t ctx, MIR_item_t fn,
                                         MIR_reg_t res,
                                         MIR_reg_t restore_val,
                                         MIR_reg_t r_bailout_off, int bc_off,
                                         MIR_reg_t r_bailout_sp, int pre_op_sp,
                                         MIR_label_t bailout_tramp,
                                         MIR_reg_t r_args_buf,
                                         jit_vstack_t *vs,
                                         MIR_reg_t *local_regs, int n_locals,
                                         MIR_reg_t r_lbuf,
                                         MIR_reg_t r_d_slot,
                                         int left_idx, bool left_is_num,
                                         int right_idx, bool right_is_num) {
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
  mir_emit_bailout_jump_typed(ctx, fn,
    r_bailout_off, bc_off,
    r_bailout_sp, pre_op_sp, bailout_tramp,
    r_args_buf, vs, local_regs, n_locals, r_lbuf, r_d_slot,
    left_idx, left_is_num, right_idx, right_is_num);
  MIR_append_insn(ctx, fn, no_bail);
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
  mir_emit_bailout_check_typed(ctx, fn, res, restore_val,
    r_bailout_off, bc_off,
    r_bailout_sp, pre_op_sp, bailout_tramp,
    r_args_buf, vs, local_regs, n_locals, r_lbuf, r_d_slot,
    -1, false, -1, false);
}

static void mir_emit_self_binding_guard(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t value, MIR_reg_t closure,
  MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
  int bc_off, int pre_op_sp,
  jit_bailout_emit_t *bail
) {
  MIR_label_t match = MIR_new_label(ctx);
  uint64_t func_tag = NANBOX_PREFIX
    | ((ant_value_t)(T_FUNC & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tag_tmp),
      MIR_new_uint_op(ctx, func_tag)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_OR,
      MIR_new_reg_op(ctx, expected_tmp),
      MIR_new_reg_op(ctx, tag_tmp),
      MIR_new_reg_op(ctx, closure)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, match),
      MIR_new_reg_op(ctx, value),
      MIR_new_reg_op(ctx, expected_tmp)));
  mir_emit_bailout_jump_typed(ctx, fn,
    bail->off, bc_off,
    bail->sp, pre_op_sp, bail->tramp,
    bail->args_buf, bail->vstack, bail->local_regs, bail->n_locals,
    bail->lbuf, bail->d_slot,
    -1, false, -1, false);
  MIR_append_insn(ctx, fn, match);
}

static void mir_emit_self_binding_guard_value_kept(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t value, MIR_reg_t closure,
  MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
  int bc_off, int op_sz, int pre_op_sp,
  jit_bailout_emit_t *bail
) {
  MIR_label_t match = MIR_new_label(ctx);
  MIR_label_t not_undef = MIR_new_label(ctx);
  uint64_t func_tag = NANBOX_PREFIX
    | ((ant_value_t)(T_FUNC & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tag_tmp),
      MIR_new_uint_op(ctx, func_tag)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_OR,
      MIR_new_reg_op(ctx, expected_tmp),
      MIR_new_reg_op(ctx, tag_tmp),
      MIR_new_reg_op(ctx, closure)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, match),
      MIR_new_reg_op(ctx, value),
      MIR_new_reg_op(ctx, expected_tmp)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, not_undef),
      MIR_new_reg_op(ctx, value),
      MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
  mir_emit_bailout_jump_typed(ctx, fn,
    bail->off, bc_off,
    bail->sp, pre_op_sp, bail->tramp,
    bail->args_buf, bail->vstack, bail->local_regs, bail->n_locals,
    bail->lbuf, bail->d_slot,
    -1, false, -1, false);
  MIR_append_insn(ctx, fn, not_undef);
  mir_emit_bailout_jump_typed(ctx, fn,
    bail->off, bc_off + op_sz,
    bail->sp, pre_op_sp + 1, bail->tramp,
    bail->args_buf, bail->vstack, bail->local_regs, bail->n_locals,
    bail->lbuf, bail->d_slot,
    -1, false, -1, false);
  MIR_append_insn(ctx, fn, match);
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
  bool *captured_params, int param_count, bool fill_all
) {
  if (!captured_params && !fill_all) return;
  for (int i = 0; i < param_count; i++) {
    if (!fill_all && !captured_params[i]) continue;
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

static void mir_emit_fill_uncaptured_param_slots_from_args(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
  bool *captured_params, int param_count
) {
  if (!captured_params) return;
  for (int i = 0; i < param_count; i++) {
    if (captured_params[i]) continue;
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
  MIR_reg_t r_open_upvalues,
  bool *captured, int start_idx, int slot_count
) {
  if (!captured || start_idx >= slot_count || slot_count <= 0 || !r_slots) return;
  if (start_idx < 0) start_idx = 0;

  int first_captured = -1;
  for (int i = start_idx; i < slot_count; i++) if (captured[i]) {
    first_captured = i;
    break;
  }
  if (first_captured < 0) return;

  static uint32_t close_guard_seq = 0;
  char open_name[32];
  snprintf(open_name, sizeof(open_name), "open_upvals_%u", close_guard_seq++);
  MIR_reg_t r_open = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, open_name);
  MIR_label_t no_open = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_open),
      MIR_new_mem_op(ctx, MIR_T_I64,
        r_open_upvalues ? 0 : (MIR_disp_t)offsetof(sv_vm_t, open_upvalues),
        r_open_upvalues ? r_open_upvalues : r_vm, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, no_open),
      MIR_new_reg_op(ctx, r_open),
      MIR_new_uint_op(ctx, 0)));

  MIR_append_insn(ctx, fn,
    MIR_new_call_insn(ctx, 7,
      MIR_new_ref_op(ctx, close_upval_proto),
      MIR_new_ref_op(ctx, imp_close_upval),
      MIR_new_reg_op(ctx, r_vm),
      MIR_new_uint_op(ctx, (uint64_t)first_captured),
      MIR_new_reg_op(ctx, r_slots),
      MIR_new_int_op(ctx, slot_count),
      r_open_upvalues ? MIR_new_reg_op(ctx, r_open_upvalues) : MIR_new_uint_op(ctx, 0)));
  MIR_append_insn(ctx, fn, no_open);
}

/* The emitted checks only eliminate definitely unnecessary calls. The C
   helper owns the closed/open-cell and young-reference policy. */
static void mir_emit_upval_write_barrier(
  MIR_context_t ctx, MIR_item_t jit_func,
  MIR_item_t upval_barrier_proto, MIR_item_t imp_upval_barrier,
  MIR_reg_t r_js, MIR_reg_t r_uv, MIR_reg_t src, int un
) {
  MIR_label_t skip_barrier = MIR_new_label(ctx);
  char rn_tmp[32];
  snprintf(rn_tmp, sizeof(rn_tmp), "uvwb%d", un);
  MIR_reg_t r_tmp = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_tmp);

  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_tmp),
      MIR_new_mem_op(ctx, MIR_T_U8,
        (MIR_disp_t)offsetof(sv_upvalue_t, in_remember_set),
        r_uv, 0, 1)));
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_BT,
      MIR_new_label_op(ctx, skip_barrier),
      MIR_new_reg_op(ctx, r_tmp)));
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_UBLE,
      MIR_new_label_op(ctx, skip_barrier),
      MIR_new_reg_op(ctx, src),
      MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_tmp),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(sv_upvalue_t, gc_epoch),
        r_uv, 0, 1)));
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_BF,
      MIR_new_label_op(ctx, skip_barrier),
      MIR_new_reg_op(ctx, r_tmp)));
  MIR_append_insn(ctx, jit_func,
    MIR_new_call_insn(ctx, 5,
      MIR_new_ref_op(ctx, upval_barrier_proto),
      MIR_new_ref_op(ctx, imp_upval_barrier),
      MIR_new_reg_op(ctx, r_js),
      MIR_new_reg_op(ctx, r_uv),
      MIR_new_reg_op(ctx, src)));
  MIR_append_insn(ctx, jit_func, skip_barrier);
}

static void mir_emit_exit_upvalue_cleanup(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
  MIR_item_t adopt_open_upvalues_proto, MIR_item_t imp_adopt_open_upvalues,
  MIR_reg_t r_vm, MIR_reg_t r_slotbuf, MIR_reg_t r_lbuf,
  MIR_reg_t r_jit_open_upvalues,
  bool has_captured_slots, bool *captured_params, int param_count,
  bool has_captures, bool *captured_locals, int n_locals
) {
  if (has_captured_slots)
    mir_emit_close_marked_slots(ctx, fn,
      close_upval_proto, imp_close_upval,
      r_vm, r_slotbuf, r_jit_open_upvalues, captured_params, 0, param_count);
  if (has_captures)
    mir_emit_close_marked_slots(ctx, fn,
      close_upval_proto, imp_close_upval,
      r_vm, r_lbuf, r_jit_open_upvalues, captured_locals, 0, n_locals);
  if (r_jit_open_upvalues) {
    MIR_append_insn(ctx, fn,
      MIR_new_call_insn(ctx, 4,
        MIR_new_ref_op(ctx, adopt_open_upvalues_proto),
        MIR_new_ref_op(ctx, imp_adopt_open_upvalues),
        MIR_new_reg_op(ctx, r_vm),
        MIR_new_reg_op(ctx, r_jit_open_upvalues)));
  }
}

static void mir_emit_exit_ret(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
  MIR_item_t adopt_open_upvalues_proto, MIR_item_t imp_adopt_open_upvalues,
  MIR_reg_t r_vm, MIR_reg_t r_slotbuf, MIR_reg_t r_lbuf,
  MIR_reg_t r_jit_open_upvalues,
  bool has_captured_slots, bool *captured_params, int param_count,
  bool has_captures, bool *captured_locals, int n_locals,
  MIR_op_t ret_op
) {
  mir_emit_exit_upvalue_cleanup(ctx, fn,
    close_upval_proto, imp_close_upval,
    adopt_open_upvalues_proto, imp_adopt_open_upvalues,
    r_vm, r_slotbuf, r_lbuf, r_jit_open_upvalues,
    has_captured_slots, captured_params, param_count,
    has_captures, captured_locals, n_locals);
  MIR_append_insn(ctx, fn, MIR_new_ret_insn(ctx, 1, ret_op));
}

static inline void mir_emit_self_tail(
  MIR_context_t ctx, MIR_item_t fn,
  int call_argc, int param_count,
  MIR_reg_t r_tco_args, MIR_reg_t r_arg_arr,
  MIR_reg_t r_args, MIR_reg_t r_argc,
  MIR_reg_t *local_regs, int n_locals,
  bool has_captured_slots, MIR_reg_t r_slotbuf, bool *captured_params,
  bool fill_all_params,
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
    mir_emit_fill_param_slots_from_args(ctx, fn, r_slotbuf, r_tco_args, r_argc, captured_params, param_count, fill_all_params);
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

static void mir_emit_branch_if_string_builder(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t value, MIR_reg_t scratch, MIR_label_t builder
) {
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

static bool jit_upvalue_is_builder_target(sv_func_t *func, uint32_t idx) {
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

static MIR_label_t mir_emit_string_builder_read_open(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t value, MIR_reg_t scratch,
  MIR_reg_t r_vm, MIR_reg_t r_js,
  MIR_item_t helper1_proto, MIR_item_t imp_str_read_value
) {
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

static void mir_emit_string_builder_append_ascii_byte(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t result,
  MIR_reg_t d_slot,
  MIR_label_t slow, MIR_label_t done,
  int owner_id, int bc_off
) {
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
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, lhs_ptr),
      MIR_new_reg_op(ctx, lhs),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
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
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, rhs_ptr),
      MIR_new_reg_op(ctx, rhs),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
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
  mir_d_to_i64(ctx, fn, cached, cached_d, d_slot);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, cached_done)));
  MIR_append_insn(ctx, fn, cached_nonnum);
  mir_load_imm(ctx, fn, cached, mkval(T_UNDEF, 0));
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
  mir_load_imm(ctx, fn, result, mkval(T_UNDEF, 0));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}

static void mir_emit_numeric_local_store_mirror(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t local_d,
  MIR_reg_t src,
  MIR_reg_t src_d,
  bool src_is_num,
  MIR_reg_t r_bool,
  int resume_bc_off,
  int post_op_sp,
  jit_bailout_emit_t *bail
) {
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
  mir_emit_bailout_check(ctx, fn, bail->val,
    0, bail->off, resume_bc_off,
    bail->sp, post_op_sp, bail->tramp,
    bail->args_buf, bail->vstack, bail->local_regs, bail->n_locals,
    bail->lbuf, bail->d_slot);
  MIR_append_insn(ctx, fn, done);
}

#define NANBOX_TFUNC_TAG  ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_FUNC)
#define NANBOX_TOBJ_TAG   ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_OBJ)
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
  MIR_label_t use_bound = MIR_new_label(ctx);
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
      MIR_new_uint_op(ctx, SV_CALL_IS_ARROW | SV_CALL_HAS_BOUND_THIS)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, use_bound),
      MIR_new_reg_op(ctx, r_flags),
      MIR_new_uint_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_reg_op(ctx, fallback_this)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, use_bound);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_bound),
      MIR_new_mem_op(ctx, MIR_JSVAL,
        (MIR_disp_t)offsetof(sv_closure_t, bound_this),
        r_closure, 0, 1)));
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
  /* Lazy closures: func_obj == 0 until materialized; the slow path
     (js_as_obj) materializes it. */
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn, done);
}

static void mir_emit_string_concat_fastpath(
  MIR_context_t ctx, MIR_item_t fn,
  MIR_reg_t r_js, MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t dst,
  MIR_label_t slow, int owner_id, int bc_off
) {
  char names[16][48];
  MIR_reg_t regs[16];
  static const char *suffix[16] = {
    "tag", "lp", "rp", "llen", "rlen", "ldepth", "rdepth", "len",
    "depth", "head", "used", "cap", "ptr", "count", "next", "tmp"
  };
  for (int i = 0; i < 16; i++) {
    snprintf(names[i], sizeof(names[i]), "sc_%s_%d_%d", suffix[i], owner_id, bc_off);
    regs[i] = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, names[i]);
  }
  MIR_reg_t tag = regs[0], lp = regs[1], rp = regs[2];
  MIR_reg_t llen = regs[3], rlen = regs[4];
  MIR_reg_t ldepth = regs[5], rdepth = regs[6], len = regs[7], depth = regs[8];
  MIR_reg_t head = regs[9], used = regs[10], cap = regs[11], ptr = regs[12];
  MIR_reg_t count = regs[13], next = regs[14], tmp = regs[15];

  MIR_label_t left_flat = MIR_new_label(ctx);
  MIR_label_t left_done = MIR_new_label(ctx);
  MIR_label_t right_flat = MIR_new_label(ctx);
  MIR_label_t right_done = MIR_new_label(ctx);
  MIR_label_t return_left = MIR_new_label(ctx);
  MIR_label_t return_right = MIR_new_label(ctx);
  MIR_label_t depth_ready = MIR_new_label(ctx);
  MIR_label_t depth_done = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag),
      MIR_new_reg_op(ctx, lhs), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, lp),
      MIR_new_reg_op(ctx, lhs), MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag),
      MIR_new_reg_op(ctx, lp), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, left_flat),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, lp),
      MIR_new_reg_op(ctx, lp), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, llen),
      MIR_new_mem_op(ctx, MIR_T_U64,
        (MIR_disp_t)offsetof(ant_rope_heap_t, len), lp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ldepth),
      MIR_new_mem_op(ctx, MIR_T_U16,
        (MIR_disp_t)offsetof(ant_rope_heap_t, depth), lp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, left_done)));
  MIR_append_insn(ctx, fn, left_flat);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, llen),
      MIR_new_mem_op(ctx, MIR_T_U64,
        (MIR_disp_t)offsetof(ant_flat_string_t, len), lp, 0, 1)));
  mir_load_imm(ctx, fn, ldepth, 0);
  MIR_append_insn(ctx, fn, left_done);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, tag),
      MIR_new_reg_op(ctx, rhs), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, rp),
      MIR_new_reg_op(ctx, rhs), MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, tag),
      MIR_new_reg_op(ctx, rp), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, right_flat),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_FLAT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, rp),
      MIR_new_reg_op(ctx, rp), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rlen),
      MIR_new_mem_op(ctx, MIR_T_U64,
        (MIR_disp_t)offsetof(ant_rope_heap_t, len), rp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rdepth),
      MIR_new_mem_op(ctx, MIR_T_U16,
        (MIR_disp_t)offsetof(ant_rope_heap_t, depth), rp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, right_done)));
  MIR_append_insn(ctx, fn, right_flat);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rlen),
      MIR_new_mem_op(ctx, MIR_T_U64,
        (MIR_disp_t)offsetof(ant_flat_string_t, len), rp, 0, 1)));
  mir_load_imm(ctx, fn, rdepth, 0);
  MIR_append_insn(ctx, fn, right_done);

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
    MIR_new_insn(ctx, MIR_UBLT, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, len), MIR_new_int_op(ctx, 13)));

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
      MIR_new_reg_op(ctx, depth), MIR_new_uint_op(ctx, UINT16_MAX)));
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
  mir_load_imm(ctx, fn, tmp, mkval(T_UNDEF, 0));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_JSVAL,
        (MIR_disp_t)offsetof(ant_rope_heap_t, cached), ptr, 0, 1),
      MIR_new_reg_op(ctx, tmp)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, dst),
      MIR_new_reg_op(ctx, ptr),
      MIR_new_uint_op(ctx, mkval(T_STR, STR_HEAP_TAG_ROPE))));
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

static bool mir_emit_put_field_ic_fastpath(
  MIR_context_t ctx, MIR_item_t fn,
  sv_func_t *func, int bc_off, uint16_t ic_idx, sv_atom_t *atom,
  MIR_reg_t r_js, MIR_reg_t obj, MIR_reg_t val,
  MIR_label_t slow, MIR_reg_t r_global_epoch,
  MIR_item_t remember_proto, MIR_item_t imp_remember
) {
  if (!func || !func->ic_slots || !atom) return false;
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return false;
  if (atom->len == 9 && memcmp(atom->str, "prototype", 9) == 0) return false;
  if ((atom->len == 4 && memcmp(atom->str, "exec", 4) == 0) ||
      (atom->len == 7 && memcmp(atom->str, "replace", 7) == 0))
    return false;

  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  char names[14][48];
  MIR_reg_t regs[14];
  static const char *suffix[14] = {
    "ic", "ice", "ce", "op", "ot", "flags", "shape", "ics",
    "idx", "pc", "limit", "overflow", "oi", "vtag"
  };
  for (int i = 0; i < 14; i++) {
    snprintf(names[i], sizeof(names[i]), "pf_%s_%d_%u", suffix[i],
             bc_off, (unsigned)ic_idx);
    regs[i] = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, names[i]);
  }
  MIR_reg_t ric = regs[0], rice = regs[1], rce = regs[2];
  MIR_reg_t optr = regs[3], otag = regs[4], flags = regs[5];
  MIR_reg_t shape = regs[6], icshape = regs[7];
  MIR_reg_t idx = regs[8], prop_count = regs[9], limit = regs[10];
  MIR_reg_t overflow = regs[11], overflow_idx = regs[12], vtag = regs[13];
  char vp_name[48], rf_name[48], need_name[48];
  snprintf(vp_name, sizeof(vp_name), "pf_vp_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(rf_name, sizeof(rf_name), "pf_rf_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(need_name, sizeof(need_name), "pf_need_%d_%u", bc_off, (unsigned)ic_idx);
  MIR_reg_t vptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, vp_name);
  MIR_reg_t rope_flags = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, rf_name);
  MIR_reg_t need_barrier = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, need_name);

  MIR_label_t classify_tag = MIR_new_label(ctx);
  MIR_label_t classify_string = MIR_new_label(ctx);
  MIR_label_t store = MIR_new_label(ctx);
  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t after_store = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ric),
      MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rice),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rce),
      MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, rice), MIR_new_reg_op(ctx, rce)));

  mir_emit_value_to_objptr_or_jmp(ctx, fn, obj, optr, otag, slow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, flags),
      MIR_new_mem_op(ctx, MIR_T_U16,
        (MIR_disp_t)offsetof(ant_object_t, flags), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, otag),
      MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXOTIC)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, otag), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, shape),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, shape), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, icshape),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, shape), MIR_new_reg_op(ctx, icshape)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, idx),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, prop_count),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(ant_object_t, prop_count), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, prop_count)));

  mir_load_imm(ctx, fn, need_barrier, 0);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, classify_tag),
      MIR_new_reg_op(ctx, val), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store)));
  MIR_append_insn(ctx, fn, classify_tag);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, vtag),
      MIR_new_reg_op(ctx, val), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, classify_string),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, NANBOX_TOBJ_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, NANBOX_TPROM_TAG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, vtag),
      MIR_new_uint_op(ctx,
        (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_GENERATOR)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store)));

  MIR_append_insn(ctx, fn, classify_string);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, vptr),
      MIR_new_reg_op(ctx, val), MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, vtag),
      MIR_new_reg_op(ctx, vptr), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, store),
      MIR_new_reg_op(ctx, vtag), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, vptr),
      MIR_new_reg_op(ctx, vptr), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rope_flags),
      MIR_new_mem_op(ctx, MIR_T_U16,
        (MIR_disp_t)offsetof(ant_rope_heap_t, flags), vptr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, rope_flags),
      MIR_new_reg_op(ctx, rope_flags), MIR_new_uint_op(ctx, ANT_ROPE_FLAG_YOUNG)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, store),
      MIR_new_reg_op(ctx, rope_flags), MIR_new_int_op(ctx, 0)));
  mir_load_imm(ctx, fn, need_barrier, 1);

  MIR_append_insn(ctx, fn, store);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, limit),
      MIR_new_mem_op(ctx, MIR_T_U8,
        (MIR_disp_t)offsetof(ant_object_t, inobj_limit), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, load_overflow),
      MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, limit)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_JSVAL,
        (MIR_disp_t)offsetof(ant_object_t, inobj), optr, idx, 8),
      MIR_new_reg_op(ctx, val)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, after_store)));
  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, overflow),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, overflow_prop), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, overflow), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_SUB, MIR_new_reg_op(ctx, overflow_idx),
      MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, limit)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_JSVAL, 0, overflow, overflow_idx, 8),
      MIR_new_reg_op(ctx, val)));
  MIR_append_insn(ctx, fn, after_store);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, done),
      MIR_new_reg_op(ctx, need_barrier), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, flags),
      MIR_new_reg_op(ctx, flags),
      MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_GENERATION | ANT_OBJECT_FLAG_REMEMBERED)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done),
      MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_GENERATION)));
  MIR_append_insn(ctx, fn,
    MIR_new_call_insn(ctx, 4,
      MIR_new_ref_op(ctx, remember_proto),
      MIR_new_ref_op(ctx, imp_remember),
      MIR_new_reg_op(ctx, r_js),
      MIR_new_reg_op(ctx, optr)));
  MIR_append_insn(ctx, fn, done);
  return true;
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
  if (!sv_gf_ic_active(ic->cached_aux) && func->code_len > 1024) return false;
  char gf_ic_name[32], gf_ice_name[32];
  char gf_ot_name[32], gf_op_name[32], gf_os_name[32], gf_ics_name[32];
  char gf_h_name[32], gf_hs_name[32], gf_idx_name[32], gf_pc_name[32];
  char gf_ica_name[32], gf_il_name[32], gf_ovf_name[32], gf_ovi_name[32];
  char gf_io_name[32], gf_src_name[32], gf_op_proto_name[32], gf_ic_proto_name[32];
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
  snprintf(gf_op_proto_name, sizeof(gf_op_proto_name), "gf_opp_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ic_proto_name, sizeof(gf_ic_proto_name), "gf_icp_%d_%u", bc_off, (unsigned)ic_idx);

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
  MIR_reg_t r_obj_proto = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_op_proto_name);
  MIR_reg_t r_ic_proto = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ic_proto_name);
  char gf_pp_name[32], gf_pid_name[32], gf_ipid_name[32];
  snprintf(gf_pp_name, sizeof(gf_pp_name), "gf_pp_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_pid_name, sizeof(gf_pid_name), "gf_pid_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ipid_name, sizeof(gf_ipid_name), "gf_ipid_%d_%u", bc_off, (unsigned)ic_idx);
  MIR_reg_t r_proto_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pp_name);
  MIR_reg_t r_proto_id = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pid_name);
  MIR_reg_t r_ic_proto_id = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ipid_name);

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
      MIR_new_reg_op(ctx, r_obj_proto),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(ant_object_t, proto), r_obj_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_proto),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(sv_ic_entry_t, guard.receiver_proto), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_obj_proto),
      MIR_new_reg_op(ctx, r_ic_proto)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, r_proto_ptr),
      MIR_new_reg_op(ctx, r_obj_proto),
      MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_proto_id),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(ant_object_t, ic_identity), r_proto_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_proto_id),
      MIR_new_mem_op(ctx, MIR_T_U64,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, r_ic_proto_id),
      MIR_new_reg_op(ctx, r_ic_proto_id),
      MIR_new_uint_op(ctx, SV_GF_IC_PROTO_ID_SHIFT)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_proto_id),
      MIR_new_reg_op(ctx, r_ic_proto_id)));
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

static bool mir_emit_get_global_ic_fastpath(
  MIR_context_t ctx, MIR_item_t fn,
  sv_func_t *func, int bc_off,
  MIR_reg_t r_js, MIR_reg_t dst,
  MIR_label_t slow, MIR_reg_t r_global_epoch,
  uint8_t *ip
) {
  if (!func || !func->ic_slots || !ip) return false;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  if (!ic) return false;

  char n_ic[32], n_e[32], n_ce[32], n_gv[32], n_gt[32], n_gp[32];
  char n_sh[32], n_ics[32], n_idx[32], n_pc[32], n_il[32], n_ov[32], n_oi[32];
  snprintf(n_ic, sizeof(n_ic), "gg_ic_%d", bc_off);
  snprintf(n_e, sizeof(n_e), "gg_e_%d", bc_off);
  snprintf(n_ce, sizeof(n_ce), "gg_ce_%d", bc_off);
  snprintf(n_gv, sizeof(n_gv), "gg_gv_%d", bc_off);
  snprintf(n_gt, sizeof(n_gt), "gg_gt_%d", bc_off);
  snprintf(n_gp, sizeof(n_gp), "gg_gp_%d", bc_off);
  snprintf(n_sh, sizeof(n_sh), "gg_sh_%d", bc_off);
  snprintf(n_ics, sizeof(n_ics), "gg_ics_%d", bc_off);
  snprintf(n_idx, sizeof(n_idx), "gg_idx_%d", bc_off);
  snprintf(n_pc, sizeof(n_pc), "gg_pc_%d", bc_off);
  snprintf(n_il, sizeof(n_il), "gg_il_%d", bc_off);
  snprintf(n_ov, sizeof(n_ov), "gg_ov_%d", bc_off);
  snprintf(n_oi, sizeof(n_oi), "gg_oi_%d", bc_off);

  MIR_reg_t r_ic = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ic);
  MIR_reg_t r_e = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_e);
  MIR_reg_t r_ce = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ce);
  MIR_reg_t r_gv = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gv);
  MIR_reg_t r_gt = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gt);
  MIR_reg_t r_gp = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gp);
  MIR_reg_t r_sh = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_sh);
  MIR_reg_t r_ics = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ics);
  MIR_reg_t r_idx = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_idx);
  MIR_reg_t r_pc = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_pc);
  MIR_reg_t r_il = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_il);
  MIR_reg_t r_ov = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ov);
  MIR_reg_t r_oi = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_oi);

  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t fast_done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic),
      MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_e),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ce),
      MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_e),
      MIR_new_reg_op(ctx, r_ce)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_gv),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(ant_t, global), r_js, 0, 1)));
  mir_emit_value_to_objptr_or_jmp(ctx, fn, r_gv, r_gp, r_gt, slow);

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_sh),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, shape), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ics),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_ics),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_sh),
      MIR_new_reg_op(ctx, r_ics)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_idx),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_pc),
      MIR_new_mem_op(ctx, MIR_T_U32,
        (MIR_disp_t)offsetof(ant_object_t, prop_count), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_idx),
      MIR_new_reg_op(ctx, r_pc)));

  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_il),
      MIR_new_mem_op(ctx, MIR_T_U8,
        (MIR_disp_t)offsetof(ant_object_t, inobj_limit), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_UBGE,
      MIR_new_label_op(ctx, load_overflow),
      MIR_new_reg_op(ctx, r_idx),
      MIR_new_reg_op(ctx, r_il)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_T_I64,
        (MIR_disp_t)offsetof(ant_object_t, inobj), r_gp, r_idx, 8)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_JMP,
      MIR_new_label_op(ctx, fast_done)));

  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ov),
      MIR_new_mem_op(ctx, MIR_T_P,
        (MIR_disp_t)offsetof(ant_object_t, overflow_prop), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, r_ov),
      MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_SUB,
      MIR_new_reg_op(ctx, r_oi),
      MIR_new_reg_op(ctx, r_idx),
      MIR_new_reg_op(ctx, r_il)));
  MIR_append_insn(ctx, fn,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, dst),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, r_ov, r_oi, 8)));
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
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_OSR_BACKEDGE) != 0) {
      if ((flags & SV_OPF_JIT_BRANCH32) != 0)
        target = src + sz + sv_get_i32(ip + 1);
      else if ((flags & SV_OPF_JIT_BRANCH8) != 0)
        target = src + sz + (int8_t)sv_get_i8(ip + 1);
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

static bool func_writes_params(sv_func_t *func) {
  if (!func || func->param_count <= 0) return false;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;

  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    if (ip + sz > end) break;
    if (op == OP_PUT_ARG || op == OP_SET_ARG) return true;
    if ((sv_op_flags[op] & SV_OPF_BUILDER_TARGET) != 0) {
      if (sv_get_u16(ip + 1) < func->param_count) return true;
    }
    ip += sz;
  }

  return false;
}

static sv_func_t *scan_closure_child(sv_func_t *func, uint8_t *ip) {
  if ((sv_op_t)*ip != OP_CLOSURE) return NULL;

  uint32_t idx = sv_get_u32(ip + 1);
  if (idx >= (uint32_t)func->const_count) return NULL;

  ant_value_t cv = func->constants[idx];
  if (vtype(cv) != T_NTARG) return NULL;

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


#define JIT_INLINE_MAX_BYTECODE 192

/* Imports/protos + the caller's args buffer, needed by the wider inline-op
   set (property writes, element/length reads, bitwise, nested calls). */
typedef struct {
  MIR_item_t helper1_proto, imp_get_length_inline;
  MIR_item_t imp_get_elem_inline;
  MIR_item_t put_field_proto, imp_put_field;
  MIR_item_t remember_obj_proto, imp_remember_obj;
  MIR_item_t call_proto, imp_call;
  MIR_item_t call_method_proto, imp_call_method;
  MIR_item_t imp_band, imp_bor, imp_bxor, imp_shl, imp_shr, imp_ushr;
  MIR_item_t self_proto; /* direct JIT->JIT dispatch for devirt-in-inline */
  MIR_reg_t r_args_buf;
} jit_inline_ext_t;

/* Ops with observable side effects when inlined. Their errors go to the
   join (thrown there), NEVER to `slow` — the slow path re-executes the
   whole callee generically, which would run the effect twice. */
static bool jit_op_inline_side_effect(sv_op_t op) {
  switch (op) {
    case OP_PUT_FIELD:
    case OP_CALL:
    case OP_CALL_METHOD:
    case OP_TAIL_CALL:
    case OP_TAIL_CALL_METHOD:
      return true;
    default:
      return false;
  }
}

/* Ops that can never branch to `slow` (no bailout, no error): the only
   ops permitted after a side-effecting op, so no path can re-execute an
   effect via the generic fallback. */
static bool jit_op_inline_pure_tail(sv_op_t op) {
  switch (op) {
    case OP_RETURN: case OP_RETURN_UNDEF:
    case OP_POP: case OP_DUP: case OP_NIP:
    case OP_GET_LOCAL: case OP_GET_LOCAL8:
    case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
    case OP_SET_LOCAL: case OP_SET_LOCAL8:
    case OP_GET_ARG:
    case OP_CONST: case OP_CONST8: case OP_CONST_I8:
    case OP_UNDEF: case OP_NULL: case OP_TRUE: case OP_FALSE:
    case OP_THIS:
    case OP_IS_UNDEF: case OP_IS_NULL: case OP_IS_UNDEF_OR_NULL:
    case OP_SEQ: case OP_SNE:
    case OP_NOP: case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
      return true;
    default:
      return false;
  }
}

uint64_t sv_stat_inline_reject_by_op[OP__COUNT];
uint64_t sv_stat_inline_reject_size;

static bool jit_inlineable(sv_func_t *f) {
  if (!f) return false;
  if (f->is_async || f->is_generator) return false;
  // derived ctors need the super-rebound `this` returned from RETURN/
  // RETURN_UNDEF (see the main emission); the inline path doesn't model it
  if (f->is_derived_ctor) return false;
  if (f->code_len > JIT_INLINE_MAX_BYTECODE) {
    sv_stat_inline_reject_size++;
    return false;
  }

  uint8_t *ip  = f->code;
  uint8_t *end = f->code + f->code_len;
  bool seen_effect = false;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) return false;
    if ((sv_op_flags[op] & SV_OPF_JIT_INLINEABLE) == 0) {
      sv_stat_inline_reject_by_op[op]++;
      return false;
    }
    // OP_SPECIAL_OBJ(0) materializes `arguments`. keep these functions on
    // the interpreter until JIT routes a real per-call activation/object
    // with matching lifetime and semantics.
    if (op == OP_SPECIAL_OBJ && sv_get_u8(ip + 1) == 0) return false;

    if (seen_effect) {
      /* Only ops that can never reach `slow` may follow a side effect
         (side-effect ops themselves raise at the join, so more of them
         are fine); forward jumps only, so control cannot re-enter a
         bail-capable region. */
      if (op == OP_JMP) {
        if (sv_get_i32(ip + 1) < 0) { sv_stat_inline_reject_by_op[op]++; return false; }
      } else if (!jit_op_inline_pure_tail(op) && !jit_op_inline_side_effect(op)) {
        sv_stat_inline_reject_by_op[op]++;
        return false;
      }
    }
    if (jit_op_inline_side_effect(op)) seen_effect = true;
    ip += sz;
  }
  return true;
}

/* Inline property helpers return SV_JIT_BAILOUT before any lookup that could
   invoke user code — that edge may safely re-execute the generic callee. A
   real T_ERR was produced by an effect-free path and must instead raise at
   `join` (like the side-effect ops), never at `slow`, whose generic fallback
   would re-run the callee and repeat the effect that produced it. */
static void mir_emit_inline_read_guard(
  MIR_context_t ctx,
  MIR_item_t fn,
  MIR_reg_t value,
  MIR_reg_t result,
  MIR_reg_t scratch,
  MIR_label_t slow,
  MIR_label_t join,
  MIR_label_t ok
) {
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
             vtype(func->constants[idx]) == T_NUM;
    }
    case OP_CONST8: {
      uint8_t idx = sv_get_u8(ip + 1);
      return (uint32_t)idx < (uint32_t)func->const_count &&
             vtype(func->constants[idx]) == T_NUM;
    }
    default:
      return false;
  }
}

static bool jit_has_immediate_numeric_local_init(sv_func_t *func, uint8_t *ip, uint8_t *end, uint16_t local_idx) {
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

static bool jit_emit_inline_body(
  MIR_context_t ctx, MIR_item_t jit_func,
  sv_func_t *callee,
  MIR_reg_t *arg_regs, int caller_argc,
  const uint8_t *arg_num, const MIR_reg_t *arg_d,
  MIR_reg_t result, MIR_label_t slow, MIR_label_t join,
  MIR_reg_t r_bool, MIR_reg_t *p_d_slot, int id,
  MIR_reg_t r_inl_closure, MIR_reg_t r_inl_this,
  MIR_reg_t r_inl_new_target, MIR_reg_t r_inl_super,
  MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_ic_epoch,
  MIR_item_t helper2_proto, MIR_item_t imp_seq,
  MIR_item_t imp_sne, MIR_item_t imp_eq, MIR_item_t imp_ne,
  MIR_item_t gf_proto, MIR_item_t imp_get_field_inline,
  MIR_item_t gg_proto, MIR_item_t imp_gg,
  MIR_item_t special_obj_proto, MIR_item_t imp_special_obj,
  const jit_inline_ext_t *ext
) {
  int inl_max_stack = callee->max_stack > 0 ? callee->max_stack : 4;
  MIR_reg_t inl_vs[inl_max_stack];
  for (int i = 0; i < inl_max_stack; i++) {
    char rn[32]; snprintf(rn, sizeof(rn), "inl%d_s%d", id, i);
    inl_vs[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
  }
  int isp = 0;

  /* Unboxed (double) mirror of inl_vs slots. A slot with inl_num[k] set
     holds its live value only in inl_d[k]; inl_vs[k] is stale until
     INL_FLUSH_SLOT boxes it. Every case that does not understand typed
     slots must flush the slots it reads and leave its outputs untyped. */
  MIR_reg_t inl_d[inl_max_stack];
  uint8_t inl_num[inl_max_stack];
  for (int i = 0; i < inl_max_stack; i++) {
    char rn[32]; snprintf(rn, sizeof(rn), "inl%d_dt%d", id, i);
    inl_d[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_D, rn);
    inl_num[i] = 0;
  }

#define INL_ENSURE_D_SLOT() do {                                        \
    if (!*p_d_slot) {                                                   \
      *p_d_slot = MIR_new_func_reg(ctx, jit_func->u.func,               \
                                   MIR_T_I64, "d_slot_inl");            \
      MIR_append_insn(ctx, jit_func,                                    \
        MIR_new_insn(ctx, MIR_ALLOCA,                                   \
          MIR_new_reg_op(ctx, *p_d_slot),                               \
          MIR_new_uint_op(ctx, 8)));                                    \
    }                                                                   \
  } while (0)
#define INL_FLUSH_SLOT(k) do {                                          \
    int _fk = (k);                                                      \
    if (inl_num[_fk]) {                                                 \
      INL_ENSURE_D_SLOT();                                              \
      mir_d_to_i64(ctx, jit_func, inl_vs[_fk], inl_d[_fk], *p_d_slot);  \
      inl_num[_fk] = 0;                                                 \
    }                                                                   \
  } while (0)
#define INL_FLUSH_ALL() do {                                            \
    for (int _fa = 0; _fa < isp; _fa++) INL_FLUSH_SLOT(_fa);            \
  } while (0)

  int inl_n_locals = callee->max_locals;
  MIR_reg_t inl_locals[inl_n_locals > 0 ? inl_n_locals : 1];
  for (int i = 0; i < inl_n_locals; i++) {
    char rn[32]; snprintf(rn, sizeof(rn), "inl%d_l%d", id, i);
    inl_locals[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
    mir_load_imm(ctx, jit_func, inl_locals[i], mkval(T_UNDEF, 0));
  }

  /* Scratch undef reg for nested-call this/super/new_target arguments.
     Loaded at entry (not lazily — every branch must see it initialized),
     but only when the body actually contains a call op, so pure inline
     bodies pay nothing. */
  MIR_reg_t inl_undef = 0;
  {
    const uint8_t *scan = callee->code;
    const uint8_t *scan_end = callee->code + callee->code_len;
    while (scan < scan_end) {
      sv_op_t sop = (sv_op_t)*scan;
      if (sop == OP_CALL || sop == OP_CALL_METHOD ||
          sop == OP_TAIL_CALL || sop == OP_TAIL_CALL_METHOD) {
        char rn[32]; snprintf(rn, sizeof(rn), "inl%d_undef", id);
        inl_undef = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn);
        mir_load_imm(ctx, jit_func, inl_undef, mkval(T_UNDEF, 0));
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
  uint8_t *ip  = callee->code;
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
        INL_FLUSH_SLOT(isp - 1);
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_PUT_LOCAL8: {
        INL_FLUSH_SLOT(isp - 1);
        uint8_t idx = sv_get_u8(ip + 1);
        if (idx >= (uint8_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[--isp])));
        break;
      }
      case OP_SET_LOCAL: {
        INL_FLUSH_SLOT(isp - 1);
        uint16_t idx = sv_get_u16(ip + 1);
        if (idx >= (uint16_t)inl_n_locals) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, inl_locals[idx]),
            MIR_new_reg_op(ctx, inl_vs[isp - 1])));
        break;
      }
      case OP_SET_LOCAL8: {
        INL_FLUSH_SLOT(isp - 1);
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
        if (jit_upvalue_is_builder_target(callee, idx))
          mir_emit_branch_if_string_builder(ctx, jit_func, dst, r_bool, slow);
        break;
      }

      case OP_POP: isp--; inl_num[isp] = 0; break;
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
        if (isp < 2) return false;
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
        if (isp < 2) return false;
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
        if (isp < 2) return false;
        INL_FLUSH_ALL();
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
        INL_FLUSH_ALL();
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
        int str_bc_off = (int)(ip - callee->code);
        uint8_t fb = sv_func_type_feedback(callee)
          ? sv_func_type_feedback(callee)[str_bc_off] : 0;
        bool fb_str_only = op == OP_ADD && fb && !(fb & ~SV_TFB_STR);
        if (fb_str_only) {
          INL_FLUSH_SLOT(isp - 1);
          INL_FLUSH_SLOT(isp - 2);
          MIR_reg_t rr = inl_vs[--isp];
          MIR_reg_t rl = inl_vs[--isp];
          MIR_reg_t rd = inl_vs[isp++];
          inl_num[isp - 1] = 0;
          mir_emit_string_concat_fastpath(
            ctx, jit_func, r_js, rl, rr, rd, slow, id, str_bc_off
          );
          break;
        }

        if (inl_num[isp - 1] && inl_num[isp - 2]) {
          MIR_insn_code_t dop;
          switch (op) {
            case OP_ADD: case OP_ADD_NUM: dop = MIR_DADD; break;
            case OP_SUB: case OP_SUB_NUM: dop = MIR_DSUB; break;
            case OP_MUL: case OP_MUL_NUM: dop = MIR_DMUL; break;
            default:                      dop = MIR_DDIV; break;
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
        INL_FLUSH_SLOT(isp - 1);
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
        if (inl_num[isp - 1] && inl_num[isp - 2]) {
          MIR_insn_code_t dcmp;
          switch (op) {
            case OP_LT: dcmp = MIR_DLT; break;
            case OP_LE: dcmp = MIR_DLE; break;
            case OP_GT: dcmp = MIR_DGT; break;
            default:    dcmp = MIR_DGE; break;
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
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_seq,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_SNE: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_sne,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_EQ: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_eq,
                         r_vm, r_js, rl, rr);
        break;
      }
      case OP_NE: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t rr = inl_vs[--isp];
        MIR_reg_t rl = inl_vs[--isp];
        MIR_reg_t rd = inl_vs[isp++];
        mir_call_helper2(ctx, jit_func, rd,
                         helper2_proto, imp_ne,
                         r_vm, r_js, rl, rr);
        break;
      }

      case OP_IS_UNDEF: case OP_IS_NULL: {
        INL_FLUSH_SLOT(isp - 1);
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

      case OP_IS_UNDEF_OR_NULL: {
        INL_FLUSH_SLOT(isp - 1);
        MIR_reg_t rs = inl_vs[isp - 1];
        MIR_label_t is_true = MIR_new_label(ctx);
        MIR_label_t is_done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_true),
            MIR_new_reg_op(ctx, rs),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_true),
            MIR_new_reg_op(ctx, rs),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        mir_load_imm(ctx, jit_func, rs, js_false);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
        MIR_append_insn(ctx, jit_func, is_true);
        mir_load_imm(ctx, jit_func, rs, js_true);
        MIR_append_insn(ctx, jit_func, is_done);
        break;
      }

      case OP_JMP: {
        INL_FLUSH_ALL();
        int target = inl_bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        if (!lbl) return false;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl)));
        break;
      }

      case OP_JMP_NOT_NULLISH: {
        INL_FLUSH_ALL();
        MIR_reg_t cond = inl_vs[isp - 1];
        int target = inl_bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = inl_label_for_offset(ctx, &inl_lm, target, isp);
        if (!lbl) return false;
        MIR_label_t done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, done),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, lbl),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        MIR_append_insn(ctx, jit_func, done);
        break;
      }

      case OP_JMP_TRUE_PEEK: case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE: case OP_JMP_FALSE:
      case OP_JMP_TRUE8: case OP_JMP_FALSE8: {
        INL_FLUSH_ALL();
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
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
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
          ctx, jit_func, dst, result, r_bool, slow, join, gf_done
        );
        MIR_append_insn(ctx, jit_func, gf_done);
        break;
      }
      case OP_GET_FIELD_OPT: {
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[--isp];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_label_t nullish = MIR_new_label(ctx);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_label_t value_ok = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
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
          ctx, jit_func, dst, result, r_bool, slow, join, value_ok
        );
        MIR_append_insn(ctx, jit_func, value_ok);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP,
            MIR_new_label_op(ctx, no_err)));
        MIR_append_insn(ctx, jit_func, nullish);
        mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }
      case OP_GET_FIELD2: {
        INL_FLUSH_SLOT(isp - 1);
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t obj = inl_vs[isp - 1];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_label_t value_ok = MIR_new_label(ctx);
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
          ctx, jit_func, dst, result, r_bool, slow, join, value_ok
        );
        MIR_append_insn(ctx, jit_func, value_ok);
        break;
      }
      case OP_GET_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *atom = &callee->atoms[idx];
        MIR_reg_t dst = inl_vs[isp++];
        MIR_label_t gg_ok = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, gg_proto),
            MIR_new_ref_op(ctx, imp_gg),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)callee),
            MIR_new_int_op(ctx, (int64_t)inl_bc_off)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_URSH,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, dst),
            MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, gg_ok),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, JIT_ERR_TAG)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, result),
            MIR_new_reg_op(ctx, dst)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, join)));
        MIR_append_insn(ctx, jit_func, gg_ok);
        break;
      }

      case OP_PUT_FIELD: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        uint32_t pf_idx = sv_get_u32(ip + 1);
        if (pf_idx >= (uint32_t)callee->atom_count) return false;
        sv_atom_t *pf_atom = &callee->atoms[pf_idx];
        uint16_t pf_ic_idx = sv_get_u16(ip + 5);
        sv_ic_entry_t *pf_ic = NULL;
        if (callee->ic_slots && pf_ic_idx != UINT16_MAX && pf_ic_idx < callee->ic_count)
          pf_ic = &callee->ic_slots[pf_ic_idx];
        MIR_reg_t pf_val = inl_vs[--isp];
        MIR_reg_t pf_obj = inl_vs[--isp];
        MIR_label_t pf_slow = MIR_new_label(ctx);
        MIR_label_t pf_ok = MIR_new_label(ctx);
        if (mir_emit_put_field_ic_fastpath(
          ctx, jit_func, callee, (int)(ip - callee->code), pf_ic_idx, pf_atom,
          r_js, pf_obj, pf_val, pf_slow, r_ic_epoch,
          ext->remember_obj_proto, ext->imp_remember_obj
        )) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, pf_ok)));
          MIR_append_insn(ctx, jit_func, pf_slow);
        }
        /* Errors land in `result` and raise at the join — a side effect
           must never fall back to `slow` (generic re-execution). */
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
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        MIR_reg_t ge_key = inl_vs[--isp];
        MIR_reg_t ge_obj = inl_vs[--isp];
        MIR_reg_t ge_dst = inl_vs[isp++];
        MIR_label_t ge_ok = MIR_new_label(ctx);
        mir_call_helper2(ctx, jit_func, ge_dst,
                         helper2_proto, ext->imp_get_elem_inline,
                         r_vm, r_js, ge_obj, ge_key);
        mir_emit_inline_read_guard(
          ctx, jit_func, ge_dst, result, r_bool, slow, join, ge_ok
        );
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
          id, inl_bc_off
        );
        mir_emit_inline_read_guard(
          ctx, jit_func, gl_dst, result, r_bool, slow, join, gl_ok
        );
        MIR_append_insn(ctx, jit_func, gl_ok);
        break;
      }

      case OP_BAND: case OP_BOR: case OP_BXOR:
      case OP_SHL:  case OP_SHR: case OP_USHR: {
        INL_FLUSH_SLOT(isp - 1);
        INL_FLUSH_SLOT(isp - 2);
        /* Num-guarded operands cannot bail or throw in the helper. */
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 1], slow);
        mir_emit_is_num_guard(ctx, jit_func, r_bool, inl_vs[isp - 2], slow);
        MIR_reg_t bw_rr = inl_vs[--isp];
        MIR_reg_t bw_rl = inl_vs[--isp];
        MIR_reg_t bw_rd = inl_vs[isp++];
        MIR_item_t bw_imp;
        switch (op) {
          case OP_BAND: bw_imp = ext->imp_band; break;
          case OP_BOR:  bw_imp = ext->imp_bor;  break;
          case OP_BXOR: bw_imp = ext->imp_bxor; break;
          case OP_SHL:  bw_imp = ext->imp_shl;  break;
          case OP_SHR:  bw_imp = ext->imp_shr;  break;
          default:      bw_imp = ext->imp_ushr; break;
        }
        mir_call_helper2(ctx, jit_func, bw_rd,
                         helper2_proto, bw_imp, r_vm, r_js, bw_rl, bw_rr);
        break;
      }

      case OP_CALL: case OP_TAIL_CALL:
      case OP_CALL_METHOD: case OP_TAIL_CALL_METHOD: {
        bool nc_method = (op == OP_CALL_METHOD || op == OP_TAIL_CALL_METHOD);
        bool nc_tail = (op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD);
        uint16_t nc_argc = sv_get_u16(ip + 1);
        if (nc_argc > 16) return false;
        if (isp < (int)nc_argc + (nc_method ? 2 : 1)) return false;
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

        /* Devirt-in-inline: mirror the main emitter's known-target
           dispatch so calls inside an inline body stay JIT->JIT.
           Routing them through the generic helpers instead measured
           DeltaBlue −7% (that is why call-op inlining was gated off). */
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

          /* super() cannot appear in inlineable bodies (derived ctors are
             excluded), but a callee equal to the inlinee's super value gets
             the generic path, matching the main emitter's dispatch order. */
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
              MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
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
          /* Tail position: the call's value (error or not) IS the return
             value; the join's throw-check dispatches errors. */
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
        mir_load_imm(ctx, jit_func, result, mkval(T_UNDEF, 0));
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
          mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
        }
        break;
      }

      case OP_NOP: case OP_LINE_NUM: case OP_COL_NUM: case OP_LABEL:
        break;

      default:
        return false;
    }
    ip += sz;
  }
  return true;
}

static void scan_branch_targets(sv_func_t *func, jit_label_map_t *lm,  MIR_context_t ctx) {
  uint8_t *ip   = func->code;
  uint8_t *end  = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_BRANCH32) != 0) {
      int off = (int)(ip - func->code) + sv_get_i32(ip + 1) + sz;
      label_for_offset(ctx, lm, off);
    } else if ((flags & SV_OPF_JIT_BRANCH8) != 0) {
      int off = (int)(ip - func->code) + (int8_t)sv_get_i8(ip + 1) + sz;
      label_for_offset(ctx, lm, off);
    }
    ip += sz;
  }
}


static bool jit_local_has_numeric_hint(sv_func_t *func, int idx) {
  if (!func || idx < 0) return false;
  if (idx >= func->max_locals) return false;
  sv_type_info_t *local_types = sv_func_local_types(func);
  if (local_types && idx < func->local_type_count &&
      local_types[idx].type == SV_TI_NUM)
    return true;
  if (func->local_type_feedback) {
    uint8_t ltf = func->local_type_feedback[idx];
    if (ltf && !(ltf & ~SV_TFB_NUM)) return true;
  }
  return false;
}

typedef struct {
  bool needs_bailout;
  bool needs_inc_local;
  bool needs_args_buf;
  bool needs_iter_roots;
  bool needs_close_upval;
  bool needs_tco_args;
  bool needs_ic_epoch;
  bool needs_this;
  bool *builder_target_slots;
} jit_features_t;

static jit_features_t jit_prescan_features(sv_func_t *func, int n_slots) {
  jit_features_t f = {0};
  if (n_slots > 0)
    f.builder_target_slots = calloc((size_t)n_slots, sizeof(bool));
  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;
    uint16_t flags = sv_op_flags[op];
    if ((flags & SV_OPF_JIT_NEEDS_BAILOUT) != 0) f.needs_bailout = true;
    if ((flags & SV_OPF_JIT_NEEDS_INC_LOCAL) != 0) f.needs_inc_local = true;
    if ((flags & SV_OPF_JIT_NEEDS_ARGS_BUF) != 0) f.needs_args_buf = true;
    if ((flags & SV_OPF_JIT_NEEDS_TCO_ARGS) != 0) f.needs_tco_args = true;
    if ((flags & SV_OPF_JIT_NEEDS_ITER_ROOTS) != 0) f.needs_iter_roots = true;
    if ((flags & SV_OPF_JIT_NEEDS_CLOSE_UPVAL) != 0) f.needs_close_upval = true;
    if ((flags & SV_OPF_JIT_NEEDS_IC_EPOCH) != 0) f.needs_ic_epoch = true;
    if (op == OP_THIS || op == OP_CLOSURE || op == OP_EVAL)
      f.needs_this = true;
    if ((flags & SV_OPF_BUILDER_TARGET) != 0 && f.builder_target_slots) {
      uint16_t slot = sv_get_u16(ip + 1);
      if ((int)slot < n_slots) f.builder_target_slots[slot] = true;
    }
    if ((flags & SV_OPF_JIT_LOCAL_NUMERIC_BAILOUT) != 0) {
      if (op == OP_PUT_LOCAL || op == OP_SET_LOCAL) {
        uint16_t idx = sv_get_u16(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      } else {
        uint8_t idx = sv_get_u8(ip + 1);
        if (jit_local_has_numeric_hint(func, idx))
          f.needs_bailout = true;
      }
    }
    ip += sz;
  }
  if (f.needs_bailout) f.needs_args_buf = true;
  return f;
}

static bool jit_value_is_closure(
  ant_value_t value, const sv_closure_t *expected
) {
  if (vtype(value) != T_FUNC) return false;
  return js_func_closure(value) == expected;
}

static bool jit_try_get_global_data(
  ant_t *js, sv_func_t *func, uint8_t *ip,
  const sv_atom_t *atom, ant_value_t *out
) {
  if (!js || !func || !ip || !atom || !out) return false;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  return sv_global_ic_try_get_hit(js->global, ic, atom->str, out) ||
    sv_global_ic_try_fill(js->global, ic, atom->str, out);
}

static bool jit_mark_self_binding_guards(
  ant_t *js, sv_func_t *func, sv_closure_t *hint_closure,
  uint8_t *guard_sites
) {
  if (!js || !func || !hint_closure || !guard_sites) return false;

  bool found = false;
  uint8_t *ip = func->code;
  uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    int sz = sv_op_size[op];
    if (sz == 0) break;

    if (op == OP_GET_UPVAL) {
      uint16_t idx = sv_get_u16(ip + 1);
      if (idx < (uint16_t)func->upvalue_count && func->upval_descs &&
          hint_closure->upvalues && hint_closure->upvalues[idx] &&
          jit_value_is_closure(
            *hint_closure->upvalues[idx]->location, hint_closure)) {
        guard_sites[ip - func->code] = 1;
        found = true;
      }
    } else if (op == OP_GET_GLOBAL || op == OP_GET_GLOBAL_UNDEF) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx < (uint32_t)func->atom_count) {
        sv_atom_t *atom = &func->atoms[idx];
        ant_value_t value;
        if (jit_try_get_global_data(js, func, ip, atom, &value) &&
            jit_value_is_closure(value, hint_closure)) {
          guard_sites[ip - func->code] = 1;
          found = true;
        }
      }
    }

    ip += sz;
  }
  return found;
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
    if ((sv_op_flags[op] & SV_OPF_JIT_ELIGIBLE) == 0) {
      if (op != OP_RE_LITERAL_EXEC &&
          op != OP_STR_RE_LITERAL_REPLACE &&
          op != OP_RE_EXEC_TRUTHY) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op %s in %s\n",
                  (op < OP__COUNT && sv_op_names[op]) ? sv_op_names[op] : "???",
                  func->debug->name ? func->debug->name : "<anonymous>");
      }
      eligible = false;
    } else if (op == OP_CLOSURE) {
      uint32_t idx = sv_get_u32(ip + 1);
      if (idx >= (uint32_t)func->const_count) return false;
      ant_value_t cv = func->constants[idx];
      if (vtype(cv) != T_NTARG) return false;
    } else if (op == OP_SPECIAL_OBJ) {
      if (sv_get_u8(ip + 1) == 0) {
        if (sv_jit_warn_unlikely)
          fprintf(stderr, "jit: ineligible op SPECIAL_OBJ(%d) in %s\n",
                  sv_get_u8(ip + 1),
                  func->debug->name ? func->debug->name : "<anonymous>");
        eligible = false;
      }
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

  if (!jit_is_eligible(func)) {
    func->jit_compile_failed = true;
    return NULL;
  }

  func->jit_compiling = true;
  sv_jit_ctx_t *jc = jit_ctx_get(js);

  if (!jc) {
    sv_jit_init(js);
    jc = jit_ctx_get(js);
  }

  if (!jc) {
    func->jit_compiling = false;
    return NULL;
  }

  jit_load_externals_once(jc);
  /* Loop-hot functions amortize codegen cost; the call-triggered long tail
     wants compile latency over code quality. */
  bool jit_compile_hot = func->jit_loop_hot ||
                         func->back_edge_count >= SV_JIT_OSR_THRESHOLD / 8;
  MIR_context_t ctx = jit_compile_hot ? jc->ctx_hot : jc->ctx;

  char fname[128];
  snprintf(fname, sizeof(fname), "jit_%s_%p",
           func->debug->name ? func->debug->name : "anon", (void *)func);

  MIR_module_t mod = MIR_new_module(ctx, fname);
  MIR_type_t ret_type = MIR_JSVAL;

  MIR_item_t self_proto = MIR_new_proto(ctx, "jit_proto",
    1, &ret_type,
    7,
    MIR_T_I64, "vm",
    MIR_JSVAL,  "this_val",
    MIR_JSVAL,  "new_target",
    MIR_JSVAL,  "super_val",
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

  MIR_type_t private_put_ret = MIR_JSVAL;
  MIR_item_t private_put_proto = MIR_new_proto(ctx, "private_put_proto",
    1, &private_put_ret,
    5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "obj",
    MIR_JSVAL, "val",
    MIR_JSVAL, "token");

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

  MIR_type_t call_call_ret = MIR_JSVAL;
  MIR_item_t call_call_proto = MIR_new_proto(ctx, "call_call_proto",
    1, &call_call_ret,
    5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js_p",
    MIR_T_P,   "base",
    MIR_T_I32, "n1",
    MIR_T_I32, "n2");

  MIR_item_t call_call_slot_proto = MIR_new_proto(ctx, "call_call_slot_proto",
    1, &call_call_ret,
    5,
    MIR_T_I64, "vm",
    MIR_T_I64, "js_p",
    MIR_JSVAL, "func",
    MIR_JSVAL, "arg1",
    MIR_T_P,   "slot");

  MIR_type_t call_method_ret = MIR_JSVAL;
  MIR_item_t call_method_proto = MIR_new_proto(ctx, "callm_proto",
    1, &call_method_ret,
    9,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL, "func",
    MIR_JSVAL, "this_val",
    MIR_T_P,   "args",
    MIR_T_I32, "argc",
    MIR_JSVAL, "super_val",
    MIR_JSVAL, "new_target",
    MIR_T_P,   "out_this");

  MIR_type_t gg_ret = MIR_JSVAL;
  MIR_item_t gg_proto = MIR_new_proto(ctx, "gg_proto",
    1, &gg_ret, 4,
    MIR_T_I64, "js",
    MIR_T_P,   "str",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t geg_ret = MIR_JSVAL;
  MIR_item_t get_eval_global_proto = MIR_new_proto(ctx, "geg_proto",
    1, &geg_ret, 7,
    MIR_T_I64, "js",
    MIR_T_P,   "closure",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off",
    MIR_T_I32, "allow_missing");

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

  MIR_type_t impd_ret = MIR_JSVAL;
  MIR_item_t import_default_proto = MIR_new_proto(ctx, "impd_proto",
    1, &impd_ret, 2,
    MIR_T_I64, "js",
    MIR_JSVAL,  "ns");

  MIR_type_t impn_ret = MIR_JSVAL;
  MIR_item_t import_named_proto = MIR_new_proto(ctx, "impn_proto",
    1, &impn_ret, 6,
    MIR_T_I64, "js",
    MIR_JSVAL,  "ns",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_P,   "func",
    MIR_T_I32, "bc_off");

  MIR_type_t exp_ret = MIR_JSVAL;
  MIR_item_t export_proto = MIR_new_proto(ctx, "exp_proto",
    1, &exp_ret, 5,
    MIR_T_I64, "js",
    MIR_T_P,   "closure",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_JSVAL,  "val");

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

  MIR_type_t ts_ret = MIR_JSVAL;
  MIR_item_t to_string_proto = MIR_new_proto(ctx, "to_string_proto",
    1, &ts_ret, 2,
    MIR_T_I64, "js",
    MIR_JSVAL, "v");

  MIR_type_t normalize_this_ret = MIR_JSVAL;
  MIR_item_t normalize_this_proto = MIR_new_proto(ctx, "normalize_this_proto",
    1, &normalize_this_ret, 2,
    MIR_T_I64, "js",
    MIR_JSVAL, "value");

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
    1, &br_ret, 12,
    MIR_T_I64,  "vm",
    MIR_T_P,    "closure",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_P,    "vstack",
    MIR_T_I64,  "vstack_sp",
    MIR_T_P,    "params",
    MIR_T_I64,  "n_params",
    MIR_T_P,    "locals",
    MIR_T_I64,  "n_locals",
    MIR_T_I64,  "bc_offset");

  MIR_type_t cl_ret = MIR_JSVAL;
  MIR_item_t closure_proto = MIR_new_proto(ctx, "closure_proto",
    1, &cl_ret, 11,
    MIR_T_I64,  "vm",
    MIR_T_I64,  "js",
    MIR_T_P,    "parent",
    MIR_JSVAL,  "this_val",
    MIR_T_P,    "slots",
    MIR_T_I32,  "slot_base",
    MIR_T_I32,  "slot_count",
    MIR_T_I32,  "const_idx",
    MIR_T_P,    "name",
    MIR_T_I32,  "name_len",
    MIR_T_P,    "open_upvalues");

  MIR_item_t close_upval_proto = MIR_new_proto(ctx, "close_upval_proto",
    0, NULL, 5,
    MIR_T_I64, "vm",
    MIR_T_I32, "slot_idx",
    MIR_T_P,   "locals",
    MIR_T_I32, "n_locals",
    MIR_T_P,   "open_upvalues");

  MIR_item_t upval_barrier_proto = MIR_new_proto(ctx, "upval_barrier_proto",
    0, NULL, 3,
    MIR_T_I64, "js",
    MIR_T_P,   "uv",
    MIR_T_I64, "val");

  MIR_item_t adopt_open_upvalues_proto = MIR_new_proto(ctx, "adopt_open_upvalues_proto",
    0, NULL, 2,
    MIR_T_I64, "vm",
    MIR_T_P,   "open_upvalues");

  MIR_item_t take_open_upvalues_proto = MIR_new_proto(ctx, "take_open_upvalues_proto",
    0, NULL, 4,
    MIR_T_I64, "vm",
    MIR_T_P,   "open_upvalues",
    MIR_T_P,   "slots",
    MIR_T_I32, "slot_count");

  MIR_item_t take_open_upvalues_rebase_proto = MIR_new_proto(ctx, "take_open_upvalues_rebase_proto",
    0, NULL, 5,
    MIR_T_I64, "vm",
    MIR_T_P,   "open_upvalues",
    MIR_T_P,   "src_slots",
    MIR_T_P,   "dst_slots",
    MIR_T_I32, "slot_count");

  MIR_item_t set_name_proto = MIR_new_proto(ctx, "sn_proto",
    0, NULL, 4,
    MIR_T_I64, "js",
    MIR_JSVAL,  "fn",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

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

  MIR_item_t define_slot_proto = MIR_new_proto(ctx, "ds_proto",
    0, NULL, 7,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_JSVAL,  "obj",
    MIR_JSVAL,  "val",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_I32, "slot");

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
    MIR_T_P,   "atom",
    MIR_T_P,   "ic");

  MIR_item_t remember_obj_proto = MIR_new_proto(ctx, "remember_obj_proto",
    0, NULL, 2,
    MIR_T_I64, "js",
    MIR_T_P,   "obj");

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

  MIR_type_t peg_ret = MIR_JSVAL;
  MIR_item_t put_eval_global_proto = MIR_new_proto(ctx, "peg_proto",
    1, &peg_ret, 6,
    MIR_T_I64, "js",
    MIR_T_P,   "closure",
    MIR_JSVAL, "val",
    MIR_T_P,   "str",
    MIR_T_I32, "len",
    MIR_T_I32, "strict");

  MIR_type_t dev_ret = MIR_JSVAL;
  MIR_item_t delete_eval_var_proto = MIR_new_proto(ctx, "dev_proto",
    1, &dev_ret, 4,
    MIR_T_I64, "js",
    MIR_T_P,   "closure",
    MIR_T_P,   "str",
    MIR_T_I32, "len");

  MIR_type_t obj_ret = MIR_JSVAL;
  MIR_item_t object_proto = MIR_new_proto(ctx, "obj_proto",
    1, &obj_ret, 4,
    MIR_T_I64, "vm",
    MIR_T_I64, "js",
    MIR_T_P,   "func",
    MIR_T_P,   "site");

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
  MIR_item_t imp_str_read_value =
    MIR_new_import(ctx, "jit_helper_str_read_value");
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
  MIR_item_t imp_call_method = MIR_new_import(ctx, "jit_helper_call_method");
  MIR_item_t imp_call_array_includes = MIR_new_import(ctx, "jit_helper_call_array_includes");
  MIR_item_t imp_apply = MIR_new_import(ctx, "jit_helper_apply");
  MIR_item_t imp_call_call = MIR_new_import(ctx, "jit_helper_call_call");
  MIR_item_t imp_call_call_slot = MIR_new_import(ctx, "jit_helper_call_call_slot");
  MIR_item_t imp_rest  = MIR_new_import(ctx, "jit_helper_rest");
  MIR_item_t imp_special_obj = MIR_new_import(ctx, "jit_helper_special_obj");
  MIR_item_t imp_for_of      = MIR_new_import(ctx, "jit_helper_for_of");
  MIR_item_t imp_dnext       = MIR_new_import(ctx, "jit_helper_destructure_next");
  MIR_item_t imp_dclose      = MIR_new_import(ctx, "jit_helper_destructure_close");
  MIR_item_t imp_gg         = MIR_new_import(ctx, "jit_helper_get_global");
  MIR_item_t imp_get_eval_global =
    MIR_new_import(ctx, "jit_helper_get_eval_global");
  MIR_item_t imp_put_eval_global =
    MIR_new_import(ctx, "jit_helper_put_eval_global");
  MIR_item_t imp_delete_eval_var =
    MIR_new_import(ctx, "jit_helper_delete_eval_var");
  MIR_item_t imp_get_field  = MIR_new_import(ctx, "jit_helper_get_field");
  MIR_item_t imp_get_field_inline =
    MIR_new_import(ctx, "jit_helper_get_field_inline");
  MIR_item_t imp_import_default = MIR_new_import(ctx, "jit_helper_import_default");
  MIR_item_t imp_import_named   = MIR_new_import(ctx, "jit_helper_import_named");
  MIR_item_t imp_export         = MIR_new_import(ctx, "jit_helper_export");
  MIR_item_t imp_to_propkey = MIR_new_import(ctx, "jit_helper_to_propkey");
  MIR_item_t imp_to_string = MIR_new_import(ctx, "js_template_to_string");
  MIR_item_t imp_resume     = MIR_new_import(ctx, "jit_helper_bailout_resume");
  MIR_item_t imp_close_upval = MIR_new_import(ctx, "jit_helper_close_upval");
  MIR_item_t imp_upval_barrier = MIR_new_import(ctx, "jit_helper_upval_barrier");
  MIR_item_t imp_adopt_open_upvalues = MIR_new_import(ctx, "jit_helper_adopt_open_upvalues");
  MIR_item_t imp_take_open_upvalues = MIR_new_import(ctx, "jit_helper_take_open_upvalues");
  MIR_item_t imp_take_open_upvalues_rebase = MIR_new_import(ctx, "jit_helper_take_open_upvalues_rebase");
  MIR_item_t imp_closure     = MIR_new_import(ctx, "jit_helper_closure");
  MIR_item_t imp_in          = MIR_new_import(ctx, "jit_helper_in");
  MIR_item_t imp_get_length  = MIR_new_import(ctx, "jit_helper_get_length");
  MIR_item_t imp_get_length_inline =
    MIR_new_import(ctx, "jit_helper_get_length_inline");
  MIR_item_t imp_define_field = MIR_new_import(ctx, "jit_helper_define_field");
  MIR_item_t imp_define_method_comp = MIR_new_import(ctx, "jit_helper_define_method_comp");
  MIR_item_t imp_seq         = MIR_new_import(ctx, "jit_helper_seq");
  MIR_item_t imp_eq          = MIR_new_import(ctx, "jit_helper_eq");
  MIR_item_t imp_ne          = MIR_new_import(ctx, "jit_helper_ne");
  MIR_item_t imp_sne         = MIR_new_import(ctx, "jit_helper_sne");
  MIR_item_t imp_put_field   = MIR_new_import(ctx, "jit_helper_put_field_ic");
  MIR_item_t imp_remember_obj = MIR_new_import(ctx, "gc_remember_add");
  MIR_item_t imp_get_elem    = MIR_new_import(ctx, "jit_helper_get_elem");
  MIR_item_t imp_put_elem    = MIR_new_import(ctx, "jit_helper_put_elem");
  MIR_item_t imp_get_private = MIR_new_import(ctx, "jit_helper_get_private");
  MIR_item_t imp_put_private = MIR_new_import(ctx, "jit_helper_put_private");
  MIR_item_t imp_put_global  = MIR_new_import(ctx, "jit_helper_put_global");
  MIR_item_t imp_object      = MIR_new_import(ctx, "jit_helper_object");
  MIR_item_t imp_define_slot = MIR_new_import(ctx, "jit_helper_define_slot");
  MIR_item_t imp_array       = MIR_new_import(ctx, "jit_helper_array");
  MIR_item_t imp_catch_value = MIR_new_import(ctx, "jit_helper_catch_value");
  MIR_item_t imp_throw       = MIR_new_import(ctx, "jit_helper_throw");
  MIR_item_t imp_throw_error = MIR_new_import(ctx, "jit_helper_throw_error");
  MIR_item_t imp_set_proto   = MIR_new_import(ctx, "jit_helper_set_proto");
  MIR_item_t imp_get_elem2   = MIR_new_import(ctx, "jit_helper_get_elem2");
  MIR_item_t imp_get_elem_inline =
    MIR_new_import(ctx, "jit_helper_get_elem_inline");
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
  MIR_item_t imp_stack_ovf_err  = MIR_new_import(ctx, "jit_helper_stack_overflow_error");
  MIR_item_t imp_normalize_this =
    MIR_new_import(ctx, "jit_helper_normalize_sloppy_this");

  MIR_item_t jit_func = MIR_new_func(ctx, fname,
    1, &ret_type,
    7,
    MIR_T_I64, "vm",
    MIR_JSVAL,  "this_val",
    MIR_JSVAL,  "new_target",
    MIR_JSVAL,  "super_val",
    MIR_T_P,    "args",
    MIR_T_I32,  "argc",
    MIR_T_P,    "closure");

  MIR_reg_t r_vm       = MIR_reg(ctx, "vm",       jit_func->u.func);
  MIR_reg_t r_this     = MIR_reg(ctx, "this_val", jit_func->u.func);
  MIR_reg_t r_new_target = MIR_reg(ctx, "new_target", jit_func->u.func);
  MIR_reg_t r_super_val = MIR_reg(ctx, "super_val", jit_func->u.func);
  MIR_reg_t r_args     = MIR_reg(ctx, "args",     jit_func->u.func);
  MIR_reg_t r_argc     = MIR_reg(ctx, "argc",     jit_func->u.func);
  MIR_reg_t r_closure  = MIR_reg(ctx, "closure",  jit_func->u.func);

  MIR_reg_t r_this_curr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "this_curr");
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_this_curr),
      MIR_new_reg_op(ctx, r_this)));

  MIR_reg_t r_js = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "js_ptr");
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_js),
      MIR_new_mem_op(ctx, MIR_T_I64, 0, r_vm, 0, 1)));

  {
    /* Inline C-stack overflow probe: an alloca'd slot is a frame-address
       proxy; the stack grows down, so overflow iff probe < cstk.floor.
       floor == NULL disables the check (probe >= 0 always, unsigned). */
    MIR_reg_t r_stk_probe = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "stk_probe");
    MIR_reg_t r_stk_floor = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "stk_floor");
    MIR_label_t no_overflow = MIR_new_label(ctx);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_stk_probe),
        MIR_new_int_op(ctx, 8)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_stk_floor),
        MIR_new_mem_op(ctx, MIR_T_I64,
          (MIR_disp_t)offsetof(ant_t, cstk.floor), r_js, 0, 1)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_UBGE,
        MIR_new_label_op(ctx, no_overflow),
        MIR_new_reg_op(ctx, r_stk_probe),
        MIR_new_reg_op(ctx, r_stk_floor)));
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
  vs.known_bool = calloc((size_t)vs.max, sizeof(uint8_t));

  if (!vs.regs || !vs.known_func || !vs.d_regs || !vs.slot_type || !vs.known_const || !vs.has_const || !vs.known_bool) {
    free(vs.regs); 
    free(vs.known_func);
    free(vs.d_regs);
    free(vs.slot_type);
    free(vs.known_const);
    free(vs.has_const);
    free(vs.known_bool);
    
    MIR_finish_func(ctx);
    MIR_finish_module(ctx);
    MIR_remove_module(ctx, mod);
    
    func->jit_compiling = false;
    return NULL;
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
      free(vs.regs);
      free(vs.known_func);
      free(vs.d_regs);
      free(vs.slot_type);
      free(vs.known_const);
      free(vs.has_const);
      free(vs.known_bool);
      free(local_regs);
      free(local_d_regs);
      free(known_func_locals);
      free(known_type_locals);
      
      MIR_finish_func(ctx);
      MIR_finish_module(ctx);
      MIR_remove_module(ctx, mod);
      
      func->jit_compiling = false;
      return NULL;
    }
    
    sv_type_info_t *local_types = sv_func_local_types(func);
    if (local_types && func->local_type_count > 0) {
      int ncopy = func->local_type_count < n_locals ? func->local_type_count : n_locals;
      for (int i = 0; i < ncopy; i++)
        known_type_locals[i] = local_types[i].type;
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

  uint8_t *self_binding_guards = calloc(
    func->code_len > 0 ? (size_t)func->code_len : 1u, 1u);
  jit_features_t feat = jit_prescan_features(
    func, func->param_count + n_locals);
  if (jit_mark_self_binding_guards(
        js, func, hint_closure, self_binding_guards)) {
    feat.needs_bailout = true;
    feat.needs_args_buf = true;
  }


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

  MIR_reg_t r_call_out_this = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "call_out_this");
  MIR_append_insn(ctx, jit_func,
    MIR_new_insn(ctx, MIR_ALLOCA,
      MIR_new_reg_op(ctx, r_call_out_this),
      MIR_new_uint_op(ctx, sizeof(ant_value_t))));

  bool normalize_sloppy_this =
    feat.needs_this && !func->is_strict && !func->is_arrow;
  MIR_reg_t r_this_root = 0;
  if (normalize_sloppy_this) {
    r_this_root =
      MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "this_root");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_this_root),
        MIR_new_uint_op(ctx, sizeof(ant_value_t))));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_this_root, 0, 1),
        MIR_new_reg_op(ctx, r_this_curr)));
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
  bool writes_params = func_writes_params(func);
  bool *captured_params = scan_captured_params(func);
  bool *captured_locals = scan_captured_locals(func, n_locals);
  bool *builder_target_slots = feat.builder_target_slots;
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
  /* "d-only" locals: uncaptured, statically numeric, never retyped by
     SET_LOCAL_UNDEF (unless immediately re-initialized numeric) and never
     targeted by the string-builder local ops. Their boxed regs are not
     maintained per store; snapshots rebox via mir_emit_dnum_rebox. */
  uint8_t *dnum_locals = NULL;
  if (n_locals > 0 && local_d_regs && known_type_locals) {
    dnum_locals = calloc((size_t)n_locals, 1);
    if (dnum_locals) {
      for (int i = 0; i < n_locals; i++)
        dnum_locals[i] = known_type_locals[i] == SV_TI_NUM &&
                         !(captured_locals && captured_locals[i]);
      uint8_t *dp = func->code, *de = func->code + func->code_len;
      while (dp < de) {
        sv_op_t dop = (sv_op_t)*dp;
        int dsz = sv_op_size[dop];
        if (dsz == 0) break;
        if (dop == OP_SET_LOCAL_UNDEF) {
          uint16_t di = sv_get_u16(dp + 1);
          if (di < (uint16_t)n_locals &&
              !jit_has_immediate_numeric_local_init(func, dp + dsz, de, di))
            dnum_locals[di] = 0;
        } else if ((sv_op_flags[dop] & SV_OPF_BUILDER_TARGET) != 0) {
          uint16_t ds = sv_get_u16(dp + 1);
          if (ds >= (uint16_t)param_count &&
              (int)ds - param_count < n_locals)
            dnum_locals[ds - param_count] = 0;
        }
        dp += dsz;
      }
    }
  }
  jit_cur_dnum_locals = dnum_locals;
  jit_cur_local_d_regs = local_d_regs;

  bool params_in_slotbuf = writes_params || has_captured_params;
  bool has_captured_slots = params_in_slotbuf || has_captures;
  bool use_unified_slotbuf = has_captured_slots && has_captures;
  int slotbuf_count = use_unified_slotbuf ? (param_count + n_locals) : param_count;

  MIR_reg_t r_param_init_argc = r_argc;
  if (writes_params && param_count > 0) {
    r_param_init_argc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "param_init_argc");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_param_init_argc),
        MIR_new_reg_op(ctx, r_argc)));

    MIR_disp_t osr_base = (MIR_disp_t)offsetof(struct sv_vm, jit_osr);
    MIR_label_t param_argc_done = MIR_new_label(ctx);
    MIR_label_t param_argc_enough = MIR_new_label(ctx);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_bool),
        MIR_new_mem_op(ctx, MIR_T_U8,
          osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
          r_vm, 0, 1)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, param_argc_done),
        MIR_new_reg_op(ctx, r_bool),
        MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_UBGT,
        MIR_new_label_op(ctx, param_argc_enough),
        MIR_new_reg_op(ctx, r_param_init_argc),
        MIR_new_int_op(ctx, (int64_t)param_count - 1)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_param_init_argc),
        MIR_new_int_op(ctx, param_count)));
    MIR_append_insn(ctx, jit_func, param_argc_enough);
    MIR_append_insn(ctx, jit_func, param_argc_done);
  }

  MIR_reg_t r_slotbuf = r_tmp2;
  if (has_captured_slots && slotbuf_count > 0) {
    r_slotbuf = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "slotbuf");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_slotbuf),
        MIR_new_uint_op(ctx, (uint64_t)slotbuf_count * sizeof(ant_value_t))));
    mir_emit_fill_param_slots_from_args(
      ctx, jit_func, r_slotbuf, r_args, r_param_init_argc,
      captured_params, param_count, writes_params);
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

  bool use_jit_upvalue_list = has_captured_slots;
  MIR_reg_t r_jit_open_upvalues = 0;
  if (use_jit_upvalue_list) {
    r_jit_open_upvalues = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, "jit_open_upvalues");
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_jit_open_upvalues),
        MIR_new_uint_op(ctx, sizeof(sv_upvalue_t *))));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_T_I64, 0, r_jit_open_upvalues, 0, 1),
        MIR_new_uint_op(ctx, 0)));
  }

  jit_bailout_emit_t bailout_ctx = {
    .val = r_bailout_val,
    .off = r_bailout_off,
    .sp = r_bailout_sp,
    .tramp = bailout_tramp,
    .args_buf = r_args_buf,
    .vstack = &vs,
    .local_regs = local_regs,
    .n_locals = n_locals,
    .lbuf = r_lbuf,
    .d_slot = r_d_slot,
  };

  if (feat.needs_tco_args && param_count > 0) {
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_ALLOCA,
        MIR_new_reg_op(ctx, r_tco_args),
        MIR_new_uint_op(ctx, (uint64_t)param_count * sizeof(ant_value_t))));
  } else mir_load_imm(ctx, jit_func, r_tco_args, 0);
  jit_label_map_t lm = {0};
  scan_branch_targets(func, &lm, ctx);

  /* Hoist read-only params into registers once per entry instead of a
     branchy argc-checked load at every GET_ARG. Emitted before the OSR
     dispatch so OSR entries populate them too; self-tail jumps re-enter
     here (args/argc are updated before jumping). Params that live in
     slotbuf (written or captured) are excluded. */
  MIR_label_t self_tail_entry = MIR_new_label(ctx);
  MIR_append_insn(ctx, jit_func, self_tail_entry);

  MIR_reg_t param_cache[8] = {0};
  {
    uint8_t read_mask = 0;
    uint8_t *pscan = func->code, *pend = func->code + func->code_len;
    while (pscan < pend) {
      sv_op_t pop_ = (sv_op_t)*pscan;
      int psz = sv_op_size[pop_];
      if (psz == 0) break;
      if (pop_ == OP_GET_ARG) {
        uint16_t pidx = sv_get_u16(pscan + 1);
        if (pidx < 8) read_mask |= (uint8_t)(1u << pidx);
      }
      pscan += psz;
    }
    for (int i = 0; i < param_count && i < 8; i++) {
      if (!(read_mask & (1u << i))) continue;
      if (writes_params || (captured_params && captured_params[i])) continue;
      char prn[16];
      snprintf(prn, sizeof(prn), "parg%d", i);
      param_cache[i] = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, prn);
      MIR_label_t in_range = MIR_new_label(ctx);
      MIR_label_t done = MIR_new_label(ctx);
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_UBGT,
          MIR_new_label_op(ctx, in_range),
          MIR_new_reg_op(ctx, r_argc),
          MIR_new_int_op(ctx, (int64_t)i)));
      mir_load_imm(ctx, jit_func, param_cache[i], mkval(T_UNDEF, 0));
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
      MIR_append_insn(ctx, jit_func, in_range);
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_reg_op(ctx, param_cache[i]),
          MIR_new_mem_op(ctx, MIR_JSVAL,
            (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args, 0, 1)));
      MIR_append_insn(ctx, jit_func, done);
    }
  }

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

    if (known_type_locals && local_d_regs) {
      MIR_label_t osr_types_ok = MIR_new_label(ctx);
      MIR_label_t osr_type_bail = MIR_new_label(ctx);
      bool osr_any_num = false;
      for (int i = 0; i < n_locals; i++)
        if (known_type_locals[i] == SV_TI_NUM) {
          osr_any_num = true;
          mir_emit_is_num_guard(ctx, jit_func, r_bool, local_regs[i], osr_type_bail);
          mir_i64_to_d(ctx, jit_func, local_d_regs[i], local_regs[i], r_d_slot);
        }
      if (osr_any_num) {
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, osr_types_ok)));
        MIR_append_insn(ctx, jit_func, osr_type_bail);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_U8,
              osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
              r_vm, 0, 1),
            MIR_new_int_op(ctx, 0)));
        /* The compiled function is still valid; this frame is simply not
           ready to enter it at the current loop header. */
        mir_load_imm(ctx, jit_func, r_bailout_val,
                     (uint64_t)SV_JIT_RETRY_INTERP);
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_bailout_val)));
        MIR_append_insn(ctx, jit_func, osr_types_ok);
      }
    }

    if (has_captured_params && r_jit_open_upvalues) {
      MIR_append_insn(ctx, jit_func,
        MIR_new_call_insn(ctx, 7,
          MIR_new_ref_op(ctx, take_open_upvalues_rebase_proto),
          MIR_new_ref_op(ctx, imp_take_open_upvalues_rebase),
          MIR_new_reg_op(ctx, r_vm),
          MIR_new_reg_op(ctx, r_jit_open_upvalues),
          MIR_new_reg_op(ctx, r_args),
          MIR_new_reg_op(ctx, r_slotbuf),
          MIR_new_int_op(ctx, param_count)));
    }

    if (has_captures) {
      MIR_reg_t r_osr_lp = MIR_new_func_reg(ctx, jit_func->u.func,
                                             MIR_T_I64, "osr_lp");
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_MOV,
          MIR_new_reg_op(ctx, r_osr_lp),
          MIR_new_mem_op(ctx, MIR_T_P,
            osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, lp),
            r_vm, 0, 1)));

      if (use_unified_slotbuf) {
        for (int i = 0; i < n_locals; i++)
          if (captured_locals && captured_locals[i])
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                  r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[i])));
      } else {
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_lbuf),
            MIR_new_reg_op(ctx, r_osr_lp)));
      }

      if (r_jit_open_upvalues) {
        if (use_unified_slotbuf)
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 7,
              MIR_new_ref_op(ctx, take_open_upvalues_rebase_proto),
              MIR_new_ref_op(ctx, imp_take_open_upvalues_rebase),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_jit_open_upvalues),
              MIR_new_reg_op(ctx, r_osr_lp),
              MIR_new_reg_op(ctx, r_lbuf),
              MIR_new_int_op(ctx, n_locals)));
        else
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 6,
              MIR_new_ref_op(ctx, take_open_upvalues_proto),
              MIR_new_ref_op(ctx, imp_take_open_upvalues),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_jit_open_upvalues),
              MIR_new_reg_op(ctx, r_lbuf),
              MIR_new_int_op(ctx, n_locals)));
      }
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

  MIR_reg_t r_result = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, "result");
  mir_load_imm(ctx, jit_func, r_result, mkval(T_UNDEF, 0));

  uint8_t *ip  = func->code;
  uint8_t *end = func->code + func->code_len;

  bool ok = true; 
  int  call_n  = 0; 
  int  upval_n = 0; 
  int  arith_n = 0; 

#define JIT_EMIT_EXIT_RET(ret_op)                          \
  mir_emit_exit_ret(ctx, jit_func,                         \
    close_upval_proto, imp_close_upval,                    \
    adopt_open_upvalues_proto, imp_adopt_open_upvalues,    \
    r_vm, r_slotbuf, r_lbuf, r_jit_open_upvalues,          \
    has_captured_slots, captured_params, param_count,      \
    has_captures, captured_locals, n_locals,               \
    (ret_op))

#define JIT_EMIT_THROW_IF_ERROR(value_reg) do {                       \
  MIR_label_t no_error = MIR_new_label(ctx);                          \
  MIR_append_insn(ctx, jit_func,                                      \
    MIR_new_insn(ctx, MIR_URSH,                                       \
      MIR_new_reg_op(ctx, r_bool),                                    \
      MIR_new_reg_op(ctx, (value_reg)),                               \
      MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));                       \
  MIR_append_insn(ctx, jit_func,                                      \
    MIR_new_insn(ctx, MIR_BNE,                                        \
      MIR_new_label_op(ctx, no_error),                                \
      MIR_new_reg_op(ctx, r_bool),                                    \
      MIR_new_uint_op(ctx, JIT_ERR_TAG)));                            \
  if (jit_try_depth > 0) {                                            \
    jit_try_entry_t *handler = &jit_try_stack[jit_try_depth - 1];     \
    MIR_append_insn(ctx, jit_func,                                    \
      MIR_new_insn(ctx, MIR_MOV,                                      \
        MIR_new_reg_op(ctx, vs.regs[handler->saved_sp]),              \
        MIR_new_reg_op(ctx, (value_reg))));                           \
    MIR_append_insn(ctx, jit_func,                                    \
      MIR_new_insn(ctx, MIR_JMP,                                      \
        MIR_new_label_op(ctx, handler->catch_label)));                \
  } else {                                                            \
    JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, (value_reg)));              \
  }                                                                   \
  MIR_append_insn(ctx, jit_func, no_error);                           \
} while (0)

  if (normalize_sloppy_this) {
    MIR_label_t normalize_global = MIR_new_label(ctx);
    MIR_label_t normalize_box = MIR_new_label(ctx);
    MIR_label_t normalize_done = MIR_new_label(ctx);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_UBLE,
        MIR_new_label_op(ctx, normalize_box),
        MIR_new_reg_op(ctx, r_this_curr),
        MIR_new_uint_op(ctx, NANBOX_PREFIX)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_URSH,
        MIR_new_reg_op(ctx, r_bool),
        MIR_new_reg_op(ctx, r_this_curr),
        MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, normalize_global),
        MIR_new_reg_op(ctx, r_bool),
        MIR_new_uint_op(ctx,
          (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_UNDEF)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, normalize_global),
        MIR_new_reg_op(ctx, r_bool),
        MIR_new_uint_op(ctx,
          (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)T_NULL)));
    const uint8_t boxed_types[] = {
      T_STR, T_BOOL, T_BIGINT, T_SYMBOL
    };
    for (size_t i = 0; i < sizeof(boxed_types); i++) {
      MIR_append_insn(ctx, jit_func,
        MIR_new_insn(ctx, MIR_BEQ,
          MIR_new_label_op(ctx, normalize_box),
          MIR_new_reg_op(ctx, r_bool),
          MIR_new_uint_op(ctx,
            (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) |
            (uint64_t)boxed_types[i])));
    }
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, normalize_done)));

    MIR_append_insn(ctx, jit_func, normalize_global);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_this_curr),
        MIR_new_mem_op(ctx, MIR_JSVAL,
          (MIR_disp_t)offsetof(ant_t, global), r_js, 0, 1)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, normalize_done)));

    MIR_append_insn(ctx, jit_func, normalize_box);
    MIR_append_insn(ctx, jit_func,
      MIR_new_call_insn(ctx, 5,
        MIR_new_ref_op(ctx, normalize_this_proto),
        MIR_new_ref_op(ctx, imp_normalize_this),
        MIR_new_reg_op(ctx, r_this_curr),
        MIR_new_reg_op(ctx, r_js),
        MIR_new_reg_op(ctx, r_this_curr)));
    JIT_EMIT_THROW_IF_ERROR(r_this_curr);
    MIR_append_insn(ctx, jit_func, normalize_done);
    MIR_append_insn(ctx, jit_func,
      MIR_new_insn(ctx, MIR_MOV,
        MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_this_root, 0, 1),
        MIR_new_reg_op(ctx, r_this_curr)));
  }

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
        if (vs.known_func)
          memset(vs.known_func, 0, (size_t)vs.max * sizeof(sv_func_t *));
        if (vs.has_const)
          memset(vs.has_const, 0, (size_t)vs.max * sizeof(bool));
        if (vs.known_bool)
          memset(vs.known_bool, 0, (size_t)vs.max);
        if (known_func_locals)
          memset(known_func_locals, 0,
                 (size_t)n_locals * sizeof(sv_func_t *));
        if (known_type_locals && local_d_regs) {
          for (int li = 0; li < n_locals; li++)
            if (known_type_locals[li] == SV_TI_NUM
                && has_captures && captured_locals && captured_locals[li])
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
            MIR_new_reg_op(ctx, r_this_curr)));
        break;
      }

      case OP_GET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        MIR_reg_t dst = vstack_push(&vs);
        if (idx < 8 && param_cache[idx]) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, param_cache[idx])));
        } else if (idx < (uint16_t)param_count
            && (writes_params || (has_captured_params && captured_params && captured_params[idx]))) {
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
        if (!builder_target_slots
            || ((int)idx < param_count && builder_target_slots[idx])) {
          MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            ctx, jit_func, dst, r_bool,
            r_vm, r_js, helper1_proto, imp_str_read_value
          );
          JIT_EMIT_THROW_IF_ERROR(dst);
          MIR_append_insn(ctx, jit_func, sbr_done);
        }
        break;
      }

      case OP_PUT_ARG:
      case OP_SET_ARG: {
        uint16_t idx = sv_get_u16(ip + 1);
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_top(&vs);
        if (idx < (uint16_t)param_count
            && (writes_params || (has_captured_params && captured_params && captured_params[idx]))) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                r_slotbuf, 0, 1),
              MIR_new_reg_op(ctx, val)));
          if (!writes_params && idx < (uint16_t)param_count) {
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
        if (op == OP_PUT_ARG) (void)vstack_pop(&vs);
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
        if (!(dnum_locals && dnum_locals[idx]))
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, local_regs[idx])));
        if ((!builder_target_slots || builder_target_slots[param_count + idx])
            && (!known_type_locals || known_type_locals[idx] != SV_TI_NUM)) {
          MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            ctx, jit_func, dst, r_bool,
            r_vm, r_js, helper1_proto, imp_str_read_value
          );
          JIT_EMIT_THROW_IF_ERROR(dst);
          MIR_append_insn(ctx, jit_func, sbr_done);
        }
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
        if (!(dnum_locals && dnum_locals[idx]))
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, local_regs[idx])));
        if ((!builder_target_slots || builder_target_slots[param_count + idx])
            && (!known_type_locals || known_type_locals[idx] != SV_TI_NUM)) {
          MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            ctx, jit_func, dst, r_bool,
            r_vm, r_js, helper1_proto, imp_str_read_value
          );
          JIT_EMIT_THROW_IF_ERROR(dst);
          MIR_append_insn(ctx, jit_func, sbr_done);
        }
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
          if (idx < (uint16_t)param_count
              && (writes_params || (has_captured_params && captured_params && captured_params[idx]))) {
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
          if (!(dnum_locals && dnum_locals[idx]))
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
        if (dnum_locals && dnum_locals[idx]) {
          /* d-only local: maintain only the d mirror. A non-numeric source
             bails and re-executes this op in the interpreter (the source
             stays in the stack snapshot at sp + 1). */
          if (!src_is_num)
            vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          MIR_reg_t dsrc = vstack_pop(&vs);
          if (known_func_locals) known_func_locals[idx] = kf;
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], dsrc, src_d, src_is_num,
            r_bool, bc_off, vs.sp + 1, &bailout_ctx);
          break;
        }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], local_regs[idx], src_d, src_is_num,
            r_bool, bc_off + sz, vs.sp, &bailout_ctx);
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
        if (dnum_locals && dnum_locals[idx]) {
          /* d-only local: maintain only the d mirror. A non-numeric source
             bails and re-executes this op in the interpreter (the source
             stays in the stack snapshot at sp + 1). */
          if (!src_is_num)
            vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          MIR_reg_t dsrc = vstack_pop(&vs);
          if (known_func_locals) known_func_locals[idx] = kf;
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], dsrc, src_d, src_is_num,
            r_bool, bc_off, vs.sp + 1, &bailout_ctx);
          break;
        }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        if (known_func_locals) known_func_locals[idx] = kf;
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], local_regs[idx], src_d, src_is_num,
            r_bool, bc_off + sz, vs.sp, &bailout_ctx);
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
        if (dnum_locals && dnum_locals[idx]) {
          /* d-only local: maintain only the d mirror. A non-numeric source
             bails and re-executes this op in the interpreter. */
          if (!src_is_num)
            vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], vs.regs[vs.sp - 1], src_d, src_is_num,
            r_bool, bc_off, vs.sp, &bailout_ctx);
          break;
        }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], local_regs[idx], src_d, src_is_num,
            r_bool, bc_off + sz, vs.sp, &bailout_ctx);
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
        if (dnum_locals && dnum_locals[idx]) {
          /* d-only local: maintain only the d mirror. A non-numeric source
             bails and re-executes this op in the interpreter. */
          if (!src_is_num)
            vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], vs.regs[vs.sp - 1], src_d, src_is_num,
            r_bool, bc_off, vs.sp, &bailout_ctx);
          break;
        }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_top(&vs);
        if (known_func_locals) known_func_locals[idx] = vs.known_func[vs.sp - 1];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, local_regs[idx]),
            MIR_new_reg_op(ctx, src)));
        if (local_d_regs && known_type_locals && known_type_locals[idx] == SV_TI_NUM) {
          mir_emit_numeric_local_store_mirror(ctx, jit_func,
            local_d_regs[idx], local_regs[idx], src_d, src_is_num,
            r_bool, bc_off + sz, vs.sp, &bailout_ctx);
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
        if (known_type_locals) {
          bool was_num = known_type_locals[idx] == SV_TI_NUM;
          bool immediate_num_init =
            was_num &&
            (!captured_locals || !captured_locals[idx]) &&
            jit_has_immediate_numeric_local_init(func, ip + sz, end, idx);
          known_type_locals[idx] = immediate_num_init ? SV_TI_NUM : SV_TI_UNKNOWN;
        }
        break;
      }

      case OP_POP:
        vstack_pop(&vs);
        break;

      case OP_DUP: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        jit_value_info_t info = vstack_value_info(&vs, vs.sp - 1);
        MIR_reg_t top = vstack_top(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        vstack_set_value_info(&vs, vs.sp - 1, info);
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
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 2);
        jit_value_info_t ib = vstack_value_info(&vs, vs.sp - 1);
        MIR_reg_t ra = vs.regs[vs.sp - 2];
        MIR_reg_t rb = vs.regs[vs.sp - 1];
        MIR_reg_t da = vstack_push(&vs);
        MIR_reg_t db = vstack_push(&vs);
        vstack_set_value_info(&vs, vs.sp - 2, ia);
        vstack_set_value_info(&vs, vs.sp - 1, ib);
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

      case OP_NIP: {
        if (vs.sp < 2) { ok = false; break; }
        int top_idx = vs.sp - 1;
        int dst_idx = vs.sp - 2;
        vstack_ensure_boxed(&vs, top_idx, ctx, jit_func, r_d_slot);
        jit_value_info_t info = vstack_value_info(&vs, top_idx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, vs.regs[dst_idx]),
            MIR_new_reg_op(ctx, vs.regs[top_idx])));
        vs.sp--;
        vs.slot_type[dst_idx] = SLOT_BOXED;
        vstack_set_value_info(&vs, dst_idx, info);
        break;
      }

      case OP_INSERT2: {
        if (vs.sp < 2) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 1);
        jit_value_info_t io = vstack_value_info(&vs, vs.sp - 2);
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
        vstack_set_value_info(&vs, vs.sp - 3, ia);
        vstack_set_value_info(&vs, vs.sp - 2, io);
        vstack_set_value_info(&vs, vs.sp - 1, ia);
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
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 1);
        jit_value_info_t iprop = vstack_value_info(&vs, vs.sp - 2);
        jit_value_info_t io = vstack_value_info(&vs, vs.sp - 3);
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
        vstack_set_value_info(&vs, vs.sp - 4, ia);
        vstack_set_value_info(&vs, vs.sp - 3, io);
        vstack_set_value_info(&vs, vs.sp - 2, iprop);
        vstack_set_value_info(&vs, vs.sp - 1, ia);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dup),
            MIR_new_reg_op(ctx, r_tmp)));
        break;
      }

      case OP_ADD:
      case OP_ADD_NUM: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool force_num_only = (op == OP_ADD_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);
        bool fb_str_only = !force_num_only && fb && !(fb & ~SV_TFB_STR);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_str_only) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_label_t slow = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);
          mir_emit_string_concat_fastpath(
            ctx, jit_func, r_js, rl, rr, rd, slow, 0, bc_off
          );
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        } else if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_add,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
	          mir_emit_bailout_jump_typed(ctx, jit_func,
	            r_bailout_off, bc_off,
	            r_bailout_sp, pre_op_sp, bailout_tramp,
	            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
	            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
	          mir_emit_bailout_jump_typed(ctx, jit_func,
	            r_bailout_off, bc_off,
	            r_bailout_sp, pre_op_sp, bailout_tramp,
	            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
	            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_SUB:
      case OP_SUB_NUM: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool force_num_only = (op == OP_SUB_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_sub,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_MUL:
      case OP_MUL_NUM: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool force_num_only = (op == OP_MUL_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_mul,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_DIV:
      case OP_DIV_NUM: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool force_num_only = (op == OP_DIV_NUM);
        bool fb_num_only  = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
        bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_div,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
        mir_emit_dnum_rebox(ctx, jit_func, local_regs, n_locals, r_d_slot);
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

      case OP_INC:
      case OP_DEC: {
        int top_idx = vs.sp - 1;
        bool input_is_num = vs.slot_type && vs.slot_type[top_idx] == SLOT_NUM;
        MIR_reg_t rs = vstack_top(&vs);

        if (vs.known_func) vs.known_func[top_idx] = NULL;
        if (vs.has_const) vs.has_const[top_idx] = false;

        MIR_label_t bailout = input_is_num ? NULL : MIR_new_label(ctx);
        MIR_label_t done = input_is_num ? NULL : MIR_new_label(ctx);
        if (!input_is_num)
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rs, bailout);

        vstack_ensure_num(&vs, top_idx, ctx, jit_func, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, op == OP_INC ? MIR_DADD : MIR_DSUB,
            MIR_new_reg_op(ctx, vs.d_regs[top_idx]),
            MIR_new_reg_op(ctx, vs.d_regs[top_idx]),
            MIR_new_reg_op(ctx, r_d_one)));

        if (!input_is_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, bailout);
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, vs.sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            top_idx, false, -1, false);
          MIR_append_insn(ctx, jit_func, done);
        }
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

      case OP_POST_DEC: {
        int old_idx = vs.sp - 1;
        bool input_is_num = vs.slot_type && vs.slot_type[old_idx] == SLOT_NUM;
        MIR_reg_t rold = vstack_top(&vs);
        vstack_push(&vs);
        int new_idx = vs.sp - 1;

        if (vs.known_func) {
          vs.known_func[old_idx] = NULL;
          vs.known_func[new_idx] = NULL;
        }
        if (vs.has_const) {
          vs.has_const[old_idx] = false;
          vs.has_const[new_idx] = false;
        }

        MIR_label_t bailout = input_is_num ? NULL : MIR_new_label(ctx);
        MIR_label_t done = input_is_num ? NULL : MIR_new_label(ctx);
        if (!input_is_num)
          mir_emit_is_num_guard(ctx, jit_func, r_bool, rold, bailout);

        vstack_ensure_num(&vs, old_idx, ctx, jit_func, r_d_slot);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_DSUB,
            MIR_new_reg_op(ctx, vs.d_regs[new_idx]),
            MIR_new_reg_op(ctx, vs.d_regs[old_idx]),
            MIR_new_reg_op(ctx, r_d_one)));
        vs.slot_type[new_idx] = SLOT_NUM;

        if (!input_is_num) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
          MIR_append_insn(ctx, jit_func, bailout);
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, vs.sp - 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            old_idx, false, -1, false);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_IS_UNDEF:
      case OP_IS_NULL: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
        break;
      }

      case OP_IS_UNDEF_OR_NULL: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t rs = vstack_top(&vs);
        MIR_label_t is_true = MIR_new_label(ctx);
        MIR_label_t is_done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_true),
            MIR_new_reg_op(ctx, rs),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, is_true),
            MIR_new_reg_op(ctx, rs),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        mir_load_imm(ctx, jit_func, rs, js_false);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, is_done)));
        MIR_append_insn(ctx, jit_func, is_true);
        mir_load_imm(ctx, jit_func, rs, js_true);
        MIR_append_insn(ctx, jit_func, is_done);
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
        break;
      }

      case OP_LT: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_lt,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_LE: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_le,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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

      case OP_JMP_NOT_NULLISH: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        MIR_reg_t cond = vstack_top(&vs);
        int target = bc_off + sz + sv_get_i32(ip + 1);
        MIR_label_t lbl = label_for_branch(ctx, &lm, target, vs.sp);
        MIR_label_t done = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, done),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, lbl),
            MIR_new_reg_op(ctx, cond),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        MIR_append_insn(ctx, jit_func, done);
        break;
      }

      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_FALSE:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE:
      case OP_JMP_TRUE8: {
        bool cond_known_bool = vs.known_bool && vs.sp > 0 &&
                               vs.known_bool[vs.sp - 1];
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        bool is_peek = (op == OP_JMP_FALSE_PEEK || op == OP_JMP_TRUE_PEEK);
        MIR_reg_t cond = is_peek ? vstack_top(&vs) : vstack_pop(&vs);
        bool short_op = (op == OP_JMP_FALSE8 || op == OP_JMP_TRUE8);
        bool is_false_branch = (op == OP_JMP_FALSE || op == OP_JMP_FALSE8
                                || op == OP_JMP_FALSE_PEEK);
        int target = bc_off + sz + (short_op ? (int8_t)sv_get_i8(ip + 1)
                                             : sv_get_i32(ip + 1));
        MIR_label_t lbl = label_for_branch(ctx, &lm, target, vs.sp);
        if (cond_known_bool) {
          /* Condition is statically a JS boolean: one compare-branch
             replaces the full truthiness decode. */
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, lbl),
              MIR_new_reg_op(ctx, cond),
              MIR_new_uint_op(ctx, is_false_branch ? js_false : js_true)));
          break;
        }
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

            /* Args (and, outside try blocks, the slots below them) may
               stay unboxed: the inline fast path reads d-regs, the slow
               path is a plain helper call, and bailing consumers re-box
               via the typed snapshot helpers. Inside a try block the
               catch path reads boxed regs, so flush everything. */
            int inl_arg_base = vs.sp - (int)call_argc;
            if (jit_try_depth > 0) {
              for (int k = 0; k < inl_arg_base; k++)
                vstack_ensure_boxed(&vs, k, ctx, jit_func, r_d_slot);
            } else if (inl_arg_base > 0) {
              vstack_ensure_boxed(&vs, inl_arg_base - 1, ctx, jit_func, r_d_slot);
            }
            uint8_t inl_arg_num[16] = {0};
            MIR_reg_t inl_arg_d[16] = {0};
            for (int i = 0; i < (int)call_argc; i++) {
              inl_arg_num[i] = vs.slot_type &&
                               vs.slot_type[inl_arg_base + i] == SLOT_NUM;
              inl_arg_d[i] = vs.d_regs[inl_arg_base + i];
            }

            MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
            for (int i = (int)call_argc - 1; i >= 0; i--)
              inl_arg_regs[i] = vstack_pop(&vs);
            MIR_reg_t r_inl_callee = vstack_pop(&vs); 

            MIR_reg_t r_call_res = vstack_push(&vs);

            MIR_label_t inl_slow = MIR_new_label(ctx);
            MIR_label_t inl_join = MIR_new_label(ctx);

            MIR_reg_t r_inl_cl = 0;
            MIR_reg_t r_inl_new_target = 0;
            MIR_reg_t r_inl_super = 0;
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
            {
              char nt_rn[32], sup_rn[32];
              snprintf(nt_rn, sizeof(nt_rn), "inl%d_nt", cn);
              snprintf(sup_rn, sizeof(sup_rn), "inl%d_sup", cn);
              r_inl_new_target = MIR_new_func_reg(ctx, jit_func->u.func,
                                                  MIR_JSVAL, nt_rn);
              r_inl_super = MIR_new_func_reg(ctx, jit_func->u.func,
                                             MIR_JSVAL, sup_rn);
              mir_load_imm(ctx, jit_func, r_inl_new_target, mkval(T_UNDEF, 0));
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, r_inl_super),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                    (MIR_disp_t)offsetof(sv_closure_t, super_val),
                    r_inl_cl, 0, 1)));
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

              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_MOV,
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_mem_op(ctx, MIR_T_U32,
                    (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                    r_inl_cl, 0, 1)));
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_AND,
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_uint_op(ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
              MIR_append_insn(ctx, jit_func,
                MIR_new_insn(ctx, MIR_BNE,
                  MIR_new_label_op(ctx, inl_slow),
                  MIR_new_reg_op(ctx, r_guard_tag),
                  MIR_new_uint_op(ctx, 0)));
            }

            /* Inlinees that never read `this` don't need the arrow/bound
               resolution chain. */
            bool inl_uses_this = false;
            {
              uint8_t *tscan = inline_callee->code;
              uint8_t *tend = inline_callee->code + inline_callee->code_len;
              while (tscan < tend) {
                sv_op_t top_ = (sv_op_t)*tscan;
                int tsz = sv_op_size[top_];
                if (tsz == 0) { inl_uses_this = true; break; }
                if (top_ == OP_THIS) { inl_uses_this = true; break; }
                tscan += tsz;
              }
            }
            if (inl_uses_this)
              mir_emit_resolve_call_this(ctx, jit_func, r_inl_this, r_inl_cl,
                                         r_this_curr, r_inl_flags, r_inl_bound);
            else
              mir_load_imm(ctx, jit_func, r_inl_this, mkval(T_UNDEF, 0));

            jit_inline_ext_t inl_ext = {
              .helper1_proto = helper1_proto,
              .imp_get_length_inline = imp_get_length_inline,
              .imp_get_elem_inline = imp_get_elem_inline,
              .put_field_proto = put_field_proto, .imp_put_field = imp_put_field,
              .remember_obj_proto = remember_obj_proto,
              .imp_remember_obj = imp_remember_obj,
              .call_proto = call_proto, .imp_call = imp_call,
              .call_method_proto = call_method_proto, .imp_call_method = imp_call_method,
              .imp_band = imp_band, .imp_bor = imp_bor, .imp_bxor = imp_bxor,
              .imp_shl = imp_shl, .imp_shr = imp_shr, .imp_ushr = imp_ushr,
              .self_proto = self_proto,
              .r_args_buf = r_args_buf,
            };
            bool inlined = jit_emit_inline_body(
              ctx, jit_func, inline_callee,
              inl_arg_regs, (int)call_argc,
              inl_arg_num, inl_arg_d,
              r_call_res, inl_slow, inl_join,
              r_bool, &r_d_slot, cn,
              r_inl_cl, r_inl_this, r_inl_new_target, r_inl_super,
              r_vm, r_js, r_ic_epoch_val,
              helper2_proto, imp_seq, imp_sne, imp_eq, imp_ne,
              gf_proto, imp_get_field_inline,
              gg_proto, imp_gg,
              special_obj_proto, imp_special_obj,
              &inl_ext);

            if (inlined) {
              MIR_append_insn(ctx, jit_func, inl_slow);
              for (int i = 0; i < (int)call_argc; i++)
                if (inl_arg_num[i])
                  mir_d_to_i64(ctx, jit_func, inl_arg_regs[i],
                               inl_arg_d[i], r_d_slot);
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
              JIT_EMIT_THROW_IF_ERROR(r_call_res);
              break;
            }

            MIR_append_insn(ctx, jit_func, inl_slow);
            MIR_append_insn(ctx, jit_func, inl_join);
            vs.sp = vs.sp - 1 + call_argc + 1;
          }
        }

        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);

        int cn = call_n++;

        char rn_arr[32], rn_this[32], rn_ccl[32], rn_cfn[32], rn_jptr[32], rn_csup[32];
        snprintf(rn_arr,  sizeof(rn_arr),  "arg_arr%d",       cn);
        snprintf(rn_this, sizeof(rn_this), "call_this%d",     cn);
        snprintf(rn_ccl,  sizeof(rn_ccl),  "callee_cl%d",     cn);
        snprintf(rn_cfn,  sizeof(rn_cfn),  "callee_func%d",   cn);
        snprintf(rn_jptr, sizeof(rn_jptr), "jit_ptr%d",       cn);
        snprintf(rn_csup, sizeof(rn_csup), "callee_super%d",  cn);

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
                               writes_params,
                               has_captures,
                               captured_locals, r_lbuf, self_tail_entry);
            break;
          }
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 10,
              MIR_new_ref_op(ctx, self_proto),
              MIR_new_ref_op(ctx, jit_func),
              MIR_new_reg_op(ctx, r_call_res),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_call_this),
              MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
              MIR_new_reg_op(ctx, r_super_val),
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
            JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          break;
        }

        MIR_label_t lbl_self_call   = MIR_new_label(ctx);
        MIR_label_t lbl_super_call  = MIR_new_label(ctx);
        MIR_label_t lbl_interp_call = MIR_new_label(ctx);
        MIR_label_t lbl_call_done   = MIR_new_label(ctx);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_super_call),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_super_val)));

        MIR_reg_t r_callee_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_ccl);
        mir_emit_get_closure(ctx, jit_func, r_callee_cl, r_call_func,
                             r_bool, lbl_interp_call);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_mem_op(ctx, MIR_T_U32,
              (MIR_disp_t)offsetof(sv_closure_t, call_flags),
              r_callee_cl, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_AND,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, lbl_interp_call),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, 0)));

        MIR_reg_t r_callee_fn = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_cfn);
        MIR_reg_t r_callee_super = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn_csup);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, func),
              r_callee_cl, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_super),
            MIR_new_mem_op(ctx, MIR_T_I64,
              (MIR_disp_t)offsetof(sv_closure_t, super_val),
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
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_reg_op(ctx, r_jit_ptr),   
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
            MIR_new_reg_op(ctx, r_callee_super),
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
                             writes_params,
                             has_captures,
                             captured_locals, r_lbuf, self_tail_entry);
        } else {
          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 10,
              MIR_new_ref_op(ctx, self_proto),
              MIR_new_ref_op(ctx, jit_func),    
              MIR_new_reg_op(ctx, r_call_res),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_call_this),
              MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
              MIR_new_reg_op(ctx, r_super_val),
              MIR_new_reg_op(ctx, r_arg_arr),
              MIR_new_int_op(ctx, (int64_t)call_argc),
              MIR_new_reg_op(ctx, r_closure)));
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_call_done)));

        MIR_append_insn(ctx, jit_func, lbl_super_call);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_call_out_this, 0, 1),
            MIR_new_reg_op(ctx, r_this_curr)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 12,
            MIR_new_ref_op(ctx, call_method_proto),
            MIR_new_ref_op(ctx, imp_call_method),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_this_curr),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_super_val),
            MIR_new_reg_op(ctx, r_new_target),
            MIR_new_reg_op(ctx, r_call_out_this)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_this_curr),
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_call_out_this, 0, 1)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
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
            JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          if (is_tail) {
            JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
          }
        }
        break;
      }

      case OP_SPECIAL_OBJ: {
        uint8_t which = sv_get_u8(ip + 1);
        MIR_reg_t dst = vstack_push(&vs);
        if (which == 1) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, r_new_target)));
        } else if (which == 2) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_reg_op(ctx, r_super_val)));
        } else if (which == 3) {
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
        bool guarded_self_upval = self_binding_guards &&
          self_binding_guards[bc_off] != 0;

        int un = upval_n++;
        char rn_uvs[32], rn_uv[32], rn_loc[32];
        snprintf(rn_uvs, sizeof(rn_uvs), "upvs%d",  un);
        snprintf(rn_uv,  sizeof(rn_uv),  "upv%d",   un);
        snprintf(rn_loc, sizeof(rn_loc),  "uvloc%d", un);

        MIR_reg_t r_uvs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uvs);
        MIR_reg_t r_uv  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_uv);
        MIR_reg_t r_loc = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_loc);
        int pre_op_sp = vs.sp;
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
        if (guarded_self_upval) {
          vs.known_func[vs.sp - 1] = func;
          mir_emit_self_binding_guard(
            ctx, jit_func, dst, r_closure, r_tmp, r_tmp2,
            bc_off, pre_op_sp, &bailout_ctx);
        } else if (jit_upvalue_is_builder_target(func, idx)) {
          MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            ctx, jit_func, dst, r_bool,
            r_vm, r_js, helper1_proto, imp_str_read_value
          );
          JIT_EMIT_THROW_IF_ERROR(dst);
          MIR_append_insn(ctx, jit_func, sbr_done);
        }
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
        mir_emit_upval_write_barrier(ctx, jit_func,
          upval_barrier_proto, imp_upval_barrier,
          r_js, r_uv, src, un);
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
        mir_emit_upval_write_barrier(ctx, jit_func,
          upval_barrier_proto, imp_upval_barrier,
          r_js, r_uv, src, un);
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
            r_vm, r_slotbuf, r_jit_open_upvalues,
            captured_params, (int)idx, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, r_jit_open_upvalues, captured_locals,
            idx >= (uint16_t)param_count ? (int)idx - param_count : 0, n_locals);
        break;
      }

      case OP_GET_GLOBAL:
      case OP_GET_GLOBAL_UNDEF: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        bool known_self_global = self_binding_guards &&
          self_binding_guards[bc_off] != 0;
        int pre_op_sp = vs.sp;
        MIR_reg_t dst = vstack_push(&vs);

        MIR_label_t gg_slow = MIR_new_label(ctx);
        MIR_label_t gg_done = MIR_new_label(ctx);
        bool gg_fast = r_ic_epoch_val != 0 &&
          mir_emit_get_global_ic_fastpath(
            ctx, jit_func, func, bc_off,
            r_js, dst, gg_slow, r_ic_epoch_val, ip);
        if (gg_fast) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, gg_done)));
          MIR_append_insn(ctx, jit_func, gg_slow);
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
        JIT_EMIT_THROW_IF_ERROR(dst);
        if (gg_fast) MIR_append_insn(ctx, jit_func, gg_done);
        if (known_self_global) {
          vs.known_func[vs.sp - 1] = func;
          mir_emit_self_binding_guard_value_kept(
            ctx, jit_func, dst, r_closure, r_tmp, r_tmp2,
            bc_off, sz, pre_op_sp, &bailout_ctx);
        }
        break;
      }

      case OP_GET_EVAL_GLOBAL:
      case OP_GET_EVAL_GLOBAL_UNDEF: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, get_eval_global_proto),
            MIR_new_ref_op(ctx, imp_get_eval_global),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_int_op(ctx, (int64_t)bc_off),
            MIR_new_int_op(ctx, op == OP_GET_EVAL_GLOBAL_UNDEF ? 1 : 0)));
        JIT_EMIT_THROW_IF_ERROR(dst);
        break;
      }

      case OP_RETURN: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t ret_val = vstack_pop(&vs);
        if (has_captured_slots)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_slotbuf, r_jit_open_upvalues, captured_params, 0, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, r_jit_open_upvalues, captured_locals, 0, n_locals);
        if (func->is_derived_ctor) {
          MIR_label_t dctor_replace = MIR_new_label(ctx);
          MIR_label_t dctor_keep = MIR_new_label(ctx);
          char rn_dt[32];
          snprintf(rn_dt, sizeof(rn_dt), "dctor%d", upval_n++);
          MIR_reg_t r_dt = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_dt);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_UBLE,
              MIR_new_label_op(ctx, dctor_replace),
              MIR_new_reg_op(ctx, ret_val),
              MIR_new_uint_op(ctx, NANBOX_PREFIX)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_URSH,
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_reg_op(ctx, ret_val),
              MIR_new_int_op(ctx, NANBOX_TYPE_SHIFT)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_AND,
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_int_op(ctx, NANBOX_TYPE_MASK)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_LSH,
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_int_op(ctx, 1),
              MIR_new_reg_op(ctx, r_dt)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_AND,
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_reg_op(ctx, r_dt),
              MIR_new_int_op(ctx, (int64_t)T_OBJECT_MASK)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BT,
              MIR_new_label_op(ctx, dctor_keep),
              MIR_new_reg_op(ctx, r_dt)));
          MIR_append_insn(ctx, jit_func, dctor_replace);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, ret_val),
              MIR_new_reg_op(ctx, r_this_curr)));
          MIR_append_insn(ctx, jit_func, dctor_keep);
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, ret_val)));
        break;
      }

      case OP_RETURN_UNDEF: {
        if (has_captured_slots)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_slotbuf, r_jit_open_upvalues, captured_params, 0, param_count);
        if (has_captures)
          mir_emit_close_marked_slots(ctx, jit_func,
            close_upval_proto, imp_close_upval,
            r_vm, r_lbuf, r_jit_open_upvalues, captured_locals, 0, n_locals);
        if (func->is_derived_ctor) {
          // implicit ctor return: hand the (possibly super-rebound) this
          // back to the caller, which has no other channel to receive it
          MIR_append_insn(ctx, jit_func,
            MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_this_curr)));
          break;
        }
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
        if (!(dnum_locals && dnum_locals[idx]))
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
        if (!(dnum_locals && dnum_locals[idx]))
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

        uint8_t fb = sv_func_type_feedback(func)
          ? sv_func_type_feedback(func)[bc_off] : 0;
        bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
        bool loc_is_num = known_type_locals && local_d_regs &&
          known_type_locals[idx] == SV_TI_NUM &&
          (!captured_locals || !captured_locals[idx]);
        bool rhs_is_num = vs.slot_type &&
          vs.slot_type[vs.sp - 1] == SLOT_NUM;

        if (fb_num_only && loc_is_num) {
          int rhs_idx = vs.sp - 1;
          int pre_op_sp = vs.sp;
          MIR_reg_t rr = vs.regs[rhs_idx];
          MIR_reg_t rr_d = vs.d_regs[rhs_idx];
          MIR_label_t bail_direct = MIR_new_label(ctx);
          MIR_label_t done = MIR_new_label(ctx);

          if (!rhs_is_num) {
            mir_emit_is_num_guard(ctx, jit_func, r_bool, rr, bail_direct);
            mir_i64_to_d(ctx, jit_func, rr_d, rr, r_d_slot);
          }

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_DADD,
              MIR_new_reg_op(ctx, local_d_regs[idx]),
              MIR_new_reg_op(ctx, local_d_regs[idx]),
              MIR_new_reg_op(ctx, rr_d)));
          if (!(dnum_locals && dnum_locals[idx]))
            mir_d_to_i64(ctx, jit_func,
              local_regs[idx], local_d_regs[idx], r_d_slot);
          if (has_captures && captured_locals && captured_locals[idx])
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)),
                  r_lbuf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[idx])));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));

          MIR_append_insn(ctx, jit_func, bail_direct);
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            -1, false, rhs_idx, rhs_is_num);
          MIR_append_insn(ctx, jit_func, done);
          vstack_pop(&vs);
          break;
        }

        int pre_op_sp = vs.sp;
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t rr = vstack_pop(&vs);

        MIR_label_t slow = MIR_new_label(ctx);
        MIR_label_t done = MIR_new_label(ctx);

        /* d-only local: the boxed reg is stale; refresh it from the
           mirror before the guard (and slow helper) read it. */
        if (dnum_locals && dnum_locals[idx])
          mir_d_to_i64(ctx, jit_func,
            local_regs[idx], local_d_regs[idx], r_d_slot);
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
          r_bailout_sp, pre_op_sp, bailout_tramp,
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
        MIR_reg_t rhs;
        MIR_reg_t lhs;
        MIR_label_t slow = MIR_new_label(ctx);
        MIR_label_t append_done = MIR_new_label(ctx);

        if ((int)slot_idx < param_count) {
          if (!writes_params) {
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
          }

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          rhs = vstack_pop(&vs);

          char lhs_name[48];
          snprintf(lhs_name, sizeof(lhs_name), "sab_param_%d", bc_off);
          lhs = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, lhs_name);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, lhs),
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)((int)slot_idx * (int)sizeof(ant_value_t)),
                writes_params ? r_slotbuf : r_args, 0, 1)));

          mir_emit_string_builder_append_ascii_byte(
            ctx, jit_func, lhs, rhs, r_err_tmp,
            r_d_slot,
            slow, append_done, -1, bc_off
          );
          MIR_append_insn(ctx, jit_func, slow);

          MIR_append_insn(ctx, jit_func,
            MIR_new_call_insn(ctx, 11,
              MIR_new_ref_op(ctx, str_append_local_proto),
              MIR_new_ref_op(ctx, imp_str_append_local),
              MIR_new_reg_op(ctx, r_err_tmp),
              MIR_new_reg_op(ctx, r_vm),
              MIR_new_reg_op(ctx, r_js),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
              writes_params ? MIR_new_reg_op(ctx, r_slotbuf) : MIR_new_reg_op(ctx, r_args),
              writes_params ? MIR_new_int_op(ctx, param_count) : MIR_new_reg_op(ctx, r_argc),
              MIR_new_uint_op(ctx, 0),
              MIR_new_int_op(ctx, (int64_t)slot_idx),
              MIR_new_reg_op(ctx, rhs)));
        } else {
          uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)param_count);
          if (local_idx >= (uint16_t)n_locals) { ok = false; break; }

          vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
          rhs = vstack_pop(&vs);

          lhs = local_regs[local_idx];
          mir_emit_string_builder_append_ascii_byte(
            ctx, jit_func, lhs, rhs, r_err_tmp,
            r_d_slot,
            slow, append_done, -1, bc_off
          );
          MIR_append_insn(ctx, jit_func, slow);

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
          if (known_type_locals && known_type_locals[local_idx] != SV_TI_NUM)
            known_type_locals[local_idx] = SV_TI_UNKNOWN;
        }

        MIR_append_insn(ctx, jit_func, append_done);

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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        if ((int)slot_idx >= param_count) {
          int gli = (int)slot_idx - param_count;
          if (gli < n_locals && local_d_regs && known_type_locals
              && known_type_locals[gli] == SV_TI_NUM)
            mir_emit_numeric_local_store_mirror(ctx, jit_func,
              local_d_regs[gli], local_regs[gli], 0, false,
              r_bool, bc_off + sz, vs.sp, &bailout_ctx);
        }
        break;
      }

      case OP_STR_ALC_SNAPSHOT: {
        uint16_t slot_idx = sv_get_u16(ip + 1);
        int pre_op_sp = vs.sp;
        if ((int)slot_idx < param_count) {
          if (!writes_params) {
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
          }

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
              writes_params ? MIR_new_reg_op(ctx, r_slotbuf) : MIR_new_reg_op(ctx, r_args),
              writes_params ? MIR_new_int_op(ctx, param_count) : MIR_new_reg_op(ctx, r_argc),
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
          if (known_type_locals && known_type_locals[local_idx] != SV_TI_NUM)
            known_type_locals[local_idx] = SV_TI_UNKNOWN;
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        if ((int)slot_idx >= param_count) {
          int gli = (int)slot_idx - param_count;
          if (gli < n_locals && local_d_regs && known_type_locals
              && known_type_locals[gli] == SV_TI_NUM)
            mir_emit_numeric_local_store_mirror(ctx, jit_func,
              local_d_regs[gli], local_regs[gli], 0, false,
              r_bool, bc_off + sz, vs.sp, &bailout_ctx);
        }
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

      case OP_TO_STRING: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t src = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_label_t ts_is_str = MIR_new_label(ctx);
        MIR_label_t ts_done   = MIR_new_label(ctx);
        MIR_label_t ts_helper = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_UBLE,
            MIR_new_label_op(ctx, ts_helper),
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
            MIR_new_label_op(ctx, ts_is_str),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_int_op(ctx, T_STR)));
        MIR_append_insn(ctx, jit_func, ts_helper);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 5,
            MIR_new_ref_op(ctx, to_string_proto),
            MIR_new_ref_op(ctx, imp_to_string),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, src)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, ts_done)));
        MIR_append_insn(ctx, jit_func, ts_is_str);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, src)));
        MIR_append_insn(ctx, jit_func, ts_done);
        JIT_EMIT_THROW_IF_ERROR(dst);
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
        if (mir_emit_get_field_ic_fastpath(
          ctx, jit_func, func, bc_off, ic_idx, atom, obj, dst, slow,
          r_ic_epoch_val)) {
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
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
        if (mir_emit_get_field_ic_fastpath(
          ctx, jit_func, func, bc_off, ic_idx, atom, obj, dst, slow,
          r_ic_epoch_val)) {
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_FIELD_OPT: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        uint16_t ic_idx = sv_get_u16(ip + 5);
        MIR_label_t nullish = MIR_new_label(ctx);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_label_t slow = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
        if (mir_emit_get_field_ic_fastpath(
          ctx, jit_func, func, bc_off, ic_idx, atom, obj, dst, slow,
          r_ic_epoch_val)) {
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, nullish);
        mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_FIELD: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        sv_ic_entry_t *ic_slot = NULL;
        uint16_t pf_ic_idx = sv_get_u16(ip + 5);
        if (func->ic_slots && pf_ic_idx != UINT16_MAX && pf_ic_idx < func->ic_count)
          ic_slot = &func->ic_slots[pf_ic_idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_label_t slow = MIR_new_label(ctx);
        MIR_label_t no_err = MIR_new_label(ctx);
        if (mir_emit_put_field_ic_fastpath(
          ctx, jit_func, func, bc_off, pf_ic_idx, atom,
          r_js, obj, val, slow, r_ic_epoch_val,
          remember_obj_proto, imp_remember_obj
        )) {
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, no_err)));
          MIR_append_insn(ctx, jit_func, slow);
        }
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, put_field_proto),
            MIR_new_ref_op(ctx, imp_put_field),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic_slot)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_IMPORT_DEFAULT: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t ns = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 5,
            MIR_new_ref_op(ctx, import_default_proto),
            MIR_new_ref_op(ctx, imp_import_default),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, ns)));
        break;
      }

      case OP_IMPORT_NAMED: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t ns = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, import_named_proto),
            MIR_new_ref_op(ctx, imp_import_named),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, ns),
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_EXPORT: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, export_proto),
            MIR_new_ref_op(ctx, imp_export),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
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

      case OP_DEFINE_SLOT: {
        uint32_t idx = sv_get_u32(ip + 1);
        uint16_t slot = sv_get_u16(ip + 5);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t obj = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, define_slot_proto),
            MIR_new_ref_op(ctx, imp_define_slot),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_int_op(ctx, (int64_t)slot)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_ELEM_OPT: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t key = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_label_t nullish = MIR_new_label(ctx);
        MIR_label_t no_err = MIR_new_label(ctx);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_NULL, 0))));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, nullish),
            MIR_new_reg_op(ctx, obj),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0))));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, nullish);
        mir_load_imm(ctx, jit_func, dst, mkval(T_UNDEF, 0));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_PRIVATE: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        MIR_reg_t token = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_call_helper2(ctx, jit_func, dst,
                         helper2_proto, imp_get_private,
                         r_vm, r_js, obj, token);
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_PRIVATE: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        MIR_reg_t token = vstack_pop(&vs);
        MIR_reg_t val = vstack_pop(&vs);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, private_put_proto),
            MIR_new_ref_op(ctx, imp_put_private),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, obj),
            MIR_new_reg_op(ctx, val),
            MIR_new_reg_op(ctx, token)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_PUT_EVAL_GLOBAL: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t val = vstack_pop(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, put_eval_global_proto),
            MIR_new_ref_op(ctx, imp_put_eval_global),
            MIR_new_reg_op(ctx, r_err_tmp),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_reg_op(ctx, val),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len),
            MIR_new_int_op(ctx, func->is_strict ? 1 : 0)));
        JIT_EMIT_THROW_IF_ERROR(r_err_tmp);
        break;
      }

      case OP_OBJECT: {
        sv_obj_site_cache_t *site = sv_obj_site_for_offset(
          func, (uint32_t)bc_off
        );
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, object_proto),
            MIR_new_ref_op(ctx, imp_object),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)func),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)site)));
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
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 2);
        jit_value_info_t ib = vstack_value_info(&vs, vs.sp - 1);
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
        vstack_set_value_info(&vs, vs.sp - 2, ib);
        vstack_set_value_info(&vs, vs.sp - 1, ia);
        break;
      }

      case OP_SWAP_UNDER: {
        if (vs.sp < 3) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 3);
        jit_value_info_t ib = vstack_value_info(&vs, vs.sp - 2);
        MIR_reg_t ra = vs.regs[vs.sp - 3];
        MIR_reg_t rb = vs.regs[vs.sp - 2];
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
        vstack_set_value_info(&vs, vs.sp - 3, ib);
        vstack_set_value_info(&vs, vs.sp - 2, ia);
        break;
      }

      case OP_ROT4_UNDER: {
        if (vs.sp < 4) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 4, ctx, jit_func, r_d_slot);
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 4);
        jit_value_info_t ib = vstack_value_info(&vs, vs.sp - 3);
        jit_value_info_t ic = vstack_value_info(&vs, vs.sp - 2);
        MIR_reg_t ra = vs.regs[vs.sp - 4];
        MIR_reg_t rb = vs.regs[vs.sp - 3];
        MIR_reg_t rc = vs.regs[vs.sp - 2];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_tmp),
            MIR_new_reg_op(ctx, ra)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, ra),
            MIR_new_reg_op(ctx, rc)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, rc),
            MIR_new_reg_op(ctx, rb)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, rb),
            MIR_new_reg_op(ctx, r_tmp)));
        vstack_set_value_info(&vs, vs.sp - 4, ic);
        vstack_set_value_info(&vs, vs.sp - 3, ia);
        vstack_set_value_info(&vs, vs.sp - 2, ib);
        break;
      }

      case OP_ROT3L: {
        if (vs.sp < 3) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 2, ctx, jit_func, r_d_slot);
        vstack_ensure_boxed(&vs, vs.sp - 3, ctx, jit_func, r_d_slot);
        jit_value_info_t ix = vstack_value_info(&vs, vs.sp - 3);
        jit_value_info_t ia = vstack_value_info(&vs, vs.sp - 2);
        jit_value_info_t ib = vstack_value_info(&vs, vs.sp - 1);
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
        vstack_set_value_info(&vs, vs.sp - 3, ia);
        vstack_set_value_info(&vs, vs.sp - 2, ib);
        vstack_set_value_info(&vs, vs.sp - 1, ix);
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
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
          char inst_lo_name[32], inst_lf_name[32], inst_lp_name[32], inst_icp_name[32];
          char inst_lpt_name[32], inst_lpo_name[32], inst_ls_name[32], inst_ich_name[32];
          char inst_ics_name[32], inst_ici_name[32], inst_pe_name[32], inst_ipe_name[32];
          snprintf(inst_ic_name, sizeof(inst_ic_name), "inst_ic_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ice_name, sizeof(inst_ice_name), "inst_ice_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_rt_name, sizeof(inst_rt_name), "inst_rt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ro_name, sizeof(inst_ro_name), "inst_ro_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ica_name, sizeof(inst_ica_name), "inst_ica_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lt_name, sizeof(inst_lt_name), "inst_lt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lo_name, sizeof(inst_lo_name), "inst_lo_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lf_name, sizeof(inst_lf_name), "inst_lf_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lp_name, sizeof(inst_lp_name), "inst_lp_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_icp_name, sizeof(inst_icp_name), "inst_icp_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lpt_name, sizeof(inst_lpt_name), "inst_lpt_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_lpo_name, sizeof(inst_lpo_name), "inst_lpo_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ls_name, sizeof(inst_ls_name), "inst_ls_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ich_name, sizeof(inst_ich_name), "inst_ich_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ics_name, sizeof(inst_ics_name), "inst_ics_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ici_name, sizeof(inst_ici_name), "inst_ici_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_pe_name, sizeof(inst_pe_name), "inst_pe_%d_%u", bc_off, (unsigned)ic_idx);
          snprintf(inst_ipe_name, sizeof(inst_ipe_name), "inst_ipe_%d_%u", bc_off, (unsigned)ic_idx);

          MIR_reg_t r_ic = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ic_name);
          MIR_reg_t r_ic_epoch = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ice_name);
          MIR_reg_t r_rhs_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_rt_name);
          MIR_reg_t r_rhs_obj = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ro_name);
          MIR_reg_t r_ic_aux = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ica_name);
          MIR_reg_t r_lhs_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lt_name);
          MIR_reg_t r_lhs_obj = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lo_name);
          MIR_reg_t r_lhs_flags = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lf_name);
          MIR_reg_t r_lhs_proto = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lp_name);
          MIR_reg_t r_ic_direct_proto = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_icp_name);
          MIR_reg_t r_lhs_proto_tag = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lpt_name);
          MIR_reg_t r_lhs_proto_obj = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_lpo_name);
          MIR_reg_t r_lhs_shape = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ls_name);
          MIR_reg_t r_ic_holder = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ich_name);
          MIR_reg_t r_ic_shape = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ics_name);
          MIR_reg_t r_ic_idx_val = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ici_name);
          MIR_reg_t r_proto_epoch = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_pe_name);
          MIR_reg_t r_ic_proto_epoch = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_ipe_name);
          MIR_label_t direct_true = MIR_new_label(ctx);

          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic),
              MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_epoch),
              MIR_new_mem_op(ctx, MIR_T_U32,
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
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_proto_epoch),
              MIR_new_mem_op(ctx, MIR_T_U32,
                (MIR_disp_t)offsetof(ant_t, prototype_write_epoch), r_js, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_proto_epoch),
              MIR_new_mem_op(ctx, MIR_T_U32,
                (MIR_disp_t)offsetof(sv_ic_entry_t, prototype_epoch), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_ic_proto_epoch),
              MIR_new_reg_op(ctx, r_proto_epoch)));

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
              MIR_new_reg_op(ctx, r_lhs_flags),
              MIR_new_mem_op(ctx, MIR_T_U8,
                (MIR_disp_t)offsetof(ant_object_t, flags), r_lhs_obj, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_AND,
              MIR_new_reg_op(ctx, r_lhs_flags),
              MIR_new_reg_op(ctx, r_lhs_flags),
              MIR_new_uint_op(ctx, 1u << 3)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_lhs_flags),
              MIR_new_uint_op(ctx, 0)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_lhs_proto),
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)offsetof(ant_object_t, proto), r_lhs_obj, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_direct_proto),
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)offsetof(sv_ic_entry_t, guard.receiver_proto), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BEQ,
              MIR_new_label_op(ctx, direct_true),
              MIR_new_reg_op(ctx, r_lhs_proto),
              MIR_new_reg_op(ctx, r_ic_direct_proto)));
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
          mir_emit_value_to_objptr_or_jmp(
            ctx, jit_func, r_lhs_proto, r_lhs_proto_obj, r_lhs_proto_tag, slow);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_ic_holder),
              MIR_new_mem_op(ctx, MIR_T_I64,
                (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_BNE,
              MIR_new_label_op(ctx, slow),
              MIR_new_reg_op(ctx, r_lhs_proto_obj),
              MIR_new_reg_op(ctx, r_ic_holder)));
          {
            /* The holder is only pointer-compared, never revalidated:
               guard against address reuse across minor GCs with the obj
               epoch (mirrors sv_instanceof_ic_eval). */
            char inst_oep_name[40], inst_oec_name[40];
            snprintf(inst_oep_name, sizeof(inst_oep_name), "inst_oep_%d_%u", bc_off, (unsigned)ic_idx);
            snprintf(inst_oec_name, sizeof(inst_oec_name), "inst_oec_%d_%u", bc_off, (unsigned)ic_idx);
            MIR_reg_t r_obj_ep_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_oep_name);
            MIR_reg_t r_obj_ep_cur = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, inst_oec_name);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)&ant_ic_obj_epoch_counter)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_cur),
                MIR_new_mem_op(ctx, MIR_T_U32, 0, r_obj_ep_ptr, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_mem_op(ctx, MIR_T_U32,
                  (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.epoch), r_ic, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, slow),
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_reg_op(ctx, r_obj_ep_cur)));
          }
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

          MIR_append_insn(ctx, jit_func, direct_true);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, dst),
              MIR_new_uint_op(ctx, js_true)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
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
              MIR_new_mem_op(ctx, MIR_T_U32,
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
          {
            /* Both sides are raw pointer compares with no revalidation:
               guard against address reuse across minor GCs with the obj
               epoch (mirrors sv_isproto_ic_eval). */
            char cip_oep_name[40], cip_oec_name[40];
            snprintf(cip_oep_name, sizeof(cip_oep_name), "cip_oep_%d_%u", bc_off, (unsigned)ic_idx);
            snprintf(cip_oec_name, sizeof(cip_oec_name), "cip_oec_%d_%u", bc_off, (unsigned)ic_idx);
            MIR_reg_t r_obj_ep_ptr = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_oep_name);
            MIR_reg_t r_obj_ep_cur = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, cip_oec_name);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)&ant_ic_obj_epoch_counter)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_cur),
                MIR_new_mem_op(ctx, MIR_T_U32, 0, r_obj_ep_ptr, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_mem_op(ctx, MIR_T_U32,
                  (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.epoch), r_ic, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, slow),
                MIR_new_reg_op(ctx, r_obj_ep_ptr),
                MIR_new_reg_op(ctx, r_obj_ep_cur)));
          }
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_GET_LENGTH: {
        bool builder_length = false;
        int raw_slot_size = sv_op_size[OP_GET_SLOT_RAW];
        if (builder_target_slots && bc_off >= raw_slot_size) {
          uint8_t *prev_ip = ip - raw_slot_size;
          if (*prev_ip == OP_GET_SLOT_RAW) {
            uint16_t slot_idx = sv_get_u16(prev_ip + 1);
            int slot_count = param_count + n_locals;
            builder_length = (int)slot_idx < slot_count &&
              builder_target_slots[slot_idx];
          }
        }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t obj = vstack_pop(&vs);
        MIR_reg_t dst = vstack_push(&vs);
        mir_emit_get_length(
          ctx, jit_func, obj, dst,
          r_vm, r_js, r_d_slot,
          helper1_proto, imp_get_length,
          builder_length,
          -1, bc_off
        );
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
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
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_gt,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, done);
        }
        break;
      }

      case OP_GE: {
        uint8_t fb = sv_func_type_feedback(func) ? sv_func_type_feedback(func)[bc_off] : 0;
        bool fb_num_only  = fb && !(fb & ~SV_TFB_NUM);
        bool fb_never_num = fb && !(fb & SV_TFB_NUM);

        bool l_is_num = vs.slot_type && vs.slot_type[vs.sp - 2] == SLOT_NUM;
        bool r_is_num = vs.slot_type && vs.slot_type[vs.sp - 1] == SLOT_NUM;

        MIR_reg_t rr = vstack_pop(&vs);
        MIR_reg_t rl = vstack_pop(&vs);
        MIR_reg_t rd = vstack_push(&vs);

        if (fb_never_num) {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_reg_op(ctx, r_bailout_val),
              MIR_new_reg_op(ctx, rl)));
          mir_call_helper2(ctx, jit_func, rd,
                           helper2_proto, imp_ge,
                           r_vm, r_js, rl, rr);
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
          mir_emit_bailout_jump_typed(ctx, jit_func,
            r_bailout_off, bc_off,
            r_bailout_sp, pre_op_sp, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
          MIR_append_insn(ctx, jit_func, skip_bail);
        } else {
          if (l_is_num) mir_d_to_i64(ctx, jit_func, rl, vs.d_regs[vs.sp - 1], r_d_slot);
          if (r_is_num) mir_d_to_i64(ctx, jit_func, rr, vs.d_regs[vs.sp], r_d_slot);
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
          mir_emit_bailout_check_typed(ctx, jit_func, rd,
            r_bailout_val, r_bailout_off, bc_off,
            r_bailout_sp, vs.sp + 1, bailout_tramp,
            r_args_buf, &vs, local_regs, n_locals, r_lbuf, r_d_slot,
            vs.sp - 1, l_is_num, vs.sp, r_is_num);
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
        mir_emit_dnum_rebox(ctx, jit_func, local_regs, n_locals, r_d_slot);
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
        if (vs.known_bool) vs.known_bool[vs.sp - 1] = 1;
        break;
      }

      case OP_TYPEOF: {
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        MIR_reg_t rs = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, helper1_proto),
            MIR_new_ref_op(ctx, imp_typeof),
            MIR_new_reg_op(ctx, rs),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, rs)));
        break;
      }

      case OP_VOID: {
        MIR_reg_t rs = vstack_top(&vs);
        mir_load_imm(ctx, jit_func, rs, mkval(T_UNDEF, 0));
        if (vs.slot_type) vs.slot_type[vs.sp - 1] = SLOT_BOXED;
        vstack_clear_value_info(&vs, vs.sp - 1);
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, dst));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_DELETE_EVAL_VAR: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[idx];
        MIR_reg_t dst = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 7,
            MIR_new_ref_op(ctx, delete_eval_var_proto),
            MIR_new_ref_op(ctx, imp_delete_eval_var),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)atom->str),
            MIR_new_uint_op(ctx, (uint64_t)atom->len)));
        JIT_EMIT_THROW_IF_ERROR(dst);
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

        MIR_reg_t r_ctor_target = vstack_pop(&vs);
        MIR_reg_t r_new_func    = vstack_pop(&vs);
        MIR_reg_t r_new_res     = vstack_push(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, new_proto),
            MIR_new_ref_op(ctx, imp_new),
            MIR_new_reg_op(ctx, r_new_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_new_func),
            MIR_new_reg_op(ctx, r_ctor_target),
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_new_res));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        break;
      }

      case OP_CALL_ARRAY_INCLUDES: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        uint16_t call_argc = sv_get_u16(ip + 1);
        if (call_argc > 16 || vs.sp < (int)call_argc + 2) { ok = false; break; }

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
        MIR_reg_t r_call_res = vstack_push(&vs);

        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 9,
            MIR_new_ref_op(ctx, call_proto),
            MIR_new_ref_op(ctx, imp_call_array_includes),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc)));

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
            MIR_new_reg_op(ctx, r_call_res),
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
              MIR_new_reg_op(ctx, r_call_res)));
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_JMP,
              MIR_new_label_op(ctx, h->catch_label)));
        } else {
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
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

        MIR_label_t cm_devirt_slow = NULL;
        MIR_label_t cm_devirt_join = NULL;
        
        if (!is_tail) {
          sv_func_t *inline_callee = sv_tfb_get_call_target(func, bc_off);
          if (inline_callee && jit_inlineable(inline_callee)
              && jit_inline_body_feasible(inline_callee)) {
            int mcn = call_n++;
            cm_devirt_slow = MIR_new_label(ctx);
            cm_devirt_join = MIR_new_label(ctx);

            MIR_reg_t inl_arg_regs[call_argc > 0 ? call_argc : 1];
            for (int i = 0; i < (int)call_argc; i++)
              inl_arg_regs[i] = vs.regs[vs.sp - call_argc + i];
            MIR_reg_t r_inl_callee = vs.regs[vs.sp - call_argc - 1];
            MIR_reg_t r_inl_recv   = vs.regs[vs.sp - call_argc - 2];
            MIR_reg_t r_inl_res    = vs.regs[vs.sp - call_argc - 2];

            char micl_rn[32], mithis_rn[32], miflags_rn[32], mibound_rn[32];
            char mint_rn[32], misup_rn[32], mitag_rn[32], mifn_rn[32];
            snprintf(micl_rn,    sizeof(micl_rn),    "mi%d_cl",    mcn);
            snprintf(mithis_rn,  sizeof(mithis_rn),  "mi%d_this",  mcn);
            snprintf(miflags_rn, sizeof(miflags_rn), "mi%d_flags", mcn);
            snprintf(mibound_rn, sizeof(mibound_rn), "mi%d_bound", mcn);
            snprintf(mint_rn,    sizeof(mint_rn),    "mi%d_nt",    mcn);
            snprintf(misup_rn,   sizeof(misup_rn),   "mi%d_sup",   mcn);
            snprintf(mitag_rn,   sizeof(mitag_rn),   "mi%d_tag",   mcn);
            snprintf(mifn_rn,    sizeof(mifn_rn),    "mi%d_fn",    mcn);

            MIR_reg_t r_inl_cl    = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64,  micl_rn);
            MIR_reg_t r_inl_this  = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL,  mithis_rn);
            MIR_reg_t r_inl_flags = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64,  miflags_rn);
            MIR_reg_t r_inl_bound = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL,  mibound_rn);
            MIR_reg_t r_inl_nt    = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL,  mint_rn);
            MIR_reg_t r_inl_sup   = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL,  misup_rn);
            MIR_reg_t r_inl_tag   = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64,  mitag_rn);
            MIR_reg_t r_inl_gfn   = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64,  mifn_rn);

            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BEQ,
                MIR_new_label_op(ctx, cm_devirt_slow),
                MIR_new_reg_op(ctx, r_inl_callee),
                MIR_new_reg_op(ctx, r_super_val)));

            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_URSH,
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_reg_op(ctx, r_inl_callee),
                MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, cm_devirt_slow),
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));

            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_AND,
                MIR_new_reg_op(ctx, r_inl_cl),
                MIR_new_reg_op(ctx, r_inl_callee),
                MIR_new_uint_op(ctx, NANBOX_DATA_MASK)));

            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_inl_gfn),
                MIR_new_mem_op(ctx, MIR_T_P,
                  (MIR_disp_t)offsetof(sv_closure_t, func),
                  r_inl_cl, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, cm_devirt_slow),
                MIR_new_reg_op(ctx, r_inl_gfn),
                MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)inline_callee)));

            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_mem_op(ctx, MIR_T_U32,
                  (MIR_disp_t)offsetof(sv_closure_t, call_flags),
                  r_inl_cl, 0, 1)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_AND,
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_uint_op(ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_BNE,
                MIR_new_label_op(ctx, cm_devirt_slow),
                MIR_new_reg_op(ctx, r_inl_tag),
                MIR_new_uint_op(ctx, 0)));

            mir_load_imm(ctx, jit_func, r_inl_nt, mkval(T_UNDEF, 0));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_inl_sup),
                MIR_new_mem_op(ctx, MIR_T_I64,
                  (MIR_disp_t)offsetof(sv_closure_t, super_val),
                  r_inl_cl, 0, 1)));

            mir_emit_resolve_call_this(
              ctx, jit_func, r_inl_this, r_inl_cl,
              r_inl_recv, r_inl_flags, r_inl_bound
            );

            jit_inline_ext_t cm_inl_ext = {
              .helper1_proto = helper1_proto,
              .imp_get_length_inline = imp_get_length_inline,
              .imp_get_elem_inline = imp_get_elem_inline,
              .put_field_proto = put_field_proto, .imp_put_field = imp_put_field,
              .remember_obj_proto = remember_obj_proto,
              .imp_remember_obj = imp_remember_obj,
              .call_proto = call_proto, .imp_call = imp_call,
              .call_method_proto = call_method_proto, .imp_call_method = imp_call_method,
              .imp_band = imp_band, .imp_bor = imp_bor, .imp_bxor = imp_bxor,
              .imp_shl = imp_shl, .imp_shr = imp_shr, .imp_ushr = imp_ushr,
              .self_proto = self_proto,
              .r_args_buf = r_args_buf,
            };
            (void)jit_emit_inline_body(
              ctx, jit_func, inline_callee,
              inl_arg_regs, (int)call_argc,
              NULL, NULL,
              r_inl_res, cm_devirt_slow, cm_devirt_join,
              r_bool, &r_d_slot, mcn,
              r_inl_cl, r_inl_this, r_inl_nt, r_inl_sup,
              r_vm, r_js, r_ic_epoch_val,
              helper2_proto, imp_seq, imp_sne, imp_eq, imp_ne,
              gf_proto, imp_get_field_inline,
              gg_proto, imp_gg,
              special_obj_proto, imp_special_obj,
              &cm_inl_ext);

            MIR_append_insn(ctx, jit_func, cm_devirt_slow);
          }
        }

        int cn = call_n++;

        char rn_arr[32], rn_ccl[32], rn_cfn[32], rn_jptr[32], rn_sup[32];
        snprintf(rn_arr,  sizeof(rn_arr),  "cm_arr%d",  cn);
        snprintf(rn_ccl,  sizeof(rn_ccl),  "cm_cl%d",   cn);
        snprintf(rn_cfn,  sizeof(rn_cfn),  "cm_fn%d",   cn);
        snprintf(rn_jptr, sizeof(rn_jptr), "cm_jptr%d", cn);
        snprintf(rn_sup, sizeof(rn_sup), "cm_sup%d", cn);

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
        MIR_reg_t r_callee_super = MIR_new_func_reg(ctx, jit_func->u.func, MIR_JSVAL, rn_sup);

        MIR_label_t lbl_cm_self   = MIR_new_label(ctx);
        MIR_label_t lbl_cm_super  = MIR_new_label(ctx);
        MIR_label_t lbl_cm_interp = MIR_new_label(ctx);
        MIR_label_t lbl_cm_done   = MIR_new_label(ctx);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BEQ,
            MIR_new_label_op(ctx, lbl_cm_super),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_super_val)));

        MIR_reg_t r_callee_cl = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_ccl);
        mir_emit_get_closure(ctx, jit_func, r_callee_cl, r_call_func,
                             r_bool, lbl_cm_interp);

        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_mem_op(ctx, MIR_T_U32,
              (MIR_disp_t)offsetof(sv_closure_t, call_flags),
              r_callee_cl, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_AND,
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, (uint64_t)SV_CALL_HAS_BOUND_ARGS)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_BNE,
            MIR_new_label_op(ctx, lbl_cm_interp),
            MIR_new_reg_op(ctx, r_bool),
            MIR_new_uint_op(ctx, 0)));

        MIR_reg_t r_callee_fn = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_cfn);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_fn),
            MIR_new_mem_op(ctx, MIR_T_P,
              (MIR_disp_t)offsetof(sv_closure_t, func),
              r_callee_cl, 0, 1)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_callee_super),
            MIR_new_mem_op(ctx, MIR_T_I64,
              (MIR_disp_t)offsetof(sv_closure_t, super_val),
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
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_reg_op(ctx, r_jit_ptr),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
            MIR_new_reg_op(ctx, r_callee_super),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_callee_cl)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_cm_done)));

        MIR_append_insn(ctx, jit_func, lbl_cm_self);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 10,
            MIR_new_ref_op(ctx, self_proto),
            MIR_new_ref_op(ctx, jit_func),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)),
            MIR_new_reg_op(ctx, r_super_val),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_closure)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_cm_done)));

        MIR_append_insn(ctx, jit_func, lbl_cm_super);
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_call_out_this, 0, 1),
            MIR_new_reg_op(ctx, r_call_this)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 12,
            MIR_new_ref_op(ctx, call_method_proto),
            MIR_new_ref_op(ctx, imp_call_method),
            MIR_new_reg_op(ctx, r_call_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_call_func),
            MIR_new_reg_op(ctx, r_call_this),
            MIR_new_reg_op(ctx, r_arg_arr),
            MIR_new_int_op(ctx, (int64_t)call_argc),
            MIR_new_reg_op(ctx, r_super_val),
            MIR_new_reg_op(ctx, r_new_target),
            MIR_new_reg_op(ctx, r_call_out_this)));
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, r_this_curr),
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_call_out_this, 0, 1)));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
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
            JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
            MIR_append_insn(ctx, jit_func, no_err);
          }
          if (is_tail) {
            JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_call_res));
          }
        }
        if (cm_devirt_join) {
          MIR_append_insn(ctx, jit_func, cm_devirt_join);
          MIR_reg_t r_join_res = vs.regs[vs.sp - 1];
          JIT_EMIT_THROW_IF_ERROR(r_join_res);
        }
        break;
      }

      case OP_CALL_CALL: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        uint8_t cc_n1 = ip[1];
        uint8_t cc_n2 = ip[2];
        int cc_total = 1 + (int)cc_n1 + (int)cc_n2;
        if (cc_total > 16 || vs.sp < cc_total) { ok = false; break; }

        /* Store [X, args1..., args2...] contiguously into args_buf. */
        for (int i = cc_total - 1; i >= 0; i--) {
          MIR_reg_t areg = vstack_pop(&vs);
          MIR_append_insn(ctx, jit_func,
            MIR_new_insn(ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_JSVAL,
                (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                r_args_buf, 0, 1),
              MIR_new_reg_op(ctx, areg)));
        }

        MIR_reg_t r_cc_res = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, call_call_proto),
            MIR_new_ref_op(ctx, imp_call_call),
            MIR_new_reg_op(ctx, r_cc_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_args_buf),
            MIR_new_int_op(ctx, (int64_t)cc_n1),
            MIR_new_int_op(ctx, (int64_t)cc_n2)));

        JIT_EMIT_THROW_IF_ERROR(r_cc_res);
        break;
      }

      case OP_CALL_CALL_SLOT: {
        vstack_flush_to_boxed(&vs, ctx, jit_func, r_d_slot);
        uint16_t cc_slot_idx = sv_get_u16(ip + 1);
        if (vs.sp < 2) { ok = false; break; }

        MIR_reg_t r_cc_arg1 = vstack_pop(&vs);
        MIR_reg_t r_cc_func = vstack_pop(&vs);
        char cc_slot_name[32];
        snprintf(cc_slot_name, sizeof(cc_slot_name), "cc_slot_%d", bc_off);
        MIR_reg_t r_cc_slot = MIR_new_func_reg(
          ctx, jit_func->u.func, MIR_T_I64, cc_slot_name);

        if ((int)cc_slot_idx < param_count) {
          uint16_t idx = cc_slot_idx;
          bool slot_backed = writes_params ||
            (has_captured_params && captured_params && captured_params[idx]);
          if (slot_backed) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_ADD,
                MIR_new_reg_op(ctx, r_cc_slot),
                MIR_new_reg_op(ctx, r_slotbuf),
                MIR_new_int_op(ctx,
                  (int64_t)idx * (int64_t)sizeof(ant_value_t))));
          } else {
            /* A non-captured parameter cannot be changed by X. Keep the
               ordinary missing-argument behavior while giving the helper a
               stable address. */
            char cc_value_name[32];
            snprintf(cc_value_name, sizeof(cc_value_name), "cc_value_%d", bc_off);
            MIR_reg_t r_cc_value = MIR_new_func_reg(
              ctx, jit_func->u.func, MIR_JSVAL, cc_value_name);
            MIR_label_t arg_in_range = MIR_new_label(ctx);
            MIR_label_t arg_done = MIR_new_label(ctx);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_UBGT,
                MIR_new_label_op(ctx, arg_in_range),
                MIR_new_reg_op(ctx, r_argc),
                MIR_new_int_op(ctx, (int64_t)idx)));
            mir_load_imm(ctx, jit_func, r_cc_value, mkval(T_UNDEF, 0));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, arg_done)));
            MIR_append_insn(ctx, jit_func, arg_in_range);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_cc_value),
                MIR_new_mem_op(ctx, MIR_JSVAL,
                  (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                  r_args, 0, 1)));
            MIR_append_insn(ctx, jit_func, arg_done);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, r_cc_value)));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_cc_slot),
                MIR_new_reg_op(ctx, r_args_buf)));
          }
        } else {
          uint16_t idx = (uint16_t)(cc_slot_idx - (uint16_t)param_count);
          if (idx >= (uint16_t)n_locals) { ok = false; break; }
          bool slot_backed = has_captures && captured_locals && captured_locals[idx];
          if (slot_backed) {
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_ADD,
                MIR_new_reg_op(ctx, r_cc_slot),
                MIR_new_reg_op(ctx, r_lbuf),
                MIR_new_int_op(ctx,
                  (int64_t)idx * (int64_t)sizeof(ant_value_t))));
          } else {
            /* D-only locals need a current boxed representation before the
               helper can treat the temporary as an ant_value_t slot. */
            if (dnum_locals && dnum_locals[idx])
              mir_d_to_i64(
                ctx, jit_func, local_regs[idx], local_d_regs[idx], r_d_slot);
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_JSVAL, 0, r_args_buf, 0, 1),
                MIR_new_reg_op(ctx, local_regs[idx])));
            MIR_append_insn(ctx, jit_func,
              MIR_new_insn(ctx, MIR_MOV,
                MIR_new_reg_op(ctx, r_cc_slot),
                MIR_new_reg_op(ctx, r_args_buf)));
          }
        }

        MIR_reg_t r_cc_res = vstack_push(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 8,
            MIR_new_ref_op(ctx, call_call_slot_proto),
            MIR_new_ref_op(ctx, imp_call_call_slot),
            MIR_new_reg_op(ctx, r_cc_res),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_cc_func),
            MIR_new_reg_op(ctx, r_cc_arg1),
            MIR_new_reg_op(ctx, r_cc_slot)));

        JIT_EMIT_THROW_IF_ERROR(r_cc_res);
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_apply_res));
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
              writes_params ? MIR_new_reg_op(ctx, r_slotbuf) : MIR_new_reg_op(ctx, r_args),
              writes_params ? MIR_new_int_op(ctx, param_count) : MIR_new_reg_op(ctx, r_argc),
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
          if (known_type_locals && known_type_locals[local_idx] != SV_TI_NUM)
            known_type_locals[local_idx] = SV_TI_UNKNOWN;
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
        }
        MIR_append_insn(ctx, jit_func, no_err);
        if ((int)slot_idx >= param_count) {
          int gli = (int)slot_idx - param_count;
          if (gli < n_locals && local_d_regs && known_type_locals
              && known_type_locals[gli] == SV_TI_NUM)
            mir_emit_numeric_local_store_mirror(ctx, jit_func,
              local_d_regs[gli], local_regs[gli], 0, false,
              r_bool, bc_off + sz, vs.sp, &bailout_ctx);
        }
        break;
      }

      case OP_SET_NAME: {
        uint32_t atom_idx = sv_get_u32(ip + 1);
        if (atom_idx >= (uint32_t)func->atom_count) { ok = false; break; }
        sv_atom_t *atom = &func->atoms[atom_idx];
        MIR_reg_t fn_val = vstack_top(&vs);
        MIR_append_insn(ctx, jit_func,
          MIR_new_call_insn(ctx, 6,
            MIR_new_ref_op(ctx, set_name_proto),
            MIR_new_ref_op(ctx, imp_set_name),
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
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
          JIT_EMIT_EXIT_RET(MIR_new_reg_op(ctx, r_err_tmp));
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
          vstack_clear_value_info(&vs, catch_saved_sp);
          vs.slot_type[catch_saved_sp] = SLOT_BOXED;
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
        if (vs.sp < 2) { ok = false; break; }
        vstack_ensure_boxed(&vs, vs.sp - 1, ctx, jit_func, r_d_slot);
        jit_value_info_t info = vstack_value_info(&vs, vs.sp - 1);
        MIR_reg_t a = vs.regs[vs.sp - 1];
        MIR_reg_t below = vs.regs[vs.sp - 2];
        MIR_append_insn(ctx, jit_func,
          MIR_new_insn(ctx, MIR_MOV,
            MIR_new_reg_op(ctx, below),
            MIR_new_reg_op(ctx, a)));
        vs.sp--;
        vs.slot_type[vs.sp - 1] = SLOT_BOXED;
        vstack_set_value_info(&vs, vs.sp - 1, info);
        break;
      }

      case OP_CLOSURE: {
        uint32_t idx = sv_get_u32(ip + 1);
        if (idx >= (uint32_t)func->const_count) { ok = false; break; }
        const char *name_str = NULL;
        uint32_t name_len = 0;
        int fused_set_name_size = 0;
        uint8_t *next_ip = ip + sz;
        if (next_ip < end && (sv_op_t)*next_ip == OP_SET_NAME) {
          int next_sz = sv_op_size[OP_SET_NAME];
          bool next_has_label = false;
          int next_bc_off = (int)(next_ip - func->code);
          for (int i = 0; i < lm.count; i++) {
            if (lm.entries[i].bc_off == next_bc_off) {
              next_has_label = true;
              break;
            }
          }
          if (!next_has_label && next_ip + next_sz <= end) {
            uint32_t name_idx = sv_get_u32(next_ip + 1);
            if (name_idx < (uint32_t)func->atom_count) {
              sv_atom_t *name_atom = &func->atoms[name_idx];
              name_str = name_atom->str;
              name_len = name_atom->len;
              fused_set_name_size = next_sz;
            }
          }
        }
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
          MIR_new_call_insn(ctx, 14,
            MIR_new_ref_op(ctx, closure_proto),
            MIR_new_ref_op(ctx, imp_closure),
            MIR_new_reg_op(ctx, dst),
            MIR_new_reg_op(ctx, r_vm),
            MIR_new_reg_op(ctx, r_js),
            MIR_new_reg_op(ctx, r_closure),
            MIR_new_reg_op(ctx, r_this_curr),
            MIR_new_reg_op(ctx, r_child_slots),
            MIR_new_int_op(ctx, child_slot_base),
            MIR_new_int_op(ctx, child_slot_count),
            MIR_new_uint_op(ctx, (uint64_t)idx),
            MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)name_str),
            MIR_new_uint_op(ctx, (uint64_t)name_len),
            r_jit_open_upvalues ? MIR_new_reg_op(ctx, r_jit_open_upvalues) : MIR_new_uint_op(ctx, 0)));
        sz += fused_set_name_size;
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
    JIT_EMIT_EXIT_RET(MIR_new_uint_op(ctx, mkval(T_UNDEF, 0)));
  }

  if (needs_bailout) {
    MIR_append_insn(ctx, jit_func, bailout_tramp);

    if (r_jit_open_upvalues) {
      MIR_append_insn(ctx, jit_func,
        MIR_new_call_insn(ctx, 4,
          MIR_new_ref_op(ctx, adopt_open_upvalues_proto),
          MIR_new_ref_op(ctx, imp_adopt_open_upvalues),
          MIR_new_reg_op(ctx, r_vm),
          MIR_new_reg_op(ctx, r_jit_open_upvalues)));
    }

    MIR_reg_t r_resume_res = MIR_new_func_reg(ctx, jit_func->u.func,
                                               MIR_JSVAL, "resume_res");
    if (has_captured_params && !writes_params) {
      mir_emit_fill_uncaptured_param_slots_from_args(
        ctx, jit_func, r_slotbuf, r_args, r_argc, captured_params, param_count);
    }
    MIR_append_insn(ctx, jit_func,
      MIR_new_call_insn(ctx, 15,
        MIR_new_ref_op(ctx, resume_proto),
        MIR_new_ref_op(ctx, imp_resume),
        MIR_new_reg_op(ctx, r_resume_res),
        MIR_new_reg_op(ctx, r_vm),
        MIR_new_reg_op(ctx, r_closure),
        MIR_new_reg_op(ctx, r_this_curr),
        MIR_new_reg_op(ctx, r_args),
        MIR_new_reg_op(ctx, r_argc),
        MIR_new_reg_op(ctx, r_args_buf),
        MIR_new_reg_op(ctx, r_bailout_sp),
        params_in_slotbuf ? MIR_new_reg_op(ctx, r_slotbuf) : MIR_new_uint_op(ctx, 0),
        MIR_new_int_op(ctx, params_in_slotbuf ? param_count : 0),
        MIR_new_reg_op(ctx, r_lbuf),
        MIR_new_int_op(ctx, n_locals),
        MIR_new_reg_op(ctx, r_bailout_off)));
    MIR_append_insn(ctx, jit_func,
      MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_resume_res)));
  }

#undef JIT_EMIT_THROW_IF_ERROR
#undef JIT_EMIT_EXIT_RET

  MIR_finish_func(ctx);
  MIR_finish_module(ctx);
  if (sv_dump_jit_unlikely) MIR_output_module(ctx, stderr, mod);

  free(vs.regs);
  free(vs.d_regs);
  free(vs.known_func);
  free(vs.slot_type);
  free(vs.known_const);
  free(vs.has_const);
  free(vs.known_bool);
  free(local_regs);
  free(dnum_locals);
  jit_cur_dnum_locals = NULL;
  jit_cur_local_d_regs = NULL;
  free(local_d_regs);
  free(known_func_locals);
  free(known_type_locals);
  free(self_binding_guards);
  free(captured_params);
  free(captured_locals);
  free(feat.builder_target_slots);

  if (!ok) {
    MIR_remove_module(ctx, mod);
    func->jit_compile_failed = true;
    func->jit_compiling = false;
    return NULL;
  }

  MIR_load_module(ctx, mod);
  MIR_link(ctx, MIR_set_gen_interface, NULL);

  sv_jit_func_t generated = MIR_gen(ctx, jit_func);
  func->jit_compiling = false;
  if (!generated) {
    func->jit_compile_failed = true;
    return NULL;
  }

  func->jit_compiled_tfb_ver = func->tfb_version;
  return generated;
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
  sv_jit_enter(js);
  ant_value_t result = jit(
    vm, ctx->this_val, js->new_target,
    ctx->super_val, ctx->args, ctx->argc, closure);
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
  /* Mark loop-hot even when already compiled: a later type-feedback
     recompile then lands in the opt3 context. */
  func->jit_loop_hot = true;
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

  func->back_edge_count = 0;
  sv_jit_enter(js);
  ant_value_t result = jit(
    vm, frame->this, frame->new_target, frame->super_val,
    frame->bp, frame->argc, closure);
  sv_jit_leave(js);

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }

  return result;
}

#endif 
