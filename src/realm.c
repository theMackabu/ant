#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "realm.h"
#include "cfunc.h"
#include "bootstrap.h"
#include "base64.h"
#include "arrays.h"
#include "internal.h"
#include "handles.h"
#include "gc/modules.h"
#include "silver/call.h"
#include "silver/jit.h"
#include "protectors.h"

#include "modules/symbol.h"
#include "modules/array.h"
#include "modules/boolean.h"
#include "modules/disposable.h"
#include "modules/function.h"
#include "modules/number.h"
#include "modules/proxy.h"
#include "modules/string.h"

static void realm_save_intrinsics(ant_t *js, ant_realm_t *realm) {
  realm->intrinsics_saved = true;
  
  #define ANT_REALM_SYM_FIELD(name) realm->sym.name = js->sym.name;
  #include "realm/sym_fields.h"
  memcpy(realm->sym.math_fns, js->sym.math_fns, sizeof(realm->sym.math_fns));

  #define ANT_BUILTIN(name)
  #define ANT_BUILTIN_ARR(name, n)
  #define ANT_REALM_BUILTIN(name) realm->builtins.name = js->builtins.name;
  #define ANT_REALM_BUILTIN_ARR(name, n) \
    memcpy(realm->builtins.name, js->builtins.name, sizeof(realm->builtins.name));
  #include "isolate_values.h"
}

static void realm_save(ant_t *js, ant_realm_t *realm) {
  realm->gc_dirty = true;
  if (!realm->intrinsics_saved) realm_save_intrinsics(js, realm);
  
  #define ANT_REALM_FIELD(name) realm->name = js->name;
  #include "realm/fields.h"
  
  realm->sym.eval_fn = js->sym.eval_fn;
  realm->builtins.callsite_proto = js->builtins.callsite_proto;

  #define ANT_MUTABLE_ROOT(name)
  #define ANT_MUTABLE_ROOT_ARR(name, n)
  #define ANT_REALM_MUTABLE_ROOT(name) realm->mutable_roots.name = js->mutable_roots.name;
  #define ANT_REALM_MUTABLE_ROOT_ARR(name, n) \
    memcpy(realm->mutable_roots.name, js->mutable_roots.name, sizeof(realm->mutable_roots.name));
  #include "isolate_values.h"
}

static void realm_load(ant_t *js, const ant_realm_t *realm) {
  #define ANT_REALM_FIELD(name) js->name = realm->name;
  #include "realm/fields.h"

  #define ANT_REALM_SYM_FIELD(name) js->sym.name = realm->sym.name;
  #include "realm/sym_fields.h"
  memcpy(js->sym.math_fns, realm->sym.math_fns, sizeof(js->sym.math_fns));

  #define ANT_BUILTIN(name)
  #define ANT_BUILTIN_ARR(name, n)
  #define ANT_REALM_BUILTIN(name) js->builtins.name = realm->builtins.name;
  #define ANT_REALM_BUILTIN_ARR(name, n) \
    memcpy(js->builtins.name, realm->builtins.name, sizeof(js->builtins.name));
  #include "isolate_values.h"

  #define ANT_MUTABLE_ROOT(name)
  #define ANT_MUTABLE_ROOT_ARR(name, n)
  #define ANT_REALM_MUTABLE_ROOT(name) js->mutable_roots.name = realm->mutable_roots.name;
  #define ANT_REALM_MUTABLE_ROOT_ARR(name, n) \
    memcpy(js->mutable_roots.name, realm->mutable_roots.name, sizeof(js->mutable_roots.name));
  #include "isolate_values.h"
}

static void realm_activate(ant_t *js, ant_realm_t *realm) {
  js->realm = realm;
  js->builtin_realm = realm->is_main ? NULL : realm;
  ant_cfunc_intern_realm = js->builtin_realm;
}

static const ant_realm_t realm_empty;
static ant_value_t realm_console_object(ant_t *js);

