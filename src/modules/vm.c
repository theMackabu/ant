#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <uv.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "cfunc.h"
#include "internal.h"
#include "interrupt.h"
#include "realm.h"
#include "silver/engine.h"
#include "silver/call.h"
#include "silver/code_unit.h"

#include "modules/vm.h"
#include "modules/timer.h"
#include "modules/buffer.h"
#include "modules/collections.h"

typedef struct {
  const char *filename;
  int32_t line_offset;
  int32_t column_offset;
  uint64_t id;
  ant_realm_token_t **realms;
  uint32_t realm_count;
  uint32_t realm_cap;
} vm_script_t;

static _Atomic uint64_t vm_next_script_id = 1;
static constexpr uint32_t VM_SCRIPT_NATIVE_TAG = Ant_NativeTag("VMSC");
static const char vm_default_filename[] = "evalmachine.<anonymous>";

static ant_value_t vm_context_anchor(ant_params_t) {
  return js_mkundef();
}

static ant_realm_t *vm_context_realm(ant_value_t object) {
  if (!is_object_type(object)) return NULL;
  ant_value_t anchor = js_get_slot(object, SLOT_VM_CONTEXT);
  return vtype(anchor) == kTypeBuiltin ? ant_cfunc_meta_realm(js_as_cfunc_meta(anchor)) : NULL;
}

static ant_value_t vm_key_get(ant_t *js, ant_value_t object, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) return js_get_sym(js, object, key);
  return js_getprop_fallback(js, object, js_getstr(js, key, NULL));
}

static ant_value_t vm_key_delete(ant_t *js, ant_value_t object, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) return js_delete_sym_prop(js, object, key);
  size_t len = 0;
  const char *name = js_getstr(js, key, &len);
  return js_delete_prop(js, object, name, len);
}

static bool vm_desc_configurable(ant_t *js, ant_value_t desc) {
  return js_truthy(js, js_get(js, desc, "configurable"));
}

#define VM_TRAP_PRELUDE                                    \
  ant_value_t target = nargs > 0 ? args[0] : js_mkundef(); \
  ant_value_t key = nargs > 1 ? args[1] : js_mkundef();    \
  ant_value_t sandbox = js_get_slot(target, SLOT_VM_SANDBOX)

static ant_value_t vm_global_get(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t in_sandbox = do_in(js, key, sandbox);
  if (is_err(in_sandbox)) return in_sandbox;
  return vm_key_get(js, js_truthy(js, in_sandbox) ? sandbox : target, key);
}

static ant_value_t vm_global_set(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t value = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t own = js_get_own_property_descriptor(js, target, key);
  if (is_err(own)) return own;

  bool read_only = is_object_type(own) &&
    !js_truthy(js, js_get(js, own, "writable")) &&
    vtype(js_get(js, own, "set")) == kTypeUndefined;
  return js_set_reporting(js, read_only ? target : sandbox, key, value);
}

static ant_value_t vm_global_has(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t in_sandbox = do_in(js, key, sandbox);
  if (is_err(in_sandbox) || js_truthy(js, in_sandbox)) return in_sandbox;
  return do_in(js, key, target);
}

static ant_value_t vm_global_delete(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t deleted = vm_key_delete(js, sandbox, key);
  if (is_err(deleted) || !js_truthy(js, deleted)) return deleted;
  return vm_key_delete(js, target, key);
}

static ant_value_t vm_global_define(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t desc = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t defined = js_define_property(js, sandbox, key, desc, true);
  if (is_err(defined) || !js_truthy(js, defined)) return defined;

  bool non_configurable = is_object_type(desc) &&
    vtype(js_get(js, desc, "configurable")) != kTypeUndefined &&
    !js_truthy(js, js_get(js, desc, "configurable"));
  if (non_configurable) return js_define_property(js, target, key, desc, true);

  return defined;
}

static ant_value_t vm_global_own_desc(ant_params_t) {
  VM_TRAP_PRELUDE;
  ant_value_t own = js_get_own_property_descriptor(js, target, key);
  if (is_err(own)) return own;
  if (is_object_type(own) && !vm_desc_configurable(js, own)) return own;

  ant_value_t desc = js_get_own_property_descriptor(js, sandbox, key);
  if (is_err(desc) || !is_object_type(desc)) return is_err(desc) ? desc : own;

  if (!vm_desc_configurable(js, desc)) js_set(js, desc, "configurable", js_true);
  return desc;
}

static ant_value_t vm_global_own_keys(ant_params_t) {
  VM_TRAP_PRELUDE;
  (void)key;
  
  ant_value_t keys = js_own_property_keys(js, sandbox, true, false);
  if (is_err(keys)) return keys;

  ant_value_t global_keys = js_own_property_keys(js, target, true, false);
  if (is_err(global_keys)) return global_keys;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, keys);
  GC_ROOT_PIN(js, global_keys);

  ant_offset_t count = js_arr_len(js, global_keys);
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t global_key = js_arr_get(js, global_keys, i);
    ant_value_t shadow = js_get_own_property_descriptor(js, sandbox, global_key);
    if (is_err(shadow)) {
      GC_ROOT_RESTORE(js, mark);
      return shadow;
    }
    if (!is_object_type(shadow)) js_arr_push(js, keys, global_key);
  }

  GC_ROOT_RESTORE(js, mark);
  return keys;
}

static ant_value_t vm_global_prevent_extensions(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot freeze");
}

#undef VM_TRAP_PRELUDE

