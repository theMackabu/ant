#include <stdlib.h>
#include <string.h>

#include "realm.h"
#include "cfunc.h"
#include "internal.h"
#include "handles.h"
#include "gc/modules.h"
#include "silver/call.h"

static void realm_save(ant_t *js, ant_realm_t *realm) {
  realm->gc_dirty = true;
  #define ANT_REALM_FIELD(name) realm->name = js->name;
  #include "realm/fields.h"

  #define ANT_REALM_SYM_FIELD(name) realm->sym.name = js->sym.name;
  #include "realm/sym_fields.h"
  memcpy(realm->sym.math_fns, js->sym.math_fns, sizeof(realm->sym.math_fns));

  #define ANT_BUILTIN(name)
  #define ANT_BUILTIN_ARR(name, n)
  #define ANT_REALM_BUILTIN(name) realm->builtins.name = js->builtins.name;
  #define ANT_REALM_BUILTIN_ARR(name, n) \
    memcpy(realm->builtins.name, js->builtins.name, sizeof(realm->builtins.name));
  #include "isolate_values.h"

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

static void realm_free(ant_t *js, ant_realm_t *realm) {
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
ant_value_t sv_invoke_builtin_in_realm(
  ant_t *js, const ant_cfunc_meta_t *meta, ant_value_t *args,
  int nargs, ant_value_t new_target
) {
  ant_realm_t *realm = ant_cfunc_meta_realm(meta);
  ant_realm_t *prev = ant_realm_enter(js, realm ? realm : ant_realm_main(js));
  ant_value_t result = sv_invoke_native(js, meta->fn, args, nargs, new_target);
  ant_realm_enter(js, prev);
  return result;
}

static ant_value_t realm_console_noop(ant_params_t) {
  return js_mkundef();
}

static ant_value_t realm_console_task_run(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "task.run requires a function");
  return sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, js_mkundef());
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
  mkprop(js, proto, js->sym.toStringTag_sym, ANT_STRING("console"), ANT_PROP_ATTR_CONFIGURABLE);

  ant_value_t console = Ant_Object_CreateBare(js);
  if (is_err(console)) { GC_ROOT_RESTORE(js, mark); return console; }
  GC_ROOT_PIN(js, console);
  js_set_proto_init(console, proto);

  ant_value_t noop = Ant_Function_Create(realm_console_noop);
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
  return js_mkundef();
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
