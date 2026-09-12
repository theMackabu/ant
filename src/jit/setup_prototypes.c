#include "compile.h"

void jit_setup_prototypes(jit_compile_t *c, MIR_type_t ret_type) {
  c->self_proto = MIR_new_proto(c->ctx, "jit_proto",
                                1, &ret_type,
                                7,
                                MIR_T_I64, "vm",
                                MIR_JSVAL, "this_val",
                                MIR_JSVAL, "new_target",
                                MIR_JSVAL, "super_val",
                                MIR_T_P, "args",
                                MIR_T_I32, "argc",
                                MIR_T_P, "closure");
  MIR_type_t h2_ret = MIR_JSVAL;
  c->helper2_proto = MIR_new_proto(c->ctx, "helper2_proto",
                                   1, &h2_ret,
                                   4,
                                   MIR_T_I64, "vm",
                                   MIR_T_I64, "js",
                                   MIR_JSVAL, "l",
                                   MIR_JSVAL, "r");

  MIR_type_t private_put_ret = MIR_JSVAL;
  c->private_put_proto = MIR_new_proto(c->ctx, "private_put_proto",
                                       1, &private_put_ret,
                                       5,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js",
                                       MIR_JSVAL, "obj",
                                       MIR_JSVAL, "val",
                                       MIR_JSVAL, "token");

  MIR_type_t call_ret = MIR_JSVAL;
  c->call_proto = MIR_new_proto(c->ctx, "call_proto",
                                1, &call_ret,
                                6,
                                MIR_T_I64, "vm",
                                MIR_T_I64, "js_p",
                                MIR_JSVAL, "func",
                                MIR_JSVAL, "this_val",
                                MIR_T_P, "args",
                                MIR_T_I32, "argc");

  c->call_string_intrinsic_proto = MIR_new_proto(
      c->ctx, "call_string_intrinsic_proto",
      1, &call_ret,
      7,
      MIR_T_I64, "vm",
      MIR_T_I64, "js_p",
      MIR_T_I32, "kind",
      MIR_JSVAL, "func",
      MIR_JSVAL, "this_val",
      MIR_T_P, "args",
      MIR_T_I32, "argc");

  c->call_map_template_proto = MIR_new_proto(
      c->ctx, "call_map_template_proto",
      1, &call_ret,
      8,
      MIR_T_I64, "vm",
      MIR_T_I64, "js_p",
      MIR_JSVAL, "func",
      MIR_JSVAL, "this_val",
      MIR_JSVAL, "value0",
      MIR_JSVAL, "value1",
      MIR_JSVAL, "value2",
      MIR_T_P, "descriptor");

  c->map_template_fast_proto = MIR_new_proto(
      c->ctx, "map_template_fast_proto",
      1, &call_ret,
      7,
      MIR_T_I64, "js_p",
      MIR_JSVAL, "func",
      MIR_JSVAL, "this_val",
      MIR_JSVAL, "value0",
      MIR_JSVAL, "value1",
      MIR_JSVAL, "value2",
      MIR_T_P, "descriptor");

  c->map_numeric_pair_fast_proto = MIR_new_proto(
      c->ctx, "map_numeric_pair_fast_proto",
      1, &call_ret,
      7,
      MIR_T_I64, "js_p",
      MIR_JSVAL, "func",
      MIR_JSVAL, "this_val",
      MIR_JSVAL, "left",
      MIR_JSVAL, "right",
      MIR_T_P, "separator",
      MIR_T_I32, "separator_len");

  c->regexp_exec_truthy_proto = MIR_new_proto(
      c->ctx, "regexp_exec_truthy_proto",
      1, &call_ret,
      5,
      MIR_T_I64, "vm",
      MIR_T_I64, "js_p",
      MIR_JSVAL, "func",
      MIR_JSVAL, "this_val",
      MIR_JSVAL, "arg");

  c->stable_load_proto = MIR_new_proto(c->ctx, "stable_load_proto",
                                       1, &call_ret,
                                       3,
                                       MIR_T_I64, "js_p",
                                       MIR_T_I32, "kind",
                                       MIR_T_P, "receiver_out");

  c->stable_call_proto = MIR_new_proto(c->ctx, "stable_call_proto",
                                       1, &call_ret,
                                       7,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js_p",
                                       MIR_T_I32, "kind",
                                       MIR_JSVAL, "func",
                                       MIR_JSVAL, "this_val",
                                       MIR_T_P, "args",
                                       MIR_T_I32, "argc");

  MIR_type_t call_call_ret = MIR_JSVAL;
  c->call_call_proto = MIR_new_proto(c->ctx, "call_call_proto",
                                     1, &call_call_ret,
                                     5,
                                     MIR_T_I64, "vm",
                                     MIR_T_I64, "js_p",
                                     MIR_T_P, "base",
                                     MIR_T_I32, "n1",
                                     MIR_T_I32, "n2");

  c->call_call_slot_proto = MIR_new_proto(c->ctx, "call_call_slot_proto",
                                          1, &call_call_ret,
                                          5,
                                          MIR_T_I64, "vm",
                                          MIR_T_I64, "js_p",
                                          MIR_JSVAL, "func",
                                          MIR_JSVAL, "arg1",
                                          MIR_T_P, "slot");

  MIR_type_t call_method_ret = MIR_JSVAL;
  c->call_method_proto = MIR_new_proto(c->ctx, "callm_proto",
                                       1, &call_method_ret,
                                       9,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js",
                                       MIR_JSVAL, "func",
                                       MIR_JSVAL, "this_val",
                                       MIR_T_P, "args",
                                       MIR_T_I32, "argc",
                                       MIR_JSVAL, "super_val",
                                       MIR_JSVAL, "new_target",
                                       MIR_T_P, "out_this");

  MIR_type_t gg_ret = MIR_JSVAL;
  c->gg_proto = MIR_new_proto(c->ctx, "gg_proto",
                              1, &gg_ret, 4,
                              MIR_T_I64, "js",
                              MIR_T_P, "str",
                              MIR_T_P, "func",
                              MIR_T_I32, "bc_off");

  MIR_type_t geg_ret = MIR_JSVAL;
  c->get_eval_global_proto = MIR_new_proto(c->ctx, "geg_proto",
                                           1, &geg_ret, 7,
                                           MIR_T_I64, "js",
                                           MIR_T_P, "closure",
                                           MIR_T_P, "str",
                                           MIR_T_I32, "len",
                                           MIR_T_P, "func",
                                           MIR_T_I32, "bc_off",
                                           MIR_T_I32, "allow_missing");

  MIR_type_t rest_ret = MIR_JSVAL;
  c->rest_proto = MIR_new_proto(c->ctx, "rest_proto",
                                1, &rest_ret, 5,
                                MIR_T_I64, "vm",
                                MIR_T_I64, "js",
                                MIR_T_P, "args",
                                MIR_T_I32, "argc",
                                MIR_T_I32, "start");

  MIR_type_t gf_ret = MIR_JSVAL;
  c->gf_proto = MIR_new_proto(c->ctx, "gf_proto",
                              1, &gf_ret, 7,
                              MIR_T_I64, "vm",
                              MIR_T_I64, "js",
                              MIR_JSVAL, "obj",
                              MIR_T_P, "str",
                              MIR_T_I32, "len",
                              MIR_T_P, "func",
                              MIR_T_I32, "bc_off");

  MIR_type_t impd_ret = MIR_JSVAL;
  c->import_default_proto = MIR_new_proto(c->ctx, "impd_proto",
                                          1, &impd_ret, 2,
                                          MIR_T_I64, "js",
                                          MIR_JSVAL, "ns");

  MIR_type_t impn_ret = MIR_JSVAL;
  c->import_named_proto = MIR_new_proto(c->ctx, "impn_proto",
                                        1, &impn_ret, 6,
                                        MIR_T_I64, "js",
                                        MIR_JSVAL, "ns",
                                        MIR_T_P, "str",
                                        MIR_T_I32, "len",
                                        MIR_T_P, "func",
                                        MIR_T_I32, "bc_off");

  MIR_type_t exp_ret = MIR_JSVAL;
  c->export_proto = MIR_new_proto(c->ctx, "exp_proto",
                                  1, &exp_ret, 5,
                                  MIR_T_I64, "js",
                                  MIR_T_P, "closure",
                                  MIR_T_P, "str",
                                  MIR_T_I32, "len",
                                  MIR_JSVAL, "val");

  MIR_type_t ge_ret = MIR_JSVAL;
  c->ge_proto = MIR_new_proto(c->ctx, "ge_proto",
                              1, &ge_ret, 6,
                              MIR_T_I64, "vm",
                              MIR_T_I64, "js",
                              MIR_JSVAL, "obj",
                              MIR_JSVAL, "key",
                              MIR_T_P, "func",
                              MIR_T_I32, "bc_off");

  MIR_type_t inst_ret = MIR_JSVAL;
  c->inst_proto = MIR_new_proto(c->ctx, "inst_proto",
                                1, &inst_ret, 6,
                                MIR_T_I64, "vm",
                                MIR_T_I64, "js",
                                MIR_JSVAL, "l",
                                MIR_JSVAL, "r",
                                MIR_T_P, "func",
                                MIR_T_I32, "bc_off");

  MIR_type_t cip_ret = MIR_JSVAL;
  c->call_is_proto = MIR_new_proto(c->ctx, "cip_proto",
                                   1, &cip_ret, 7,
                                   MIR_T_I64, "vm",
                                   MIR_T_I64, "js",
                                   MIR_JSVAL, "this_val",
                                   MIR_JSVAL, "func_val",
                                   MIR_JSVAL, "arg",
                                   MIR_T_P, "func",
                                   MIR_T_I32, "bc_off");

  MIR_type_t h1_ret = MIR_JSVAL;
  c->helper1_proto = MIR_new_proto(c->ctx, "helper1_proto",
                                   1, &h1_ret, 3,
                                   MIR_T_I64, "vm",
                                   MIR_T_I64, "js",
                                   MIR_JSVAL, "v");

  MIR_type_t ts_ret = MIR_JSVAL;
  c->to_string_proto = MIR_new_proto(c->ctx, "to_string_proto",
                                     1, &ts_ret, 2,
                                     MIR_T_I64, "js",
                                     MIR_JSVAL, "v");

  MIR_type_t normalize_this_ret = MIR_JSVAL;
  c->normalize_this_proto = MIR_new_proto(c->ctx, "normalize_this_proto",
                                          1, &normalize_this_ret, 2,
                                          MIR_T_I64, "js",
                                          MIR_JSVAL, "value");

  MIR_type_t sal_ret = MIR_JSVAL;
  c->str_append_local_proto = MIR_new_proto(c->ctx, "sal_proto",
                                            1, &sal_ret, 8,
                                            MIR_T_I64, "vm",
                                            MIR_T_I64, "js",
                                            MIR_T_P, "func",
                                            MIR_T_P, "args",
                                            MIR_T_I32, "argc",
                                            MIR_T_P, "locals",
                                            MIR_T_I32, "slot_idx",
                                            MIR_JSVAL, "rhs");

  MIR_type_t sals_ret = MIR_JSVAL;
  c->str_append_local_snapshot_proto = MIR_new_proto(c->ctx, "sals_proto",
                                                     1, &sals_ret, 9,
                                                     MIR_T_I64, "vm",
                                                     MIR_T_I64, "js",
                                                     MIR_T_P, "func",
                                                     MIR_T_P, "args",
                                                     MIR_T_I32, "argc",
                                                     MIR_T_P, "locals",
                                                     MIR_T_I32, "slot_idx",
                                                     MIR_JSVAL, "lhs",
                                                     MIR_JSVAL, "rhs");

  MIR_type_t sfl_ret = MIR_JSVAL;
  c->str_flush_local_proto = MIR_new_proto(c->ctx, "sfl_proto",
                                           1, &sfl_ret, 7,
                                           MIR_T_I64, "vm",
                                           MIR_T_I64, "js",
                                           MIR_T_P, "func",
                                           MIR_T_P, "args",
                                           MIR_T_I32, "argc",
                                           MIR_T_P, "locals",
                                           MIR_T_I32, "slot_idx");

  MIR_type_t truthy_ret = MIR_T_I64;
  c->truthy_proto = MIR_new_proto(c->ctx, "truthy_proto",
                                  1, &truthy_ret, 2,
                                  MIR_T_I64, "js",
                                  MIR_JSVAL, "v");

  MIR_type_t br_ret = MIR_JSVAL;
  c->resume_proto = MIR_new_proto(c->ctx, "resume_proto",
                                  1, &br_ret, 14,
                                  MIR_T_I64, "vm",
                                  MIR_T_P, "closure",
                                  MIR_JSVAL, "this_val",
                                  MIR_JSVAL, "new_target",
                                  MIR_JSVAL, "super_val",
                                  MIR_T_P, "args",
                                  MIR_T_I32, "argc",
                                  MIR_T_P, "vstack",
                                  MIR_T_I64, "vstack_sp",
                                  MIR_T_P, "params",
                                  MIR_T_I64, "n_params",
                                  MIR_T_P, "locals",
                                  MIR_T_I64, "n_locals",
                                  MIR_T_I64, "bc_offset");

  MIR_type_t cl_ret = MIR_JSVAL;
  c->closure_proto = MIR_new_proto(c->ctx, "closure_proto",
                                   1, &cl_ret, 11,
                                   MIR_T_I64, "vm",
                                   MIR_T_I64, "js",
                                   MIR_T_P, "parent",
                                   MIR_JSVAL, "this_val",
                                   MIR_T_P, "slots",
                                   MIR_T_I32, "slot_base",
                                   MIR_T_I32, "slot_count",
                                   MIR_T_I32, "const_idx",
                                   MIR_T_P, "name",
                                   MIR_T_I32, "name_len",
                                   MIR_T_P, "open_upvalues");

  c->close_upval_proto = MIR_new_proto(c->ctx, "close_upval_proto",
                                       0, NULL, 5,
                                       MIR_T_I64, "vm",
                                       MIR_T_I32, "slot_idx",
                                       MIR_T_P, "locals",
                                       MIR_T_I32, "n_locals",
                                       MIR_T_P, "open_upvalues");

  c->upval_barrier_proto = MIR_new_proto(c->ctx, "upval_barrier_proto",
                                         0, NULL, 3,
                                         MIR_T_I64, "js",
                                         MIR_T_P, "uv",
                                         MIR_T_I64, "val");

  c->adopt_open_upvalues_proto = MIR_new_proto(c->ctx, "adopt_open_upvalues_proto",
                                               0, NULL, 2,
                                               MIR_T_I64, "vm",
                                               MIR_T_P, "open_upvalues");

  c->take_open_upvalues_proto = MIR_new_proto(c->ctx, "take_open_upvalues_proto",
                                              0, NULL, 4,
                                              MIR_T_I64, "vm",
                                              MIR_T_P, "open_upvalues",
                                              MIR_T_P, "slots",
                                              MIR_T_I32, "slot_count");

  c->take_open_upvalues_rebase_proto = MIR_new_proto(c->ctx, "take_open_upvalues_rebase_proto",
                                                     0, NULL, 5,
                                                     MIR_T_I64, "vm",
                                                     MIR_T_P, "open_upvalues",
                                                     MIR_T_P, "src_slots",
                                                     MIR_T_P, "dst_slots",
                                                     MIR_T_I32, "slot_count");

  c->set_name_proto = MIR_new_proto(c->ctx, "sn_proto",
                                    0, NULL, 4,
                                    MIR_T_I64, "js",
                                    MIR_JSVAL, "fn",
                                    MIR_T_P, "str",
                                    MIR_T_I32, "len");

  MIR_type_t soe_ret = MIR_JSVAL;
  c->stack_ovf_err_proto = MIR_new_proto(c->ctx, "soe_proto",
                                         1, &soe_ret, 2,
                                         MIR_T_I64, "vm",
                                         MIR_T_I64, "js");

  MIR_type_t i64_ret = MIR_T_I64;
  c->promote_start_proto = MIR_new_proto(c->ctx, "promote_start_proto", 0, NULL, 1,
                                         MIR_T_P, "slot");
  c->promote_due_proto = MIR_new_proto(c->ctx, "promote_due_proto",
                                       1, &i64_ret, 1,
                                       MIR_T_P, "slot");

  if (c->cold_tier) c->cold_ns_item = MIR_new_bss(c->ctx, "cold_ns", 3 * sizeof(int64_t));

  MIR_type_t tier_ret = MIR_T_I64;
  c->tier_up_proto = MIR_new_proto(c->ctx, "tier_up_proto",
                                   1, &tier_ret, 3,
                                   MIR_T_I64, "js",
                                   MIR_T_I64, "func",
                                   MIR_T_P, "closure");

  c->define_field_proto = MIR_new_proto(c->ctx, "df_proto",
                                        0, NULL, 6,
                                        MIR_T_I64, "vm",
                                        MIR_T_I64, "js",
                                        MIR_JSVAL, "obj",
                                        MIR_JSVAL, "val",
                                        MIR_T_P, "str",
                                        MIR_T_I32, "len");

  c->define_slot_proto = MIR_new_proto(c->ctx, "ds_proto",
                                       0, NULL, 7,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js",
                                       MIR_JSVAL, "obj",
                                       MIR_JSVAL, "val",
                                       MIR_T_P, "str",
                                       MIR_T_I32, "len",
                                       MIR_T_I32, "slot");

  c->define_method_comp_proto = MIR_new_proto(c->ctx, "dmc_proto",
                                              0, NULL, 5,
                                              MIR_T_I64, "js",
                                              MIR_JSVAL, "obj",
                                              MIR_JSVAL, "key",
                                              MIR_JSVAL, "fn",
                                              MIR_T_I32, "flags");

  MIR_type_t pf_ret = MIR_JSVAL;
  c->put_field_proto = MIR_new_proto(c->ctx, "pf_proto",
                                     1, &pf_ret, 6,
                                     MIR_T_I64, "vm",
                                     MIR_T_I64, "js",
                                     MIR_JSVAL, "obj",
                                     MIR_JSVAL, "val",
                                     MIR_T_P, "atom",
                                     MIR_T_P, "ic");

  c->remember_obj_proto = MIR_new_proto(c->ctx, "remember_obj_proto",
                                        0, NULL, 2,
                                        MIR_T_I64, "js",
                                        MIR_T_P, "obj");

  c->shape_transition_proto = MIR_new_proto(c->ctx, "shape_transition_proto",
                                            0, NULL, 2,
                                            MIR_T_P, "obj",
                                            MIR_T_P, "to_shape");

  MIR_type_t pe_ret = MIR_JSVAL;
  c->put_elem_proto = MIR_new_proto(c->ctx, "pe_proto",
                                    1, &pe_ret, 5,
                                    MIR_T_I64, "vm",
                                    MIR_T_I64, "js",
                                    MIR_JSVAL, "obj",
                                    MIR_JSVAL, "key",
                                    MIR_JSVAL, "val");

  MIR_type_t pg_ret = MIR_JSVAL;
  c->put_global_proto = MIR_new_proto(c->ctx, "pg_proto",
                                      1, &pg_ret, 6,
                                      MIR_T_I64, "vm",
                                      MIR_T_I64, "js",
                                      MIR_JSVAL, "val",
                                      MIR_T_P, "str",
                                      MIR_T_I32, "len",
                                      MIR_T_I32, "strict");

  MIR_type_t peg_ret = MIR_JSVAL;
  c->put_eval_global_proto = MIR_new_proto(c->ctx, "peg_proto",
                                           1, &peg_ret, 6,
                                           MIR_T_I64, "js",
                                           MIR_T_P, "closure",
                                           MIR_JSVAL, "val",
                                           MIR_T_P, "str",
                                           MIR_T_I32, "len",
                                           MIR_T_I32, "strict");

  MIR_type_t dev_ret = MIR_JSVAL;
  c->delete_eval_var_proto = MIR_new_proto(c->ctx, "dev_proto",
                                           1, &dev_ret, 4,
                                           MIR_T_I64, "js",
                                           MIR_T_P, "closure",
                                           MIR_T_P, "str",
                                           MIR_T_I32, "len");

  MIR_type_t obj_ret = MIR_JSVAL;
  c->object_proto = MIR_new_proto(c->ctx, "obj_proto",
                                  1, &obj_ret, 4,
                                  MIR_T_I64, "vm",
                                  MIR_T_I64, "js",
                                  MIR_T_P, "func",
                                  MIR_T_P, "site");

  MIR_type_t arr_ret = MIR_JSVAL;
  c->array_proto = MIR_new_proto(c->ctx, "arr_proto",
                                 1, &arr_ret, 4,
                                 MIR_T_I64, "vm",
                                 MIR_T_I64, "js",
                                 MIR_T_P, "elements",
                                 MIR_T_I32, "count");

  MIR_type_t regexp_ret = MIR_JSVAL;
  c->regexp_proto = MIR_new_proto(c->ctx, "regexp_proto",
                                  1, &regexp_ret, 4,
                                  MIR_T_I64, "vm",
                                  MIR_T_I64, "js",
                                  MIR_JSVAL, "pattern",
                                  MIR_JSVAL, "flags");

  MIR_type_t te_ret = MIR_JSVAL;
  c->throw_error_proto = MIR_new_proto(c->ctx, "te_proto",
                                       1, &te_ret, 5,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js",
                                       MIR_T_P, "str",
                                       MIR_T_I32, "len",
                                       MIR_T_I32, "err_type");

  MIR_type_t nw_ret = MIR_JSVAL;
  c->new_proto = MIR_new_proto(c->ctx, "new_proto",
                               1, &nw_ret, 6,
                               MIR_T_I64, "vm",
                               MIR_T_I64, "js",
                               MIR_JSVAL, "func",
                               MIR_JSVAL, "new_target",
                               MIR_T_P, "args",
                               MIR_T_I32, "argc");

  MIR_type_t special_obj_ret = MIR_JSVAL;
  c->special_obj_proto = MIR_new_proto(c->ctx, "soj_proto",
                                       1, &special_obj_ret, 3,
                                       MIR_T_I64, "vm",
                                       MIR_T_I64, "js",
                                       MIR_T_I32, "which");

  c->strict_arguments_proto = MIR_new_proto(
      c->ctx, "strict_arguments_proto",
      1, &special_obj_ret,
      4,
      MIR_T_I64, "vm",
      MIR_T_I64, "js",
      MIR_T_P, "args",
      MIR_T_I32, "argc");

  c->forward_arguments_proto = MIR_new_proto(c->ctx, "forward_arguments_proto",
      1, &special_obj_ret, 7, MIR_T_I64, "vm", MIR_T_I64, "js",
      MIR_T_I64, "apply", MIR_T_I64, "target", MIR_T_I64, "receiver",
      MIR_T_P, "args", MIR_T_I32, "argc");
  c->imp_forward_arguments = MIR_new_import(c->ctx, "jit_helper_forward_arguments");

  MIR_type_t for_of_ret = MIR_JSVAL;
  c->for_of_proto = MIR_new_proto(c->ctx, "fo_proto",
                                  1, &for_of_ret, 4,
                                  MIR_T_I64, "vm",
                                  MIR_T_I64, "js",
                                  MIR_JSVAL, "iterable",
                                  MIR_T_P, "iter_buf");

  MIR_type_t iter_next_ret = MIR_JSVAL;
  c->iter_next_proto = MIR_new_proto(c->ctx, "inext_proto",
                                     1, &iter_next_ret, 4,
                                     MIR_T_I64, "vm",
                                     MIR_T_I64, "js",
                                     MIR_T_P, "iter_buf",
                                     MIR_T_I32, "hint");

  c->destructure_close_proto = MIR_new_proto(c->ctx, "dclose_proto",
                                             0, NULL, 3,
                                             MIR_T_I64, "vm",
                                             MIR_T_I64, "js",
                                             MIR_T_P, "iter_buf");

  MIR_type_t destructure_next_ret = MIR_JSVAL;
  c->destructure_next_proto = MIR_new_proto(c->ctx, "dnext_proto",
                                            1, &destructure_next_ret, 3,
                                            MIR_T_I64, "vm",
                                            MIR_T_I64, "js",
                                            MIR_T_P, "iter_buf");

  c->imp_add = MIR_new_import(c->ctx, "jit_helper_add");
  c->imp_sub = MIR_new_import(c->ctx, "jit_helper_sub");
  c->imp_mul = MIR_new_import(c->ctx, "jit_helper_mul");
  c->imp_div = MIR_new_import(c->ctx, "jit_helper_div");
  c->imp_mod = MIR_new_import(c->ctx, "jit_helper_mod");
  c->imp_str_read_value =
      MIR_new_import(c->ctx, "jit_helper_str_read_value");
  c->imp_str_append_local =
      MIR_new_import(c->ctx, "jit_helper_str_append_local");
  c->imp_str_append_local_snapshot =
      MIR_new_import(c->ctx, "jit_helper_str_append_local_snapshot");
  c->imp_str_flush_local =
      MIR_new_import(c->ctx, "jit_helper_str_flush_local");
  c->imp_lt = MIR_new_import(c->ctx, "jit_helper_lt");
  c->imp_le = MIR_new_import(c->ctx, "jit_helper_le");
  c->imp_gt = MIR_new_import(c->ctx, "jit_helper_gt");
  c->imp_ge = MIR_new_import(c->ctx, "jit_helper_ge");
  c->imp_call = MIR_new_import(c->ctx, "jit_helper_call");
  c->imp_call_method = MIR_new_import(c->ctx, "jit_helper_call_method");
  c->imp_call_array_includes = MIR_new_import(c->ctx, "jit_helper_call_array_includes");
  c->imp_call_char_code_at = MIR_new_import(c->ctx, "jit_helper_call_char_code_at");
  c->imp_call_string_intrinsic =
      MIR_new_import(c->ctx, "jit_helper_call_string_intrinsic");
  c->imp_call_map_template =
      MIR_new_import(c->ctx, "jit_helper_call_map_template");
  c->imp_map_template_fast =
      MIR_new_import(c->ctx, "jit_helper_map_template_fast");
  c->imp_map_numeric_pair_fast =
      MIR_new_import(c->ctx, "jit_helper_map_numeric_pair_fast");
  c->imp_regexp_exec_truthy =
      MIR_new_import(c->ctx, "jit_helper_regexp_exec_truthy");
  c->imp_load_stable_builtin = MIR_new_import(c->ctx, "jit_helper_load_stable_builtin");
  c->imp_call_stable_builtin = MIR_new_import(c->ctx, "jit_helper_call_stable_builtin");
  c->imp_apply = MIR_new_import(c->ctx, "jit_helper_apply");
  c->imp_call_call = MIR_new_import(c->ctx, "jit_helper_call_call");
  c->imp_call_call_slot = MIR_new_import(c->ctx, "jit_helper_call_call_slot");
  c->imp_rest = MIR_new_import(c->ctx, "jit_helper_rest");
  c->imp_special_obj = MIR_new_import(c->ctx, "jit_helper_special_obj");
  c->imp_strict_arguments =
      MIR_new_import(c->ctx, "jit_helper_strict_arguments");
  c->imp_for_of = MIR_new_import(c->ctx, "jit_helper_for_of");
  c->imp_iter_next = MIR_new_import(c->ctx, "jit_helper_iter_next");
  c->imp_dnext = MIR_new_import(c->ctx, "jit_helper_destructure_next");
  c->imp_dclose = MIR_new_import(c->ctx, "jit_helper_destructure_close");
  c->imp_gg = MIR_new_import(c->ctx, "jit_helper_get_global");
  c->imp_get_eval_global =
      MIR_new_import(c->ctx, "jit_helper_get_eval_global");
  c->imp_put_eval_global =
      MIR_new_import(c->ctx, "jit_helper_put_eval_global");
  c->imp_delete_eval_var =
      MIR_new_import(c->ctx, "jit_helper_delete_eval_var");
  c->imp_get_field = MIR_new_import(c->ctx, "jit_helper_get_field");
  c->imp_get_field_inline =
      MIR_new_import(c->ctx, "jit_helper_get_field_inline");
  c->imp_import_default = MIR_new_import(c->ctx, "jit_helper_import_default");
  c->imp_import_named = MIR_new_import(c->ctx, "jit_helper_import_named");
  c->imp_export = MIR_new_import(c->ctx, "jit_helper_export");
  c->imp_to_propkey = MIR_new_import(c->ctx, "jit_helper_to_propkey");
  c->imp_to_string = MIR_new_import(c->ctx, "js_template_to_string");
  c->imp_resume = MIR_new_import(c->ctx, "jit_helper_bailout_resume");
  c->imp_promote_resume = MIR_new_import(c->ctx, "jit_helper_promote_resume");
  c->imp_promote_start = MIR_new_import(c->ctx, "jit_helper_promote_start");
  c->imp_promote_due = MIR_new_import(c->ctx, "jit_helper_promote_due");
  c->imp_close_upval = MIR_new_import(c->ctx, "jit_helper_close_upval");
  c->imp_upval_barrier = MIR_new_import(c->ctx, "jit_helper_upval_barrier");
  c->imp_adopt_open_upvalues = MIR_new_import(c->ctx, "jit_helper_adopt_open_upvalues");
  c->imp_take_open_upvalues = MIR_new_import(c->ctx, "jit_helper_take_open_upvalues");
  c->imp_take_open_upvalues_rebase = MIR_new_import(c->ctx, "jit_helper_take_open_upvalues_rebase");
  c->imp_closure = MIR_new_import(c->ctx, "jit_helper_closure");
  c->imp_in = MIR_new_import(c->ctx, "jit_helper_in");
  c->imp_get_length = MIR_new_import(c->ctx, "jit_helper_get_length");
  c->imp_get_length_inline = MIR_new_import(c->ctx, "jit_helper_get_length_inline");
  c->imp_define_field = MIR_new_import(c->ctx, "jit_helper_define_field");
  c->imp_define_method_comp = MIR_new_import(c->ctx, "jit_helper_define_method_comp");
  c->imp_seq = MIR_new_import(c->ctx, "jit_helper_seq");
  c->imp_eq = MIR_new_import(c->ctx, "jit_helper_eq");
  c->imp_ne = MIR_new_import(c->ctx, "jit_helper_ne");
  c->imp_sne = MIR_new_import(c->ctx, "jit_helper_sne");
  c->imp_put_field = MIR_new_import(c->ctx, "jit_helper_put_field_ic");
  c->imp_shape_transition = MIR_new_import(c->ctx, "jit_helper_shape_transition");
  c->imp_remember_obj = MIR_new_import(c->ctx, "gc_remember_add");
  c->imp_get_elem = MIR_new_import(c->ctx, "jit_helper_get_elem");
  c->imp_put_elem = MIR_new_import(c->ctx, "jit_helper_put_elem");
  c->imp_get_private = MIR_new_import(c->ctx, "jit_helper_get_private");
  c->imp_put_private = MIR_new_import(c->ctx, "jit_helper_put_private");
  c->imp_put_global = MIR_new_import(c->ctx, "jit_helper_put_global");
  c->imp_object = MIR_new_import(c->ctx, "jit_helper_object");
  c->imp_object_template = MIR_new_import(c->ctx, "jit_helper_object_template");
  c->imp_regexp = MIR_new_import(c->ctx, "jit_helper_regexp");
  c->imp_define_slot = MIR_new_import(c->ctx, "jit_helper_define_slot");
  c->imp_array = MIR_new_import(c->ctx, "jit_helper_array");
  c->imp_catch_value = MIR_new_import(c->ctx, "jit_helper_catch_value");
  c->imp_throw = MIR_new_import(c->ctx, "jit_helper_throw");
  c->imp_throw_error = MIR_new_import(c->ctx, "jit_helper_throw_error");
  c->imp_set_proto = MIR_new_import(c->ctx, "jit_helper_set_proto");
  c->imp_get_elem2 = MIR_new_import(c->ctx, "jit_helper_get_elem2");
  c->imp_get_elem_inline = MIR_new_import(c->ctx, "jit_helper_get_elem_inline");
  c->imp_band = MIR_new_import(c->ctx, "jit_helper_band");
  c->imp_bor = MIR_new_import(c->ctx, "jit_helper_bor");
  c->imp_bxor = MIR_new_import(c->ctx, "jit_helper_bxor");
  c->imp_bnot = MIR_new_import(c->ctx, "jit_helper_bnot");
  c->imp_shl = MIR_new_import(c->ctx, "jit_helper_shl");
  c->imp_shr = MIR_new_import(c->ctx, "jit_helper_shr");
  c->imp_ushr = MIR_new_import(c->ctx, "jit_helper_ushr");
  c->imp_not = MIR_new_import(c->ctx, "jit_helper_not");
  c->imp_is_truthy = MIR_new_import(c->ctx, "jit_helper_is_truthy");
  c->imp_typeof = MIR_new_import(c->ctx, "jit_helper_typeof");
  c->imp_new = MIR_new_import(c->ctx, "jit_helper_new");
  c->imp_instanceof = MIR_new_import(c->ctx, "jit_helper_instanceof");
  c->imp_call_is_proto = MIR_new_import(c->ctx, "jit_helper_call_is_proto");
  c->imp_delete = MIR_new_import(c->ctx, "jit_helper_delete");
  c->imp_set_name = MIR_new_import(c->ctx, "jit_helper_set_name");
  c->imp_stack_ovf_err = MIR_new_import(c->ctx, "jit_helper_stack_overflow_error");
  c->imp_tier_up = MIR_new_import(c->ctx, "jit_helper_tier_up");
  c->imp_normalize_this = MIR_new_import(c->ctx, "jit_helper_normalize_sloppy_this");
}