ant_realm_t *ant_realm_create(ant_t *js) {
  ant_realm_t *realm = calloc(1, sizeof(*realm));
  if (!realm) return NULL;

  ant_realm_t *prev = js->realm;
  bool was_single = !ant_realm_multiple(js);
  
  realm_save(js, prev);
  realm_load(js, &realm_empty);

  Ant_List_Insert(&js->realms, realm);
  realm_activate(js, realm);

  realm->initializing = true;
  bool ok = js_realm_init_intrinsics(js);
  realm->initializing = false;
  realm_save(js, realm);

  realm_load(js, prev);
  realm_activate(js, prev);
  
  if (ok && was_single && ant_realm_multiple(js) && js->jit_ctx) sv_jit_drop_all_code(js);
  if (ok) return realm;

  Ant_List_Remove(realm);
  free(realm->global_lexicals);
  free(realm->global_lexical_index);
  free(realm->iterators.entries);
  free(realm);
  
  return NULL;
}

ant_realm_t *ant_realm_enter(ant_t *js, ant_realm_t *realm) {
  ant_realm_t *prev = js->realm;
  if (realm == prev) return prev;
  
  realm_save(js, prev);
  realm_load(js, realm);
  realm_activate(js, realm);
  
  return prev;
}

static void realm_mark_map(ant_t *js, const ant_ptr_pair_map_t *map, void (*mark)(ant_t *js, ant_value_t value)) {
  if (!map->entries) return;
  for (uint32_t i = 0; i <= map->mask; i++) if (map->entries[i].key_a) mark(js, map->entries[i].value);
}

static void realm_mark_one(ant_t *js, ant_realm_t *realm, void (*mark)(ant_t *js, ant_value_t value)) {
  realm->gc_dirty = false;
  realm_mark_map(js, &realm->cfunc_promoted, mark);
  realm_mark_map(js, &realm->vm_scripts, mark);
  if (realm == js->realm) return;

  mark(js, realm->global);
  mark(js, realm->global_this);
  mark(js, realm->global_sandbox);
  for (uint32_t i = 0; i < realm->global_lexical_count; i++)
    mark(js, realm->global_lexicals[i].value);
  gc_mark_iterator_registry(js, &realm->iterators, mark);

  #define ANT_REALM_SYM_FIELD(name) mark(js, realm->sym.name);
  #include "realm/sym_fields.h"
  for (int i = 0; i < ANT_MATH_INTRINSIC_COUNT; i++) mark(js, realm->sym.math_fns[i]);

  #define ANT_BUILTIN(name)
  #define ANT_BUILTIN_ARR(name, n)
  #define ANT_REALM_BUILTIN(name) mark(js, realm->builtins.name);
  #define ANT_REALM_BUILTIN_ARR(name, n) \
    for (size_t i = 0; i < n; i++) mark(js, realm->builtins.name[i]);
  #include "isolate_values.h"

  #define ANT_MUTABLE_ROOT(name)
  #define ANT_MUTABLE_ROOT_ARR(name, n)
  #define ANT_REALM_MUTABLE_ROOT(name) mark(js, realm->mutable_roots.name);
  #define ANT_REALM_MUTABLE_ROOT_ARR(name, n) \
    for (size_t i = 0; i < n; i++) mark(js, realm->mutable_roots.name[i]);
  #include "isolate_values.h"
}

static inline bool realm_has_jobs(const ant_realm_t *realm) {
  return realm->microtasks || realm->microtasks_processing;
}

void ant_realm_mark(ant_t *js, void (*mark)(ant_t *js, ant_value_t value)) {
  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) {
    bool root = js->gc.minor ? realm->gc_dirty : realm->is_main || realm == js->realm || realm_has_jobs(realm);
    if (!root) continue;
    realm->gc_epoch = js->gc.epoch;
    realm_mark_one(js, realm, mark);
  }
}