typedef struct {
  bool microtasks_after_evaluate;
  ant_codegen_flags_t codegen_blocked;
} vm_context_options_t;

static ant_value_t vm_context_create(ant_t *js, ant_value_t sandbox, const vm_context_options_t *options) {
  ant_realm_t *realm = ant_realm_create(js);
  if (!realm) return js_mkerr(js, "Failed to create a vm context");
  if (options->microtasks_after_evaluate) realm->microtask_realm = realm;
  realm->codegen_blocked = options->codegen_blocked;

  GC_ROOT_SAVE(mark, js);
  ant_realm_t *prev = ant_realm_enter(js, realm);
  ant_value_t anchor = Ant_Function_Create(vm_context_anchor);
  ant_value_t global = js->global;
  
  if (!is_object_type(sandbox)) {
    js_set_slot(global, SLOT_VM_CONTEXT, anchor);
    ant_realm_enter(js, prev);
    GC_ROOT_RESTORE(js, mark);
    return global;
  }

  js_set_slot(global, SLOT_VM_SANDBOX, sandbox);
  js_obj_ptr(global)->flags.is_exotic = 1;
  ant_ic_epoch_bump(js);

  ant_value_t handler = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, handler);
  js_set(js, handler, "get", Ant_Function_Create(vm_global_get));
  js_set(js, handler, "set", Ant_Function_Create(vm_global_set));
  js_set(js, handler, "has", Ant_Function_Create(vm_global_has));
  js_set(js, handler, "deleteProperty", Ant_Function_Create(vm_global_delete));
  js_set(js, handler, "defineProperty", Ant_Function_Create(vm_global_define));
  js_set(js, handler, "getOwnPropertyDescriptor", Ant_Function_Create(vm_global_own_desc));
  js_set(js, handler, "ownKeys", Ant_Function_Create(vm_global_own_keys));
  js_set(js, handler, "preventExtensions", Ant_Function_Create(vm_global_prevent_extensions));

  ant_value_t proxy = js_mkproxy(js, global, handler);
  if (!is_err(proxy)) {
    js->global_sandbox = sandbox;
    js->global_this = proxy;
    js->promise_resolve_lookup_protector_invalid = true;
    js_set_global_builtin(js, "globalThis", proxy);
  }

  ant_realm_enter(js, prev);
  GC_ROOT_RESTORE(js, mark);
  if (is_err(proxy)) return proxy;

  js_set_slot(sandbox, SLOT_VM_CONTEXT, anchor);
  return sandbox;
}

static bool vm_option_bool(ant_t *js, ant_value_t options, const char *name) {
  return is_object_type(options) && js_truthy(js, js_get(js, options, name));
}

static ant_value_t vm_option_string(ant_t *js, ant_value_t options, const char *name) {
  ant_value_t value = js_get(js, options, name);
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeString) return js_mkundef();
  char arg[64];
  snprintf(arg, sizeof(arg), "options.%s", name);
  return Ant_Error_CreateInvalidArgType(js, arg, "of type string", value);
}

static ant_value_t vm_import_option(ant_t *js, ant_value_t options, ant_value_t *out) {
  *out = js_mknull();
  if (!is_object_type(options)) return js_mkundef();
  
  ant_value_t value = js_get(js, options, "importModuleDynamically");
  if (vtype(value) == kTypeUndefined) return js_mkundef();
  if (!is_callable(value) && value != js->builtins.vm_main_context_loader)
    return Ant_Error_CreateInvalidArgType(js, "options.importModuleDynamically", "of type function", value);
  
  *out = value;
  return js_mkundef();
}

static ant_value_t vm_context_options(ant_t *js, ant_value_t options, bool prefixed, vm_context_options_t *out) {
  *out = (vm_context_options_t){0};
  if (!is_object_type(options)) return js_mkundef();

  ant_value_t checked = vm_option_string(js, options, prefixed ? "contextName" : "name");
  if (is_err(checked)) return checked;
  
  checked = vm_option_string(js, options, prefixed ? "contextOrigin" : "origin");
  if (is_err(checked)) return checked;

  const char *codegen_name = prefixed ? "contextCodeGeneration" : "codeGeneration";
  ant_value_t codegen = js_get(js, options, codegen_name);
  
  if (vtype(codegen) != kTypeUndefined) {
    char arg[64];
    snprintf(arg, sizeof(arg), "options.%s", codegen_name);
    if (!is_object_type(codegen)) return Ant_Error_CreateInvalidArgType(js, arg, "of type object", codegen);

    static const struct { const char *name; ant_codegen_flags_t flag; } kinds[] = {
      { "strings", ANT_CODEGEN_NO_STRINGS },
      { "wasm",    ANT_CODEGEN_NO_WASM },
    };
    
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
      ant_value_t allowed = js_get(js, codegen, kinds[i].name);
      if (vtype(allowed) == kTypeUndefined) continue;
      
      if (vtype(allowed) != kTypeBool) {
        char field[80];
        snprintf(field, sizeof(field), "%s.%s", arg, kinds[i].name);
        return Ant_Error_CreateInvalidArgType(js, field, "of type boolean", allowed);
      }
      
      if (!js_truthy(js, allowed)) out->codegen_blocked |= kinds[i].flag;
    }
  }

  ant_value_t mode = js_get(js, options, "microtaskMode");
  if (vtype(mode) != kTypeUndefined) {
    size_t len = 0;
    const char *text = vtype(mode) == kTypeString ? js_getstr(js, mode, &len) : NULL;
    
    static const char after_evaluate[] = "afterEvaluate";
    if (!text || len != sizeof(after_evaluate) - 1 || memcmp(text, after_evaluate, len) != 0)
      return Ant_Error_CreateInvalidArgValue(js, "options.microtaskMode", "'afterEvaluate', undefined", mode);
    
    out->microtasks_after_evaluate = true;
  }

  ant_value_t ignored;
  return prefixed ? js_mkundef() : vm_import_option(js, options, &ignored);
}

