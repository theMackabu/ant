// meson test -C build jit-address-modes
#include "../src/jit/jit_internal.h"
#include <math.h>
#include "mir.h"
#include "mir-gen.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void check_load(MIR_type_t type, size_t width, int64_t displacement) {
  MIR_context_t ctx = MIR_init();
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 3);
  MIR_module_t module = MIR_new_module(ctx, "address_modes");
  MIR_type_t result_type = MIR_T_I64;
  MIR_item_t function = MIR_new_func(ctx, "load", 1, &result_type,
      2, MIR_T_P, "base", MIR_T_I64, "index");
  MIR_reg_t base = MIR_reg(ctx, "base", function->u.func);
  MIR_reg_t index = MIR_reg(ctx, "index", function->u.func);
  MIR_reg_t address = MIR_new_func_reg(ctx, function->u.func, MIR_T_I64, "address");
  MIR_reg_t result = MIR_new_func_reg(ctx, function->u.func, MIR_T_I64, "result");
  MIR_append_insn(ctx, function, MIR_new_insn(ctx, MIR_MUL,
      MIR_new_reg_op(ctx, address), MIR_new_reg_op(ctx, index), MIR_new_int_op(ctx, width)));
  MIR_append_insn(ctx, function, MIR_new_insn(ctx, MIR_ADD,
      MIR_new_reg_op(ctx, address), MIR_new_reg_op(ctx, base), MIR_new_reg_op(ctx, address)));
  MIR_append_insn(ctx, function, MIR_new_insn(ctx, MIR_ADD,
      MIR_new_reg_op(ctx, address), MIR_new_reg_op(ctx, address), MIR_new_int_op(ctx, displacement)));
  MIR_append_insn(ctx, function, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, result), MIR_new_mem_op(ctx, type, 0, address, 0, 1)));
  MIR_append_insn(ctx, function, MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, result)));
  MIR_finish_func(ctx);
  MIR_finish_module(ctx);
  MIR_load_module(ctx, module);
  MIR_link(ctx, MIR_set_gen_interface, NULL);
  uint64_t (*load)(void *, int64_t) = (uint64_t (*)(void *, int64_t))MIR_gen(ctx, function);
  assert(load);
  _Alignas(16) uint8_t data[65536] = {0};
  uint8_t *origin = data + 64;
  for (int64_t i = 0; i < 4; i++) {
    uint64_t value = 37 + i * 19;
    memcpy(origin + displacement + i * width, &value, width);
  }
  for (int64_t i = 0; i < 4; i++) assert(load(origin, i) == (uint64_t)(37 + i * 19));
  MIR_gen_finish(ctx);
  MIR_finish(ctx);
}

static void check_index_guard(void) {
  MIR_context_t ctx = MIR_init();
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 3);
  MIR_module_t module = MIR_new_module(ctx, "index_guard");
  MIR_type_t ret = MIR_T_I64;
  MIR_item_t function = MIR_new_func(ctx, "index", 1, &ret, 1, MIR_T_D, "number");
  MIR_reg_t number = MIR_reg(ctx, "number", function->u.func);
  MIR_label_t slow = MIR_new_label(ctx);
  MIR_reg_t index = mir_emit_array_index_guard(ctx, function, 0, number, true, 0, slow, 0);
  MIR_append_insn(ctx, function, MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, index)));
  MIR_append_insn(ctx, function, slow);
  MIR_append_insn(ctx, function, MIR_new_ret_insn(ctx, 1, MIR_new_int_op(ctx, -1)));
  MIR_finish_func(ctx); MIR_finish_module(ctx);
  MIR_load_module(ctx, module); MIR_link(ctx, MIR_set_gen_interface, NULL);
  int64_t (*guard)(double) = (int64_t (*)(double))MIR_gen(ctx, function);
  const double cases[] = {0, -0.0, 1, 0.5, 1.5, 2147483648.0, 4294967294.0,
    4294967294.5, 4294967295.0, -1, INFINITY, -INFINITY, NAN};
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    double d = cases[i];
    int64_t expected = !signbit(d) && isfinite(d) && d <= 4294967294.0 && trunc(d) == d
      ? (int64_t)d : -1;
    assert(guard(d) == expected);
  }
  for (int exponent = 0; exponent <= 31; exponent++) {
    double center = ldexp(1.0, exponent);
    const double nearby[] = {nextafter(center, 0), center, nextafter(center, INFINITY)};
    for (size_t i = 0; i < 3; i++) {
      double d = nearby[i];
      assert(guard(d) == (trunc(d) == d ? (int64_t)d : -1));
    }
  }
  uint64_t state = UINT64_C(0xa638217ac94bd8);
  for (int i = 0; i < 20000; i++) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    double d; memcpy(&d, &state, sizeof(d));
    int64_t expected = !signbit(d) && isfinite(d) && d <= 4294967294.0 && trunc(d) == d
      ? (int64_t)d : -1;
    assert(guard(d) == expected);
  }
  MIR_gen_finish(ctx); MIR_finish(ctx);
}