void ant_realm_reach_slow(ant_t *js, ant_realm_t *realm, void (*mark)(ant_t *js, ant_value_t value)) {
  realm->gc_epoch = js->gc.epoch;
  realm_mark_one(js, realm, mark);
}

ant_realm_token_t *ant_realm_token_retain(ant_realm_t *realm) {
  if (!realm->token) {
    realm->token = calloc(1, sizeof(*realm->token));
    if (!realm->token) return NULL;
    *realm->token = (ant_realm_token_t){ .realm = realm, .refs = 1 };
  }
  
  realm->token->refs++;
  return realm->token;
}

void ant_realm_token_release(ant_realm_token_t *token) {
  if (token && --token->refs == 0) free(token);
}

static void realm_free(ant_t *js, ant_realm_t *realm) {
  if (realm->token) {
    realm->token->realm = NULL;
    ant_realm_token_release(realm->token);
  }
  
  if (realm != js->realm) {
    free(realm->global_lexicals);
    free(realm->global_lexical_index);
    free(realm->iterators.entries);
  }
  
  ant_cfunc_release_realm(realm);
  ant_ptr_pair_free(&realm->builtin_records);
  ant_ptr_pair_free(&realm->cfunc_promoted);
  ant_ptr_pair_free(&realm->cfunc_named);
  ant_ptr_pair_free(&realm->vm_scripts);
  
  free(realm);
}

void ant_realm_collect(ant_t *js) {
  ant_realm_t *next;
  for (ant_realm_t *realm = js->realms; realm; realm = next) {
    next = realm->next;
    if (realm->is_main || realm == js->realm || realm->gc_epoch == js->gc.epoch || realm_has_jobs(realm)) continue;
    Ant_List_Remove(realm);
    realm_free(js, realm);
  }
}

void ant_realm_destroy_all(ant_t *js) {
  ant_realm_t *realm = js->realms;
  
  while (realm) {
    ant_realm_t *next = realm->next;
    realm_free(js, realm);
    realm = next;
  }
  
  js->realms = NULL;
  js->main_realm = NULL;
  js->realm = NULL;
}

ant_realm_t *ant_realm_of_builtin(ant_t *js, ant_value_t builtin) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(builtin);
  ant_realm_t *realm = meta ? ant_cfunc_meta_realm(meta) : NULL;
  return realm ? realm : ant_realm_main(js);
}

ant_value_t ant_realm_function_proto(ant_t *js, ant_realm_t *realm) {
  return !realm || realm == js->realm ? js->sym.function_proto : realm->sym.function_proto;
}

bool ant_realm_foreign_array_ctor(ant_t *js, ant_value_t ctor) {
  if (!ant_realm_multiple(js)) return false;
  for (ant_realm_t *realm = js->realms; realm; realm = realm->next)
    if (realm != js->realm && ctor == realm->sym.array_ctor) return true;
  return false;
}

__attribute__((noinline, cold))
static bool realm_free_call(const ant_cfunc_meta_t *meta, ant_args_t) {
  if (!(meta->flags & CFUNC_REALM_FREE)) return false;
  
  for (int i = 0; i < nargs; i++) {
    uint8_t t = vtype(args[i]);
    if (!is_boxable_primitive_type(t) && t != kTypeNull && t != kTypeUndefined) return false;
  }
  
  return true;
}

ant_value_t sv_invoke_builtin_in_realm(
  ant_t *js, const ant_cfunc_meta_t *meta,
  ant_value_t this_val, ant_value_t func, ant_call_params_t
) {
  ant_realm_t *realm = ant_cfunc_meta_realm(meta);
  if (!realm) realm = ant_realm_main(js);
  
  if (realm_free_call(meta, args, nargs)) {
    ant_realm_t *prev_error = js->error_realm;
    js->error_realm = realm;
    ant_value_t result = sv_invoke_native(js, meta->fn, this_val, func, args, nargs, new_target);
    js->error_realm = prev_error;
    return result;
  }
  
  ant_realm_t *prev = ant_realm_enter(js, realm);
  ant_value_t result = sv_invoke_native(js, meta->fn, this_val, func, args, nargs, new_target);
  ant_realm_enter(js, prev);
  
  return result;
}