static ant_value_t vm_context_for(ant_t *js, ant_value_t object, ant_value_t options, bool prefixed) {
  if (vtype(object) == kTypeUndefined) object = Ant_Object_CreateBare(js);
  bool dont_contextify = object == js->builtins.vm_dont_contextify;

  if (!dont_contextify && (!is_object_type(object) || vtype(object) == kTypeFunction))
    return Ant_Error_CreateInvalidArgType(js, "object", "of type object", object);
  
  if (!dont_contextify && vm_context_realm(object)) return object;
  
  if (vtype(options) != kTypeUndefined && !is_object_type(options))
    return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);

  vm_context_options_t settings;
  ant_value_t checked = vm_context_options(js, options, prefixed, &settings);
  if (is_err(checked)) return checked;

  return vm_context_create(js, dont_contextify ? js_mkundef() : object, &settings);
}

static ant_value_t vm_create_context(ant_params_t) {
  return vm_context_for(js, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef(), false);
}

static ant_value_t vm_is_context(ant_params_t) {
  ant_value_t object = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(object)) return Ant_Error_CreateInvalidArgType(js, "object", "of type object", object);
  return js_bool(vm_context_realm(object) != NULL);
}

static vm_script_t *vm_script_state(ant_value_t script) {
  return (vm_script_t *)js_get_native(script, VM_SCRIPT_NATIVE_TAG);
}

static void vm_script_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  vm_script_t *st = vm_script_state(value);
  
  if (st) {
    for (uint32_t i = 0; i < st->realm_count; i++) {
      ant_realm_token_t *token = st->realms[i];
      if (token->realm) ant_ptr_pair_remove(&token->realm->vm_scripts, (uintptr_t)st->id, 0);
      ant_realm_token_release(token);
    }
    
    free(st->realms);
    free(st);
  }
  
  js_clear_native(value, VM_SCRIPT_NATIVE_TAG);
}

static void vm_offset_positions(sv_func_t *func, int32_t lines, int32_t columns) {
  if (func->debug) {
    for (int i = 0; i < func->debug->srcpos_count; i++) {
      sv_srcpos_t *pos = &func->debug->srcpos[i];
      if (pos->line == 1 && (int64_t)pos->col + columns > 0) pos->col += (uint32_t)columns;
      if ((int64_t)pos->line + lines > 0) pos->line += (uint32_t)lines;
    }
    
    if (func->debug->source_line + lines > 0) func->debug->source_line += lines;
  }
  
  for (int i = 0; i < func->child_func_count; i++)
    vm_offset_positions(func->child_funcs[i], lines, columns);
}

static void vm_set_import(ant_t *js, sv_func_t *func, ant_value_t handler, ant_value_t referrer) {
  if (!func || !func->unit) return;
  sv_code_unit_retain_template(js, func, handler);
  sv_code_unit_retain_template(js, func, referrer);
  func->unit->import_handler = handler;
  func->unit->import_referrer = referrer;
}

static ant_value_t vm_import_referrer(sv_code_unit_t *unit) {
  ant_value_t script;
  return js_weakref_target(unit->import_referrer, &script) ? script : unit->import_referrer;
}

ant_value_t js_vm_dynamic_import(ant_t *js, sv_code_unit_t *unit, ant_value_t specifier, ant_value_t options) {
  ant_value_t handler = unit->import_handler;
  
  if (handler == js->builtins.vm_main_context_loader) {
    ant_realm_t *main = ant_realm_main(js);
    ant_value_t global = main == js->realm ? js->global : main->global;
    ant_value_t import_fn = js_getprop_fallback(js, global, "import");
    ant_value_t argv[2] = { specifier, options };
    return sv_vm_call(js->vm, js, import_fn, global, argv, 2, NULL, js_mkundef());
  }

  GC_ROOT_SAVE(mark, js);
  ant_value_t promise = js_mkpromise(js);
  
  GC_ROOT_PIN(js, promise);
  ant_value_t outcome;

  if (is_callable(handler)) {
    ant_value_t name = js_tostring_val(js, specifier);
    ant_value_t attributes = is_object_type(options) ? js_get(js, options, "with") : js_mkundef();
    if (!is_object_type(attributes)) attributes = Ant_Object_CreateBare(js);
    
    ant_value_t argv[3] = { name, vm_import_referrer(unit), attributes };
    outcome = is_err(name) ? name : sv_vm_call(js->vm, js, handler, js_mkundef(), argv, 3, NULL, js_mkundef());
  } else outcome = js_mkerr_code(
    js, JS_ERR_TYPE, "ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING",
    "A dynamic import callback was not specified."
  );

  if (is_err(outcome)) js_reject_promise(js, promise, js_take_thrown(js, outcome));
  else js_resolve_promise(js, promise, outcome);
  
  GC_ROOT_RESTORE(js, mark);
  return promise;
}

