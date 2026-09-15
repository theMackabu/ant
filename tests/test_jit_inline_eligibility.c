// meson test -C build jit-inline-eligibility
#include "../src/jit/jit_internal.h"
#include <assert.h>

static bool can_write_parameter(sv_op_t op, uint16_t param_count, uint16_t idx) {
  uint8_t code[] = {OP_UNDEF, (uint8_t)op, (uint8_t)idx, (uint8_t)(idx >> 8), OP_RETURN_UNDEF};
  sv_func_t callee = {0};
  callee.code = code;
  callee.code_len = sizeof(code);
  callee.param_count = param_count;
  callee.max_stack = 1;
  return jit_inlineable(&callee);
}

static void check_branch_boundaries(void) {
  const sv_op_t branches[] = {OP_JMP, OP_JMP_TRUE, OP_JMP_FALSE, OP_JMP_TRUE8, OP_JMP_FALSE8};
  for (size_t i = 0; i < sizeof(branches) / sizeof(*branches); i++) {
    sv_op_t op = branches[i];
    int next = 3 + sv_op_size[op];
    uint8_t code[16] = {OP_OBJECT, OP_POP, OP_TRUE, (uint8_t)op};
    code[next] = OP_CONST_I8;
    code[next + 1] = 0;
    code[next + 2] = OP_POP;
    code[next + 3] = OP_RETURN_UNDEF;
    sv_func_t callee = {.code = code, .code_len = next + 4, .max_stack = 2};
    const int32_t deltas[] = {0, 1, 3, 4, 5, -1, INT32_MAX};
    for (size_t j = 0; j < sizeof(deltas) / sizeof(*deltas); j++) {
      int32_t delta = deltas[j];
      if (sv_op_size[op] == 2 && delta == INT32_MAX) continue;
      for (int byte = 0; byte < sv_op_size[op] - 1; byte++)
        code[4 + byte] = (uint8_t)((uint32_t)delta >> (byte * 8));
      bool expected = delta == 0 || delta == 3;
      assert(jit_inlineable(&callee) == expected);
      // Dead branches are still emitted and must obey the same boundaries.
      code[0] = OP_RETURN_UNDEF;
      assert(jit_inlineable(&callee) == expected);
      code[0] = OP_OBJECT;
    }
  }
  uint8_t truncated[] = {OP_GET_ARG, 0};
  sv_func_t callee = {.code = truncated, .code_len = sizeof(truncated)};
  assert(!jit_inlineable(&callee));
}

static void check_stack_effects(void) {
  const struct {
    uint8_t code[5];
    int pops, pushes;
  } cases[] = {
    {{OP_OBJECT}, 0, 1},
    {{OP_CALL, 4, 0}, 5, 1},
    {{OP_CALL_METHOD, 44, 1}, 302, 1},
    {{OP_CALL_STABLE_BUILTIN, 0, 3, 0}, 5, 1},
    {{OP_CALL_CALL, 2, 3}, 6, 1},
    {{OP_ARRAY, 2, 0}, 2, 1},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    int pops, pushes;
    assert(sv_op_stack_effect(NULL, cases[i].code, &pops, &pushes));
    assert(pops == cases[i].pops && pushes == cases[i].pushes);
  }
  struct {
    sv_map_template_desc_t desc;
    sv_map_template_table_header_t header;
    uint8_t code[5];
  } map = {
    .desc = {.substitution_count = 2, .operation = SV_MAP_TEMPLATE_GET},
    .header = {.count = 1, .magic = SV_MAP_TEMPLATE_TABLE_MAGIC},
    .code = {OP_CALL_MAP_TEMPLATE, 0, 0, 0, 0},
  };
  sv_func_t callee = {.code = map.code, .has_map_templates = true};
  int pops, pushes;
  assert(sv_op_stack_effect(&callee, map.code, &pops, &pushes));
  assert(pops == 4 && pushes == 1);
  map.code[0] = OP_TAIL_MAP_TEMPLATE;
  assert(sv_op_stack_effect(&callee, map.code, &pops, &pushes));
  assert(pops == 4 && pushes == 0);
  map.code[1] = 1;
  assert(!sv_op_stack_effect(&callee, map.code, &pops, &pushes));
}