static ant_value_t realm_console_task_run(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "task.run requires a function");
  return sv_vm_call(js->vm, js, args[0], Ant_Value_Undefined, NULL, 0, NULL, Ant_Value_Undefined);
}

static ant_value_t realm_console_create_task(ant_params_t) {
  ant_value_t task = Ant_Object_CreateBare(js);
  if (is_err(task)) return task;
  js_set_proto_init(task, js->sym.object_proto);
  js_set(js, task, "run", Ant_Function_Create(realm_console_task_run));
  return task;
}

static ant_value_t realm_console_context(ant_params_t) {
  return realm_console_object(js);
}

static ant_value_t realm_console_object(ant_t *js) {
  static const char *const noops[] = {
    "assert", "clear", "count", "countReset", "debug", "dir", "dirxml", "error",
    "group", "groupCollapsed", "groupEnd", "info", "log", "profile", "profileEnd",
    "table", "time", "timeEnd", "timeLog", "timeStamp", "trace", "warn",
  };

  GC_ROOT_SAVE(mark, js);
  ant_value_t proto = Ant_Object_CreateBare(js);
  if (is_err(proto)) { GC_ROOT_RESTORE(js, mark); return proto; }
  GC_ROOT_PIN(js, proto);
  js_set_proto_init(proto, js->sym.object_proto);
  mkprop(js, proto, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "console"), ANT_PROP_ATTR_CONFIGURABLE);

  ant_value_t console = Ant_Object_CreateBare(js);
  if (is_err(console)) { GC_ROOT_RESTORE(js, mark); return console; }
  GC_ROOT_PIN(js, console);
  js_set_proto_init(console, proto);

  ant_value_t noop = Ant_Function_Create(js_cfunc_noop);
  for (size_t i = 0; i < sizeof(noops) / sizeof(*noops); i++) js_set(js, console, noops[i], noop);
  js_set(js, console, "context", Ant_Function_Create(realm_console_context));
  js_set(js, console, "createTask", Ant_Function_Create(realm_console_create_task));

  GC_ROOT_RESTORE(js, mark);
  return console;
}

void ant_realm_install_console(ant_t *js) {
  ant_value_t console = realm_console_object(js);
  if (is_err(console)) return;
  js_set_global_builtin(js, "console", console);
}

static ant_value_t realm_handle_anchor(ant_params_t) {
  return Ant_Value_Undefined;
}

static ant_realm_t *realm_from_handle(ant_t *js, ant_value_t handle) {
  if (vtype(handle) != kTypeObject) return NULL;
  ant_value_t anchor = js_get_slot(handle, SLOT_DATA);
  return vtype(anchor) == kTypeBuiltin ? ant_cfunc_meta_realm(js_as_cfunc_meta(anchor)) : NULL;
}

static ant_value_t realm_lib_create(ant_params_t) {
  ant_realm_t *realm = ant_realm_create(js);
  if (!realm) return js_mkerr(js, "could not create realm");
  
  ant_realm_t *prev = ant_realm_enter(js, realm);
  ant_value_t anchor = Ant_Function_Create(realm_handle_anchor);
  ant_realm_enter(js, prev);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, anchor);

  ant_value_t handle = Ant_Object_CreateBare(js);
  if (is_err(handle)) {
    GC_ROOT_RESTORE(js, mark);
    return handle;
  }

  js_set_slot(handle, SLOT_DATA, anchor);
  js_set(js, handle, "global", realm->global);
  GC_ROOT_RESTORE(js, mark);
  
  return handle;
}