static bool vm_script_note_realm(vm_script_t *st, ant_realm_t *realm) {
  if (st->realm_count == st->realm_cap) {
    uint32_t cap = st->realm_cap ? st->realm_cap * 2 : 2;
    ant_realm_token_t **realms = realloc(st->realms, cap * sizeof(*realms));
    if (!realms) return false;
    st->realms = realms;
    st->realm_cap = cap;
  }
  
  ant_realm_token_t *token = ant_realm_token_retain(realm);
  if (!token) return false;
  
  st->realms[st->realm_count++] = token;
  return true;
}

static ant_value_t vm_script_compiled(ant_t *js, ant_value_t script, ant_realm_t *realm) {
  vm_script_t *st = vm_script_state(script);
  ant_value_t cached;
  if (ant_ptr_pair_get(&realm->vm_scripts, (uintptr_t)st->id, 0, &cached)) return cached;

  size_t len = 0;
  const char *source = js_getstr(js, js_get_slot(script, SLOT_DATA), &len);

  ant_realm_t *prev = ant_realm_switch(js, realm);
  const char *saved_filename = js->filename;
  js->filename = st->filename;
  
  ant_value_t compiled = js_vm_script_compile(js, source, len);
  
  js->filename = saved_filename;
  ant_realm_restore(js, prev);

  if (is_err(compiled)) return compiled;
  if (st->line_offset || st->column_offset)
    vm_offset_positions((sv_func_t *)vptr(compiled), st->line_offset, st->column_offset);

  ant_value_t handler = js_get_slot(script, SLOT_VM_IMPORT);
  ant_value_t referrer = is_callable(handler) ? js_weakref_create(js, script) : js_mkundef();
  if (is_err(referrer)) return referrer;
  vm_set_import(js, (sv_func_t *)vptr(compiled), handler, referrer);
  
  if (!vm_script_note_realm(st, realm) || !ant_ptr_pair_put(&realm->vm_scripts, (uintptr_t)st->id, 0, compiled))
    return js_mkerr(js, "out of memory");
  realm->gc_dirty = true;

  return compiled;
}

typedef struct vm_interrupt {
  ant_t *js;
  uint32_t timeout_ms;
  bool sigint;
  _Atomic uint8_t fired;
  
  uv_thread_t thread;
  uv_mutex_t lock;
  uv_cond_t cond;
  bool done;
  bool watching;
  
  struct vm_interrupt *outer;
  struct vm_interrupt *outer_sigint;
#ifndef _WIN32
  struct sigaction prev_sigint;
#else
  void (*prev_sigint)(int);
#endif
} vm_interrupt_t;

enum { VM_FIRED_TIMEOUT = 1, VM_FIRED_SIGINT = 2 };
static vm_interrupt_t *_Atomic vm_sigint_run;

static void vm_interrupt_fire(vm_interrupt_t *it, uint8_t why) {
  uint8_t none = 0;
  if (atomic_compare_exchange_strong(&it->fired, &none, why))
    atomic_store_explicit(&it->js->interrupt_pending, 1, memory_order_relaxed);
}

static void vm_watchdog(void *arg) {
  vm_interrupt_t *it = arg;
  uint64_t deadline = uv_hrtime() + (uint64_t)it->timeout_ms * 1000000u;
  
  uv_mutex_lock(&it->lock);
  while (!it->done) {
    uint64_t now = uv_hrtime();
    
    if (now >= deadline || uv_cond_timedwait(&it->cond, &it->lock, deadline - now) == UV_ETIMEDOUT) {
      if (!it->done) vm_interrupt_fire(it, VM_FIRED_TIMEOUT);
      break;
    }
  }
  
  uv_mutex_unlock(&it->lock);
}

static void vm_sigint_handler(int sig) {
  (void)sig;
  vm_interrupt_t *it = atomic_load(&vm_sigint_run);
  if (it) vm_interrupt_fire(it, VM_FIRED_SIGINT);
}

static ant_value_t vm_run_options(ant_t *js, ant_value_t options, vm_interrupt_t *it) {
  *it = (vm_interrupt_t){ .js = js };
  if (!is_object_type(options)) return js_mkundef();

  ant_value_t timeout = js_get(js, options, "timeout");
  if (vtype(timeout) != kTypeUndefined) {
    if (vtype(timeout) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "options.timeout", "of type number", timeout);
    double ms = js_getnum(timeout);
    if (ms != floor(ms)) return Ant_Error_CreateOutOfRange(js, "options.timeout", "an integer", timeout);
    if (ms <= 0 || ms > 4294967295.0) return Ant_Error_CreateOutOfRange(js, "options.timeout", ">= 1 && <= 4294967295", timeout);
    it->timeout_ms = (uint32_t)ms;
  }

  ant_value_t sigint = js_get(js, options, "breakOnSigint");
  if (vtype(sigint) != kTypeUndefined && vtype(sigint) != kTypeBool)
    return Ant_Error_CreateInvalidArgType(js, "options.breakOnSigint", "of type boolean", sigint);
  it->sigint = js_truthy(js, sigint);
  
  return js_mkundef();
}

static void vm_interrupt_begin(vm_interrupt_t *it) {
  if (!it->timeout_ms && !it->sigint) return;
  sv_jit_arm_interrupts(it->js);
  
  it->outer = it->js->interrupt_run;
  it->js->interrupt_run = it;

  if (it->timeout_ms) {
    uv_mutex_init(&it->lock);
    uv_cond_init(&it->cond);
    it->watching = uv_thread_create(&it->thread, vm_watchdog, it) == 0;
  }

  if (it->sigint) {
    it->outer_sigint = atomic_exchange(&vm_sigint_run, it);
  #ifndef _WIN32
    struct sigaction action = {0};
    action.sa_handler = vm_sigint_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &it->prev_sigint);
  #else
    it->prev_sigint = signal(SIGINT, vm_sigint_handler);
  #endif
  }
}