static void check_inline_emission(sv_func_t *callee, uintptr_t live_slot, uintptr_t dead_slot,
                                 int expected_calls, int expected_live_refs, int expected_dead_refs) {
  assert(jit_inlineable(callee));
  MIR_context_t ctx = MIR_init();
  MIR_new_module(ctx, "inline_object_sites");
  MIR_type_t ret_type = MIR_JSVAL;
  jit_inline_ext_t ext = {0};
  ext.object_proto = MIR_new_proto(ctx, "object_proto", 1, &ret_type, 4,
      MIR_T_I64, "vm", MIR_T_I64, "js", MIR_T_P, "func", MIR_T_P, "site");
  ext.imp_object = MIR_new_import(ctx, "object_helper");
  MIR_item_t fn = MIR_new_func(ctx, "inline_objects", 1, &ret_type, 0);
  MIR_reg_t result = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, "result");
  MIR_reg_t scratch = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, "scratch");
  MIR_reg_t vm = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, "vm");
  MIR_reg_t js = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, "js");
  MIR_label_t slow = MIR_new_label(ctx), join = MIR_new_label(ctx);
  MIR_reg_t d_slot = 0;
  int reg_site = 0;
  jit_emit_inline_body(ctx, fn, NULL, callee, NULL, 0, NULL, NULL,
      result, slow, join, scratch, &d_slot, 0, &reg_site,
      0, 0, 0, 0, vm, js, 0,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &ext);
  MIR_append_insn(ctx, fn, slow);
  MIR_append_insn(ctx, fn, join);
  MIR_append_insn(ctx, fn, MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, result)));
  MIR_finish_func(ctx);
  MIR_finish_module(ctx);

  int live_refs = 0, dead_refs = 0, calls = 0;
  for (MIR_insn_t insn = DLIST_HEAD(MIR_insn_t, fn->u.func->insns); insn;
       insn = DLIST_NEXT(MIR_insn_t, insn)) {
    if (insn->code == MIR_CALL) calls++;
    if (insn->code != MIR_MOV || insn->ops[1].mode != MIR_OP_UINT) continue;
    uint64_t address = insn->ops[1].u.u;
    if (address == live_slot) live_refs++;
    if (address == dead_slot) dead_refs++;
  }
  assert(calls == expected_calls);
  assert(live_refs == expected_live_refs);
  assert(dead_refs == expected_dead_refs);
  MIR_finish(ctx);
}

static void check_dead_object_site(int dead_keys) {
  // The forward jump hides the second OBJECT from escape analysis, but the
  // inline emitter still visits it. Inspect MIR before dead-code elimination.
  uint8_t code[] = {
    OP_OBJECT, OP_POP, OP_JMP, 2, 0, 0, 0,
    OP_OBJECT, OP_POP, OP_RETURN_UNDEF,
  };
  sv_obj_site_cache_t sites[] = {
    {.bc_off = 0},
    {.bc_off = 7, .key_count = (uint16_t)(dead_keys > 0 ? dead_keys : 0)},
  };
  sv_func_t callee = {0};
  callee.code = code;
  callee.code_len = sizeof(code);
  callee.max_stack = 1;
  callee.obj_sites = sites;
  callee.obj_site_count = dead_keys < 0 ? 1 : 2;
  uintptr_t live_slot = (uintptr_t)&sites[0].literal_template;
  uintptr_t dead_slot = dead_keys < 0
    ? offsetof(sv_obj_site_cache_t, literal_template)
    : (uintptr_t)&sites[1].literal_template;
  assert(!callee.jit_inline_reuse_checked);
  for (int pass = 0; pass < 3; pass++) {
    // Recompilation/OSR feedback changes do not invalidate bytecode analysis.
    callee.tfb_version++;
    callee.jit_code_cold = !callee.jit_code_cold;
    check_inline_emission(&callee, live_slot, dead_slot, 2, 2, dead_keys == 0 ? 2 : 0);
    assert(callee.jit_inline_reuse_checked && callee.jit_inline_reuse_empty);
  }
}

static void check_rejected_reuse(bool has_object) {
  uint8_t code[] = {has_object ? OP_OBJECT : OP_UNDEF, OP_RETURN};
  sv_obj_site_cache_t site = {.bc_off = 0};
  sv_func_t callee = {0};
  callee.code = code;
  callee.code_len = sizeof(code);
  callee.max_stack = 1;
  callee.obj_sites = has_object ? &site : NULL;
  callee.obj_site_count = has_object ? 1 : 0;
  assert(!callee.jit_inline_reuse_checked);
  for (int pass = 0; pass < 3; pass++) {
    check_inline_emission(&callee, (uintptr_t)&site.literal_template,
                         offsetof(sv_obj_site_cache_t, literal_template), has_object ? 1 : 0, 0, 0);
    assert(callee.jit_inline_reuse_checked && !callee.jit_inline_reuse_empty);
  }
}

int main(void) {
  check_branch_boundaries();
  puts("PASS inline branches require complete, forward instruction targets");
  check_stack_effects();
  puts("PASS shared stack effects include variadic operands and map descriptors");
  const sv_op_t writes[] = {OP_PUT_ARG, OP_SET_ARG};
  for (size_t i = 0; i < sizeof(writes) / sizeof(*writes); i++) {
    sv_op_t op = writes[i];
    assert(!can_write_parameter(op, 0, 0));
    assert(can_write_parameter(op, 1, 0));
    assert(!can_write_parameter(op, 1, 1));
    assert(!can_write_parameter(op, 3, 4));
    assert(!can_write_parameter(op, 3, SV_JIT_ARGS_BUF_CAP - 1));
    assert(can_write_parameter(op, SV_JIT_ARGS_BUF_CAP, SV_JIT_ARGS_BUF_CAP - 1));
    assert(can_write_parameter(op, SV_JIT_ARGS_BUF_CAP + 1, SV_JIT_ARGS_BUF_CAP - 1));
    assert(!can_write_parameter(op, SV_JIT_ARGS_BUF_CAP + 1, SV_JIT_ARGS_BUF_CAP));
  }
  puts("PASS inline parameter writes respect the callee and argument buffer bounds");
  check_dead_object_site(-1);
  check_dead_object_site(1);
  check_dead_object_site(0);
  puts("PASS inline object reuse guards each site, including unreachable objects");
  check_rejected_reuse(false);
  check_rejected_reuse(true);
  puts("PASS inline object reuse caches positive and negative decisions across emissions");
}