static ant_value_t realm_lib_evaluate(ant_params_t) {
  ant_realm_t *realm = nargs > 0 ? realm_from_handle(js, args[0]) : NULL;
  if (!realm) return js_mkerr_typed(js, JS_ERR_TYPE, "evaluate requires a realm handle");
  if (nargs < 2 || vtype(args[1]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "evaluate requires source text");

  size_t len = 0;
  const char *source = js_getstr(js, args[1], &len);
  
  ant_realm_t *prev = ant_realm_enter(js, realm);
  js_eval_result_t result = js_eval_bytecode_repl(js, source, len);
  ant_realm_enter(js, prev);
  
  return result.value;
}

ant_value_t realm_internal_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  js_set(js, lib, "create", Ant_Function_Create(realm_lib_create));
  js_set(js, lib, "evaluate", Ant_Function_Create(realm_lib_evaluate));
  return lib;
}

void realm_init_core(ant_t *js) {
  js->global = mkobj(js, 0);
  js->global_this = js->global;
  js->global_sandbox = Ant_Value_Undefined;

  ant_value_t glob = js->global;
  ant_value_t object_proto = Ant_Object_CreateBare(js);
  set_proto(js, object_proto, Ant_Value_Null);
  
  js_object_init_prototype(js, object_proto);
  
  ant_value_t function_proto_obj = Ant_Object_CreateBare(js);
  set_proto(js, function_proto_obj, object_proto);
  js_function_init_prototype(js, function_proto_obj);
  
  ant_value_t function_proto = js_obj_to_func(js, function_proto_obj);
  set_slot(glob, SLOT_FUNC_PROTO, function_proto);
  js_define_function_length(js, function_proto_obj, 0);
  js_set_function_name(js, function_proto, "", 0);
  
  ant_value_t array_proto = alloc_array_with_proto_capacity(js, object_proto, MAX_DENSE_INITIAL_CAP, 0, false);
  js_array_init_prototype(js, array_proto);
  
  ant_value_t string_proto = Ant_Object_CreateBare(js);
  set_proto(js, string_proto, object_proto);
  js_string_init_prototype(js, string_proto);

  ant_value_t number_proto = Ant_Object_CreateBare(js);
  set_proto(js, number_proto, object_proto);
  js_number_init_prototype(js, number_proto);
  
  ant_value_t boolean_proto = Ant_Object_CreateBare(js);
  set_proto(js, boolean_proto, object_proto);
  js_boolean_init_prototype(js, boolean_proto);
  
  js_errors_init(js, object_proto, function_proto);

  js_disposable_stack_init(js, object_proto, function_proto);
  
  ant_value_t promise_proto = Ant_Object_CreateBare(js);
  set_proto(js, promise_proto, object_proto);
  js_promise_init_prototype(js, promise_proto);
  
  ant_value_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  js_object_init_constructor(js, obj_func_obj, object_proto);
  ant_value_t obj_func = js_obj_to_func(js, obj_func_obj);
  js_set_global_builtin(js, "Object", obj_func);
  
  ant_value_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  js_function_init_constructor(js, func_ctor_obj, function_proto);
  ant_value_t func_ctor_func = js_obj_to_func(js, func_ctor_obj);
  js_set_global_builtin(js, "Function", func_ctor_func);
  
  js_function_init_kinds(js, function_proto, func_ctor_func);
  
  ant_value_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  js_string_init_constructor(js, str_ctor_obj, string_proto);
  
  ant_value_t str_ctor_func = js_obj_to_func(js, str_ctor_obj);
  js->sym.string_ctor = str_ctor_func;
  js_set_global_builtin(js, "String", str_ctor_func);
  
  ant_value_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  
  ant_value_t number_parse_int = Ant_Value_Undefined;
  ant_value_t number_parse_float = Ant_Value_Undefined;
  js_number_init_constructor(js, number_ctor_obj, number_proto, &number_parse_int, &number_parse_float);
  ant_value_t number_ctor_func = js_obj_to_func(js, number_ctor_obj);
  js_set_global_builtin(js, "Number", number_ctor_func);
  
  ant_value_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  js_boolean_init_constructor(js, bool_ctor_obj, boolean_proto);
  ant_value_t bool_ctor_func = js_obj_to_func(js, bool_ctor_obj);
  js_set_global_builtin(js, "Boolean", bool_ctor_func);
  
  ant_value_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  js_array_init_constructor(js, arr_ctor_obj, array_proto);
  
  ant_value_t arr_ctor_func = js_obj_to_func(js, arr_ctor_obj);
  js->sym.array_ctor = arr_ctor_func;
  js_set_global_builtin(js, "Array", arr_ctor_func);
  
  ant_value_t proxy_ctor_obj = mkobj(js, 0);
  set_proto(js, proxy_ctor_obj, function_proto);
  js_proxy_init_constructor(js, proxy_ctor_obj);
  js_set_global_builtin(js, "Proxy", js_obj_to_func(js, proxy_ctor_obj));
  
  ant_value_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  js_promise_init_constructor(js, p_ctor_obj);
  
  js_setprop_readonly_nonconfigurable(js, p_ctor_obj, "prototype", 9, promise_proto);
  ant_value_t p_ctor_func = js_obj_to_func(js, p_ctor_obj);
  defhidden(js, promise_proto, "constructor", 11, p_ctor_func);
  js_set_function_name(js, p_ctor_obj, "Promise", sizeof("Promise") - 1);
  js_set_global_builtin(js, "Promise", p_ctor_func);
  
  defalias(js, glob, "parseInt", 8, number_parse_int);
  defalias(js, glob, "parseFloat", 10, number_parse_float);
  defmethod(js, glob, "eval", 4, js_builtin_eval(js));
  js_number_init_globals(js, glob);
  js_base64_init_globals(js, glob);
  
  js_setprop(js, glob, js_mkstr(js, "NaN", 3), Ant_Value_FromNumber(ANT_NAN));
  js_set_descriptor(js, glob, "NaN", 3, 0);
  js_setprop(js, glob, js_mkstr(js, "Infinity", 8), Ant_Value_FromNumber(ANT_INFINITY));
  js_set_descriptor(js, glob, "Infinity", 8, 0);
  js_setprop(js, glob, js_mkstr(js, "undefined", 9), Ant_Value_Undefined);
  js_set_descriptor(js, glob, "undefined", 9, 0);
  
  ant_value_t import_obj = mkobj(js, 0);
  set_proto(js, import_obj, function_proto);
  
  set_slot(import_obj, SLOT_CFUNC, Ant_Function_Create(js_builtin_import));
  js_set_global_builtin(js, "import", js_obj_to_func(js, import_obj));
  
  defhidden(js, object_proto, "constructor", 11, obj_func);
  defhidden(js, function_proto, "constructor", 11, func_ctor_func);
  defhidden(js, array_proto, "constructor", 11, arr_ctor_func);
  defhidden(js, string_proto, "constructor", 11, str_ctor_func);
  defhidden(js, number_proto, "constructor", 11, number_ctor_func);
  defhidden(js, boolean_proto, "constructor", 11, bool_ctor_func);
  set_proto(js, glob, object_proto);
  
  js->sym.object_proto = object_proto;
  js->sym.array_proto = array_proto;
  js->sym.function_proto = function_proto;
  js->sym.string_proto = string_proto;
  js->sym.number_proto = number_proto;
  js->sym.boolean_proto = boolean_proto;
  js->sym.promise_ctor = p_ctor_func;
  js->sym.promise_proto = promise_proto;
  
  ant_protector_rearm(js, ANT_PROTECTOR_PROMISE_RESOLVE_LOOKUP);

  ant_protector_holder_mark(glob);
  ant_protector_holder_mark(object_proto);
  ant_protector_holder_mark(array_proto);
  ant_protector_holder_mark(js->sym.array_ctor);
  ant_protector_holder_mark(p_ctor_func);
  ant_protector_holder_mark(promise_proto);
  ant_protector_holder_mark(string_proto);
  ant_iterator_protector_holder_mark(object_proto);
}