static bool vm_interrupt_outer_fired(vm_interrupt_t *it) {
  for (vm_interrupt_t *outer = it->outer; outer; outer = outer->outer)
    if (atomic_load(&outer->fired)) return true;
  return false;
}

static ant_value_t vm_interrupt_end(vm_interrupt_t *it, ant_value_t result) {
  if (!it->timeout_ms && !it->sigint) return result;
  ant_t *js = it->js;
  js->interrupt_run = it->outer;

  if (it->sigint) {
  #ifndef _WIN32
    sigaction(SIGINT, &it->prev_sigint, NULL);
  #else
    signal(SIGINT, it->prev_sigint);
  #endif
    atomic_store(&vm_sigint_run, it->outer_sigint);
  }

  if (it->timeout_ms) {
    uv_mutex_lock(&it->lock);
    it->done = true;
    uv_cond_signal(&it->cond);
    uv_mutex_unlock(&it->lock);
    
    if (it->watching) uv_thread_join(&it->thread);
    uv_cond_destroy(&it->cond);
    uv_mutex_destroy(&it->lock);
  }

  uint8_t fired = atomic_load(&it->fired);
  if (!fired) return result;

  if (is_err(result)) js_take_thrown(js, result);
  atomic_store_explicit(&js->interrupt_pending, 0, memory_order_relaxed);
  
  js->terminating = vm_interrupt_outer_fired(it);
  if (js->terminating) atomic_store_explicit(&js->interrupt_pending, 1, memory_order_relaxed);

  char message[96];
  if (fired == VM_FIRED_TIMEOUT) {
    snprintf(message, sizeof(message), "Script execution timed out after %ums", (unsigned)it->timeout_ms);
    return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_SCRIPT_EXECUTION_TIMEOUT", message);
  }
  return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_SCRIPT_EXECUTION_INTERRUPTED", "Script execution was interrupted by `SIGINT`");
}

static ant_value_t vm_script_run(ant_t *js, ant_value_t script, ant_realm_t *realm, ant_value_t options) {
  vm_interrupt_t interrupt;
  ant_value_t checked = vm_run_options(js, options, &interrupt);
  if (is_err(checked)) return checked;

  ant_value_t compiled = vm_script_compiled(js, script, realm);
  if (is_err(compiled)) return compiled;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, compiled);

  ant_realm_t *prev = ant_realm_switch(js, realm);
  vm_interrupt_begin(&interrupt);
  
  ant_value_t result = js_vm_script_run(js, compiled);
  if (!is_err(result) && realm->microtask_realm) {
    GC_ROOT_PIN(js, result);
    ant_realm_run_microtasks(js, realm);
  }
  
  result = vm_interrupt_end(&interrupt, result);
  ant_realm_restore(js, prev);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t vm_option_int32(ant_t *js, ant_value_t options, const char *name, int32_t *out) {
  ant_value_t value = js_get(js, options, name);
  if (vtype(value) == kTypeUndefined) return js_mkundef();

  char arg[64];
  snprintf(arg, sizeof(arg), "options.%s", name);
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, arg, "of type number", value);
  
  double number = js_getnum(value);
  if (number != floor(number)) return Ant_Error_CreateOutOfRange(js, arg, "an integer", value);
  if (number < -2147483648.0 || number > 2147483647.0)
    return Ant_Error_CreateOutOfRange(js, arg, ">= -2147483648 && <= 2147483647", value);

  *out = (int32_t)number;
  return js_mkundef();
}

static ant_value_t vm_source_map_url(ant_t *js, const char *source, size_t len) {
  static const char marker[] = "sourceMappingURL=";
  const char *found = NULL;

  for (const char *p = source; (p = memmem(p, len - (size_t)(p - source), marker, sizeof(marker) - 1)); p++) {
    if (p - source >= 4 && p[-4] == '/' && p[-3] == '/' && (p[-2] == '#' || p[-2] == '@') && p[-1] == ' ')
      found = p + sizeof(marker) - 1;
  }

  if (!found) return js_mkundef();
  const char *end = found;

  while (end < source + len && *end != '\n' && *end != '\r' && *end != ' ' && *end != '\t') end++;
  return js_mkstr(js, found, (size_t)(end - found));
}