// Exercise the emitted dense-read guards independently of feedback/warmup.
static void check_dense_read_guard(void) {
  ant_t *js = ant_create();
  assert(js);
  MIR_context_t ctx = MIR_init();
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 3);
  MIR_module_t module = MIR_new_module(ctx, "dense_read_guard");
  MIR_type_t ret = MIR_T_I64;
  MIR_item_t function = MIR_new_func(ctx, "read", 1, &ret,
      2, MIR_T_I64, "object", MIR_T_I64, "index");
  MIR_reg_t cage = MIR_new_func_reg(ctx, function->u.func, MIR_T_I64, "cage_base");
  mir_load_imm(ctx, function, cage, ant_cage_base());
  MIR_reg_t object = MIR_reg(ctx, "object", function->u.func);
  MIR_reg_t index = MIR_reg(ctx, "index", function->u.func);
  MIR_reg_t value = MIR_new_func_reg(ctx, function->u.func, MIR_T_I64, "value");
  MIR_label_t slow = MIR_new_label(ctx);
  mir_emit_dense_element_guard(ctx, function, object, index, value,
      JIT_ELEMENT_NUMERIC_READ, slow, 0);
  MIR_append_insn(ctx, function, MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, value)));
  MIR_append_insn(ctx, function, slow);
  MIR_append_insn(ctx, function, MIR_new_ret_insn(ctx, 1, MIR_new_uint_op(ctx, js_mkundef())));
  MIR_finish_func(ctx); MIR_finish_module(ctx);
  MIR_load_module(ctx, module); MIR_link(ctx, MIR_set_gen_interface, NULL);
  ant_value_t (*read)(ant_value_t, uint64_t) =
      (ant_value_t (*)(ant_value_t, uint64_t))MIR_gen(ctx, function);
  ant_value_t boxed = js_mkarr(js);
  assert(!is_err(boxed));
  ant_object_t *array = js_obj_ptr(boxed);
  assert(array && array->flags.fast_array && array->u.array.data);
  assert(read(boxed, 0) == js_mkundef());
  js_arr_push(js, boxed, tov(41));
  js_arr_push(js, boxed, tov(42));
  js_arr_push(js, boxed, js_mkundef());
  assert(read(boxed, 0) == tov(41));
  assert(read(boxed, 1) == tov(42));
  assert(read(boxed, 2) == js_mkundef());
  assert(read(boxed, 3) == js_mkundef());
  assert(read(boxed, UINT64_MAX) == js_mkundef());
  array->flags.is_exotic = 1;
  assert(read(boxed, 0) == js_mkundef());
  array->flags.is_exotic = 0;
  array->flags.dense_length_fits = 0;
  assert(read(boxed, 0) == js_mkundef());
  array->flags.dense_length_fits = 1;
  js_destroy(js);
  MIR_gen_finish(ctx); MIR_finish(ctx);
}

int main(void) {
  check_dense_read_guard();
  check_index_guard();
  const MIR_type_t types[] = {MIR_T_U8, MIR_T_U16, MIR_T_U32, MIR_T_U64};
  for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) {
    size_t width = (size_t)1 << i;
    check_load(types[i], width, 0);
    check_load(types[i], width, width);
    check_load(types[i], width, 2 * width);
    check_load(types[i], width, 4095 * width);
    check_load(types[i], width, 4096 * width);
    check_load(types[i], width, -(int64_t)width);
  }
  puts("PASS JIT address scales, aligned offsets and displacement boundaries");
  return 0;
}