void init_intrinsic_symbols(ant_t *js) {
  mkprop(js, js->global, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "global"), ANT_PROP_ATTR_CONFIGURABLE);
  
  mkprop(
    js, js->sym.array_proto, js->sym.iterator_sym, 
    js->sym.array_values_fn, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);

  ant_value_t unscopables = Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, unscopables);
  
  js_set(js, unscopables, "at", Ant_Value_True);
  js_set(js, unscopables, "copyWithin", Ant_Value_True);
  js_set(js, unscopables, "entries", Ant_Value_True);
  js_set(js, unscopables, "fill", Ant_Value_True);
  js_set(js, unscopables, "find", Ant_Value_True);
  js_set(js, unscopables, "findIndex", Ant_Value_True);
  js_set(js, unscopables, "findLast", Ant_Value_True);
  js_set(js, unscopables, "findLastIndex", Ant_Value_True);
  js_set(js, unscopables, "flat", Ant_Value_True);
  js_set(js, unscopables, "flatMap", Ant_Value_True);
  js_set(js, unscopables, "includes", Ant_Value_True);
  js_set(js, unscopables, "keys", Ant_Value_True);
  js_set(js, unscopables, "toReversed", Ant_Value_True);
  js_set(js, unscopables, "toSorted", Ant_Value_True);
  js_set(js, unscopables, "toSpliced", Ant_Value_True);
  js_set(js, unscopables, "values", Ant_Value_True);
  
  mkprop(js, js->sym.array_proto, js->sym.unscopables_sym, unscopables, ANT_PROP_ATTR_CONFIGURABLE);
  GC_ROOT_RESTORE(js, mark);

  mkprop(js, js->sym.promise_proto, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "Promise"), ANT_PROP_ATTR_CONFIGURABLE);
  ant_value_t async_proto = js_get_slot(js->global, SLOT_ASYNC_PROTO);
  
  mkprop(js, async_proto, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "AsyncFunction"), ANT_PROP_ATTR_CONFIGURABLE);
  js_define_species_getter(js, js->sym.promise_ctor);
  js_define_species_getter(js, js_get(js, js->global, "Array"));
  
  defalias_sym_of(js, js->builtins.disposable_stack_proto, js->sym.dispose_sym, "dispose");
  defalias_sym_of(js, js->builtins.async_disposable_stack_proto, js->sym.asyncDispose_sym, "disposeAsync");
  
  ant_protector_rearm(js,
    ANT_PROTECTOR_PROMISE_SPECIES | ANT_PROTECTOR_ARRAY_SPECIES | 
    ANT_PROTECTOR_ARRAY_ITERATION | ANT_PROTECTOR_ARRAY_ASYNC_ITERATION | ANT_PROTECTOR_OBJECT_THEN |
    ANT_PROTECTOR_STRING_ITERATION | ANT_PROTECTOR_COLLECTION_ITERATION |
    ANT_PROTECTOR_ITERATOR_RETURN | ANT_PROTECTOR_CONCAT_SPREADABLE);
}

bool js_realm_finish_intrinsics(ant_t *js) {
  js_set_global_builtin(js, "globalThis", js->global);
  
  static const char *const not_ecmascript[] = { "Buffer", "atob", "btoa", "AsyncIterator", "InternalError", "import" };
  for (size_t i = 0; i < sizeof(not_ecmascript) / sizeof(*not_ecmascript); i++)
    js_delete_prop_ordinary(js, js->global, not_ecmascript[i], strlen(not_ecmascript[i]));
    
  ant_realm_install_console(js);
  return !Ant_Exception_Pending(js);
}

bool js_realm_init_intrinsics(ant_t *js) {
  realm_init_core(js);
  ant_bootstrap_realm_intrinsics(js);
  return js_realm_finish_intrinsics(js);
}