static ant_value_t vm_script_init(ant_t *js, ant_value_t script, ant_value_t code, ant_value_t options) {
  if (vtype(code) != kTypeString) code = js_tostring_val(js, code);
  if (is_err(code)) return code;
  
  if (vtype(options) == kTypeString) {
    ant_value_t filename = options;
    options = Ant_Object_CreateBare(js);
    js_set(js, options, "filename", filename);
  } else if (vtype(options) != kTypeUndefined && !is_object_type(options))
    return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);

  vm_script_t *st = calloc(1, sizeof(*st));
  if (!st) return js_mkerr(js, "out of memory");
  
  st->filename = vm_default_filename;
  st->id = atomic_fetch_add(&vm_next_script_id, 1);
  
  js_set_native(script, st, VM_SCRIPT_NATIVE_TAG);
  js_set_finalizer(script, vm_script_finalize);

  js_set_slot(script, SLOT_DATA, code);
  js_set_slot(script, SLOT_VM_IMPORT, js_mknull());

  if (is_object_type(options)) {
    ant_value_t filename = js_get(js, options, "filename");
    if (vtype(filename) == kTypeString) {
      size_t len = 0;
      const char *text = js_getstr(js, filename, &len);
      st->filename = intern_string(text, len);
      if (!st->filename) return js_mkerr(js, "out of memory");
    } else if (vtype(filename) != kTypeUndefined) return Ant_Error_CreateInvalidArgType(js, "options.filename", "of type string", filename);

    ant_value_t import_handler;
    ant_value_t result = vm_import_option(js, options, &import_handler);
    
    if (is_err(result)) return result;
    js_set_slot(script, SLOT_VM_IMPORT, import_handler);

    result = vm_option_int32(js, options, "lineOffset", &st->line_offset);
    if (is_err(result)) return result;
    
    result = vm_option_int32(js, options, "columnOffset", &st->column_offset);
    if (is_err(result)) return result;

    ant_value_t cached = js_get(js, options, "cachedData");
    if (vtype(cached) != kTypeUndefined) {
      if (!buffer_is_dataview(cached) && !buffer_get_typedarray_data(cached)) return Ant_Error_CreateInvalidArgType(
        js, "options.cachedData", "an instance of Buffer, TypedArray, or DataView", cached
      );
      js_set(js, script, "cachedDataRejected", js_false);
    }
    
    if (vm_option_bool(js, options, "produceCachedData")) {
      js_set(js, script, "cachedDataProduced", js_true);
      js_set(js, script, "cachedData", create_typed_array(js, TYPED_ARRAY_UINT8, create_array_buffer_data(js, 0), 0, 0, js->builtins.buffer_proto));
    }
  }

  size_t len = 0;
  const char *source = js_getstr(js, code, &len);
  js_set(js, script, "sourceMapURL", vm_source_map_url(js, source, len));

  ant_value_t compiled = vm_script_compiled(js, script, js->realm);
  return is_err(compiled) ? compiled : script;
}

static ant_value_t vm_script_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Script cannot be invoked without 'new'");

  ant_value_t script = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.vm_script_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(script, proto);

  return vm_script_init(js, script, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef());
}

static ant_value_t vm_this_script(ant_t *js) {
  ant_value_t script = js->this_val;
  if (!is_object_type(script) || !vm_script_state(script))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation: receiver is not a vm.Script");
  return script;
}

static ant_value_t vm_script_run_in_this_context(ant_params_t) {
  ant_value_t script = vm_this_script(js);
  if (is_err(script)) return script;
  return vm_script_run(js, script, js->realm, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t vm_script_run_in_context(ant_params_t) {
  ant_value_t script = vm_this_script(js);
  if (is_err(script)) return script;

  ant_value_t context = nargs > 0 ? args[0] : js_mkundef();
  ant_realm_t *realm = vm_context_realm(context);
  if (!realm) return Ant_Error_CreateInvalidArgType(js, "contextifiedObject", "an vm.Context", context);

  return vm_script_run(js, script, realm, nargs > 1 ? args[1] : js_mkundef());
}

static ant_value_t vm_script_run_in_new_context(ant_params_t) {
  ant_value_t script = vm_this_script(js);
  if (is_err(script)) return script;

  ant_value_t context = vm_context_for(js, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef(), true);
  if (is_err(context)) return context;

  return vm_script_run(js, script, vm_context_realm(context), nargs > 1 ? args[1] : js_mkundef());
}

static ant_value_t vm_script_create_cached_data(ant_params_t) {
  ant_value_t script = vm_this_script(js);
  if (is_err(script)) return script;
  return create_typed_array(js, TYPED_ARRAY_UINT8, create_array_buffer_data(js, 0), 0, 0, js->builtins.buffer_proto);
}

static ant_value_t vm_new_script(ant_t *js, ant_value_t code, ant_value_t options) {
  ant_value_t script = Ant_Object_CreateBare(js);
  js_set_proto_init(script, js->builtins.vm_script_proto);
  return vm_script_init(js, script, code, options);
}

static ant_value_t vm_run_in_this_context(ant_params_t) {
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t script = vm_new_script(js, nargs > 0 ? args[0] : js_mkundef(), options);
  if (is_err(script)) return script;
  return vm_script_run(js, script, js->realm, options);
}

static ant_value_t vm_run_in_context(ant_params_t) {
  ant_value_t context = nargs > 1 ? args[1] : js_mkundef();
  ant_realm_t *realm = vm_context_realm(context);
  if (!realm) return Ant_Error_CreateInvalidArgType(js, "contextifiedObject", "an vm.Context", context);

  ant_value_t options = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t script = vm_new_script(js, nargs > 0 ? args[0] : js_mkundef(), options);
  if (is_err(script)) return script;

  return vm_script_run(js, script, realm, options);
}

static ant_value_t vm_run_in_new_context(ant_params_t) {
  ant_value_t options = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t script = vm_new_script(js, nargs > 0 ? args[0] : js_mkundef(), options);
  if (is_err(script)) return script;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, script);
  ant_value_t context = vm_context_for(js, nargs > 1 ? args[1] : js_mkundef(), vtype(options) == kTypeString ? js_mkundef() : options, true);

  ant_value_t result = is_err(context) ? context : vm_script_run(js, script, vm_context_realm(context), options);
  GC_ROOT_RESTORE(js, mark);

  return result;
}

static constexpr int32_t VM_FUNCTION_WRAPPER_LINES = 2;

static ant_value_t vm_function_with_extensions(
  ant_t *js, ant_value_t *parts, int param_count, ant_value_t extensions
) {
  ant_offset_t count = js_arr_len(js, extensions);
  static const char head[] = "return function anonymous(";
  static const char middle[] = "\n) {\n";
  static const char tail[] = "\n}";

  size_t total = sizeof(head) + sizeof(middle) + sizeof(tail);
  for (ant_offset_t i = 0; i < count; i++) total += (size_t)snprintf(NULL, 0, "with (this[%u]) ", (unsigned)i);
  for (int i = 0; i <= param_count; i++) {
    size_t len = 0;
    js_getstr(js, parts[i], &len);
    total += len + 1;
  }
  
  char *text = malloc(total);
  if (!text) return js_mkerr(js, "out of memory");
  size_t pos = 0;

  for (ant_offset_t i = 0; i < count; i++)
    pos += (size_t)snprintf(text + pos, total - pos, "with (this[%u]) ", (unsigned)i);
  memcpy(text + pos, head, sizeof(head) - 1);
  pos += sizeof(head) - 1;
  
  for (int i = 0; i < param_count; i++) {
    size_t len = 0;
    const char *param = js_getstr(js, parts[i], &len);
    if (i) text[pos++] = ',';
    memcpy(text + pos, param, len);
    pos += len;
  }
  
  size_t body_len = 0;
  const char *body = js_getstr(js, parts[param_count], &body_len);
  memcpy(text + pos, middle, sizeof(middle) - 1);
  pos += sizeof(middle) - 1;
  
  memcpy(text + pos, body, body_len);
  pos += body_len;
  
  memcpy(text + pos, tail, sizeof(tail) - 1);
  pos += sizeof(tail) - 1;

  GC_ROOT_SAVE(mark, js);
  ant_value_t source = js_mkstr(js, text, pos);
  free(text);
  GC_ROOT_PIN(js, source);

  ant_value_t wrapper = js_function_from_source(js, &source, 1);
  if (is_err(wrapper)) {
    GC_ROOT_RESTORE(js, mark);
    return wrapper;
  }
  GC_ROOT_PIN(js, wrapper);

  ant_value_t scopes = js_mkarr(js);
  GC_ROOT_PIN(js, scopes);
  for (ant_offset_t i = 0; i < count; i++) js_arr_push(js, scopes, js_arr_get(js, extensions, i));

  ant_value_t fn = sv_vm_call(js->vm, js, wrapper, scopes, NULL, 0, NULL, js_mkundef());
  GC_ROOT_RESTORE(js, mark);
  return fn;
}

static ant_value_t vm_compile_function(ant_params_t) {
  ant_value_t code = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t params = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t options = nargs > 2 ? args[2] : js_mkundef();

  if (vtype(code) != kTypeString) return Ant_Error_CreateInvalidArgType(js, "code", "of type string", code);
  if (vtype(params) != kTypeUndefined && vtype(params) != kTypeArray)
    return Ant_Error_CreateInvalidArgType(js, "params", "an instance of Array", params);
  if (vtype(options) != kTypeUndefined && !is_object_type(options))
    return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);

  ant_realm_t *realm = js->realm;
  const char *filename = "";
  ant_value_t extensions = js_mkundef();
  ant_value_t import_handler = js_mknull();

  if (is_object_type(options)) {
    ant_value_t parsing_context = js_get(js, options, "parsingContext");
    if (vtype(parsing_context) != kTypeUndefined) {
      realm = vm_context_realm(parsing_context);
      if (!realm) return Ant_Error_CreateInvalidArgType(js, "options.parsingContext", "an instance of Context", parsing_context);
    }

    extensions = js_get(js, options, "contextExtensions");
    if (vtype(extensions) != kTypeUndefined && vtype(extensions) != kTypeArray)
      return Ant_Error_CreateInvalidArgType(js, "options.contextExtensions", "an instance of Array", extensions);
    for (ant_offset_t i = 0; vtype(extensions) == kTypeArray && i < js_arr_len(js, extensions); i++) {
      ant_value_t extension = js_arr_get(js, extensions, i);
      if (!is_object_type(extension)) {
        char arg[48];
        snprintf(arg, sizeof(arg), "options.contextExtensions[%u]", (unsigned)i);
        return Ant_Error_CreateInvalidArgType(js, arg, "of type object", extension);
      }
    }

    ant_value_t checked = vm_import_option(js, options, &import_handler);
    if (is_err(checked)) return checked;

    ant_value_t name = js_get(js, options, "filename");
    if (vtype(name) == kTypeString) {
      size_t len = 0;
      const char *text = js_getstr(js, name, &len);
      filename = intern_string(text, len);
      if (!filename) return js_mkerr(js, "out of memory");
    } else if (vtype(name) != kTypeUndefined) return Ant_Error_CreateInvalidArgType(js, "options.filename", "of type string", name);
  }

  ant_offset_t param_count = vtype(params) == kTypeArray ? js_arr_len(js, params) : 0;
  if (param_count > 65535) return js_mkerr_typed(js, JS_ERR_RANGE, "too many parameters");

  ant_value_t *parts = malloc(((size_t)param_count + 1) * sizeof(*parts));
  if (!parts) return js_mkerr(js, "out of memory");

  GC_ROOT_SAVE(mark, js);
  for (ant_offset_t i = 0; i < param_count; i++) {
    parts[i] = js_arr_get(js, params, i);
    if (vtype(parts[i]) != kTypeString) {
      char arg[32];
      snprintf(arg, sizeof(arg), "params[%u]", (unsigned)i);
      ant_value_t error = Ant_Error_CreateInvalidArgType(js, arg, "of type string", parts[i]);
      free(parts);
      GC_ROOT_RESTORE(js, mark);
      return error;
    }
    GC_ROOT_PIN(js, parts[i]);
  }
  parts[param_count] = code;

  ant_realm_t *prev = ant_realm_switch(js, realm);
  const char *saved_filename = js->filename;
  js->filename = filename;
  
  bool extended = vtype(extensions) == kTypeArray && js_arr_len(js, extensions) > 0;
  ant_value_t fn = js_function_from_source(js, parts, (int)param_count + 1);
  if (!is_err(fn) && extended) fn = vm_function_with_extensions(js, parts, (int)param_count, extensions);
  
  if (vtype(fn) == kTypeFunction) {
    sv_func_t *func = js_func_closure(fn)->func;
    vm_offset_positions(func, -VM_FUNCTION_WRAPPER_LINES * (extended ? 2 : 1), 0);
    vm_set_import(js, func, import_handler, is_callable(import_handler) ? fn : js_mkundef());
  }
  
  js->filename = saved_filename;
  ant_realm_restore(js, prev);

  free(parts);
  GC_ROOT_RESTORE(js, mark);
  return fn;
}

static ant_value_t vm_option_one_of(
  ant_t *js, ant_value_t options, const char *name, const char *arg,
  const char *const *allowed, size_t count, const char *expected
) {
  ant_value_t value = js_get(js, options, name);
  if (vtype(value) == kTypeUndefined) return js_mkundef();
  
  const char *text = vtype(value) == kTypeString ? js_getstr(js, value, NULL) : NULL;
  for (size_t i = 0; text && i < count; i++) if (strcmp(text, allowed[i]) == 0) return js_mkundef();
  
  return Ant_Error_CreateInvalidArgValue(js, arg, expected, value);
}

static ant_value_t vm_measure_memory(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(options) != kTypeUndefined && !is_object_type(options))
    return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);

  if (is_object_type(options)) {
    static const char *const modes[] = { "summary", "detailed" };
    static const char *const executions[] = { "default", "eager" };
    
    ant_value_t checked = vm_option_one_of(js, options, "mode", "options.mode", modes, countof(modes), "'summary', 'detailed'");
    if (is_err(checked)) return checked;
    
    checked = vm_option_one_of(js, options, "execution", "options.execution", executions, countof(executions), "'default', 'eager'");
    if (is_err(checked)) return checked;
  }

  size_t used =
    js->obj_arena.live_count * js->obj_arena.elem_size +
    js->closure_arena.live_count * js->closure_arena.elem_size +
    js->gc.pool_last_live + js->gc.pool_alloc +
    js->alloc_bytes.closures + js->alloc_bytes.upvalues;
  
  ant_value_t estimate = js_mknum((double)used);
  ant_value_t range = js_mkarr(js);
  js_arr_push(js, range, estimate);
  js_arr_push(js, range, estimate);

  ant_value_t total = Ant_Object_CreateBare(js);
  js_set(js, total, "jsMemoryEstimate", estimate);
  js_set(js, total, "jsMemoryRange", range);

  ant_value_t wasm = Ant_Object_CreateBare(js);
  js_set(js, wasm, "code", js_mknum(0));
  js_set(js, wasm, "metadata", js_mknum(0));

  ant_value_t result = Ant_Object_CreateBare(js);
  js_set(js, result, "total", total);
  js_set(js, result, "WebAssembly", wasm);

  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  
  return promise;
}

ant_value_t vm_library(ant_t *js) {
  if (!js->builtins.vm_script_proto) {
    ant_value_t proto = Ant_Object_CreateBare(js);
    js->builtins.vm_script_proto = proto;
    js_set(js, proto, "runInThisContext", Ant_Function_Create(vm_script_run_in_this_context));
    js_set(js, proto, "runInContext", Ant_Function_Create(vm_script_run_in_context));
    js_set(js, proto, "runInNewContext", Ant_Function_Create(vm_script_run_in_new_context));
    js_set(js, proto, "createCachedData", Ant_Function_Create(vm_script_create_cached_data));
    js->builtins.vm_dont_contextify = js_mksym(js, "vm_dont_contextify");
    js->builtins.vm_main_context_loader = js_mksym(js, "vm_dynamic_import_main_context_default");
  }

  ant_value_t lib = Ant_Object_CreateBare(js);
  js_set(js, lib, "Script", Ant_Function_CreateConstructor(js, vm_script_ctor, js->builtins.vm_script_proto, "Script", 6));
  js_set(js, lib, "createContext", Ant_Function_Create(vm_create_context));
  js_set(js, lib, "isContext", Ant_Function_Create(vm_is_context));
  js_set(js, lib, "runInContext", Ant_Function_Create(vm_run_in_context));
  js_set(js, lib, "runInNewContext", Ant_Function_Create(vm_run_in_new_context));
  js_set(js, lib, "runInThisContext", Ant_Function_Create(vm_run_in_this_context));
  js_set(js, lib, "compileFunction", Ant_Function_Create(vm_compile_function));
  js_set(js, lib, "measureMemory", Ant_Function_Create(vm_measure_memory));

  ant_value_t constants = Ant_Object_CreateBare(js);
  js_set(js, constants, "USE_MAIN_CONTEXT_DEFAULT_LOADER", js->builtins.vm_main_context_loader);
  js_set(js, constants, "DONT_CONTEXTIFY", js->builtins.vm_dont_contextify);
  js_set(js, lib, "constants", constants);

  return lib;
}
